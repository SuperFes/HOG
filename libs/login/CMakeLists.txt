add_library(monocypher STATIC
    "${Monocypher_SOURCE_DIR}/src/monocypher.c"
    "${Monocypher_SOURCE_DIR}/src/optional/monocypher-ed25519.c")
target_include_directories(monocypher SYSTEM PUBLIC
    "${Monocypher_SOURCE_DIR}/src" "${Monocypher_SOURCE_DIR}/src/optional")

add_library(hog_login
    src/keys.cpp
    src/service.cpp
    src/token.cpp)
add_library(hog::login ALIAS hog_login)

target_include_directories(hog_login PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_login PUBLIC hog::core PRIVATE monocypher)
if(WIN32)
    target_link_libraries(hog_login PRIVATE bcrypt)
endif()
hog_target_defaults(hog_login)

hog_add_tests(hog_login
    tests/service_tests.cpp
    tests/token_tests.cpp)
