add_library(hog_terrain
    src/patch.cpp
    src/selection.cpp
    src/stitching.cpp)
add_library(hog::terrain ALIAS hog_terrain)

target_include_directories(hog_terrain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_terrain PUBLIC hog::core hog::planet)
hog_target_defaults(hog_terrain)

hog_add_tests(hog_terrain
    tests/patch_tests.cpp
    tests/selection_tests.cpp
    tests/stitching_tests.cpp)

hog_add_benchmarks(hog_terrain
    bench/patch_bench.cpp)
