add_library(hog_flow
    src/region.cpp
    src/slump.cpp
    src/simulation.cpp)
add_library(hog::flow ALIAS hog_flow)

target_include_directories(hog_flow PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_flow PUBLIC hog::core hog::globe)
hog_target_defaults(hog_flow)

hog_add_tests(hog_flow
    tests/region_tests.cpp
    tests/slump_tests.cpp
    tests/simulation_tests.cpp)
