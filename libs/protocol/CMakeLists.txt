add_library(hog_protocol
    src/messages.cpp)
add_library(hog::protocol ALIAS hog_protocol)

target_include_directories(hog_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_protocol PUBLIC hog::core)
hog_target_defaults(hog_protocol)

hog_add_tests(hog_protocol
    tests/messages_tests.cpp)
