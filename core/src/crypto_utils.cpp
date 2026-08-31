// RemoteX — core/crypto_utils.cpp
// Ed25519 via Windows CNG, SHA-256 via BCrypt, machine key via hardware IDs.

#include "core/crypto_utils.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <sddl.h>       // ConvertSidToStringSidW

#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <memory>

namespace remotex::core {

// ─── Helpers ────────────────────────────────────────────────────────────────

static std::string ToHexLower(const uint8_t* data, size_t len) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out += hex[(data[i] >> 4) & 0x0F];
        out += hex[data[i] & 0x0F];
    }
    return out;
}

// ─── SHA-256 via Windows CNG ────────────────────────────────────────────────

std::vector<uint8_t> SHA256Raw(std::span<const uint8_t> data)
{
    std::vector<uint8_t> hash(32, 0);

    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status))
        return hash;

    BCRYPT_HASH_HANDLE hash_handle = nullptr;
    status = BCryptCreateHash(alg, &hash_handle, nullptr, 0, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return hash;
    }

    BCryptHashData(hash_handle,
                   const_cast<PUCHAR>(data.data()),
                   static_cast<ULONG>(data.size()), 0);

    BCryptFinishHash(hash_handle, hash.data(), static_cast<ULONG>(hash.size()), 0);

    BCryptDestroyHash(hash_handle);
    BCryptCloseAlgorithmProvider(alg, 0);
    return hash;
}

std::string SHA256Hex(std::span<const uint8_t> data)
{
    auto hash = SHA256Raw(data);
    return ToHexLower(hash.data(), hash.size());
}

// ─── Ed25519 Verify via Windows CNG ────────────────────────────────────────
//
// Windows 10+ suporta Ed25519 via CNG a partir de SubjectPublicKeyInfo (OID 1.3.101.112).
#ifndef BCRYPT_EDDSA_PUBLIC_ED25519_MAGIC
#define BCRYPT_EDDSA_PUBLIC_ED25519_MAGIC 0x31444445 // 'EDD1'
#endif

#ifndef BCRYPT_EDDSA_ALGORITHM
#define BCRYPT_EDDSA_ALGORITHM L"EDDSA"
#endif

struct Ed25519PubKeyBlob {
    BCRYPT_ECCKEY_BLOB header;
    uint8_t            key_data[32];
};

bool Ed25519Verify(std::span<const uint8_t> public_key,
                   std::span<const uint8_t> data,
                   std::span<const uint8_t> signature)
{
    if (public_key.size() != kEd25519PublicKeySize ||
        signature.size()  != kEd25519SignatureSize)
        return false;

    // Approach 1: CryptImportPublicKeyInfoEx2 with X.509 OID 1.3.101.112.
    {
        CRYPT_BIT_BLOB bit_blob{};
        bit_blob.cbData = static_cast<DWORD>(public_key.size());
        bit_blob.pbData = const_cast<BYTE*>(public_key.data());
        bit_blob.cUnusedBits = 0;

        CERT_PUBLIC_KEY_INFO key_info{};
        key_info.Algorithm.pszObjId = const_cast<char*>("1.3.101.112");
        key_info.PublicKey = bit_blob;

        BCRYPT_KEY_HANDLE key_handle = nullptr;
        if (CryptImportPublicKeyInfoEx2(X509_ASN_ENCODING, &key_info, 0, nullptr, &key_handle)) {
            NTSTATUS status = BCryptVerifySignature(
                key_handle,
                nullptr,
                const_cast<PUCHAR>(data.data()),
                static_cast<ULONG>(data.size()),
                const_cast<PUCHAR>(signature.data()),
                static_cast<ULONG>(signature.size()),
                0);

            BCryptDestroyKey(key_handle);
            if (BCRYPT_SUCCESS(status))
                return true;
        }
        // Fall through to approach 2 if import or verify failed.
    }

    // Approach 2: Manual EDDSA blob import (fallback for CNG builds without
    // X.509-based Ed25519 import or where the OID-based verify disagrees).
    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_EDDSA_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status))
        return false;

    auto try_blob = [&](LPCWSTR blob_type) -> bool {
        Ed25519PubKeyBlob blob{};
        blob.header.dwMagic = BCRYPT_EDDSA_PUBLIC_ED25519_MAGIC;
        blob.header.cbKey = 32;
        std::memcpy(blob.key_data, public_key.data(), 32);

        BCRYPT_KEY_HANDLE imported = nullptr;
        NTSTATUS import_status = BCryptImportKeyPair(
            alg, nullptr, blob_type, &imported,
            reinterpret_cast<PUCHAR>(&blob),
            sizeof(blob), 0);
        if (!BCRYPT_SUCCESS(import_status))
            return false;

        NTSTATUS verify_status = BCryptVerifySignature(
            imported,
            nullptr,
            const_cast<PUCHAR>(data.data()),
            static_cast<ULONG>(data.size()),
            const_cast<PUCHAR>(signature.data()),
            static_cast<ULONG>(signature.size()),
            0);

        BCryptDestroyKey(imported);
        return BCRYPT_SUCCESS(verify_status);
    };

    bool verified = try_blob(BCRYPT_PUBLIC_KEY_BLOB) || try_blob(BCRYPT_ECCPUBLIC_BLOB);
    BCryptCloseAlgorithmProvider(alg, 0);
    return verified;
}

// ─── Machine Key ────────────────────────────────────────────────────────────

std::string GenerateMachineKey()
{
    std::ostringstream components;

    // 1. Volume Serial Number do disco do sistema (C:\).
    DWORD serial = 0;
    if (GetVolumeInformationW(L"C:\\", nullptr, 0, &serial, nullptr, nullptr, nullptr, 0))
        components << "vol:" << serial << ";";

    // 2. Computer Name.
    wchar_t name_buf[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD name_size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(name_buf, &name_size)) {
        char utf8[256]{};
        WideCharToMultiByte(CP_UTF8, 0, name_buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
        components << "cn:" << utf8 << ";";
    }

    // 3. Windows Product ID (do registro).
    HKEY hkey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                      0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        wchar_t product_id[256]{};
        DWORD size = sizeof(product_id);
        if (RegQueryValueExW(hkey, L"ProductId", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(product_id), &size) == ERROR_SUCCESS) {
            char utf8[256]{};
            WideCharToMultiByte(CP_UTF8, 0, product_id, -1, utf8, sizeof(utf8), nullptr, nullptr);
            components << "pid:" << utf8 << ";";
        }
        RegCloseKey(hkey);
    }

    // Hash SHA-256 de todos os componentes.
    std::string raw = components.str();
    auto span = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
    return SHA256Hex(span);
}

} // namespace remotex::core
