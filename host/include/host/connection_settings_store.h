#pragma once
// Backward-compatibility shim — connection settings moved to core/.
#include "core/connection_settings.h"

namespace remotex::host::detail {

using HostConnectionSettings = remotex::core::ConnectionSettings;

using remotex::core::ReadRegistryStringValue;
using remotex::core::WriteRegistryStringValue;
using remotex::core::ReadRegistryStringValueWithFallback;
using remotex::core::ReadRegistryDwordValue;
using remotex::core::WriteRegistryDwordValue;
using remotex::core::ReadRegistryDwordValueWithFallback;
using remotex::core::LoadConnectionSettingsFromRegistry;
using remotex::core::LoadConnectionSettingsFromRegistryWithFallback;
using remotex::core::SaveConnectionSettingsToRegistry;

} // namespace remotex::host::detail
