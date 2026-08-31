# ── Dependências de terceiros ────────────────────────────────────────────────
# Gerenciadas via vcpkg; este arquivo apenas localiza os pacotes.

# nlohmann/json
find_package(nlohmann_json CONFIG QUIET)

# spdlog
find_package(spdlog CONFIG QUIET)

# OpenSSL
find_package(OpenSSL QUIET)

# CURL
find_package(CURL QUIET)

# zlib
find_package(ZLIB QUIET)

# Qt 6 (necessário apenas para o Viewer)
find_package(Qt6 COMPONENTS Core Gui Widgets OpenGLWidgets Network)
if(Qt6_FOUND)
    message(STATUS "Qt6 found: ${Qt6_DIR}")
    if(TARGET Qt6::Widgets)
        message(STATUS "Qt6::Widgets target EXISTS")
    else()
        message(STATUS "Qt6::Widgets target MISSING")
    endif()
else()
    message(STATUS "Qt6 NOT found by find_package")
endif()

# FFmpeg (necessário para gravação de sessão no Host)
# Configurar via FFMPEG_ROOT ou variável de ambiente
if(DEFINED FFMPEG_ROOT OR DEFINED ENV{FFMPEG_ROOT})
    if(NOT DEFINED FFMPEG_ROOT)
        set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}")
    endif()
    message(STATUS "Looking for FFmpeg in: ${FFMPEG_ROOT}")
    set(AVCODEC_INCLUDE_DIR "${FFMPEG_ROOT}/include")
    set(AVCODEC_LIBRARY "${FFMPEG_ROOT}/lib/avcodec.lib")
    set(AVFORMAT_LIBRARY "${FFMPEG_ROOT}/lib/avformat.lib")
    set(AVUTIL_LIBRARY "${FFMPEG_ROOT}/lib/avutil.lib")
    set(SWSCALE_LIBRARY "${FFMPEG_ROOT}/lib/swscale.lib")

    if(EXISTS "${AVCODEC_LIBRARY}" AND EXISTS "${AVFORMAT_LIBRARY}"
       AND EXISTS "${AVUTIL_LIBRARY}" AND EXISTS "${SWSCALE_LIBRARY}")
        set(FFMPEG_FOUND TRUE CACHE INTERNAL "FFmpeg available for host recording" FORCE)
        add_library(ffmpeg::avcodec UNKNOWN IMPORTED GLOBAL)
        set_target_properties(ffmpeg::avcodec PROPERTIES
            IMPORTED_LOCATION "${AVCODEC_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${AVCODEC_INCLUDE_DIR}")
        add_library(ffmpeg::avformat UNKNOWN IMPORTED GLOBAL)
        set_target_properties(ffmpeg::avformat PROPERTIES
            IMPORTED_LOCATION "${AVFORMAT_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${AVCODEC_INCLUDE_DIR}")
        add_library(ffmpeg::avutil UNKNOWN IMPORTED GLOBAL)
        set_target_properties(ffmpeg::avutil PROPERTIES
            IMPORTED_LOCATION "${AVUTIL_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${AVCODEC_INCLUDE_DIR}")
        add_library(ffmpeg::swscale UNKNOWN IMPORTED GLOBAL)
        set_target_properties(ffmpeg::swscale PROPERTIES
            IMPORTED_LOCATION "${SWSCALE_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${AVCODEC_INCLUDE_DIR}")
        message(STATUS "FFmpeg found at: ${FFMPEG_ROOT}")
    else()
        set(FFMPEG_FOUND FALSE CACHE INTERNAL "FFmpeg available for host recording" FORCE)
        message(STATUS "FFmpeg libraries not found in ${FFMPEG_ROOT}")
    endif()
else()
    set(FFMPEG_FOUND FALSE CACHE INTERNAL "FFmpeg available for host recording" FORCE)
    message(STATUS "FFmpeg NOT configured (set FFMPEG_ROOT to enable session recording)")
endif()

# libwebrtc é linkada manualmente por enquanto (build from source)
# O caminho será configurado via variável LIBWEBRTC_ROOT
