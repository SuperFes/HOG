add_library(hog_waves
    src/foam.cpp
    src/shoaling.cpp
    src/waves.cpp)
add_library(hog::waves ALIAS hog_waves)

target_include_directories(hog_waves PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_waves PUBLIC hog::core)
hog_target_defaults(hog_waves)

hog_add_tests(hog_waves
    tests/foam_tests.cpp
    tests/shoaling_tests.cpp
    tests/waves_tests.cpp)
