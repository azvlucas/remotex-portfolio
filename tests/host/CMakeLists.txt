# ── Testes do host ───────────────────────────────────────────────────────────

add_executable(test_screen_capture
    test_screen_capture.cpp
    ${CMAKE_SOURCE_DIR}/host/src/desktop_context.cpp
    ${CMAKE_SOURCE_DIR}/host/src/screen_capture.cpp
)
target_include_directories(test_screen_capture PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
)
target_link_libraries(test_screen_capture PRIVATE remotex_core GTest::gtest_main
    d3d11 dxgi user32)
target_compile_features(test_screen_capture PRIVATE cxx_std_20)
add_test(NAME host_screen_capture COMMAND test_screen_capture)

add_executable(test_screen_capture_rotation
    test_screen_capture_rotation.cpp
    ${CMAKE_SOURCE_DIR}/host/src/desktop_context.cpp
    ${CMAKE_SOURCE_DIR}/host/src/screen_capture.cpp
)
target_include_directories(test_screen_capture_rotation PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
)
target_link_libraries(test_screen_capture_rotation PRIVATE remotex_core GTest::gtest_main
    d3d11 dxgi user32)
target_compile_features(test_screen_capture_rotation PRIVATE cxx_std_20)
add_test(NAME host_screen_capture_rotation COMMAND test_screen_capture_rotation)

add_executable(test_video_encoder
    test_video_encoder.cpp
)
target_include_directories(test_video_encoder PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
    ${CMAKE_SOURCE_DIR}/core/include
)
target_link_libraries(test_video_encoder PRIVATE remotex_core GTest::gtest_main)
target_compile_features(test_video_encoder PRIVATE cxx_std_20)
add_test(NAME host_video_encoder COMMAND test_video_encoder)

add_executable(test_encode_quality
    test_encode_quality.cpp
)
target_include_directories(test_encode_quality PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
    ${CMAKE_SOURCE_DIR}/core/include
)
target_link_libraries(test_encode_quality PRIVATE remotex_core GTest::gtest_main)
target_compile_features(test_encode_quality PRIVATE cxx_std_20)
add_test(NAME host_encode_quality COMMAND test_encode_quality)

add_executable(test_resolution_adaptation
    test_resolution_adaptation.cpp
)
target_include_directories(test_resolution_adaptation PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
)
target_link_libraries(test_resolution_adaptation PRIVATE remotex_core GTest::gtest_main)
target_compile_features(test_resolution_adaptation PRIVATE cxx_std_20)
add_test(NAME host_resolution_adaptation COMMAND test_resolution_adaptation)

add_executable(test_recording_utils test_recording_utils.cpp)
target_include_directories(test_recording_utils PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
)
target_link_libraries(test_recording_utils PRIVATE remotex_core GTest::gtest_main)
target_compile_features(test_recording_utils PRIVATE cxx_std_20)
add_test(NAME host_recording_utils COMMAND test_recording_utils)

add_executable(test_recording_finalize test_recording_finalize.cpp)
target_include_directories(test_recording_finalize PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
)
target_link_libraries(test_recording_finalize PRIVATE GTest::gtest_main)
target_compile_features(test_recording_finalize PRIVATE cxx_std_20)
add_test(NAME host_recording_finalize COMMAND test_recording_finalize)

add_executable(test_recording_uploader
    test_recording_uploader.cpp
    ${CMAKE_SOURCE_DIR}/host/src/recording_uploader.cpp
)
target_include_directories(test_recording_uploader PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
    ${CMAKE_SOURCE_DIR}/core/include
)
target_link_libraries(test_recording_uploader PRIVATE
    remotex_core
    GTest::gtest_main
    ws2_32
)
target_compile_features(test_recording_uploader PRIVATE cxx_std_20)
add_test(NAME host_recording_uploader COMMAND test_recording_uploader)

add_executable(test_clipboard_sync
    test_clipboard_sync.cpp
    ${CMAKE_SOURCE_DIR}/host/src/clipboard_sync.cpp
)
target_include_directories(test_clipboard_sync PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
)
target_link_libraries(test_clipboard_sync PRIVATE remotex_core GTest::gtest_main user32)
target_compile_features(test_clipboard_sync PRIVATE cxx_std_20)
add_test(NAME host_clipboard_sync COMMAND test_clipboard_sync)

add_executable(test_input_injector
    test_input_injector.cpp
    ${CMAKE_SOURCE_DIR}/host/src/desktop_context.cpp
    ${CMAKE_SOURCE_DIR}/host/src/input_injector.cpp
)
target_include_directories(test_input_injector PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
)
target_link_libraries(test_input_injector PRIVATE remotex_core GTest::gtest_main user32)
target_compile_features(test_input_injector PRIVATE cxx_std_20)
add_test(NAME host_input_injector COMMAND test_input_injector)

add_executable(test_auto_updater
    test_auto_updater.cpp
    ${CMAKE_SOURCE_DIR}/host/src/auto_updater.cpp
)
target_include_directories(test_auto_updater PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
)
target_link_libraries(test_auto_updater PRIVATE
    remotex_core
    GTest::gtest_main
    advapi32
)
target_compile_features(test_auto_updater PRIVATE cxx_std_20)
add_test(NAME host_auto_updater COMMAND test_auto_updater)

