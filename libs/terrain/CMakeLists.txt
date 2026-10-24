add_library(hog_terrain
    src/baker.cpp
    src/bend.cpp
    src/material.cpp
    src/sea.cpp
    src/water.cpp
    src/patch.cpp
    src/selection.cpp
    src/shore.cpp
    src/stitching.cpp
    src/stream.cpp)
add_library(hog::terrain ALIAS hog_terrain)

target_include_directories(hog_terrain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_terrain PUBLIC hog::core hog::flora hog::jobs hog::planet)
hog_target_defaults(hog_terrain)

hog_add_tests(hog_terrain
    tests/baker_tests.cpp
    tests/bend_tests.cpp
    tests/material_tests.cpp
    tests/sea_tests.cpp
    tests/water_tests.cpp
    tests/patch_tests.cpp
    tests/selection_tests.cpp
    tests/shore_tests.cpp
    tests/stitching_tests.cpp
    tests/stream_tests.cpp)

hog_add_benchmarks(hog_terrain
    bench/patch_bench.cpp
    bench/stream_bench.cpp)
