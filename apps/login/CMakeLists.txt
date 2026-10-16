add_library(hog_login_app
    src/options.cpp)
add_library(hog::login_app ALIAS hog_login_app)

target_include_directories(hog_login_app PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_login_app PUBLIC hog::core)
hog_target_defaults(hog_login_app)

add_executable(hog_login_cli src/main.cpp)
set_target_properties(hog_login_cli PROPERTIES OUTPUT_NAME hog-login)
target_link_libraries(hog_login_cli PRIVATE hog::login_app hog::login hog::net)
hog_target_defaults(hog_login_cli)

hog_add_tests(hog_login_app
    tests/options_tests.cpp)