add_executable(test_connection_settings_store
    test_connection_settings_store.cpp
    ${CMAKE_SOURCE_DIR}/host/src/connection_settings_store.cpp
)
target_include_directories(test_connection_settings_store PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
    ${CMAKE_SOURCE_DIR}/core/include
)
target_link_libraries(test_connection_settings_store PRIVATE
    remotex_core
    GTest::gtest_main
    advapi32
)
target_compile_features(test_connection_settings_store PRIVATE cxx_std_20)
add_test(NAME host_connection_settings_store COMMAND test_connection_settings_store)

add_executable(test_session_instrumentation
    test_session_instrumentation.cpp
)
target_include_directories(test_session_instrumentation PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
    ${CMAKE_SOURCE_DIR}/core/include
)
target_link_libraries(test_session_instrumentation PRIVATE
    GTest::gtest_main
)
target_compile_features(test_session_instrumentation PRIVATE cxx_std_20)
add_test(NAME host_session_instrumentation COMMAND test_session_instrumentation)

add_executable(test_runtime_paths
    test_runtime_paths.cpp
)
target_include_directories(test_runtime_paths PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
)
target_link_libraries(test_runtime_paths PRIVATE
    GTest::gtest_main
)
target_compile_features(test_runtime_paths PRIVATE cxx_std_20)
add_test(NAME host_runtime_paths COMMAND test_runtime_paths)

add_executable(test_worker_watchdog
    test_worker_watchdog.cpp
)
target_include_directories(test_worker_watchdog PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
)
target_link_libraries(test_worker_watchdog PRIVATE
    GTest::gtest_main
)
target_compile_features(test_worker_watchdog PRIVATE cxx_std_20)
add_test(NAME host_worker_watchdog COMMAND test_worker_watchdog)

add_executable(test_capture_cadence
    test_capture_cadence.cpp
    ${CMAKE_SOURCE_DIR}/host/src/capture_cadence.cpp
)
target_include_directories(test_capture_cadence PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
)
target_link_libraries(test_capture_cadence PRIVATE
    GTest::gtest_main
)
target_compile_features(test_capture_cadence PRIVATE cxx_std_20)
add_test(NAME test_capture_cadence COMMAND test_capture_cadence)

add_executable(test_capture_frame_preparer
    test_capture_frame_preparer.cpp
    ${CMAKE_SOURCE_DIR}/host/src/capture_damage.cpp
    ${CMAKE_SOURCE_DIR}/host/src/capture_frame_preparer.cpp
    ${CMAKE_SOURCE_DIR}/host/src/desktop_context.cpp
    ${CMAKE_SOURCE_DIR}/host/src/screen_capture.cpp
)
target_include_directories(test_capture_frame_preparer PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
)
target_link_libraries(test_capture_frame_preparer PRIVATE
    GTest::gtest_main
    d3d11
    dxgi
    user32
)
target_compile_features(test_capture_frame_preparer PRIVATE cxx_std_20)
add_test(NAME test_capture_frame_preparer COMMAND test_capture_frame_preparer)

add_executable(test_rotation_geometry
    test_rotation_geometry.cpp
    ${CMAKE_SOURCE_DIR}/host/src/rotation_geometry.cpp
)
target_include_directories(test_rotation_geometry PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
)
target_link_libraries(test_rotation_geometry PRIVATE
    GTest::gtest_main
    d3d11
    dxgi
)
target_compile_features(test_rotation_geometry PRIVATE cxx_std_20)
add_test(NAME host_rotation_geometry COMMAND test_rotation_geometry)

add_executable(test_frame_rotator
    test_frame_rotator.cpp
    ${CMAKE_SOURCE_DIR}/host/src/frame_rotator.cpp
)
target_include_directories(test_frame_rotator PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
)
target_link_libraries(test_frame_rotator PRIVATE
    GTest::gtest_main
    d3d11
    d3dcompiler
    dxgi
)
target_compile_features(test_frame_rotator PRIVATE cxx_std_20)
add_test(NAME host_frame_rotator COMMAND test_frame_rotator)

add_executable(test_capture_rotation_pipeline
    test_capture_rotation_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/host/src/capture_damage.cpp
    ${CMAKE_SOURCE_DIR}/host/src/capture_frame_preparer.cpp
    ${CMAKE_SOURCE_DIR}/host/src/desktop_context.cpp
    ${CMAKE_SOURCE_DIR}/host/src/frame_rotator.cpp
    ${CMAKE_SOURCE_DIR}/host/src/rotation_geometry.cpp
    ${CMAKE_SOURCE_DIR}/host/src/screen_capture.cpp
)
target_include_directories(test_capture_rotation_pipeline PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
)
target_link_libraries(test_capture_rotation_pipeline PRIVATE
    GTest::gtest_main
    d3d11
    d3dcompiler
    dxgi
    user32
)
target_compile_features(test_capture_rotation_pipeline PRIVATE cxx_std_20)
add_test(NAME host_capture_rotation_pipeline COMMAND test_capture_rotation_pipeline)
