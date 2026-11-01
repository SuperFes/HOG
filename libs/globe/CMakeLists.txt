add_library(hog_globe
    src/accretion.cpp
    src/birth.cpp
    src/cell.cpp
    src/differentiation.cpp
    src/globe.cpp
    src/hex_cell.cpp
    src/hex_grid.cpp
    src/hexel_mesh.cpp
    src/hexel_store.cpp
    src/layers.cpp
    src/material.cpp
    src/planet.cpp
    src/planet_file.cpp
    src/settling.cpp)
add_library(hog::globe ALIAS hog_globe)

target_include_directories(hog_globe PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_globe PUBLIC hog::core PRIVATE tomlplusplus)
hog_target_defaults(hog_globe)

hog_add_tests(hog_globe
    tests/accretion_tests.cpp
    tests/cell_tests.cpp
    tests/differentiation_tests.cpp
    tests/globe_tests.cpp
    tests/hex_cell_tests.cpp
    tests/hexel_mesh_tests.cpp
    tests/hexel_store_tests.cpp
    tests/layers_tests.cpp
    tests/maths_tests.cpp
    tests/planet_tests.cpp
    tests/settling_tests.cpp)
