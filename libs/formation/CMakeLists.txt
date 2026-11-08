add_library(hog_formation
    src/fields.cpp
    src/plates.cpp
    src/sphere.cpp)
add_library(hog::formation ALIAS hog_formation)

target_include_directories(hog_formation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_formation PUBLIC hog::core)
hog_target_defaults(hog_formation)

hog_add_tests(hog_formation
    tests/fields_tests.cpp
    tests/plates_tests.cpp
    tests/sphere_tests.cpp)
