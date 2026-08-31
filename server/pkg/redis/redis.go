package redis

import (
	"context"
	"log/slog"
	"time"

	"github.com/redis/go-redis/v9"
)

// PubSub encapsula um cliente Redis para publicar e subscrever mensagens.
type PubSub struct {
	client *redis.Client
	ctx    context.Context
	cancel context.CancelFunc
}

// Config contém as opções de conexão Redis.
type Config struct {
	Addr     string
	Password string
	DB       int
}

// New cria um novo PubSub conectado ao Redis.
func New(cfg Config) (*PubSub, error) {
	client := redis.NewClient(&redis.Options{
		Addr:         cfg.Addr,
		Password:     cfg.Password,
		DB:           cfg.DB,
		DialTimeout:  5 * time.Second,
		ReadTimeout:  3 * time.Second,
		WriteTimeout: 3 * time.Second,
	})

	ctx, cancel := context.WithCancel(context.Background())

	// Verificar conectividade.
	pingCtx, pingCancel := context.WithTimeout(ctx, 5*time.Second)
	defer pingCancel()
	if err := client.Ping(pingCtx).Err(); err != nil {
		cancel()
		client.Close()
		return nil, err
	}

	slog.Info("redis connected", "addr", cfg.Addr)

	return &PubSub{
		client: client,
		ctx:    ctx,
		cancel: cancel,
	}, nil
}

// Publish envia uma mensagem para um canal Redis.
func (ps *PubSub) Publish(channel string, data []byte) error {
	return ps.client.Publish(ps.ctx, channel, data).Err()
}

// Subscribe retorna um canal Go que recebe mensagens do canal Redis.
func (ps *PubSub) Subscribe(channel string) (<-chan string, error) {
	sub := ps.client.Subscribe(ps.ctx, channel)

	// Esperar confirmação da subscription.
	_, err := sub.Receive(ps.ctx)
	if err != nil {
		return nil, err
	}

	out := make(chan string, 128)

	go func() {
		defer close(out)
		ch := sub.Channel()
		for {
			select {
			case msg, ok := <-ch:
				if !ok {
					return
				}
				out <- msg.Payload
			case <-ps.ctx.Done():
				sub.Close()
				return
			}
		}
	}()

	return out, nil
}

// Ping verifica se a conexão Redis está ativa.
func (ps *PubSub) Ping() error {
	ctx, cancel := context.WithTimeout(ps.ctx, 3*time.Second)
	defer cancel()
	return ps.client.Ping(ctx).Err()
}

// Set stores a key-value pair with an optional TTL.
func (ps *PubSub) Set(key, value string, ttl time.Duration) error {
	ctx, cancel := context.WithTimeout(ps.ctx, 3*time.Second)
	defer cancel()
	return ps.client.Set(ctx, key, value, ttl).Err()
}

// Get retrieves the value for a key. Returns "" and redis.Nil if the key does not exist.
func (ps *PubSub) Get(key string) (string, error) {
	ctx, cancel := context.WithTimeout(ps.ctx, 3*time.Second)
	defer cancel()
	return ps.client.Get(ctx, key).Result()
}

// Del deletes one or more keys.
func (ps *PubSub) Del(keys ...string) error {
	ctx, cancel := context.WithTimeout(ps.ctx, 3*time.Second)
	defer cancel()
	return ps.client.Del(ctx, keys...).Err()
}

// Close encerra a conexão Redis.
func (ps *PubSub) Close() error {
	ps.cancel()
	return ps.client.Close()
}
