# SDL3 refuses to build on Linux without X11 or Wayland, and turns on X11 whenever libX11 is
# there, then wants every extension it supports; so each is used only when complete. Headless
# builds (containers, bare CI images) still compile the apps, though they can't open a window.
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
        message(WARNING "No complete X11 or Wayland headers: apps are built without a window "
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

add_library(hog_window src/window.cpp)
add_library(hog::window ALIAS hog_window)

target_include_directories(hog_window PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_window PUBLIC hog::core hog::render SDL3::SDL3-static)
hog_target_defaults(hog_window)

hog_add_tests(hog_window
    tests/window_tests.cpp)
