add_library(hog_render
    src/camera.cpp
    src/cube_sphere.cpp
    src/orbit.cpp)
add_library(hog::render ALIAS hog_render)

target_include_directories(hog_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_render PUBLIC hog::core PRIVATE hog::planet)
hog_target_defaults(hog_render)

hog_add_tests(hog_render
    tests/camera_tests.cpp
    tests/cube_sphere_tests.cpp
    tests/orbit_tests.cpp)
if(TARGET hog_render_tests)
    target_link_libraries(hog_render_tests PRIVATE hog::planet)
endif()
