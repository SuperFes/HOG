add_library(hog_physics
    src/character.cpp
    src/ground.cpp
    src/space.cpp)
add_library(hog::physics ALIAS hog_physics)

target_include_directories(hog_physics PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_physics PUBLIC hog::core hog::planet PRIVATE hog::terrain Jolt)
hog_target_defaults(hog_physics)

hog_add_tests(hog_physics
    tests/character_tests.cpp
    tests/ground_tests.cpp
    tests/space_tests.cpp)
if(TARGET hog_physics_tests)
    # The ground as drawn, to compare with.
    target_link_libraries(hog_physics_tests PRIVATE hog::terrain)
endif()
