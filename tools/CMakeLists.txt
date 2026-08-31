find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_executable(sign_release
    sign_release.cpp
)

target_compile_features(sign_release PRIVATE cxx_std_20)

target_link_libraries(sign_release PRIVATE OpenSSL::Crypto)
