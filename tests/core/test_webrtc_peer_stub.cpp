#include <gtest/gtest.h>

#include "core/webrtc_peer.h"

#include <chrono>
#include <condition_variable>
#include <array>
#include <mutex>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

using namespace remotex::core;

namespace {

class ScopedWinsock final {
public:
    ScopedWinsock() {
        WSADATA data{};
        const int rc = WSAStartup(MAKEWORD(2, 2), &data);
        EXPECT_EQ(rc, 0);
    }

    ~ScopedWinsock() {
        WSACleanup();
    }
};

class FakeRelayServer final {
public:
    static constexpr int kExpectedConnections = 3;

    FakeRelayServer() {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        EXPECT_NE(listener_, INVALID_SOCKET);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        const int reuse = 1;
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        if (bind(listener_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            ADD_FAILURE() << "bind failed: " << WSAGetLastError();
            return;
        }
        if (listen(listener_, kExpectedConnections) != 0) {
            ADD_FAILURE() << "listen failed: " << WSAGetLastError();
            return;
        }

        int addr_len = sizeof(addr);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0) {
            ADD_FAILURE() << "getsockname failed: " << WSAGetLastError();
            return;
        }
        port_ = ntohs(addr.sin_port);

        thread_ = std::thread([this]() { Run(); });
    }

    ~FakeRelayServer() {
        Stop();
    }

    uint16_t port() const { return port_; }

    void CloseClient() {
        std::lock_guard lock(mutex_);
        for (auto& client : clients_) {
            if (client != INVALID_SOCKET) {
                shutdown(client, SD_BOTH);
                closesocket(client);
                client = INVALID_SOCKET;
            }
        }
    }

    void Stop() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
            for (auto& client : clients_) {
                if (client != INVALID_SOCKET) {
                    shutdown(client, SD_BOTH);
                    closesocket(client);
                    client = INVALID_SOCKET;
                }
            }
            if (listener_ != INVALID_SOCKET) {
                closesocket(listener_);
                listener_ = INVALID_SOCKET;
            }
        }

        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void Run() {
        for (int index = 0; index < kExpectedConnections; ++index) {
            SOCKET accepted = accept(listener_, nullptr, nullptr);
            {
                std::lock_guard lock(mutex_);
                if (stop_ || accepted == INVALID_SOCKET) {
                    if (accepted != INVALID_SOCKET) {
                        closesocket(accepted);
                    }
                    return;
                }
                clients_[index] = accepted;
            }

            std::string handshake;
            char ch = '\0';
            while (recv(accepted, &ch, 1, 0) == 1) {
                handshake.push_back(ch);
                if (ch == '\n') {
                    break;
                }
            }

            if (!handshake.empty()) {
                send(accepted, "READY\n", 6, 0);
            }
        }
    }

    mutable std::mutex mutex_;
    SOCKET listener_ = INVALID_SOCKET;
    std::array<SOCKET, kExpectedConnections> clients_{
        INVALID_SOCKET, INVALID_SOCKET, INVALID_SOCKET};
    uint16_t port_ = 0;
    bool stop_ = false;
    std::thread thread_;
};

class PeerListener final : public WebRTCPeerListener {
public:
    size_t Mark() const {
        std::lock_guard lock(mutex_);
        return states_.size();
    }

    bool WaitForStateAfter(PeerConnectionState expected,
                           size_t start_index,
                           std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            for (size_t i = start_index; i < states_.size(); ++i) {
                if (states_[i] == expected) {
                    return true;
                }
            }
            return false;
        });
    }

    void OnLocalDescription(const SessionDescription&) override {}
    void OnIceCandidate(const IceCandidate&) override {}
    void OnConnectionStateChanged(PeerConnectionState state) override {
        {
            std::lock_guard lock(mutex_);
            states_.push_back(state);
        }
        cv_.notify_all();
    }
    void OnIceGatheringStateChanged(IceGatheringState) override {}
    void OnVideoFrame(const VideoFrame&) override {}
    void OnDataChannelMessage(const DataChannelMessage&) override {}
    void OnDataChannelOpen(const std::string&) override {}
    void OnDataChannelClosed(const std::string&) override {}

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<PeerConnectionState> states_;
};

} // namespace

TEST(WebRTCPeerStubTest, ReconnectsAfterTransportDrops) {
#ifdef REMOTEX_HAS_LIBWEBRTC
    GTEST_SKIP() << "Stub-specific test";
#else
    ScopedWinsock winsock;
    PeerListener listener;
    WebRTCPeer peer;
    peer.SetListener(&listener);

    FakeRelayServer relay1;

    PeerConfig cfg;
    cfg.session_id = "session-test";
    cfg.is_offerer = true;
    cfg.relay_host = "127.0.0.1";
    cfg.relay_port = relay1.port();
    cfg.relay_ticket = "test-ticket";
    cfg.relay_ticket_expires_unix = 1893456000;

    size_t start = listener.Mark();
    ASSERT_TRUE(peer.Initialize(cfg));
    peer.CreateOffer();
    ASSERT_TRUE(peer.SetRemoteDescription({"answer", "stub-answer"}));
    ASSERT_TRUE(listener.WaitForStateAfter(PeerConnectionState::Connecting, start));
    ASSERT_TRUE(listener.WaitForStateAfter(PeerConnectionState::Connected, start));

    start = listener.Mark();
    relay1.CloseClient();
    ASSERT_TRUE(listener.WaitForStateAfter(PeerConnectionState::Disconnected, start));

    FakeRelayServer relay2;
    cfg.relay_port = relay2.port();
    cfg.relay_ticket = "test-ticket-2";

    start = listener.Mark();
    ASSERT_TRUE(peer.Initialize(cfg));
    ASSERT_TRUE(peer.SetRemoteDescription({"answer", "stub-answer"}));
    ASSERT_TRUE(listener.WaitForStateAfter(PeerConnectionState::Connecting, start));
    ASSERT_TRUE(listener.WaitForStateAfter(PeerConnectionState::Connected, start));

    peer.Shutdown();
    EXPECT_EQ(peer.GetConnectionState(), PeerConnectionState::Closed);
#endif
}
