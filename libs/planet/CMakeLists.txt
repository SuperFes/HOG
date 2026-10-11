add_library(hog_planet
    src/cell_id.cpp
    src/neighbors.cpp
    src/planet_config.cpp
    src/projection.cpp
    src/tangent_frame.cpp
    src/tile.cpp)
add_library(hog::planet ALIAS hog_planet)

target_include_directories(hog_planet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_planet PUBLIC hog::core PRIVATE tomlplusplus)
hog_target_defaults(hog_planet)

hog_add_tests(hog_planet
    tests/cell_id_tests.cpp
    tests/neighbors_tests.cpp
    tests/noise_tests.cpp
    tests/planet_config_tests.cpp
    tests/projection_tests.cpp
    tests/tangent_frame_tests.cpp
    tests/tile_tests.cpp)
hog_add_benchmarks(hog_planet
    bench/noise_bench.cpp)

if(TARGET hog_planet_tests)
    target_compile_definitions(hog_planet_tests PRIVATE HOG_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endif()
