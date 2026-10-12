add_library(hog_probe
    src/messages.cpp)
add_library(hog::probe ALIAS hog_probe)

target_include_directories(hog_probe PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_probe PUBLIC hog::core)
hog_target_defaults(hog_probe)

hog_add_tests(hog_probe
    tests/messages_tests.cpp)
