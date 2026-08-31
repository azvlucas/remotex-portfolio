cmake_minimum_required(VERSION 3.25)
project(RemoteX VERSION 0.1.0 LANGUAGES CXX)

# ── C++20 ────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Plataforma ───────────────────────────────────────────────────────────────
if(NOT WIN32)
    message(FATAL_ERROR "RemoteX targets Windows only.")
endif()

# ── Opções de build ──────────────────────────────────────────────────────────
option(BUILD_TESTS   "Build unit and integration tests" ON)
option(BUILD_HOST    "Build the Host agent"             ON)
option(BUILD_VIEWER  "Build the Viewer application"     ON)
option(BUILD_UPDATER "Build the Updater service"        ON)

# ── Diretório de saída ──────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ── Dependências de terceiros ────────────────────────────────────────────────
add_subdirectory(libs)
add_subdirectory(tools)

# ── Biblioteca core (compartilhada entre Host e Viewer) ──────────────────────
add_subdirectory(core)

# ── Aplicações ───────────────────────────────────────────────────────────────
if(BUILD_HOST)
    add_subdirectory(host)
endif()

if(BUILD_VIEWER)
    add_subdirectory(viewer)
endif()

if(BUILD_UPDATER)
    add_subdirectory(updater)
endif()

# ── Testes ───────────────────────────────────────────────────────────────────
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
