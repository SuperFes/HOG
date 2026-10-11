add_library(hog_planet
    src/cell_id.cpp
    src/neighbors.cpp)
add_library(hog::planet ALIAS hog_planet)

target_include_directories(hog_planet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_planet PUBLIC hog::core)
hog_target_defaults(hog_planet)

hog_add_tests(hog_planet
    tests/cell_id_tests.cpp
    tests/neighbors_tests.cpp)
