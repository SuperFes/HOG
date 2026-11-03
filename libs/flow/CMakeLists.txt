add_library(hog_flow
    src/region.cpp
    src/region_rocks.cpp
    src/region_slump.cpp
    src/region_support.cpp
    src/slump.cpp
    src/support.cpp
    src/wear.cpp
    src/simulation.cpp)
add_library(hog::flow ALIAS hog_flow)

target_include_directories(hog_flow PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_flow PUBLIC hog::core hog::globe)
hog_target_defaults(hog_flow)

hog_add_tests(hog_flow
    tests/collapse_tests.cpp
    tests/flight_tests.cpp
    tests/region_tests.cpp
    tests/rock_tests.cpp
    tests/slump_tests.cpp
    tests/support_tests.cpp
    tests/wear_tests.cpp
    tests/wetting_tests.cpp
    tests/simulation_tests.cpp)
