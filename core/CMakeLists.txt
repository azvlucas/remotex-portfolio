# ── Core: biblioteca compartilhada entre Host e Viewer ───────────────────────
add_library(remotex_core STATIC)

target_sources(remotex_core PRIVATE
    src/webrtc_peer.cpp
    src/signaling_client.cpp
    src/file_transfer.cpp
    src/input_protocol.cpp
    src/auth_client.cpp
    src/machine_client.cpp
    src/crypto_utils.cpp
    src/log_redaction.cpp
    src/turn_client.cpp
    src/connection_settings.cpp
    src/video_encoder.cpp
    src/windows_shell_utils.cpp
)

target_include_directories(remotex_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(remotex_core PUBLIC cxx_std_20)

# ── Dependências ─────────────────────────────────────────────────────────────
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(remotex_core PUBLIC nlohmann_json::nlohmann_json)
endif()

if(TARGET spdlog::spdlog)
    target_link_libraries(remotex_core PUBLIC spdlog::spdlog)
endif()

find_package(OpenSSL QUIET COMPONENTS SSL Crypto)
if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
    target_link_libraries(remotex_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

if(CURL_FOUND)
    target_link_libraries(remotex_core PUBLIC CURL::libcurl)
endif()

find_package(ZLIB)
if(ZLIB_FOUND)
    target_link_libraries(remotex_core PUBLIC ZLIB::ZLIB)
endif()

if(TARGET ffmpeg::avcodec AND
   TARGET ffmpeg::avutil AND
   TARGET ffmpeg::swscale)
    target_compile_definitions(remotex_core PUBLIC REMOTEX_HAS_FFMPEG)
    target_link_libraries(remotex_core PUBLIC
        ffmpeg::avcodec
        ffmpeg::avutil
        ffmpeg::swscale
    )
endif()

# libwebrtc (configurável via LIBWEBRTC_ROOT)
if(DEFINED LIBWEBRTC_ROOT)
    target_include_directories(remotex_core PUBLIC ${LIBWEBRTC_ROOT}/include)
    target_link_directories(remotex_core PUBLIC ${LIBWEBRTC_ROOT}/lib)
    target_link_libraries(remotex_core PUBLIC webrtc)
    target_compile_definitions(remotex_core PUBLIC REMOTEX_HAS_LIBWEBRTC)
endif()

# Windows libs
target_link_libraries(remotex_core PUBLIC
    ws2_32
    winhttp
    bcrypt      # CNG (SHA-256, Ed25519)
    advapi32    # Credential Manager (CredWrite/CredRead), Registry
    crypt32     # Certificate helpers
    ole32
    oleaut32
    shell32
    wtsapi32    # Session/user token lookup for interactive-shell path resolution
)
