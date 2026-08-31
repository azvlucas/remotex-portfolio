#pragma once
// RemoteX — host/heartbeat.h
// Thread em background que envia heartbeat periódico ao backend.

#include "core/machine_client.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace remotex::host {

// Configuração do heartbeat.
struct HeartbeatConfig {
    std::string machine_id;
    std::string machine_key;
    std::string agent_version;
    std::function<void(const std::string&)> on_error;
    std::function<void()> on_force_update;
    uint32_t    interval_sec = 30;   // Padrão: 30 segundos
};

// Heartbeat envia sinal de presença periódico ao backend via MachineClient.
//
// Uso:
//   Heartbeat hb;
//   hb.Start(config, &machineClient);
//   // ... aplicação rodando ...
//   hb.Stop();
class Heartbeat {
public:
    Heartbeat()  = default;
    ~Heartbeat();

    Heartbeat(const Heartbeat&)            = delete;
    Heartbeat& operator=(const Heartbeat&) = delete;

    // Inicia a thread de heartbeat.
    void Start(const HeartbeatConfig& config, remotex::core::MachineClient* client);

    // Para a thread de heartbeat.
    void Stop();

    bool IsRunning() const { return running_; }

    // Retorna o uptime em segundos desde o Start().
    int64_t GetUptimeSeconds() const;

private:
    void HeartbeatLoop();

    HeartbeatConfig              config_;
    remotex::core::MachineClient* client_ = nullptr;
    std::thread                  thread_;
    std::atomic<bool>            running_{false};
    std::atomic<bool>            should_stop_{false};
    std::chrono::steady_clock::time_point start_time_;
};

} // namespace remotex::host
