add_library(hog_replication
    src/states.cpp
    src/view.cpp)
add_library(hog::replication ALIAS hog_replication)

target_include_directories(hog_replication PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_replication PUBLIC hog::core hog::protocol)
hog_target_defaults(hog_replication)

hog_add_tests(hog_replication
    tests/states_tests.cpp
    tests/view_tests.cpp)
