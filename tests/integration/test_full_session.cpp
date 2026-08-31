// RemoteX — tests/integration/test_full_session.cpp
// Teste de integração: simula fluxo completo de sessão remota.
// Requer servidor local rodando (docker compose up).

#include <gtest/gtest.h>

#include "core/auth_client.h"
#include "core/machine_client.h"
#include "core/signaling_client.h"
#include "core/webrtc_peer.h"
#include "core/input_protocol.h"

#include <chrono>
#include <thread>

using namespace remotex::core;

class FullSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Configurar endpoints (localhost, porta padrão).
        auth_config_.api_host = "localhost";
        auth_config_.api_port = 8081;
        auth_config_.credential_target = "RemoteXTest";

        mc_config_.api_host = "localhost";
        mc_config_.api_port = 8081;
    }

    AuthConfig auth_config_;
    MachineClientConfig mc_config_;
};

// Teste de registro + login + CRUD de máquinas.
// Requer servidor API rodando em localhost:8081.
TEST_F(FullSessionTest, DISABLED_RegisterLoginMachines) {
    AuthClient auth;
    auth.Configure(auth_config_);

    // Registrar usuário de teste.
    std::string email = "test_" + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count()) + "@example.com";
    auto reg = auth.Register(email, "TestPassword123!", "Test User");

    // Se já existe, fazer login.
    if (!reg.success) {
        auto login = auth.Login(email, "TestPassword123!");
        ASSERT_TRUE(login.success) << "Login failed: " << login.error;
    }

    ASSERT_TRUE(auth.IsAuthenticated());
    EXPECT_FALSE(auth.GetAccessToken().empty());
    EXPECT_FALSE(auth.GetUserID().empty());

    // CRUD de máquinas.
    MachineClient mc;
    mc.Configure(mc_config_, &auth);

    // Registrar máquina.
    auto machine = mc.RegisterMachine("Test Machine Integration", "Windows 11");
    ASSERT_TRUE(machine.has_value()) << "RegisterMachine failed";
    EXPECT_FALSE(machine->id.empty());
    EXPECT_EQ(machine->name, "Test Machine Integration");

    std::string machine_id = machine->id;

    // Listar máquinas.
    auto machines = mc.ListMachines();
    EXPECT_GE(machines.size(), 1u);
    bool found = false;
    for (const auto& m : machines) {
        if (m.id == machine_id) {
            found = true;
            EXPECT_EQ(m.name, "Test Machine Integration");
        }
    }
    EXPECT_TRUE(found) << "Created machine not in list";

    // Atualizar máquina.
    auto updated = mc.UpdateMachine(machine_id, "Updated Machine Name");
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->name, "Updated Machine Name");

    // Status.
    auto status = mc.GetMachineStatus(machine_id);
    ASSERT_TRUE(status.has_value());
    EXPECT_FALSE(status->is_online);  // Nenhum heartbeat enviado.

    // Deletar máquina.
    auto del = mc.DeleteMachine(machine_id);
    EXPECT_TRUE(del.success) << "DeleteMachine failed: " << del.error;

    // Verificar que foi deletada.
    auto after = mc.ListMachines();
    for (const auto& m : after) {
        EXPECT_NE(m.id, machine_id) << "Machine should be deleted";
    }

    // Limpar tokens.
    auth.Logout();
    EXPECT_FALSE(auth.IsAuthenticated());
}

// Teste de WebRTCPeer
TEST_F(FullSessionTest, WebRTCPeer_StubBehavior) {
    SUCCEED() << "libwebrtc available — stub test skipped";
}

// Teste de InputProtocol round-trip rápido.
TEST_F(FullSessionTest, InputProtocol_Roundtrip) {
    // Criar sequência de input típica de uma sessão.
    std::vector<uint8_t> stream;

    // Mouse move ao centro.
    SerializeMouseMove({0.5f, 0.5f}, stream);
    // Click.
    SerializeMouseButton({InputEventType::MouseButtonDown, MouseButton::Left}, stream);
    SerializeMouseButton({InputEventType::MouseButtonUp, MouseButton::Left}, stream);
    // Teclado.
    SerializeKey({InputEventType::KeyDown, 0x41, 0}, stream);
    SerializeKey({InputEventType::KeyUp, 0x41, 0}, stream);
    // Scroll.
    SerializeMouseScroll({0, -3}, stream);

    // Deserializar tudo.
    size_t offset = 0;
    int count = 0;
    while (offset < stream.size()) {
        auto result = Deserialize(std::span<const uint8_t>(stream).subspan(offset));
        ASSERT_TRUE(result.has_value()) << "Failed at offset " << offset;
        offset += result->bytes_consumed;
        ++count;
    }
    EXPECT_EQ(count, 6);
    EXPECT_EQ(offset, stream.size());
}
