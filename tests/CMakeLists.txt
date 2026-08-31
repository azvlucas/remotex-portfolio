# ── Testes ────────────────────────────────────────────────────────────────────

find_package(GTest CONFIG QUIET)

if(NOT TARGET GTest::gtest_main)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

if(TARGET GTest::gtest_main)
    # Testes do core (input protocol, file transfer)
    add_subdirectory(core)

    # Testes do host (screen capture, video encoder)
    add_subdirectory(host)

    # Testes de integração (full session, file transfer, backup)
    add_subdirectory(integration)

    # Testes do viewer — requer Qt6
    add_subdirectory(viewer)
else()
    message(STATUS "GTest not found — tests will not be built. Install via vcpkg: vcpkg install gtest")
endif()
