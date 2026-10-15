add_library(hog_atmosphere
    src/model.cpp)
add_library(hog::atmosphere ALIAS hog_atmosphere)

target_include_directories(hog_atmosphere PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_atmosphere PUBLIC hog::core)
hog_target_defaults(hog_atmosphere)

hog_add_tests(hog_atmosphere
    tests/model_tests.cpp)
