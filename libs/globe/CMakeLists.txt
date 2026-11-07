add_library(hog_globe
    src/brick.cpp
    src/caves.cpp
    src/cell.cpp
    src/craters.cpp
    src/crust.cpp
    src/drainage.cpp
    src/erosion.cpp
    src/generator.cpp
    src/grip.cpp
    src/layers.cpp
    src/material.cpp
    src/weathering.cpp
    src/noise.cpp
    src/planet.cpp
    src/planet_file.cpp
    src/pockets.cpp
    src/qef.cpp
    src/relief.cpp
    src/rockfall.cpp
    src/strata.cpp
    src/surface.cpp
    src/vents.cpp
    src/volcanoes.cpp
    src/column_cache.cpp
    src/cube.cpp
    src/voxel.cpp
    src/voxel_tree.cpp
    src/volume.cpp
    src/volume_build.cpp
    src/volume_archive.cpp
    src/voxel_mesh.cpp
    src/world.cpp)
add_library(hog::globe ALIAS hog_globe)

target_include_directories(hog_globe PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_globe PUBLIC hog::core PRIVATE tomlplusplus)
hog_target_defaults(hog_globe)

hog_add_tests(hog_globe
    tests/brick_tests.cpp
    tests/caves_tests.cpp
    tests/cave_bed_tests.cpp
    tests/cell_tests.cpp
    tests/craters_tests.cpp
    tests/drainage_tests.cpp
    tests/generator_tests.cpp
    tests/grip_tests.cpp
    tests/landform_survey_tests.cpp
    tests/lake_bed_tests.cpp
    tests/layers_tests.cpp
    tests/material_tests.cpp
    tests/maths_tests.cpp
    tests/noise_tests.cpp
    tests/planet_tests.cpp
    tests/pockets_tests.cpp
    tests/qef_tests.cpp
    tests/rockfall_tests.cpp
    tests/strata_tests.cpp
    tests/vents_tests.cpp
    tests/volcanoes_tests.cpp
    tests/octree_estimate_tests.cpp
    tests/temperature_tests.cpp
    tests/column_cache_tests.cpp
    tests/cube_tests.cpp
    tests/voxel_tests.cpp
    tests/voxel_tree_tests.cpp
    tests/volume_tests.cpp
    tests/voxel_mesh_tests.cpp
    tests/weathering_tests.cpp
    tests/world_tests.cpp)

if(TARGET hog_globe_tests)
    target_compile_definitions(hog_globe_tests PRIVATE HOG_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endif()
