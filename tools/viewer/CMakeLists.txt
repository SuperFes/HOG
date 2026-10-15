add_library(hog_viewer
    src/clock_text.cpp
    src/daylight.cpp
    src/frame.cpp
    src/globe_view.cpp
    src/ground.cpp
    src/inspector.cpp
    src/link.cpp
    src/map_view.cpp
    src/options.cpp
    src/overlay.cpp
    src/texels.cpp)
add_library(hog::viewer ALIAS hog_viewer)

target_include_directories(hog_viewer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_viewer PUBLIC hog::mapgen hog::probe hog::render hog::sim hog::jobs)
hog_target_defaults(hog_viewer)

hog_add_tests(hog_viewer
    tests/clock_text_tests.cpp
    tests/daylight_tests.cpp
    tests/frame_tests.cpp
    tests/globe_view_tests.cpp
    tests/ground_tests.cpp
    tests/inspector_tests.cpp
    tests/link_tests.cpp
    tests/map_view_tests.cpp
    tests/options_tests.cpp
    tests/overlay_tests.cpp
    tests/texels_tests.cpp)
if(TARGET hog_viewer_tests)
    # A real probe service to talk to.
    target_link_libraries(hog_viewer_tests PRIVATE hog::server)
endif()

if(NOT HOG_BUILD_VIEWER)
    return()
endif()

add_executable(hog_viewer_cli src/main.cpp)
set_target_properties(hog_viewer_cli PROPERTIES OUTPUT_NAME hog-viewer)
target_link_libraries(hog_viewer_cli PRIVATE hog::viewer hog::render hog::window imgui)
hog_target_defaults(hog_viewer_cli)

# GPU drivers are opaque to sanitizers, so those runs leave the smoke test out.
if(HOG_BUILD_TESTS AND NOT HOG_SANITIZE)
    # Runs the app for a moment without a window or a server, on a software device with
    # validation, saving what it drew. Skipped where there's no Vulkan.
    add_test(NAME hog_viewer_smoke
        COMMAND hog_viewer_cli --port 1 --quit-after 1 --software --validate
                --capture "${CMAKE_CURRENT_BINARY_DIR}/smoke.png")
    add_test(NAME hog_viewer_smoke_globe
        COMMAND hog_viewer_cli --port 1 --quit-after 1 --software --validate --globe
                --capture "${CMAKE_CURRENT_BINARY_DIR}/smoke_globe.png")
    set_tests_properties(hog_viewer_smoke hog_viewer_smoke_globe PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen"
        SKIP_RETURN_CODE 77
        TIMEOUT 60)
endif()
