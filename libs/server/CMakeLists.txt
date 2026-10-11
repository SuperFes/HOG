add_library(hog_server
    src/pacer.cpp
    src/tick_clock.cpp)
add_library(hog::server ALIAS hog_server)

target_include_directories(hog_server PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_server PUBLIC hog::core)
hog_target_defaults(hog_server)

hog_add_tests(hog_server
    tests/pacer_tests.cpp
    tests/tick_clock_tests.cpp
    tests/tick_loop_tests.cpp)
