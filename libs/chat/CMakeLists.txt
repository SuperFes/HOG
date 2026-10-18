add_library(hog_chat
    src/filter.cpp)
add_library(hog::chat ALIAS hog_chat)

target_include_directories(hog_chat PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
hog_target_defaults(hog_chat)

hog_add_tests(hog_chat
    tests/filter_tests.cpp)
