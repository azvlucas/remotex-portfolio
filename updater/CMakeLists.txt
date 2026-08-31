# ── Updater: serviço independente de auto-update ──────────────────────────────
set(REMOTEX_UPDATER_SOURCES
    src/main.cpp
    src/updater_service.cpp
    src/host_updater.cpp
    src/self_updater.cpp
    src/health_monitor.cpp
)

add_executable(remotex_updater ${REMOTEX_UPDATER_SOURCES})

set(REMOTEX_VERSION_FILE "${CMAKE_SOURCE_DIR}/VERSION")
if(NOT EXISTS "${REMOTEX_VERSION_FILE}")
    message(FATAL_ERROR "Missing VERSION file at ${REMOTEX_VERSION_FILE}")
endif()

file(READ "${REMOTEX_VERSION_FILE}" REMOTEX_UPDATER_VERSION)
string(STRIP "${REMOTEX_UPDATER_VERSION}" REMOTEX_UPDATER_VERSION)
if(REMOTEX_UPDATER_VERSION STREQUAL "")
    message(FATAL_ERROR "VERSION file is empty")
endif()

target_include_directories(remotex_updater PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(remotex_updater PRIVATE cxx_std_20)
target_compile_definitions(remotex_updater PRIVATE REMOTEX_UPDATER_VERSION="${REMOTEX_UPDATER_VERSION}")

# Exigir elevação UAC (requireAdministrator) no manifest embutido pelo linker.
if(MSVC)
    target_link_options(remotex_updater PRIVATE
        "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()

# ── Dependências ─────────────────────────────────────────────────────────────
# Core (AuthClient, CryptoUtils, MachineClient, ConnectionSettings)
target_link_libraries(remotex_updater PRIVATE remotex_core)

# Windows APIs — serviço (advapi32 already linked via core)
# No DXGI, DirectX, user32, wtsapi32, userenv, FFmpeg, Qt needed.

if(TARGET spdlog::spdlog)
    target_link_libraries(remotex_updater PRIVATE spdlog::spdlog)
endif()

if(ZLIB_FOUND)
    target_link_libraries(remotex_updater PRIVATE ZLIB::ZLIB)
endif()
