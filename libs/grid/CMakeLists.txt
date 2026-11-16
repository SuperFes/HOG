add_library(hog_grid
    src/grid.cpp
    src/page.cpp
    src/tree.cpp)
add_library(hog::grid ALIAS hog_grid)

target_include_directories(hog_grid PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_grid PUBLIC hog::core)
hog_target_defaults(hog_grid)

hog_add_tests(hog_grid
    tests/grid_tests.cpp
    tests/key_tests.cpp
    tests/page_tests.cpp
    tests/tree_tests.cpp)
