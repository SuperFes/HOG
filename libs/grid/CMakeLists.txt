add_library(hog_grid
    src/edits.cpp
    src/grid.cpp
    src/heat.cpp
    src/page.cpp
    src/tree.cpp
    src/water_bodies.cpp)
add_library(hog::grid ALIAS hog_grid)

target_include_directories(hog_grid PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_grid PUBLIC hog::core)
hog_target_defaults(hog_grid)

hog_add_tests(hog_grid
    tests/edits_tests.cpp
    tests/grid_tests.cpp
    tests/heat_tests.cpp
    tests/key_tests.cpp
    tests/page_tests.cpp
    tests/tree_tests.cpp
    tests/water_bodies_tests.cpp)
