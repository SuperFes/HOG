cmake_minimum_required(VERSION 3.28)

project(HOG
    VERSION 0.0.1
    DESCRIPTION "A seamless-planet MMORPG and engine"
    HOMEPAGE_URL "https://github.com/SuperFes/HOG"
    LANGUAGES C CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported; use a preset, e.g. cmake --preset debug")
endif()

# Project-wide so dependencies match our ABI (Catch2's string_view support depends on it).
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(HogOptions)

# No C++ modules yet; scanning adds flags that clang-tidy and older tools reject.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
include(GetCPM)

# Header-only use (XXH_INLINE_ALL); BSD-2-Clause.
CPMAddPackage(
    NAME xxHash
    GITHUB_REPOSITORY Cyan4973/xxHash
    VERSION 0.8.4
    DOWNLOAD_ONLY YES)

# Header-only; MIT. Configured through hog_core's compile definitions.
CPMAddPackage(
    NAME glm
    GITHUB_REPOSITORY g-truc/glm
    GIT_TAG 1.0.3
    DOWNLOAD_ONLY YES)

# Task scheduler; Zlib. Built by hog_jobs from source to skip its C interface and examples.
CPMAddPackage(
    NAME enkiTS
    GITHUB_REPOSITORY dougbinks/enkiTS
    GIT_TAG v1.12
    DOWNLOAD_ONLY YES)

# Header-only; MIT. Config files are TOML.
CPMAddPackage(
    NAME tomlplusplus
    GITHUB_REPOSITORY marzer/tomlplusplus
    VERSION 3.4.0
    DOWNLOAD_ONLY YES)
add_library(tomlplusplus INTERFACE)
target_include_directories(tomlplusplus SYSTEM INTERFACE "${tomlplusplus_SOURCE_DIR}/include")
# Parse errors come back as values, matching hog::Result.
target_compile_definitions(tomlplusplus INTERFACE TOML_EXCEPTIONS=0)

# Public domain. The amalgamation is one C file; only servers keep a database.
CPMAddPackage(
    NAME sqlite3
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip
    URL_HASH SHA3_256=7b670a62fdfbd672b75fef004cb703c8a3e87d3a5cc7d675b4a08337004a2d93
    DOWNLOAD_ONLY YES)

# Entity system; MIT. Static, from its own CMake so Windows gets its socket libraries.
CPMAddPackage(
    NAME flecs
    GITHUB_REPOSITORY SanderMertens/flecs
    VERSION 4.1.6
    SYSTEM YES
    OPTIONS "FLECS_SHARED OFF" "FLECS_STATIC ON" "FLECS_TESTS OFF")

# Physics; MIT. Double precision, so bodies sit at planet coordinates. Built for any x86-64 since
# SSE4.2 rather than for the machine building it, and with our flags rather than its own.
CPMAddPackage(
    NAME JoltPhysics
    GITHUB_REPOSITORY jrouwe/JoltPhysics
    VERSION 5.6.0
    SOURCE_SUBDIR Build
    SYSTEM YES
    OPTIONS "DOUBLE_PRECISION ON" "OVERRIDE_CXX_FLAGS OFF" "INTERPROCEDURAL_OPTIMIZATION OFF"
            "ENABLE_OBJECT_STREAM OFF" "ENABLE_INSTALL OFF" "TARGET_UNIT_TESTS OFF"
            "TARGET_HELLO_WORLD OFF" "TARGET_PERFORMANCE_TEST OFF" "TARGET_SAMPLES OFF"
            "TARGET_VIEWER OFF" "USE_AVX OFF" "USE_AVX2 OFF" "USE_AVX512 OFF" "USE_F16C OFF"
            "USE_FMADD OFF" "USE_LZCNT OFF" "USE_TZCNT OFF" "ENABLE_ALL_WARNINGS OFF"
            "USE_STATIC_MSVC_RUNTIME_LIBRARY OFF" "DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF"
            "PROFILER_IN_DEBUG_AND_RELEASE OFF" "JPH_USE_DX12 OFF" "JPH_USE_VK OFF" "JPH_USE_MTL OFF"
            "JPH_USE_CPU_COMPUTE OFF")

# Public domain or MIT. Only stb_image_write, for map images.
CPMAddPackage(
    NAME stb
    GITHUB_REPOSITORY nothings/stb
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
    DOWNLOAD_ONLY YES)

# Nothing here is installed, and dependencies' install rules expect what they use to be
# installed too.
set(CMAKE_SKIP_INSTALL_RULES ON)

# The connection: GameNetworkingSockets (BSD-3), which needs protobuf (BSD-3) and Abseil
# (Apache-2.0). All three are built here, so every toolchain and standard library links them.
CPMAddPackage(
    NAME absl
    URL https://github.com/abseil/abseil-cpp/archive/refs/tags/20250512.1.tar.gz
    SYSTEM YES
    OPTIONS "ABSL_PROPAGATE_CXX_STD ON" "ABSL_BUILD_TESTING OFF" "ABSL_ENABLE_INSTALL OFF")
CPMAddPackage(
    NAME protobuf
    URL https://github.com/protocolbuffers/protobuf/archive/refs/tags/v36.2.tar.gz
    SYSTEM YES
    OPTIONS "protobuf_INSTALL OFF" "protobuf_BUILD_TESTS OFF" "protobuf_BUILD_LIBUPB OFF"
            "protobuf_BUILD_SHARED_LIBS OFF" "protobuf_MSVC_STATIC_RUNTIME OFF"
            "protobuf_WITH_ZLIB OFF")
# GameNetworkingSockets looks for protobuf as an installed package; this one answers for ours.
configure_file(cmake/protobuf-config.cmake.in
    "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/protobuf-config.cmake" @ONLY)
if(WIN32)
    set(hog_gns_crypto BCrypt)
else()
    set(hog_gns_crypto OpenSSL)
endif()
CPMAddPackage(
    NAME GameNetworkingSockets
    URL https://github.com/ValveSoftware/GameNetworkingSockets/archive/refs/tags/v1.6.1.tar.gz
    SYSTEM YES
    OPTIONS "BUILD_STATIC_LIB ON" "BUILD_SHARED_LIB OFF" "ENABLE_ICE OFF"
            "USE_CRYPTO ${hog_gns_crypto}")

# Ed25519 for connect tokens; BSD-2-Clause or CC0. Two C files, built by hog_login.
CPMAddPackage(
    NAME Monocypher
    URL https://github.com/LoupVaillant/Monocypher/archive/refs/tags/4.0.3.tar.gz
    DOWNLOAD_ONLY YES)

if(HOG_BUILD_TESTS)
    enable_testing()
    CPMAddPackage(
        NAME Catch2
        GITHUB_REPOSITORY catchorg/Catch2
        VERSION 3.16.1
        SYSTEM YES)
    list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
    include(Catch)
endif()

# Registers a test executable for a HOG library: hog_add_tests(<lib> <sources>...)
function(hog_add_tests lib)
    if(NOT HOG_BUILD_TESTS)
        return()
    endif()
    add_executable(${lib}_tests ${ARGN})
    target_link_libraries(${lib}_tests PRIVATE ${lib} hog::testing Catch2::Catch2WithMain)
    hog_target_defaults(${lib}_tests)
    catch_discover_tests(${lib}_tests DISCOVERY_MODE PRE_TEST)
endfunction()

# Benchmarks are separate executables so ctest (and sanitizer runs) never pay for them.
# Run them all with: cmake --build --preset release --target benchmarks
function(hog_add_benchmarks lib)
    if(NOT HOG_BUILD_TESTS)
        return()
    endif()
    add_executable(${lib}_bench ${ARGN})
    target_link_libraries(${lib}_bench PRIVATE ${lib} Catch2::Catch2WithMain)
    hog_target_defaults(${lib}_bench)
    if(NOT TARGET benchmarks)
        add_custom_target(benchmarks)
    endif()
    add_custom_target(run_${lib}_bench COMMAND ${lib}_bench USES_TERMINAL)
    add_dependencies(benchmarks run_${lib}_bench)
endfunction()

add_subdirectory(libs/atmosphere)
add_subdirectory(libs/core)
add_subdirectory(libs/jobs)
add_subdirectory(libs/login)
add_subdirectory(libs/net)
add_subdirectory(libs/physics)
add_subdirectory(libs/planet)
add_subdirectory(libs/probe)
add_subdirectory(libs/protocol)
add_subdirectory(libs/render)
add_subdirectory(libs/replication)
add_subdirectory(libs/save)
add_subdirectory(libs/server)
add_subdirectory(libs/sim)
add_subdirectory(libs/store)
add_subdirectory(libs/terrain)
if(HOG_BUILD_VIEWER OR HOG_BUILD_CLIENT)
    add_subdirectory(libs/window)
endif()
add_subdirectory(tools/mapgen)
add_subdirectory(tools/viewer)
add_subdirectory(apps/login)
add_subdirectory(apps/server)
add_subdirectory(apps/client)
if(HOG_BUILD_TESTS)
    add_subdirectory(libs/testing)
endif()
