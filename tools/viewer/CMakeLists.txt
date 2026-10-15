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

# Debug UI; MIT. Built here with the backends for SDL3 and Vulkan, which it takes from volk.
CPMAddPackage(
    NAME imgui
    GITHUB_REPOSITORY ocornut/imgui
    GIT_TAG v1.92.9
    DOWNLOAD_ONLY YES)
add_library(imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
target_include_directories(imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK VK_NO_PROTOTYPES)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static volk::volk)

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
