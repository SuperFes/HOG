add_library(hog_planet
    src/archipelago.cpp
    src/cell_id.cpp
    src/change.cpp
    src/climate.cpp
    src/continents.cpp
    src/drainage.cpp
    src/elevation.cpp
    src/erosion.cpp
    src/geology.cpp
    src/level_grid.cpp
    src/moisture.cpp
    src/neighbors.cpp
    src/planet_config.cpp
    src/projection.cpp
    src/tangent_frame.cpp
    src/tile.cpp
    src/world.cpp)
add_library(hog::planet ALIAS hog_planet)

target_include_directories(hog_planet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_planet PUBLIC hog::core PRIVATE tomlplusplus)
hog_target_defaults(hog_planet)

hog_add_tests(hog_planet
    tests/archipelago_tests.cpp
    tests/cell_id_tests.cpp
    tests/change_tests.cpp
    tests/climate_tests.cpp
    tests/continents_tests.cpp
    tests/drainage_tests.cpp
    tests/elevation_tests.cpp
    tests/erosion_tests.cpp
    tests/geology_tests.cpp
    tests/golden_tests.cpp
    tests/level_grid_tests.cpp
    tests/neighbors_tests.cpp
    tests/noise_tests.cpp
    tests/planet_config_tests.cpp
    tests/projection_tests.cpp
    tests/tangent_frame_tests.cpp
    tests/tile_tests.cpp
    tests/world_tests.cpp)
hog_add_benchmarks(hog_planet
    bench/noise_bench.cpp)

if(TARGET hog_planet_tests)
    target_compile_definitions(hog_planet_tests PRIVATE HOG_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endif()
