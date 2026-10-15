add_library(hog_client
    src/flight.cpp
    src/ground.cpp
    src/options.cpp)
add_library(hog::client ALIAS hog_client)

target_include_directories(hog_client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_client
    PUBLIC hog::core hog::render hog::terrain
    PRIVATE hog::mapgen hog::planet)
hog_target_defaults(hog_client)

hog_add_tests(hog_client
    tests/flight_tests.cpp
    tests/ground_tests.cpp
    tests/options_tests.cpp)
if(TARGET hog_client_tests)
    target_link_libraries(hog_client_tests PRIVATE hog::planet)
endif()

if(NOT HOG_BUILD_CLIENT)
    return()
endif()

add_executable(hog_client_cli src/main.cpp)
set_target_properties(hog_client_cli PROPERTIES OUTPUT_NAME hog-client)
target_link_libraries(hog_client_cli PRIVATE hog::client hog::jobs hog::mapgen hog::planet hog::window)
hog_target_defaults(hog_client_cli)

# GPU drivers are opaque to sanitizers, so those runs leave the smoke test out.
if(HOG_BUILD_TESTS AND NOT HOG_SANITIZE)
    # Flies for a moment without a window, on a software device with validation, saving what it
    # drew. Skipped where there's no Vulkan.
    add_test(NAME hog_client_smoke
        COMMAND hog_client_cli --planet "${PROJECT_SOURCE_DIR}/config/planet.toml"
                --quit-after 1 --threads 2 --software --validate
                --capture "${CMAKE_CURRENT_BINARY_DIR}/smoke.png")
    set_tests_properties(hog_client_smoke PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen"
        SKIP_RETURN_CODE 77
        TIMEOUT 120)
endif()
