configure_file(version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/include/hog/core/version.hpp" @ONLY)

# The PNG encoder's implementation, compiled once in C without HOG's warnings; public domain.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/stb_image_write.c"
    "#define STB_IMAGE_WRITE_IMPLEMENTATION\n#include <stb_image_write.h>\n")
add_library(stb_image_write STATIC "${CMAKE_CURRENT_BINARY_DIR}/stb_image_write.c")
target_include_directories(stb_image_write SYSTEM PUBLIC "${stb_SOURCE_DIR}")
# Writes through callbacks only, so stdio's file functions are never needed.
target_compile_definitions(stb_image_write PUBLIC STBI_WRITE_NO_STDIO)

add_library(hog_core
    src/assert.cpp
    src/build_info.cpp
    src/bytes.cpp
    src/hash.cpp
    src/image.cpp
    src/line_sink.cpp
    src/log.cpp
    src/metrics.cpp
    src/stage_timer.cpp
    src/text_file.cpp)
add_library(hog::core ALIAS hog_core)

target_include_directories(hog_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/include")
target_compile_definitions(hog_core PRIVATE HOG_BUILD_CONFIG="$<IF:$<CONFIG:>,unspecified,$<CONFIG>>")
target_include_directories(hog_core SYSTEM PRIVATE "${xxHash_SOURCE_DIR}")
target_link_libraries(hog_core PRIVATE stb_image_write)
target_include_directories(hog_core SYSTEM PUBLIC "${glm_SOURCE_DIR}")
target_compile_definitions(hog_core PUBLIC
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_EXPLICIT_CTOR
    GLM_FORCE_SIZE_T_LENGTH)
hog_target_defaults(hog_core)
if(HOG_PROFILE)
    target_link_libraries(hog_core PUBLIC Tracy::TracyClient)
    target_compile_definitions(hog_core PUBLIC HOG_PROFILE)
endif()

hog_add_tests(hog_core
    tests/assert_tests.cpp
    tests/build_info_tests.cpp
    tests/flat_map_tests.cpp
    tests/bytes_tests.cpp
    tests/hash_tests.cpp
    tests/line_sink_tests.cpp
    tests/log_tests.cpp
    tests/math_tests.cpp
    tests/metrics_tests.cpp
    tests/profile_tests.cpp
    tests/result_tests.cpp
    tests/slot_map_tests.cpp
    tests/stage_timer_tests.cpp
    tests/text_file_tests.cpp
    tests/time_tests.cpp)

hog_add_benchmarks(hog_core
    bench/slot_map_bench.cpp)
