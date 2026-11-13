add_library(hog_flow
    src/creep.cpp
    src/glacier.cpp
    src/region.cpp
    src/region_creep.cpp
    src/region_glacier.cpp
    src/region_lava.cpp
    src/region_rocks.cpp
    src/region_slump.cpp
    src/region_soil.cpp
    src/region_support.cpp
    src/runoff.cpp
    src/region_runoff.cpp
    src/slump.cpp
    src/support.cpp
    src/lava.cpp
    src/vent.cpp
    src/wash.cpp
    src/wear.cpp
    src/simulation.cpp)
add_library(hog::flow ALIAS hog_flow)

target_include_directories(hog_flow PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_flow PUBLIC hog::core hog::globe)
hog_target_defaults(hog_flow)

hog_add_tests(hog_flow
    tests/collapse_tests.cpp
    tests/flight_tests.cpp
    tests/creep_probe_tests.cpp
    tests/creep_tests.cpp
    tests/glacier_tests.cpp
    tests/lava_tests.cpp
    tests/network_cost_bench.cpp
    tests/region_tests.cpp
    tests/rock_tests.cpp
    tests/runoff_tests.cpp
    tests/runoff_region_tests.cpp
    tests/slump_tests.cpp
    tests/soil_region_tests.cpp
    tests/support_tests.cpp
    tests/vent_tests.cpp
    tests/wash_tests.cpp
    tests/wear_tests.cpp
    tests/wetting_tests.cpp
    tests/simulation_tests.cpp)
# Probes read their options from the environment before any thread starts.
if(TARGET hog_flow_tests)
    target_link_libraries(hog_flow_tests PRIVATE hog::test_world)
endif()
if(MSVC AND TARGET hog_flow_tests)
    target_compile_definitions(hog_flow_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
