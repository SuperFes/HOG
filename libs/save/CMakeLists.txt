add_library(hog_save
    src/snapshot.cpp)
add_library(hog::save ALIAS hog_save)

target_include_directories(hog_save PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_save PUBLIC hog::core)
hog_target_defaults(hog_save)

hog_add_tests(hog_save
    tests/snapshot_tests.cpp)
