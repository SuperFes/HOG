# The implementation, compiled once in C without HOG's warnings; pictures are PNGs.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/stb_image.c"
    "#define STB_IMAGE_IMPLEMENTATION\n#include <stb_image.h>\n")
add_library(stb_image STATIC "${CMAKE_CURRENT_BINARY_DIR}/stb_image.c")
target_include_directories(stb_image SYSTEM PUBLIC "${stb_SOURCE_DIR}")
target_compile_definitions(stb_image PUBLIC STBI_ONLY_PNG)

add_library(hog_client
    src/daylight.cpp
    src/flight.cpp
    src/frame_times.cpp
    src/ground.cpp
    src/likeness.cpp
    src/options.cpp
    src/others.cpp
    src/picture.cpp
    src/scene.cpp
    src/session.cpp
    src/views.cpp)
add_library(hog::client ALIAS hog_client)

target_include_directories(hog_client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_client
    PUBLIC hog::atmosphere hog::core hog::jobs hog::net hog::planet hog::protocol hog::render
        hog::replication hog::sim hog::terrain
    PRIVATE hog::mapgen hog::physics stb_image)
hog_target_defaults(hog_client)

hog_add_tests(hog_client
    tests/daylight_tests.cpp
    tests/flight_tests.cpp
    tests/frame_times_tests.cpp
    tests/ground_tests.cpp
    tests/likeness_tests.cpp
    tests/options_tests.cpp
    tests/others_tests.cpp
    tests/picture_tests.cpp
    tests/scene_tests.cpp
    tests/session_tests.cpp
    tests/views_tests.cpp)
if(TARGET hog_client_tests)
    # The server, for players to meet through.
    target_link_libraries(hog_client_tests PRIVATE hog::app hog::planet hog::render_testing)
    target_compile_definitions(hog_client_tests PRIVATE HOG_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
                                                        HOG_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
endif()

if(NOT HOG_BUILD_CLIENT)
    return()
endif()

add_executable(hog_client_cli src/main.cpp)
set_target_properties(hog_client_cli PROPERTIES OUTPUT_NAME hog-client)
target_link_libraries(hog_client_cli PRIVATE hog::client hog::jobs hog::mapgen hog::login hog::net hog::planet hog::protocol
    hog::window)
hog_target_defaults(hog_client_cli)

# GPU drivers are opaque to sanitizers, so those runs leave the smoke test out.
if(HOG_BUILD_TESTS AND NOT HOG_SANITIZE)
    # Flies for a moment without a window, on a software device with validation and the timings
    # shown, saving what it drew. Skipped where there's no Vulkan.
    add_test(NAME hog_client_smoke
        COMMAND hog_client_cli --planet "${PROJECT_SOURCE_DIR}/config/planet.toml"
                --companion "${PROJECT_SOURCE_DIR}/res/Matt.png"
                --quit-after 1 --threads 2 --timings --software --validate
                --capture "${CMAKE_CURRENT_BINARY_DIR}/smoke.png")
    set_tests_properties(hog_client_smoke PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen"
        SKIP_RETURN_CODE 77
        TIMEOUT 120)
endif()
