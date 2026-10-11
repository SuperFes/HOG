configure_file(version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/include/hog/core/version.hpp" @ONLY)

add_library(hog_core
    src/assert.cpp
    src/build_info.cpp
    src/hash.cpp
    src/log.cpp)
add_library(hog::core ALIAS hog_core)

target_include_directories(hog_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/include")
target_compile_definitions(hog_core PRIVATE HOG_BUILD_CONFIG="$<IF:$<CONFIG:>,unspecified,$<CONFIG>>")
target_include_directories(hog_core SYSTEM PRIVATE "${xxHash_SOURCE_DIR}")
hog_target_defaults(hog_core)

hog_add_tests(hog_core
    tests/assert_tests.cpp
    tests/build_info_tests.cpp
    tests/hash_tests.cpp
    tests/log_tests.cpp
    tests/result_tests.cpp
    tests/slot_map_tests.cpp)
