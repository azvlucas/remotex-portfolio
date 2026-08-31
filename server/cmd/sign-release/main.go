// sign-release — Ferramenta CLI para assinar binários de release com Ed25519.
//
// Uso:
//   go run ./cmd/sign-release -key private_key.bin -file remotex_host.exe
//
// Gera:
//   - remotex_host.exe.sig  (assinatura binária, 64 bytes)
//   - Imprime a assinatura em hex no stdout
//
// Para gerar um par de chaves Ed25519:
//   go run ./cmd/sign-release -genkey
//   → gera ed25519_private.bin (64 bytes) e ed25519_public.bin (32 bytes)
package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/hex"
	"flag"
	"fmt"
	"os"
)

func main() {
	genkey := flag.Bool("genkey", false, "Gera par de chaves Ed25519")
	keyFile := flag.String("key", "ed25519_private.bin", "Arquivo da chave privada Ed25519 (64 bytes)")
	binFile := flag.String("file", "", "Arquivo binário para assinar")
	flag.Parse()

	if *genkey {
		generateKeyPair()
		return
	}

	if *binFile == "" {
		fmt.Fprintln(os.Stderr, "Uso: sign-release -key <private_key.bin> -file <binary>")
		fmt.Fprintln(os.Stderr, "     sign-release -genkey")
		os.Exit(1)
	}

	signFile(*keyFile, *binFile)
}

func generateKeyPair() {
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Erro ao gerar chaves: %v\n", err)
		os.Exit(1)
	}

	if err := os.WriteFile("ed25519_private.bin", priv, 0600); err != nil {
		fmt.Fprintf(os.Stderr, "Erro ao salvar chave privada: %v\n", err)
		os.Exit(1)
	}

	if err := os.WriteFile("ed25519_public.bin", pub, 0644); err != nil {
		fmt.Fprintf(os.Stderr, "Erro ao salvar chave pública: %v\n", err)
		os.Exit(1)
	}

	fmt.Println("Chaves geradas:")
	fmt.Println("  Privada: ed25519_private.bin (64 bytes)")
	fmt.Println("  Pública: ed25519_public.bin  (32 bytes)")
	fmt.Println()
	fmt.Printf("  Pública (hex): %s\n", hex.EncodeToString(pub))
}

func signFile(keyFile, binFile string) {
	// Ler chave privada.
	privBytes, err := os.ReadFile(keyFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Erro ao ler chave privada: %v\n", err)
		os.Exit(1)
	}
	if len(privBytes) != ed25519.PrivateKeySize {
		fmt.Fprintf(os.Stderr, "Chave privada inválida: esperado %d bytes, obtido %d\n",
			ed25519.PrivateKeySize, len(privBytes))
		os.Exit(1)
	}
	priv := ed25519.PrivateKey(privBytes)

	// Ler binário.
	data, err := os.ReadFile(binFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Erro ao ler binário: %v\n", err)
		os.Exit(1)
	}

	// Assinar.
	sig := ed25519.Sign(priv, data)

	// Salvar assinatura.
	sigFile := binFile + ".sig"
	if err := os.WriteFile(sigFile, sig, 0644); err != nil {
		fmt.Fprintf(os.Stderr, "Erro ao salvar assinatura: %v\n", err)
		os.Exit(1)
	}

	sigHex := hex.EncodeToString(sig)

	fmt.Printf("Arquivo:     %s (%d bytes)\n", binFile, len(data))
	fmt.Printf("Assinatura:  %s\n", sigFile)
	fmt.Printf("Hex:         %s\n", sigHex)
	fmt.Printf("File size:   %d\n", len(data))
}
