set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 COMPONENTS Core Gui Widgets Test OpenGLWidgets Concurrent REQUIRED)

add_executable(test_machine_display_utils
    test_machine_display_utils.cpp
    ${CMAKE_SOURCE_DIR}/viewer/src/machine_display_utils.cpp
)

target_include_directories(test_machine_display_utils PRIVATE
    ${CMAKE_SOURCE_DIR}/viewer/include
    ${CMAKE_SOURCE_DIR}/core/include
)

target_link_libraries(test_machine_display_utils PRIVATE
    Qt6::Core
    GTest::gtest_main
)

target_compile_features(test_machine_display_utils PRIVATE cxx_std_20)
add_test(NAME viewer_machine_display_utils COMMAND test_machine_display_utils)
set_tests_properties(viewer_machine_display_utils PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)

add_executable(test_presigned_url_utils
    test_presigned_url_utils.cpp
)

target_include_directories(test_presigned_url_utils PRIVATE
    ${CMAKE_SOURCE_DIR}/viewer/include
)

target_link_libraries(test_presigned_url_utils PRIVATE
    Qt6::Core
    Qt6::Gui
    GTest::gtest_main
)

target_compile_features(test_presigned_url_utils PRIVATE cxx_std_20)
add_test(NAME viewer_presigned_url_utils COMMAND test_presigned_url_utils)
set_tests_properties(viewer_presigned_url_utils PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)

add_executable(test_connection_settings
    test_connection_settings.cpp
    ${CMAKE_SOURCE_DIR}/viewer/src/connection_settings.cpp
)

target_include_directories(test_connection_settings PRIVATE
    ${CMAKE_SOURCE_DIR}/viewer/include
    ${CMAKE_SOURCE_DIR}/core/include
)

target_link_libraries(test_connection_settings PRIVATE
    Qt6::Core
    GTest::gtest_main
)

target_compile_features(test_connection_settings PRIVATE cxx_std_20)
add_test(NAME viewer_connection_settings COMMAND test_connection_settings)
set_tests_properties(viewer_connection_settings PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)

add_executable(test_reports_dialog
    test_reports_dialog.cpp
    ${CMAKE_SOURCE_DIR}/viewer/include/viewer/reports_dialog.h
    ${CMAKE_SOURCE_DIR}/viewer/src/reports_dialog.cpp
    ${CMAKE_SOURCE_DIR}/viewer/src/connection_settings.cpp
)

target_include_directories(test_reports_dialog PRIVATE
    ${CMAKE_SOURCE_DIR}/viewer/include
    ${CMAKE_SOURCE_DIR}/core/include
)

target_link_libraries(test_reports_dialog PRIVATE
    remotex_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Test
    GTest::gtest
)

target_compile_features(test_reports_dialog PRIVATE cxx_std_20)
add_custom_command(TARGET test_reports_dialog POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:Qt6::Concurrent>
        $<TARGET_FILE_DIR:test_reports_dialog>
)
add_test(NAME viewer_reports_dialog COMMAND test_reports_dialog)
set_tests_properties(viewer_reports_dialog PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)

add_executable(test_file_transfer_dialog
    test_file_transfer_dialog.cpp
    ${CMAKE_SOURCE_DIR}/viewer/include/viewer/file_transfer_dialog.h
    ${CMAKE_SOURCE_DIR}/viewer/src/file_transfer_dialog.cpp
)

target_include_directories(test_file_transfer_dialog PRIVATE
    ${CMAKE_SOURCE_DIR}/viewer/include
    ${CMAKE_SOURCE_DIR}/core/include
)

target_link_libraries(test_file_transfer_dialog PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGLWidgets
    Qt6::Test
    GTest::gtest
)

target_compile_features(test_file_transfer_dialog PRIVATE cxx_std_20)
add_test(NAME viewer_file_transfer_dialog COMMAND test_file_transfer_dialog)
set_tests_properties(viewer_file_transfer_dialog PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)

add_executable(test_session_diagnostics
    test_session_diagnostics.cpp
)

target_include_directories(test_session_diagnostics PRIVATE
    ${CMAKE_SOURCE_DIR}/viewer/include
    ${CMAKE_SOURCE_DIR}/core/include
)

target_link_libraries(test_session_diagnostics PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGLWidgets
    GTest::gtest_main
)

target_compile_features(test_session_diagnostics PRIVATE cxx_std_20)
add_test(NAME viewer_session_diagnostics COMMAND test_session_diagnostics)
set_tests_properties(viewer_session_diagnostics PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)

add_executable(test_frame_drop_counting
    test_frame_drop_counting.cpp
)

target_include_directories(test_frame_drop_counting PRIVATE
    ${CMAKE_SOURCE_DIR}/viewer/include
    ${CMAKE_SOURCE_DIR}/core/include
)

target_link_libraries(test_frame_drop_counting PRIVATE
    GTest::gtest_main
)

target_compile_features(test_frame_drop_counting PRIVATE cxx_std_20)
add_test(NAME viewer_frame_drop_counting COMMAND test_frame_drop_counting)

add_executable(test_rotation_session_contract
    test_rotation_session_contract.cpp
)

target_include_directories(test_rotation_session_contract PRIVATE
    ${CMAKE_SOURCE_DIR}/viewer/include
    ${CMAKE_SOURCE_DIR}/core/include
)

target_link_libraries(test_rotation_session_contract PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGLWidgets
    GTest::gtest_main
)

target_compile_features(test_rotation_session_contract PRIVATE cxx_std_20)
add_test(NAME viewer_rotation_session_contract COMMAND test_rotation_session_contract)
set_tests_properties(viewer_rotation_session_contract PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)
