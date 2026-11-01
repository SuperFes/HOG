add_library(hog_terrain
    src/bend.cpp
    src/block_mesh.cpp
    src/material.cpp
    src/sea.cpp
    src/shore.cpp
    src/water.cpp)
add_library(hog::terrain ALIAS hog_terrain)

target_include_directories(hog_terrain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_terrain PUBLIC hog::core hog::planet)
hog_target_defaults(hog_terrain)

hog_add_tests(hog_terrain
    tests/bend_tests.cpp
    tests/block_mesh_tests.cpp
    tests/material_tests.cpp
    tests/sea_tests.cpp
    tests/shore_tests.cpp
    tests/water_tests.cpp)
