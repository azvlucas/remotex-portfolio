#pragma once
// RemoteX — core/connection_settings.h
// Registry-based connection settings shared between Host and Updater.

#include <windows.h>

#include <cstdint>
#include <string>

namespace remotex::core {

struct ConnectionSettings {
    std::string api_host = "demo.remotex.example";
    uint16_t    api_port = 443;
    bool        api_use_tls = true;
    std::string signaling_host = "demo.remotex.example";
    uint16_t    signaling_port = 443;
    bool        signaling_use_tls = true;
    std::string relay_host = "demo.remotex.example";
    uint16_t    relay_port = 9100;
};

std::string ReadRegistryStringValue(HKEY root, const wchar_t* subkey, const char* value_name);
bool WriteRegistryStringValue(HKEY root, const wchar_t* subkey, const char* value_name, const std::string& value);
std::string ReadRegistryStringValueWithFallback(HKEY root,
                                                const wchar_t* primary_subkey,
                                                const wchar_t* legacy_subkey,
                                                const char* value_name,
                                                bool* used_legacy = nullptr);

DWORD ReadRegistryDwordValue(HKEY root, const wchar_t* subkey, const char* value_name, DWORD fallback = 0);
bool WriteRegistryDwordValue(HKEY root, const wchar_t* subkey, const char* value_name, DWORD value);
DWORD ReadRegistryDwordValueWithFallback(HKEY root,
                                         const wchar_t* primary_subkey,
                                         const wchar_t* legacy_subkey,
                                         const char* value_name,
                                         DWORD fallback = 0,
                                         bool* used_legacy = nullptr);

ConnectionSettings LoadConnectionSettingsFromRegistry(HKEY root, const wchar_t* subkey);
ConnectionSettings LoadConnectionSettingsFromRegistryWithFallback(HKEY root,
                                                                  const wchar_t* primary_subkey,
                                                                  const wchar_t* legacy_subkey,
                                                                  bool* used_legacy = nullptr);
bool SaveConnectionSettingsToRegistry(HKEY root, const wchar_t* subkey, const ConnectionSettings& settings);

} // namespace remotex::core
