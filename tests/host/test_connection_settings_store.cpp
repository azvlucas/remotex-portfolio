#include <gtest/gtest.h>

#include "host/connection_settings_store.h"

namespace {

constexpr wchar_t kTestRegKey[] = L"SOFTWARE\\RemoteX\\Tests\\ConnectionSettingsStore";
constexpr wchar_t kLegacyTestRegKey[] = L"SOFTWARE\\RemoteAccess\\Tests\\ConnectionSettingsStore";

class ConnectionSettingsStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        RegDeleteTreeW(HKEY_CURRENT_USER, kTestRegKey);
        RegDeleteTreeW(HKEY_CURRENT_USER, kLegacyTestRegKey);
    }

    void TearDown() override {
        RegDeleteTreeW(HKEY_CURRENT_USER, kTestRegKey);
        RegDeleteTreeW(HKEY_CURRENT_USER, kLegacyTestRegKey);
    }
};

} // namespace

TEST_F(ConnectionSettingsStoreTest, ReadRegistryStringValueUsesRequestedValueName) {
    ASSERT_TRUE(remotex::host::detail::WriteRegistryStringValue(
        HKEY_CURRENT_USER, kTestRegKey, "machine_id", "machine-123"));
    ASSERT_TRUE(remotex::host::detail::WriteRegistryStringValue(
        HKEY_CURRENT_USER, kTestRegKey, "signaling_host", "signal.example.com"));

    EXPECT_EQ(
        remotex::host::detail::ReadRegistryStringValue(HKEY_CURRENT_USER, kTestRegKey, "machine_id"),
        "machine-123");
    EXPECT_EQ(
        remotex::host::detail::ReadRegistryStringValue(HKEY_CURRENT_USER, kTestRegKey, "signaling_host"),
        "signal.example.com");
}

TEST_F(ConnectionSettingsStoreTest, LoadConnectionSettingsPreservesDistinctHostsAndPorts) {
    remotex::host::detail::HostConnectionSettings expected;
    expected.api_host = "api.example.com";
    expected.api_port = 8443;
    expected.api_use_tls = false;
    expected.signaling_host = "signal.example.com";
    expected.signaling_port = 9443;
    expected.signaling_use_tls = true;
    expected.relay_host = "relay.example.com";
    expected.relay_port = 9101;

    ASSERT_TRUE(remotex::host::detail::WriteRegistryStringValue(
        HKEY_CURRENT_USER, kTestRegKey, "machine_id", "machine-123"));
    ASSERT_TRUE(remotex::host::detail::SaveConnectionSettingsToRegistry(
        HKEY_CURRENT_USER, kTestRegKey, expected));

    const auto loaded = remotex::host::detail::LoadConnectionSettingsFromRegistry(HKEY_CURRENT_USER, kTestRegKey);

    EXPECT_EQ(loaded.api_host, expected.api_host);
    EXPECT_EQ(loaded.api_port, expected.api_port);
    EXPECT_EQ(loaded.api_use_tls, expected.api_use_tls);
    EXPECT_EQ(loaded.signaling_host, expected.signaling_host);
    EXPECT_EQ(loaded.signaling_port, expected.signaling_port);
    EXPECT_EQ(loaded.signaling_use_tls, expected.signaling_use_tls);
    EXPECT_EQ(loaded.relay_host, expected.relay_host);
    EXPECT_EQ(loaded.relay_port, expected.relay_port);
}

TEST_F(ConnectionSettingsStoreTest, ReadRegistryStringValueWithFallbackUsesLegacyValueWhenPrimaryMissing) {
    ASSERT_TRUE(remotex::host::detail::WriteRegistryStringValue(
        HKEY_CURRENT_USER, kLegacyTestRegKey, "machine_id", "legacy-machine"));

    bool used_legacy = false;
    const auto value = remotex::host::detail::ReadRegistryStringValueWithFallback(
        HKEY_CURRENT_USER, kTestRegKey, kLegacyTestRegKey, "machine_id", &used_legacy);

    EXPECT_EQ(value, "legacy-machine");
    EXPECT_TRUE(used_legacy);
}

TEST_F(ConnectionSettingsStoreTest, LoadConnectionSettingsWithFallbackReadsLegacyKey) {
    remotex::host::detail::HostConnectionSettings expected;
    expected.api_host = "legacy-api.example.com";
    expected.api_port = 7443;
    expected.api_use_tls = false;
    expected.signaling_host = "legacy-signal.example.com";
    expected.signaling_port = 9555;
    expected.signaling_use_tls = false;
    expected.relay_host = "legacy-relay.example.com";
    expected.relay_port = 9222;

    ASSERT_TRUE(remotex::host::detail::SaveConnectionSettingsToRegistry(
        HKEY_CURRENT_USER, kLegacyTestRegKey, expected));

    bool used_legacy = false;
    const auto loaded = remotex::host::detail::LoadConnectionSettingsFromRegistryWithFallback(
        HKEY_CURRENT_USER, kTestRegKey, kLegacyTestRegKey, &used_legacy);

    EXPECT_TRUE(used_legacy);
    EXPECT_EQ(loaded.api_host, expected.api_host);
    EXPECT_EQ(loaded.api_port, expected.api_port);
    EXPECT_EQ(loaded.api_use_tls, expected.api_use_tls);
    EXPECT_EQ(loaded.signaling_host, expected.signaling_host);
    EXPECT_EQ(loaded.signaling_port, expected.signaling_port);
    EXPECT_EQ(loaded.signaling_use_tls, expected.signaling_use_tls);
    EXPECT_EQ(loaded.relay_host, expected.relay_host);
    EXPECT_EQ(loaded.relay_port, expected.relay_port);
}
