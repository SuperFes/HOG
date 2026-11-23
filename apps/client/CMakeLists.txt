# The implementation, compiled once in C without HOG's warnings; pictures are PNGs.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/stb_image.c"
    "#define STB_IMAGE_IMPLEMENTATION\n#include <stb_image.h>\n")
add_library(stb_image STATIC "${CMAKE_CURRENT_BINARY_DIR}/stb_image.c")
target_include_directories(stb_image SYSTEM PUBLIC "${stb_SOURCE_DIR}")
target_compile_definitions(stb_image PUBLIC STBI_ONLY_PNG)

add_library(hog_client
    src/chat.cpp
    src/chunk_cache.cpp
    src/chunk_intake.cpp
    src/drawn.cpp
    src/tile_meshes.cpp
    src/cutaway.cpp
    src/daylight.cpp
    src/flight.cpp
    src/forecast.cpp
    src/frame_times.cpp
    src/globe_view.cpp
    src/block_meshes.cpp
    src/ground_blocks.cpp
    src/ground_field.cpp
    src/water_field.cpp
    src/held_pieces.cpp
    src/report.cpp
    src/piece_asking.cpp
    src/grains.cpp
    src/likeness.cpp
    src/options.cpp
    src/others.cpp
    src/picture.cpp
    src/player.cpp
    src/scene.cpp
    src/transvoxel.cpp
    src/transvoxel_tables.cpp
    src/sonar.cpp
    src/session.cpp
    src/tides.cpp
    src/tree_stands.cpp)
add_library(hog::client ALIAS hog_client)

target_include_directories(hog_client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_client
    PUBLIC hog::atmosphere hog::core hog::flora hog::globe hog::jobs hog::net hog::physics hog::planet
        hog::protocol hog::render hog::replication hog::sim hog::terrain
    PRIVATE lmdbxx stb_image tomlplusplus)
hog_target_defaults(hog_client)

hog_add_tests(hog_client
    tests/chat_tests.cpp
    tests/chunk_cache_tests.cpp
    tests/chunk_intake_tests.cpp
    tests/drawn_tests.cpp
    tests/tile_meshes_tests.cpp
    tests/tile_look_tests.cpp
    tests/lod_look_tests.cpp
    tests/daylight_tests.cpp
    tests/cutaway_tests.cpp
    tests/flight_tests.cpp
    tests/forecast_tests.cpp
    tests/frame_times_tests.cpp
    tests/glacier_lab_tests.cpp
    tests/globe_view_tests.cpp
    tests/block_meshes_tests.cpp
    tests/ground_blocks_tests.cpp
    tests/ground_field_tests.cpp
    tests/water_field_tests.cpp
    tests/held_pieces_tests.cpp
    tests/piece_asking_tests.cpp
    tests/report_tests.cpp
    tests/grains_tests.cpp
    tests/likeness_tests.cpp
    tests/options_tests.cpp
    tests/others_tests.cpp
    tests/picture_tests.cpp
    tests/player_tests.cpp
    tests/scene_tests.cpp
    tests/transvoxel_tests.cpp
    tests/sonar_tests.cpp
    tests/session_tests.cpp
    tests/streaming_tests.cpp
    tests/tides_tests.cpp
    tests/tree_stands_tests.cpp)
if(TARGET hog_client_tests)
    # The server, for players to meet through.
    target_link_libraries(hog_client_tests PRIVATE hog::flow hog::app hog::planet hog::render_testing
                                                   hog::test_world)
    target_compile_definitions(hog_client_tests PRIVATE HOG_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
                                                        HOG_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
endif()

if(NOT HOG_BUILD_CLIENT)
    return()
endif()

# Opens the window and plays.
add_library(hog_play src/play.cpp)
add_library(hog::play ALIAS hog_play)
target_include_directories(hog_play PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_play PUBLIC hog::chat hog::client hog::jobs hog::login hog::net
    hog::planet hog::protocol hog::window)
hog_target_defaults(hog_play)

# The game client: connects to a server and draws its world.
add_executable(hog_client_cli src/main.cpp)
set_target_properties(hog_client_cli PROPERTIES OUTPUT_NAME hog-client)
target_link_libraries(hog_client_cli PRIVATE hog::play)
hog_target_defaults(hog_client_cli)
