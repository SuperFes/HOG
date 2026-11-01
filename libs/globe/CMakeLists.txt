add_library(hog_globe
    src/brick.cpp
    src/brick_archive.cpp
    src/caves.cpp
    src/cell.cpp
    src/craters.cpp
    src/crust.cpp
    src/erosion.cpp
    src/generator.cpp
    src/layers.cpp
    src/material.cpp
    src/noise.cpp
    src/planet.cpp
    src/planet_file.cpp
    src/relief.cpp
    src/strata.cpp
    src/surface_mesh.cpp
    src/voxel.cpp
    src/voxel_store.cpp
    src/world.cpp)
add_library(hog::globe ALIAS hog_globe)

target_include_directories(hog_globe PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_globe PUBLIC hog::core PRIVATE tomlplusplus)
hog_target_defaults(hog_globe)

hog_add_tests(hog_globe
    tests/brick_archive_tests.cpp
    tests/brick_tests.cpp
    tests/caves_tests.cpp
    tests/cell_tests.cpp
    tests/craters_tests.cpp
    tests/generator_tests.cpp
    tests/layers_tests.cpp
    tests/maths_tests.cpp
    tests/noise_tests.cpp
    tests/planet_tests.cpp
    tests/strata_tests.cpp
    tests/surface_mesh_tests.cpp
    tests/voxel_store_tests.cpp
    tests/voxel_tests.cpp
    tests/world_tests.cpp)
