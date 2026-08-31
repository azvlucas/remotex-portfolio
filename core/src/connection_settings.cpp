// RemoteX — core/connection_settings.cpp
// Registry-based connection settings shared between Host and Updater.

#include "core/connection_settings.h"

#include <algorithm>
#include <vector>

namespace remotex::core {

std::string ReadRegistryStringValue(HKEY root, const wchar_t* subkey, const char* value_name) {
    HKEY hkey = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &hkey) != ERROR_SUCCESS) {
        return {};
    }

    DWORD type = 0;
    DWORD size = 0;
    const LONG query_size = RegQueryValueExA(hkey, value_name, nullptr, &type, nullptr, &size);
    if (query_size != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || size == 0) {
        RegCloseKey(hkey);
        return {};
    }

    std::vector<char> buffer(size + 1, '\0');
    DWORD actual_size = size;
    const LONG query_value = RegQueryValueExA(
        hkey,
        value_name,
        nullptr,
        &type,
        reinterpret_cast<BYTE*>(buffer.data()),
        &actual_size);
    RegCloseKey(hkey);

    if (query_value != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return {};
    }

    buffer[std::min<std::size_t>(actual_size, buffer.size() - 1)] = '\0';
    return std::string(buffer.data());
}

std::string ReadRegistryStringValueWithFallback(HKEY root,
                                                const wchar_t* primary_subkey,
                                                const wchar_t* legacy_subkey,
                                                const char* value_name,
                                                bool* used_legacy) {
    if (used_legacy != nullptr) {
        *used_legacy = false;
    }

    const std::string primary_value = ReadRegistryStringValue(root, primary_subkey, value_name);
    if (!primary_value.empty()) {
        return primary_value;
    }

    if (legacy_subkey == nullptr) {
        return {};
    }

    const std::string legacy_value = ReadRegistryStringValue(root, legacy_subkey, value_name);
    if (!legacy_value.empty() && used_legacy != nullptr) {
        *used_legacy = true;
    }
    return legacy_value;
}

bool WriteRegistryStringValue(HKEY root, const wchar_t* subkey, const char* value_name, const std::string& value) {
    HKEY hkey = nullptr;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkey, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    const LONG result = RegSetValueExA(
        hkey,
        value_name,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>(value.size() + 1));
    RegCloseKey(hkey);
    return result == ERROR_SUCCESS;
}

DWORD ReadRegistryDwordValue(HKEY root, const wchar_t* subkey, const char* value_name, DWORD fallback) {
    HKEY hkey = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &hkey) != ERROR_SUCCESS) {
        return fallback;
    }

    DWORD type = 0;
    DWORD value = fallback;
    DWORD size = sizeof(value);
    const LONG result = RegQueryValueExA(
        hkey,
        value_name,
        nullptr,
        &type,
        reinterpret_cast<BYTE*>(&value),
        &size);
    RegCloseKey(hkey);

    if (result != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value)) {
        return fallback;
    }

    return value;
}

DWORD ReadRegistryDwordValueWithFallback(HKEY root,
                                         const wchar_t* primary_subkey,
                                         const wchar_t* legacy_subkey,
                                         const char* value_name,
                                         DWORD fallback,
                                         bool* used_legacy) {
    if (used_legacy != nullptr) {
        *used_legacy = false;
    }

    HKEY hkey = nullptr;
    if (RegOpenKeyExW(root, primary_subkey, 0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        DWORD type = 0;
        DWORD value = fallback;
        DWORD size = sizeof(value);
        const LONG result = RegQueryValueExA(
            hkey,
            value_name,
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(&value),
            &size);
        RegCloseKey(hkey);

        if (result == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(value)) {
            return value;
        }
    }

    if (legacy_subkey == nullptr) {
        return fallback;
    }

    const DWORD legacy_value = ReadRegistryDwordValue(root, legacy_subkey, value_name, fallback);
    if (legacy_value != fallback && used_legacy != nullptr) {
        *used_legacy = true;
    }
    return legacy_value;
}

bool WriteRegistryDwordValue(HKEY root, const wchar_t* subkey, const char* value_name, DWORD value) {
    HKEY hkey = nullptr;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkey, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    const LONG result = RegSetValueExA(
        hkey,
        value_name,
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&value),
        sizeof(value));
    RegCloseKey(hkey);
    return result == ERROR_SUCCESS;
}

