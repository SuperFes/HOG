# Helpers shared by every library's unit tests; never linked into shipping targets.
add_library(hog_testing
    src/assertion_guard.cpp
    src/ports.cpp
    src/quick.cpp)
add_library(hog::testing ALIAS hog_testing)

target_include_directories(hog_testing PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_testing PUBLIC hog::core)
hog_target_defaults(hog_testing)
# The quick switch is read from the environment before any test thread starts.
if(MSVC)
    target_compile_definitions(hog_testing PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

hog_add_tests(hog_testing
    tests/ports_tests.cpp
    tests/quick_tests.cpp)
