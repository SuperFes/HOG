add_library(hog_server
    src/pacer.cpp
    src/persistence.cpp
    src/players.cpp
    src/probe_service.cpp
    src/server_config.cpp
    src/snapshot_writer.cpp
    src/spatial_index.cpp
    src/spatial_tracking.cpp
    src/survey.cpp
    src/tick_clock.cpp)
add_library(hog::server ALIAS hog_server)

target_include_directories(hog_server PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_server
    PUBLIC hog::core hog::jobs hog::net hog::planet hog::probe hog::protocol hog::replication hog::save
        flecs::flecs_static
    PRIVATE tomlplusplus)
hog_target_defaults(hog_server)

hog_add_tests(hog_server
    tests/catch_up_tests.cpp
    tests/pacer_tests.cpp
    tests/persistence_tests.cpp
    tests/players_tests.cpp
    tests/probe_service_tests.cpp
    tests/server_config_tests.cpp
    tests/snapshot_writer_tests.cpp
    tests/spatial_index_tests.cpp
    tests/spatial_tracking_tests.cpp
    tests/survey_tests.cpp
    tests/tick_clock_tests.cpp
    tests/tick_loop_tests.cpp)

if(TARGET hog_server_tests)
    target_compile_definitions(hog_server_tests PRIVATE HOG_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endif()
