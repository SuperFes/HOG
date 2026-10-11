# Helpers shared by every library's unit tests; never linked into shipping targets.
add_library(hog_testing
    src/assertion_guard.cpp)
add_library(hog::testing ALIAS hog_testing)

target_include_directories(hog_testing PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_testing PUBLIC hog::core)
hog_target_defaults(hog_testing)
