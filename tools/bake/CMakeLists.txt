add_library(hog_bake src/bake.cpp)
add_library(hog::bake ALIAS hog_bake)

target_include_directories(hog_bake PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_bake PUBLIC hog::baked hog::globe hog::jobs)
hog_target_defaults(hog_bake)

add_executable(hog_bake_cli src/main.cpp)
set_target_properties(hog_bake_cli PROPERTIES OUTPUT_NAME hog-bake)
target_link_libraries(hog_bake_cli PRIVATE hog::bake)
hog_target_defaults(hog_bake_cli)

hog_add_tests(hog_bake tests/bake_tests.cpp)
if(TARGET hog_bake_tests)
    target_link_libraries(hog_bake_tests PRIVATE hog::test_world)
endif()

add_executable(hog_baked_bench src/bench.cpp)
set_target_properties(hog_baked_bench PROPERTIES OUTPUT_NAME hog-baked-bench)
target_link_libraries(hog_baked_bench PRIVATE hog::baked)
hog_target_defaults(hog_baked_bench)
