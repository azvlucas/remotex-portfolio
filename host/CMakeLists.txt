# â”€â”€ Host: agente que roda na mÃ¡quina controlada â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
set(REMOTEX_HOST_SOURCES
    src/main.cpp
    src/capture_cadence.cpp
    src/capture_damage.cpp
    src/capture_frame_preparer.cpp
    src/desktop_context.cpp
    src/frame_rotator.cpp
    src/gdi_desktop_snapshot.cpp
    src/rotation_geometry.cpp
    src/screen_capture.cpp
    src/input_injector.cpp
    src/backup_agent.cpp
    src/auto_updater.cpp
    src/connection_settings_store.cpp
    src/heartbeat.cpp
    src/service_manager.cpp
    src/chat_window.cpp
    src/clipboard_sync.cpp
    src/session_recorder.cpp
    src/recording_uploader.cpp
    src/privacy_screen.cpp
)

if(EXISTS "${CMAKE_SOURCE_DIR}/remotex.ico")
    file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/remotex.ico" REMOTEX_HOST_ICON_RC_PATH)
    string(REPLACE "\\" "\\\\" REMOTEX_HOST_ICON_RC_PATH "${REMOTEX_HOST_ICON_RC_PATH}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/remotex_host.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/remotex_host.rc"
        @ONLY
    )
    list(APPEND REMOTEX_HOST_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/remotex_host.rc")
else()
    message(WARNING "Host icon missing at ${CMAKE_SOURCE_DIR}/remotex.ico - building without custom icon.")
endif()

add_executable(remotex_host ${REMOTEX_HOST_SOURCES})

set(REMOTEX_VERSION_FILE "${CMAKE_SOURCE_DIR}/VERSION")
if(NOT EXISTS "${REMOTEX_VERSION_FILE}")
    message(FATAL_ERROR "Missing VERSION file at ${REMOTEX_VERSION_FILE}")
endif()

file(READ "${REMOTEX_VERSION_FILE}" REMOTEX_HOST_VERSION)
string(STRIP "${REMOTEX_HOST_VERSION}" REMOTEX_HOST_VERSION)
if(REMOTEX_HOST_VERSION STREQUAL "")
    message(FATAL_ERROR "VERSION file is empty")
endif()

target_include_directories(remotex_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(remotex_host PRIVATE cxx_std_20)
target_compile_definitions(remotex_host PRIVATE REMOTEX_HOST_VERSION="${REMOTEX_HOST_VERSION}")

# Exigir elevaÃ§Ã£o UAC (requireAdministrator) no manifest embutido pelo linker.
if(MSVC)
    target_link_options(remotex_host PRIVATE
        "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()

# â”€â”€ DependÃªncias â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
target_link_libraries(remotex_host PRIVATE remotex_core)

# Windows APIs (DXGI, DirectX 11, user32 para SendInput, serviÃ§o)
target_link_libraries(remotex_host PRIVATE
    d3d11
    d3dcompiler
    dxgi
    user32
    advapi32   # Service Control Manager
    wtsapi32   # Remote Desktop Services (WTSGetActiveConsoleSessionId)
    userenv    # CreateEnvironmentBlock
)

if(TARGET spdlog::spdlog)
    target_link_libraries(remotex_host PRIVATE spdlog::spdlog)
endif()

if(ZLIB_FOUND)
    target_link_libraries(remotex_host PRIVATE ZLIB::ZLIB)
endif()

if(TARGET ffmpeg::avcodec AND
   TARGET ffmpeg::avformat AND
   TARGET ffmpeg::avutil AND
   TARGET ffmpeg::swscale)
    target_compile_definitions(remotex_host PRIVATE REMOTEX_HAS_FFMPEG)
    target_link_libraries(remotex_host PRIVATE
        ffmpeg::avcodec
        ffmpeg::avformat
        ffmpeg::avutil
        ffmpeg::swscale
    )
    message(STATUS "Host: session recording ENABLED (FFmpeg)")
else()
    message(STATUS "Host: session recording DISABLED (no FFmpeg)")
endif()
