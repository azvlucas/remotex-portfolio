# ── Testes de integração ──────────────────────────────────────────────────────

find_package(Qt6 COMPONENTS Core Gui Widgets OpenGLWidgets REQUIRED)

add_executable(test_full_session test_full_session.cpp)
target_include_directories(test_full_session PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
)
target_link_libraries(test_full_session PRIVATE remotex_core GTest::gtest_main)
target_compile_features(test_full_session PRIVATE cxx_std_20)
add_test(NAME integration_full_session COMMAND test_full_session)

add_executable(test_file_transfer_integration test_file_transfer.cpp)
target_link_libraries(test_file_transfer_integration PRIVATE remotex_core GTest::gtest_main)
target_compile_features(test_file_transfer_integration PRIVATE cxx_std_20)
add_test(NAME integration_file_transfer COMMAND test_file_transfer_integration)

add_executable(test_backup_cycle test_backup_cycle.cpp)
target_link_libraries(test_backup_cycle PRIVATE remotex_core GTest::gtest_main)
target_compile_features(test_backup_cycle PRIVATE cxx_std_20)
add_test(NAME integration_backup_cycle COMMAND test_backup_cycle)

add_executable(test_session_metrics test_session_metrics.cpp)
target_include_directories(test_session_metrics PRIVATE
    ${CMAKE_SOURCE_DIR}/host/include
    ${CMAKE_SOURCE_DIR}/viewer/include
    ${CMAKE_SOURCE_DIR}/core/include
)
target_link_libraries(test_session_metrics PRIVATE
    remotex_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGLWidgets
    GTest::gtest_main
)
target_compile_features(test_session_metrics PRIVATE cxx_std_20)
add_test(NAME integration_session_metrics COMMAND test_session_metrics)
