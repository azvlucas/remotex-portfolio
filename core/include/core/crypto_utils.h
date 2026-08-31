#pragma once
// RemoteX — core/crypto_utils.h
// Utilitários de criptografia:
// - Verificação de assinatura Ed25519 (para auto-update de binários)
// - Geração de machine key (identificador único da máquina)

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace remotex::core {

// Tamanhos fixos do Ed25519.
inline constexpr size_t kEd25519PublicKeySize = 32;
inline constexpr size_t kEd25519SignatureSize = 64;

// Verifica uma assinatura Ed25519.
// public_key: chave pública (32 bytes).
// data: dados que foram assinados.
// signature: assinatura (64 bytes).
// Retorna true se a assinatura é válida.
// Usa Windows CNG (Cryptography Next Generation).
bool Ed25519Verify(std::span<const uint8_t> public_key,
                   std::span<const uint8_t> data,
                   std::span<const uint8_t> signature);

// Gera um machine key único baseado em hardware IDs do sistema.
// Combina: Volume Serial Number do disco do sistema + Computer Name + SID do usuário.
// Retorna uma string hex de 64 caracteres (SHA-256 dos componentes).
std::string GenerateMachineKey();

// Calcula SHA-256 de um bloco de dados.
// Retorna o hash como string hex lowercase (64 caracteres).
std::string SHA256Hex(std::span<const uint8_t> data);

// Calcula SHA-256 de um bloco de dados.
// Retorna o hash como vetor de 32 bytes.
std::vector<uint8_t> SHA256Raw(std::span<const uint8_t> data);

} // namespace remotex::core
