// RemoteX — host/heartbeat.cpp
// Thread de heartbeat periódico.

#include "host/heartbeat.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace remotex::host {

Heartbeat::~Heartbeat()
{
    Stop();
}

void Heartbeat::Start(const HeartbeatConfig& config, remotex::core::MachineClient* client)
{
    if (running_)
        return;

    config_      = config;
    client_      = client;
    should_stop_ = false;
    start_time_  = std::chrono::steady_clock::now();
    running_     = true;

    thread_ = std::thread(&Heartbeat::HeartbeatLoop, this);
}

void Heartbeat::Stop()
{
    should_stop_ = true;
    if (thread_.joinable())
        thread_.join();
    running_ = false;
}

int64_t Heartbeat::GetUptimeSeconds() const
{
    if (!running_)
        return 0;
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
}

void Heartbeat::HeartbeatLoop()
{
    while (!should_stop_) {
        // Enviar heartbeat.
        if (client_) {
            auto uptime = GetUptimeSeconds();
            const auto result = client_->SendHeartbeat(
                config_.machine_id,
                config_.machine_key,
                uptime,
                config_.agent_version);
            if (result.success) {
                if (result.force_update && config_.on_force_update) {
                    config_.on_force_update();
                }
            } else if (config_.on_error) {
                std::string message = "heartbeat failed";
                if (!result.error.empty()) {
                    message += ": " + result.error;
                }
                config_.on_error(message);
            }
        }

        // Dormir em intervalos curtos para reagir rápido ao Stop().
        uint32_t wait_ms = config_.interval_sec * 1000;
        for (uint32_t elapsed = 0; elapsed < wait_ms && !should_stop_; elapsed += 250)
            Sleep(250);
    }
}

} // namespace remotex::host
