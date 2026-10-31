add_library(hog_globe
    src/hex_cell.cpp)
add_library(hog::globe ALIAS hog_globe)

target_include_directories(hog_globe PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_globe PUBLIC hog::core)
hog_target_defaults(hog_globe)

hog_add_tests(hog_globe
    tests/hex_cell_tests.cpp)
