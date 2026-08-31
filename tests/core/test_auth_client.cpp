#include <gtest/gtest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>

#include "core/auth_client.h"

#include <string>
#include <utility>
#include <vector>

namespace {

std::wstring ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring wide(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), wide.data(), len);
    return wide;
}

void DeleteCredentialTarget(const std::string& target) {
    for (const char* suffix : {"access", "refresh"}) {
        const auto wide_target = ToWide(target + "_" + suffix);
        CredDeleteW(wide_target.c_str(), CRED_TYPE_GENERIC, 0);
    }
}

remotex::core::AuthConfig MakeConfig(const std::string& target,
                                     std::vector<std::string> legacy_targets = {}) {
    remotex::core::AuthConfig config;
    config.credential_target = target;
    config.legacy_credential_targets = std::move(legacy_targets);
    return config;
}

} // namespace

TEST(AuthClient, LoadStoredTokensMigratesLegacyCredentialTargets) {
    const std::string unique = std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64());
    const std::string primary_target = "RemoteXTestAuth_" + unique;
    const std::string legacy_target = "RemoteAccessTestAuth_" + unique;

    DeleteCredentialTarget(primary_target);
    DeleteCredentialTarget(legacy_target);

    remotex::core::AuthClient legacy_client;
    legacy_client.Configure(MakeConfig(legacy_target));
    legacy_client.SetTokensDirect("machine-123", "access-token-123", "refresh-token-456");
    ASSERT_TRUE(legacy_client.SaveTokens());

    remotex::core::AuthClient migrated_client;
    migrated_client.Configure(MakeConfig(primary_target, {legacy_target}));
    ASSERT_TRUE(migrated_client.LoadStoredTokens());
    EXPECT_EQ(migrated_client.GetUserID(), "machine-123");
    EXPECT_EQ(migrated_client.GetAccessToken(), "access-token-123");
    EXPECT_EQ(migrated_client.GetRefreshToken(), "refresh-token-456");

    remotex::core::AuthClient primary_client;
    primary_client.Configure(MakeConfig(primary_target));
    EXPECT_TRUE(primary_client.LoadStoredTokens());
    EXPECT_EQ(primary_client.GetUserID(), "machine-123");
    EXPECT_EQ(primary_client.GetAccessToken(), "access-token-123");
    EXPECT_EQ(primary_client.GetRefreshToken(), "refresh-token-456");

    remotex::core::AuthClient legacy_verify_client;
    legacy_verify_client.Configure(MakeConfig(legacy_target));
    EXPECT_FALSE(legacy_verify_client.LoadStoredTokens());

    DeleteCredentialTarget(primary_target);
    DeleteCredentialTarget(legacy_target);
}
