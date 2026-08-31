# ── Testes do core ───────────────────────────────────────────────────────────

add_executable(test_input_protocol test_input_protocol.cpp)
target_link_libraries(test_input_protocol PRIVATE remotex_core GTest::gtest_main)
target_compile_features(test_input_protocol PRIVATE cxx_std_20)
add_test(NAME core_input_protocol COMMAND test_input_protocol)

add_executable(test_file_transfer test_file_transfer.cpp)
target_link_libraries(test_file_transfer PRIVATE remotex_core GTest::gtest_main)
target_compile_features(test_file_transfer PRIVATE cxx_std_20)
add_test(NAME core_file_transfer COMMAND test_file_transfer)

add_executable(test_webrtc_peer_stub test_webrtc_peer_stub.cpp)
target_link_libraries(test_webrtc_peer_stub PRIVATE remotex_core GTest::gtest_main)
target_compile_features(test_webrtc_peer_stub PRIVATE cxx_std_20)
add_test(NAME core_webrtc_peer_stub COMMAND test_webrtc_peer_stub)

add_executable(test_auth_client test_auth_client.cpp)
target_link_libraries(test_auth_client PRIVATE remotex_core GTest::gtest_main advapi32)
target_compile_features(test_auth_client PRIVATE cxx_std_20)
add_test(NAME core_auth_client COMMAND test_auth_client)

add_executable(test_windows_shell_utils test_windows_shell_utils.cpp)
target_link_libraries(test_windows_shell_utils PRIVATE remotex_core GTest::gtest_main)
target_compile_features(test_windows_shell_utils PRIVATE cxx_std_20)
add_test(NAME core_windows_shell_utils COMMAND test_windows_shell_utils)
