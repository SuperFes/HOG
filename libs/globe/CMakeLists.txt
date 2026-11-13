add_library(hog_globe
    src/brick.cpp
    src/caves.cpp
    src/cell.cpp
    src/dissection.cpp
    src/joints.cpp
    src/drainage.cpp
    src/formed.cpp
    src/lake_fill.cpp
    src/valleys.cpp
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
    src/rockfall.cpp
    src/shoreface.cpp
    src/strata.cpp
    src/surface.cpp
    src/terms.cpp
    src/vents.cpp
    src/cube.cpp
    src/voxel.cpp
    src/voxel_tree.cpp
    src/volume.cpp
    src/land.cpp
    src/held_tiles.cpp
    src/volume_build.cpp
    src/volume_archive.cpp
    src/voxel_mesh.cpp
    src/world.cpp)
add_library(hog::globe ALIAS hog_globe)

target_include_directories(hog_globe PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_globe PUBLIC hog::core hog::formation hog::planet PRIVATE tomlplusplus)
hog_target_defaults(hog_globe)

hog_add_tests(hog_globe
    tests/brick_tests.cpp
    tests/caves_tests.cpp
    tests/courses_tests.cpp
    tests/cell_tests.cpp
    tests/dissection_tests.cpp
    tests/joints_tests.cpp
    tests/drainage_tests.cpp
    tests/formed_tests.cpp
    tests/lake_fill_tests.cpp
    tests/valleys_tests.cpp
    tests/generator_tests.cpp
    tests/grip_tests.cpp
    tests/held_tiles_tests.cpp
    tests/land_tests.cpp
    tests/layers_tests.cpp
    tests/material_tests.cpp
    tests/maths_tests.cpp
    tests/noise_tests.cpp
    tests/planet_tests.cpp
    tests/pockets_tests.cpp
    tests/qef_tests.cpp
    tests/rockfall_tests.cpp
    tests/shoreface_tests.cpp
    tests/strata_tests.cpp
    tests/vents_tests.cpp
    tests/octree_estimate_tests.cpp
    tests/temperature_tests.cpp
    tests/cube_tests.cpp
    tests/voxel_tests.cpp
    tests/voxel_tree_tests.cpp
    tests/volume_tests.cpp
    tests/voxel_mesh_tests.cpp
    tests/weathering_tests.cpp
    tests/world_tests.cpp)

if(TARGET hog_globe_tests)
    target_compile_definitions(hog_globe_tests PRIVATE HOG_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(hog_globe_tests PRIVATE hog::test_world)
endif()
