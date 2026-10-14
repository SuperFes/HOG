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

# SDL3 refuses to build on Linux without X11 or Wayland, and turns on X11 whenever libX11 is
# there, then wants every extension it supports; so each is used only when complete. Headless
# builds (containers, bare CI images) still compile the viewer, though it can't open a window.
set(hog_sdl_console OFF)
set(hog_x11 OFF)
set(hog_wayland OFF)
if(UNIX AND NOT APPLE)
    find_package(X11 QUIET)
    if(X11_FOUND AND X11_Xcursor_FOUND AND X11_Xext_FOUND AND X11_Xdbe_FOUND AND X11_Xshape_FOUND
       AND X11_Xi_FOUND AND X11_Xfixes_FOUND AND X11_Xrandr_FOUND AND X11_Xss_FOUND
       AND X11_XTest_FOUND)
        set(hog_x11 ON)
    endif()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(hog_wayland_pkgs QUIET wayland-client wayland-cursor wayland-egl egl
                          xkbcommon)
        if(hog_wayland_pkgs_FOUND)
            set(hog_wayland ON)
        endif()
    endif()
    if(NOT hog_x11 AND NOT hog_wayland)
        message(WARNING "No complete X11 or Wayland headers: the viewer is built without a window "
                        "system")
        set(hog_sdl_console ON)
    endif()
endif()

# Windows, input and 2D drawing; zlib. Without D-Bus, which only adds desktop niceties, such as
# input methods, that a debug tool can do without.
CPMAddPackage(
    NAME SDL3
    GITHUB_REPOSITORY libsdl-org/SDL
    GIT_TAG release-3.4.18
    SYSTEM YES
    OPTIONS
        "SDL_SHARED OFF" "SDL_STATIC ON" "SDL_TEST_LIBRARY OFF" "SDL_TESTS OFF" "SDL_EXAMPLES OFF"
        "SDL_INSTALL OFF" "SDL_AUDIO OFF" "SDL_CAMERA OFF" "SDL_DBUS OFF" "SDL_IBUS OFF"
        "SDL_UNIX_CONSOLE_BUILD ${hog_sdl_console}" "SDL_X11 ${hog_x11}" "SDL_WAYLAND ${hog_wayland}")

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
target_link_libraries(hog_viewer_cli PRIVATE hog::viewer hog::render imgui)
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
