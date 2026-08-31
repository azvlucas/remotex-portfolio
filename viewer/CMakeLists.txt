# ── Viewer: aplicação Qt do técnico ──────────────────────────────────────────
if(NOT TARGET Qt6::Widgets)
    message(STATUS "Viewer: Qt6::Widgets not found as target, trying find_package...")
    find_package(Qt6 COMPONENTS Core Gui Widgets OpenGLWidgets Network Concurrent QUIET)
    if(NOT TARGET Qt6::Widgets)
        message(WARNING "Qt6 not found — skipping Viewer build.")
        return()
    endif()
endif()
message(STATUS "Viewer: Qt6::Widgets found, building viewer")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(REMOTEX_VIEWER_SOURCES
    src/connection_settings.cpp
    src/diagnostics_window.cpp
    src/main.cpp
    src/file_transfer_dialog.cpp
    src/group_management_dialog.cpp
    src/login_dialog.cpp
    src/machine_display_utils.cpp
    src/machine_edit_dialog.cpp
    src/main_window.cpp
    src/remote_session_window.cpp
    src/settings_dialog.cpp
    src/user_management_dialog.cpp
    src/reports_dialog.cpp
    src/auto_updater.cpp
    src/chat_panel.cpp
    src/standalone_chat_dialog.cpp
    src/video_renderer.cpp
    src/input_capture.cpp
    include/viewer/auto_updater.h
    include/viewer/chat_panel.h
    include/viewer/diagnostics_window.h
    include/viewer/standalone_chat_dialog.h
    include/viewer/connection_settings.h
    include/viewer/file_transfer_dialog.h
    include/viewer/group_management_dialog.h
    include/viewer/login_dialog.h
    include/viewer/machine_display_utils.h
    include/viewer/machine_edit_dialog.h
    include/viewer/main_window.h
    include/viewer/remote_session_window.h
    include/viewer/settings_dialog.h
    include/viewer/user_management_dialog.h
    include/viewer/reports_dialog.h
    include/viewer/video_renderer.h
    include/viewer/input_capture.h
)

if(EXISTS "${CMAKE_SOURCE_DIR}/remotex.ico")
    file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/remotex.ico" REMOTEX_VIEWER_ICON_RC_PATH)
    string(REPLACE "\\" "\\\\" REMOTEX_VIEWER_ICON_RC_PATH "${REMOTEX_VIEWER_ICON_RC_PATH}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/remotex_viewer.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/remotex_viewer.rc"
        @ONLY
    )
    list(APPEND REMOTEX_VIEWER_SOURCES
        resources/viewer_resources.qrc
        "${CMAKE_CURRENT_BINARY_DIR}/remotex_viewer.rc"
    )
else()
    message(WARNING "Viewer icon missing at ${CMAKE_SOURCE_DIR}/remotex.ico - building without custom icon.")
endif()

# Read version from root VERSION file.
file(READ "${CMAKE_SOURCE_DIR}/VERSION" _viewer_version_raw)
string(STRIP "${_viewer_version_raw}" REMOTEX_VIEWER_VERSION)

add_executable(remotex_viewer WIN32 ${REMOTEX_VIEWER_SOURCES})

target_compile_definitions(remotex_viewer PRIVATE
    REMOTEX_VIEWER_VERSION="${REMOTEX_VIEWER_VERSION}"
)

target_include_directories(remotex_viewer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(remotex_viewer PRIVATE cxx_std_20)

# ── Dependências ─────────────────────────────────────────────────────────────
target_link_libraries(remotex_viewer PRIVATE
    remotex_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGLWidgets
    Qt6::Network
    Qt6::Concurrent
    opengl32        # OpenGL
)

if(TARGET spdlog::spdlog)
    target_link_libraries(remotex_viewer PRIVATE spdlog::spdlog)
endif()
