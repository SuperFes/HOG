add_library(hog_sim
    src/calendar.cpp
    src/cell_grid.cpp)
add_library(hog::sim ALIAS hog_sim)

target_include_directories(hog_sim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_sim PUBLIC hog::core hog::planet)
hog_target_defaults(hog_sim)

hog_add_tests(hog_sim
    tests/calendar_tests.cpp
    tests/cell_grid_tests.cpp)
