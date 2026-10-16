add_library(hog_net
    src/client.cpp
    src/events.cpp
    src/library.cpp
    src/server.cpp)
add_library(hog::net ALIAS hog_net)

target_include_directories(hog_net PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_net PUBLIC hog::core PRIVATE GameNetworkingSockets::static)
hog_target_defaults(hog_net)

hog_add_tests(hog_net
    tests/connection_tests.cpp)
