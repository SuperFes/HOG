add_library(hog_build_world src/build_world.cpp)
add_library(hog::build_world ALIAS hog_build_world)

target_include_directories(hog_build_world PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_build_world PUBLIC hog::globe hog::jobs)
hog_target_defaults(hog_build_world)

add_executable(hog_build_world_cli src/main.cpp)
set_target_properties(hog_build_world_cli PROPERTIES OUTPUT_NAME hog-build-world)
target_link_libraries(hog_build_world_cli PRIVATE hog::build_world hog::store)
hog_target_defaults(hog_build_world_cli)

hog_add_tests(hog_build_world tests/build_world_tests.cpp)
if(TARGET hog_build_world_tests)
    target_link_libraries(hog_build_world_tests PRIVATE hog::test_world)
endif()
