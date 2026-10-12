add_library(hog_viewer
    src/frame.cpp
    src/overlay.cpp)
add_library(hog::viewer ALIAS hog_viewer)

target_include_directories(hog_viewer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_viewer PUBLIC hog::mapgen hog::probe hog::sim hog::jobs)
hog_target_defaults(hog_viewer)

hog_add_tests(hog_viewer
    tests/frame_tests.cpp
    tests/overlay_tests.cpp)
