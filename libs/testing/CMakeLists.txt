# Helpers shared by every library's unit tests; never linked into shipping targets.
add_library(hog_testing
    src/assertion_guard.cpp
    src/ports.cpp
    src/quick.cpp)
add_library(hog::testing ALIAS hog_testing)

target_include_directories(hog_testing PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_testing PUBLIC hog::core)
hog_target_defaults(hog_testing)

# The small planet tests stand on, apart from the one the game is played on.
add_library(hog_test_world src/world.cpp)
add_library(hog::test_world ALIAS hog_test_world)
target_include_directories(hog_test_world PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_test_world PUBLIC hog::globe)
hog_target_defaults(hog_test_world)

hog_add_tests(hog_testing
    tests/ports_tests.cpp
    tests/quick_tests.cpp
    tests/world_tests.cpp)
if(TARGET hog_testing_tests)
    target_link_libraries(hog_testing_tests PRIVATE hog::test_world)
endif()
