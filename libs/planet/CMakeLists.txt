add_library(hog_planet
    src/archipelago.cpp
    src/bake.cpp
    src/beds.cpp
    src/caves.cpp
    src/cell_id.cpp
    src/change.cpp
    src/climate.cpp
    src/continents.cpp
    src/drainage.cpp
    src/elevation.cpp
    src/erosion.cpp
    src/geology.cpp
    src/hillslope.cpp
    src/hotspots.cpp
    src/landform.cpp
    src/level_grid.cpp
    src/moisture.cpp
    src/neighbors.cpp
    src/planet_config.cpp
    src/places.cpp
    src/plates.cpp
    src/projection.cpp
    src/rivers.cpp
    src/tangent_frame.cpp
    src/tile.cpp
    src/volcanoes.cpp
    src/world.cpp)
add_library(hog::planet ALIAS hog_planet)

target_include_directories(hog_planet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Bakes are keyed by the code that made them, so changing it bakes again.
file(GLOB_RECURSE hog_planet_baking CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
    "${PROJECT_SOURCE_DIR}/libs/core/src/*.cpp" "${PROJECT_SOURCE_DIR}/libs/core/include/*.hpp")
string(REPLACE ";" "|" hog_planet_baking_list "${hog_planet_baking}")
set(hog_planet_source_hash "${CMAKE_CURRENT_BINARY_DIR}/generated/hog/planet/source_hash.hpp")
add_custom_command(OUTPUT "${hog_planet_source_hash}"
    COMMAND "${CMAKE_COMMAND}" "-DFILES=${hog_planet_baking_list}"
        "-DOUTPUT=${hog_planet_source_hash}" -P "${PROJECT_SOURCE_DIR}/cmake/HashSources.cmake"
    DEPENDS ${hog_planet_baking} "${PROJECT_SOURCE_DIR}/cmake/HashSources.cmake"
    COMMENT "Hashing the planet's sources"
    VERBATIM)
target_sources(hog_planet PRIVATE "${hog_planet_source_hash}")
target_include_directories(hog_planet PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(hog_planet PUBLIC hog::core PRIVATE tomlplusplus)
hog_target_defaults(hog_planet)

hog_add_tests(hog_planet
    tests/archipelago_tests.cpp
    tests/bake_tests.cpp
    tests/beds_tests.cpp
    tests/caves_tests.cpp
    tests/cell_id_tests.cpp
    tests/change_tests.cpp
    tests/climate_tests.cpp
    tests/continents_tests.cpp
    tests/drainage_tests.cpp
    tests/elevation_tests.cpp
    tests/erosion_tests.cpp
    tests/geology_tests.cpp
    tests/hillslope_tests.cpp
    tests/hotspots_tests.cpp
    tests/golden_tests.cpp
    tests/landform_tests.cpp
    tests/level_grid_tests.cpp
    tests/neighbors_tests.cpp
    tests/noise_tests.cpp
    tests/planet_config_tests.cpp
    tests/places_tests.cpp
    tests/plates_tests.cpp
    tests/projection_tests.cpp
    tests/rivers_tests.cpp
    tests/tangent_frame_tests.cpp
    tests/tile_tests.cpp
    tests/volcanoes_tests.cpp
    tests/world_tests.cpp)
hog_add_benchmarks(hog_planet
    bench/noise_bench.cpp)

if(TARGET hog_planet_tests)
    target_compile_definitions(hog_planet_tests PRIVATE HOG_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endif()

# Plates grow and landforms evolve or load whenever a planet is built; unoptimized it makes every
# test that builds one take minutes.
if(NOT MSVC)
    target_compile_options(hog_planet PRIVATE "$<$<CONFIG:Debug>:-O2>")
endif()
