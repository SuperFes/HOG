add_library(hog_app
    src/logging.cpp
    src/options.cpp
    src/probe_layers.cpp
    src/server.cpp)
add_library(hog::app ALIAS hog_app)

target_include_directories(hog_app PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_app PUBLIC hog::probe hog::server hog::sim hog::store)
hog_target_defaults(hog_app)

add_executable(hog_server_cli src/main.cpp)
set_target_properties(hog_server_cli PROPERTIES OUTPUT_NAME hog-server)
target_link_libraries(hog_server_cli PRIVATE hog::app)
hog_target_defaults(hog_server_cli)

hog_add_tests(hog_app
    tests/logging_tests.cpp
    tests/options_tests.cpp
    tests/server_tests.cpp)
if(TARGET hog_app_tests)
    target_compile_definitions(hog_app_tests PRIVATE HOG_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endif()
