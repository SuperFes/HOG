add_library(hog_viewer
    src/frame.cpp
    src/link.cpp
    src/map_view.cpp
    src/options.cpp
    src/overlay.cpp)
add_library(hog::viewer ALIAS hog_viewer)

target_include_directories(hog_viewer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_viewer PUBLIC hog::mapgen hog::probe hog::sim hog::jobs)
hog_target_defaults(hog_viewer)

hog_add_tests(hog_viewer
    tests/frame_tests.cpp
    tests/link_tests.cpp
    tests/map_view_tests.cpp
    tests/options_tests.cpp
    tests/overlay_tests.cpp)
if(TARGET hog_viewer_tests)
    # A real probe service to talk to.
    target_link_libraries(hog_viewer_tests PRIVATE hog::server)
endif()
