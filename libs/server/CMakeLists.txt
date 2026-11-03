add_library(hog_server
    src/brick_sharing.cpp
    src/grain_sharing.cpp
    src/ground_wetting.cpp
    src/move_check.cpp
    src/pacer.cpp
    src/persistence.cpp
    src/players.cpp
    src/server_config.cpp
    src/snapshot_writer.cpp
    src/spatial_index.cpp
    src/spatial_tracking.cpp
    src/tick_clock.cpp
    src/weather_keeper.cpp)
add_library(hog::server ALIAS hog_server)

target_include_directories(hog_server PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_server
    PUBLIC hog::core hog::flow hog::globe hog::jobs hog::net hog::physics hog::planet hog::protocol
        hog::replication hog::save hog::sim hog::waves hog::weather flecs::flecs_static
    PRIVATE tomlplusplus)
hog_target_defaults(hog_server)

hog_add_tests(hog_server
    tests/brick_sharing_tests.cpp
    tests/catch_up_tests.cpp
    tests/grain_sharing_tests.cpp
    tests/ground_wetting_tests.cpp
    tests/move_check_tests.cpp
    tests/network_cost_bench.cpp
    tests/pacer_tests.cpp
    tests/persistence_tests.cpp
    tests/players_tests.cpp
    tests/server_config_tests.cpp
    tests/snapshot_writer_tests.cpp
    tests/spatial_index_tests.cpp
    tests/spatial_tracking_tests.cpp
    tests/tick_clock_tests.cpp
    tests/tick_loop_tests.cpp
    tests/weather_keeper_tests.cpp)

if(TARGET hog_server_tests)
    target_compile_definitions(hog_server_tests PRIVATE HOG_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endif()
