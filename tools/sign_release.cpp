#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr size_t kEd25519SeedSize = 32;
constexpr size_t kEd25519PublicKeySize = 32;
constexpr size_t kEd25519SignatureSize = 64;

using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

std::string ToHexLower(std::span<const uint8_t> data) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto byte : data) {
        out << std::setw(2) << static_cast<unsigned>(byte);
    }
    return out.str();
}

std::string LastOpenSSLError() {
    const unsigned long err = ERR_get_error();
    if (err == 0) {
        return "sem detalhes do OpenSSL";
    }

    char buf[256]{};
    ERR_error_string_n(err, buf, sizeof(buf));
    return std::string(buf);
}

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error("Erro ao abrir arquivo: " + path.string());
    }

    const auto size = file.tellg();
    if (size < 0) {
        throw std::runtime_error("Erro ao obter tamanho do arquivo: " + path.string());
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file.good() && !file.eof()) {
        throw std::runtime_error("Erro ao ler arquivo: " + path.string());
    }
    return bytes;
}

void WriteFileBytes(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        throw std::runtime_error("Erro ao criar arquivo: " + path.string());
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file.good()) {
        throw std::runtime_error("Erro ao gravar arquivo: " + path.string());
    }
}

std::vector<uint8_t> NormalizePrivateKey(std::span<const uint8_t> key_bytes) {
    if (key_bytes.size() == kEd25519SeedSize) {
        return std::vector<uint8_t>(key_bytes.begin(), key_bytes.end());
    }
    if (key_bytes.size() == 64) {
        return std::vector<uint8_t>(key_bytes.begin(), key_bytes.begin() + kEd25519SeedSize);
    }

    throw std::runtime_error(
        "Chave privada invalida: esperado 32 bytes (seed) ou 64 bytes (formato Go), obtido " +
        std::to_string(key_bytes.size()));
}

EvpPkeyPtr LoadPrivateKeyFromSeed(std::span<const uint8_t> seed) {
    if (seed.size() != kEd25519SeedSize) {
        throw std::runtime_error("Seed Ed25519 invalido");
    }

    EVP_PKEY* raw = EVP_PKEY_new_raw_private_key_ex(
        nullptr,
        "ED25519",
        nullptr,
        seed.data(),
        seed.size());
    if (raw == nullptr) {
        throw std::runtime_error("EVP_PKEY_new_raw_private_key_ex falhou: " + LastOpenSSLError());
    }

    return EvpPkeyPtr(raw, EVP_PKEY_free);
}

std::vector<uint8_t> ExportPublicKey(EVP_PKEY* key) {
    size_t public_key_size = kEd25519PublicKeySize;
    std::vector<uint8_t> public_key(public_key_size);
    if (EVP_PKEY_get_raw_public_key(key, public_key.data(), &public_key_size) != 1) {
        throw std::runtime_error("EVP_PKEY_get_raw_public_key falhou: " + LastOpenSSLError());
    }

    public_key.resize(public_key_size);
    if (public_key.size() != kEd25519PublicKeySize) {
        throw std::runtime_error(
            "Chave publica Ed25519 invalida: esperado 32 bytes, obtido " +
            std::to_string(public_key.size()));
    }
    return public_key;
}

std::vector<uint8_t> SignData(EVP_PKEY* key, std::span<const uint8_t> data) {
    EvpMdCtxPtr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx) {
        throw std::runtime_error("EVP_MD_CTX_new falhou");
    }

    if (EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key) != 1) {
        throw std::runtime_error("EVP_DigestSignInit falhou: " + LastOpenSSLError());
    }

    size_t signature_size = 0;
    if (EVP_DigestSign(ctx.get(), nullptr, &signature_size, data.data(), data.size()) != 1) {
        throw std::runtime_error("EVP_DigestSign(size) falhou: " + LastOpenSSLError());
    }

    std::vector<uint8_t> signature(signature_size);
    if (EVP_DigestSign(ctx.get(), signature.data(), &signature_size, data.data(), data.size()) != 1) {
        throw std::runtime_error("EVP_DigestSign falhou: " + LastOpenSSLError());
    }

    signature.resize(signature_size);
    if (signature.size() != kEd25519SignatureSize) {
        throw std::runtime_error(
            "Assinatura Ed25519 invalida: esperado 64 bytes, obtido " +
            std::to_string(signature.size()));
    }

    return signature;
}

void ValidateGoPrivateKeySuffix(std::span<const uint8_t> key_bytes,
                                std::span<const uint8_t> derived_public_key) {
    if (key_bytes.size() != 64) {
        return;
    }

    std::span<const uint8_t> embedded_public(key_bytes.data() + kEd25519SeedSize, kEd25519PublicKeySize);
    if (!std::equal(embedded_public.begin(), embedded_public.end(), derived_public_key.begin(), derived_public_key.end())) {
        throw std::runtime_error("A chave privada de 64 bytes nao corresponde ao public key embutido");
    }
}

void GenerateKeyPair() {
    std::vector<uint8_t> seed(kEd25519SeedSize);
    if (RAND_bytes(seed.data(), static_cast<int>(seed.size())) != 1) {
        throw std::runtime_error("RAND_bytes falhou: " + LastOpenSSLError());
    }

    auto key = LoadPrivateKeyFromSeed(seed);
    auto public_key = ExportPublicKey(key.get());

    WriteFileBytes("ed25519_private.bin", seed);
    WriteFileBytes("ed25519_public.bin", public_key);

    std::cout << "Chaves geradas:\n";
    std::cout << "  Privada: ed25519_private.bin (32 bytes, seed)\n";
    std::cout << "  Publica: ed25519_public.bin  (32 bytes)\n\n";
    std::cout << "  Publica (hex): " << ToHexLower(public_key) << "\n";
}

void SignFile(const std::filesystem::path& key_path, const std::filesystem::path& file_path) {
    auto key_bytes = ReadFileBytes(key_path);
    auto seed = NormalizePrivateKey(key_bytes);
    auto key = LoadPrivateKeyFromSeed(seed);
    auto public_key = ExportPublicKey(key.get());
    ValidateGoPrivateKeySuffix(key_bytes, public_key);

    auto data = ReadFileBytes(file_path);
    auto signature = SignData(key.get(), data);

    const auto sig_path = file_path.string() + ".sig";
    WriteFileBytes(sig_path, signature);

    std::cout << "Arquivo:     " << file_path.string() << " (" << data.size() << " bytes)\n";
    std::cout << "Assinatura:  " << sig_path << "\n";
    std::cout << "Hex:         " << ToHexLower(signature) << "\n";
    std::cout << "File size:   " << data.size() << "\n";
}

void PrintUsage() {
    std::cerr << "Uso: sign_release -key <private_key.bin> -file <binary>\n";
    std::cerr << "     sign_release -genkey\n";
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        ERR_clear_error();

        bool genkey = false;
        std::string key_file = "ed25519_private.bin";
        std::string bin_file;

        for (int i = 1; i < argc; ++i) {
            std::string_view arg = argv[i];
            if (arg == "-genkey") {
                genkey = true;
            } else if (arg == "-key") {
                if (i + 1 >= argc) {
                    PrintUsage();
                    return 1;
                }
                key_file = argv[++i];
            } else if (arg == "-file") {
                if (i + 1 >= argc) {
                    PrintUsage();
                    return 1;
                }
                bin_file = argv[++i];
            } else {
                PrintUsage();
                return 1;
            }
        }

        if (genkey) {
            GenerateKeyPair();
            return 0;
        }

        if (bin_file.empty()) {
            PrintUsage();
            return 1;
        }

        SignFile(key_file, bin_file);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