ConnectionSettings LoadConnectionSettingsFromRegistry(HKEY root, const wchar_t* subkey) {
    ConnectionSettings settings;

    const std::string api_host = ReadRegistryStringValue(root, subkey, "api_host");
    const std::string signaling_host = ReadRegistryStringValue(root, subkey, "signaling_host");
    const std::string relay_host = ReadRegistryStringValue(root, subkey, "relay_host");

    if (!api_host.empty()) settings.api_host = api_host;
    if (!signaling_host.empty()) settings.signaling_host = signaling_host;
    if (!relay_host.empty()) settings.relay_host = relay_host;

    const DWORD api_port = ReadRegistryDwordValue(root, subkey, "api_port", settings.api_port);
    const DWORD signaling_port = ReadRegistryDwordValue(root, subkey, "signaling_port", settings.signaling_port);
    const DWORD relay_port = ReadRegistryDwordValue(root, subkey, "relay_port", settings.relay_port);

    if (api_port > 0 && api_port <= 65535) settings.api_port = static_cast<uint16_t>(api_port);
    if (signaling_port > 0 && signaling_port <= 65535) settings.signaling_port = static_cast<uint16_t>(signaling_port);
    if (relay_port > 0 && relay_port <= 65535) settings.relay_port = static_cast<uint16_t>(relay_port);

    settings.api_use_tls = ReadRegistryDwordValue(root, subkey, "api_use_tls", settings.api_use_tls ? 1u : 0u) != 0;
    settings.signaling_use_tls = ReadRegistryDwordValue(root, subkey, "signaling_use_tls", settings.signaling_use_tls ? 1u : 0u) != 0;

    return settings;
}

ConnectionSettings LoadConnectionSettingsFromRegistryWithFallback(HKEY root,
                                                                  const wchar_t* primary_subkey,
                                                                  const wchar_t* legacy_subkey,
                                                                  bool* used_legacy) {
    ConnectionSettings settings;
    bool any_legacy = false;

    bool value_from_legacy = false;
    const std::string api_host = ReadRegistryStringValueWithFallback(
        root, primary_subkey, legacy_subkey, "api_host", &value_from_legacy);
    any_legacy = any_legacy || value_from_legacy;

    value_from_legacy = false;
    const std::string signaling_host = ReadRegistryStringValueWithFallback(
        root, primary_subkey, legacy_subkey, "signaling_host", &value_from_legacy);
    any_legacy = any_legacy || value_from_legacy;

    value_from_legacy = false;
    const std::string relay_host = ReadRegistryStringValueWithFallback(
        root, primary_subkey, legacy_subkey, "relay_host", &value_from_legacy);
    any_legacy = any_legacy || value_from_legacy;

    if (!api_host.empty()) settings.api_host = api_host;
    if (!signaling_host.empty()) settings.signaling_host = signaling_host;
    if (!relay_host.empty()) settings.relay_host = relay_host;

    value_from_legacy = false;
    const DWORD api_port = ReadRegistryDwordValueWithFallback(
        root, primary_subkey, legacy_subkey, "api_port", settings.api_port, &value_from_legacy);
    any_legacy = any_legacy || value_from_legacy;

    value_from_legacy = false;
    const DWORD signaling_port = ReadRegistryDwordValueWithFallback(
        root, primary_subkey, legacy_subkey, "signaling_port", settings.signaling_port, &value_from_legacy);
    any_legacy = any_legacy || value_from_legacy;

    value_from_legacy = false;
    const DWORD relay_port = ReadRegistryDwordValueWithFallback(
        root, primary_subkey, legacy_subkey, "relay_port", settings.relay_port, &value_from_legacy);
    any_legacy = any_legacy || value_from_legacy;

    if (api_port > 0 && api_port <= 65535) settings.api_port = static_cast<uint16_t>(api_port);
    if (signaling_port > 0 && signaling_port <= 65535) settings.signaling_port = static_cast<uint16_t>(signaling_port);
    if (relay_port > 0 && relay_port <= 65535) settings.relay_port = static_cast<uint16_t>(relay_port);

    value_from_legacy = false;
    settings.api_use_tls = ReadRegistryDwordValueWithFallback(
        root, primary_subkey, legacy_subkey, "api_use_tls", settings.api_use_tls ? 1u : 0u, &value_from_legacy) != 0;
    any_legacy = any_legacy || value_from_legacy;

    value_from_legacy = false;
    settings.signaling_use_tls = ReadRegistryDwordValueWithFallback(
        root, primary_subkey, legacy_subkey, "signaling_use_tls", settings.signaling_use_tls ? 1u : 0u, &value_from_legacy) != 0;
    any_legacy = any_legacy || value_from_legacy;

    if (used_legacy != nullptr) {
        *used_legacy = any_legacy;
    }
    return settings;
}

bool SaveConnectionSettingsToRegistry(HKEY root, const wchar_t* subkey, const ConnectionSettings& settings) {
    return WriteRegistryStringValue(root, subkey, "api_host", settings.api_host) &&
           WriteRegistryStringValue(root, subkey, "signaling_host", settings.signaling_host) &&
           WriteRegistryStringValue(root, subkey, "relay_host", settings.relay_host) &&
           WriteRegistryDwordValue(root, subkey, "api_port", settings.api_port) &&
           WriteRegistryDwordValue(root, subkey, "signaling_port", settings.signaling_port) &&
           WriteRegistryDwordValue(root, subkey, "relay_port", settings.relay_port) &&
           WriteRegistryDwordValue(root, subkey, "api_use_tls", settings.api_use_tls ? 1u : 0u) &&
           WriteRegistryDwordValue(root, subkey, "signaling_use_tls", settings.signaling_use_tls ? 1u : 0u);
}

} // namespace remotex::core
