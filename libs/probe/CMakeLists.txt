add_library(hog_probe
    src/channel.cpp
    src/messages.cpp
    src/socket.cpp)
add_library(hog::probe ALIAS hog_probe)

target_include_directories(hog_probe PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_probe PUBLIC hog::core)
hog_target_defaults(hog_probe)

hog_add_tests(hog_probe
    tests/channel_tests.cpp
    tests/messages_tests.cpp)

if(WIN32)
    target_link_libraries(hog_probe PRIVATE ws2_32)
endif()
