add_library(hog_flora src/asset.cpp src/placement.cpp src/season.cpp src/woods.cpp)
add_library(hog::flora ALIAS hog_flora)

target_include_directories(hog_flora PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_flora PUBLIC hog::core)
hog_target_defaults(hog_flora)

hog_add_tests(hog_flora
    tests/asset_tests.cpp
    tests/placement_tests.cpp
    tests/woods_tests.cpp)
