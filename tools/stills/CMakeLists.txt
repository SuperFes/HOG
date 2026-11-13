add_library(hog_stills src/stills.cpp)
add_library(hog::stills ALIAS hog_stills)

target_include_directories(hog_stills PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# stb_image_write comes from libs/core, added first.
target_link_libraries(hog_stills PUBLIC hog::globe PRIVATE stb_image_write)
hog_target_defaults(hog_stills)

add_executable(hog_stills_cli src/main.cpp)
set_target_properties(hog_stills_cli PROPERTIES OUTPUT_NAME hog-stills)
target_link_libraries(hog_stills_cli PRIVATE hog::stills)
hog_target_defaults(hog_stills_cli)

hog_add_tests(hog_stills tests/stills_tests.cpp)
if(TARGET hog_stills_tests)
    target_link_libraries(hog_stills_tests PRIVATE hog::test_world)
endif()
