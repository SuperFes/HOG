cmake_minimum_required(VERSION 3.28)

project(HOG
    VERSION 0.0.1
    DESCRIPTION "A seamless-planet MMORPG and engine"
    HOMEPAGE_URL "https://github.com/SuperFes/HOG"
    LANGUAGES CXX)

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

add_subdirectory(libs/core)
if(HOG_BUILD_TESTS)
    add_subdirectory(libs/testing)
endif()
