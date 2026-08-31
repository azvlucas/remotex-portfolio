// RemoteX — tests/host/test_auto_updater.cpp
// Focused tests for release-key loading and Ed25519 verification helpers.

#include <gtest/gtest.h>

#include "host/auto_updater.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> HexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

fs::path WriteTempFile(const std::string& name, std::span<const uint8_t> data) {
    auto path = fs::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    file.close();
    return path;
}

} // namespace

TEST(AutoUpdater, LoadEd25519PublicKeyFileSuccess) {
    const auto key = HexToBytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    auto path = WriteTempFile("remotex_test_public_key.bin", key);

    auto loaded = remotex::host::detail::LoadEd25519PublicKeyFile(path);

    EXPECT_EQ(loaded, key);
    fs::remove(path);
}

TEST(AutoUpdater, LoadEd25519PublicKeyFileRejectsInvalidLength) {
    const std::vector<uint8_t> short_key(31, 0x42);
    auto path = WriteTempFile("remotex_test_invalid_public_key.bin", short_key);

    auto loaded = remotex::host::detail::LoadEd25519PublicKeyFile(path);

    EXPECT_TRUE(loaded.empty());
    fs::remove(path);
}

TEST(AutoUpdater, VerifyFileEd25519SignatureRejectsWrongKey) {
    const auto wrong_key = HexToBytes("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    const std::string signature =
        "4d96260d8a9684e390d4ae8250d3c87cec5ace8d026ea5c59740d75d6d52944f"
        "81c86c6bfbd18317a80f48159d624dbfa8ad37918e96d5bbfb2638203a424609";
    const std::vector<uint8_t> message{
        'r', 'e', 'm', 'o', 't', 'e', 'x', '-', 'a', 'u', 't', 'o', '-',
        'u', 'p', 'd', 'a', 't', 'e', 'r', '-', 't', 'e', 's', 't',
    };

    auto path = WriteTempFile("remotex_test_message_invalid.bin", message);

    EXPECT_FALSE(remotex::host::detail::VerifyFileEd25519Signature(path, wrong_key, signature));
    fs::remove(path);
}

TEST(AutoUpdater, ValidateDownloadedReleaseRejectsMissingPublicKey) {
    const std::vector<uint8_t> payload{'u', 'p', 'd', 'a', 't', 'e'};
    auto path = WriteTempFile("remotex_test_release_no_key.bin", payload);

    EXPECT_EQ(remotex::host::detail::ValidateDownloadedRelease(path, {}, "abcd"),
              remotex::host::detail::ReleaseTrustResult::MissingPublicKey);
    fs::remove(path);
}

TEST(AutoUpdater, ValidateDownloadedReleaseRejectsMissingSignature) {
    const std::vector<uint8_t> payload{'u', 'p', 'd', 'a', 't', 'e'};
    const auto key = HexToBytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    auto path = WriteTempFile("remotex_test_release_no_signature.bin", payload);

    EXPECT_EQ(remotex::host::detail::ValidateDownloadedRelease(path, key, ""),
              remotex::host::detail::ReleaseTrustResult::MissingSignature);
    fs::remove(path);
}

TEST(AutoUpdater, ValidateDownloadedReleaseRejectsInvalidSignature) {
    const std::vector<uint8_t> payload{'u', 'p', 'd', 'a', 't', 'e'};
    const auto key = HexToBytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    auto path = WriteTempFile("remotex_test_release_invalid_signature.bin", payload);

    EXPECT_EQ(remotex::host::detail::ValidateDownloadedRelease(path, key, "abcd"),
              remotex::host::detail::ReleaseTrustResult::InvalidSignature);
    fs::remove(path);
}
