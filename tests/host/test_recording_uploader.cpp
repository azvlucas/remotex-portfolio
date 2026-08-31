#include "host/recording_uploader.h"

#include <gtest/gtest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

using namespace remotex::host;
namespace fs = std::filesystem;

namespace remotex::host {

class RecordingUploaderTestPeer {
public:
    static RecordingUploader::HttpResponse HttpRequest(RecordingUploader& uploader,
                                                       const std::string& method,
                                                       const std::string& path,
                                                       const std::string& json_body) {
        return uploader.HttpRequest(method, path, json_body);
    }

    static RecordingUploader::HttpResponse HttpPut(RecordingUploader& uploader,
                                                   const std::string& url,
                                                   const fs::path& file_path) {
        return uploader.HttpPut(url, file_path);
    }
};

} // namespace remotex::host

namespace {

class WsaScope {
public:
    WsaScope() {
        WSADATA data{};
        const int rc = WSAStartup(MAKEWORD(2, 2), &data);
        if (rc != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    }

    ~WsaScope() {
        WSACleanup();
    }
};

class SilentTcpServer {
public:
    SilentTcpServer() {
        listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_socket_ == INVALID_SOCKET) {
            throw std::runtime_error("socket failed");
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        if (bind(listen_socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::runtime_error("bind failed");
        }
        if (listen(listen_socket_, 1) != 0) {
            throw std::runtime_error("listen failed");
        }

        int addr_len = sizeof(addr);
        if (getsockname(listen_socket_, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0) {
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(addr.sin_port);

        accept_thread_ = std::thread([this]() {
            SOCKET client = accept(listen_socket_, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                return;
            }
            client_socket_ = client;
            while (!stop_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }

    ~SilentTcpServer() {
        stop_.store(true);

        if (client_socket_ != INVALID_SOCKET) {
            shutdown(client_socket_, SD_BOTH);
            closesocket(client_socket_);
            client_socket_ = INVALID_SOCKET;
        }
        if (listen_socket_ != INVALID_SOCKET) {
            shutdown(listen_socket_, SD_BOTH);
            closesocket(listen_socket_);
            listen_socket_ = INVALID_SOCKET;
        }
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
    }

    uint16_t port() const { return port_; }

private:
    SOCKET listen_socket_ = INVALID_SOCKET;
    SOCKET client_socket_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread accept_thread_;
};

RecordingUploader::Config TestConfig(uint16_t port) {
    RecordingUploader::Config config;
    config.api_host = "127.0.0.1";
    config.api_port = port;
    config.use_tls = false;
    config.machine_id = "machine-1";
    config.resolve_timeout_ms = 100;
    config.connect_timeout_ms = 100;
    config.send_timeout_ms = 100;
    config.receive_timeout_ms = 200;
    return config;
}

} // namespace

TEST(RecordingUploader, HttpRequestTimesOutWhenServerAcceptsButDoesNotRespond) {
    WsaScope wsa;
    SilentTcpServer server;

    RecordingUploader uploader;
    uploader.Initialize(TestConfig(server.port()), nullptr);

    const auto start = std::chrono::steady_clock::now();
    const auto response = remotex::host::RecordingUploaderTestPeer::HttpRequest(
        uploader,
        "POST",
        "/recordings/upload/machine-1",
        "{}");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    EXPECT_EQ(response.status_code, 0);
    EXPECT_NE(response.body.find("winhttp_error="), std::string::npos);
    EXPECT_LT(elapsed.count(), 10000);
}

TEST(RecordingUploader, HttpPutTimesOutWhenServerAcceptsButDoesNotRespond) {
    WsaScope wsa;
    SilentTcpServer server;

    const fs::path temp_file = fs::temp_directory_path() / "remotex-recording-uploader-timeout-test.mp4";
    {
        std::ofstream out(temp_file, std::ios::binary);
        out << "test";
    }

    RecordingUploader uploader;
    uploader.Initialize(TestConfig(server.port()), nullptr);

    const std::string url = "http://127.0.0.1:" + std::to_string(server.port()) + "/upload";
    const auto start = std::chrono::steady_clock::now();
    const auto response = remotex::host::RecordingUploaderTestPeer::HttpPut(
        uploader,
        url,
        temp_file);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    EXPECT_EQ(response.status_code, 0);
    EXPECT_NE(response.body.find("winhttp_error="), std::string::npos);
    EXPECT_LT(elapsed.count(), 10000);

    std::error_code ec;
    fs::remove(temp_file, ec);
}
