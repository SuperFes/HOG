add_library(hog_bot
    src/bot.cpp
    src/options.cpp
    src/wander.cpp)
add_library(hog::bot ALIAS hog_bot)

target_include_directories(hog_bot PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_bot PUBLIC hog::client hog::core hog::planet)
hog_target_defaults(hog_bot)

add_executable(hog_bot_cli src/main.cpp)
set_target_properties(hog_bot_cli PROPERTIES OUTPUT_NAME hog-bot)
target_link_libraries(hog_bot_cli PRIVATE hog::bot hog::client hog::login hog::net hog::planet)
hog_target_defaults(hog_bot_cli)

hog_add_tests(hog_bot
    tests/bot_tests.cpp
    tests/options_tests.cpp
    tests/wander_tests.cpp)
if(TARGET hog_bot_tests)
    # The server, for a bot to walk on.
    target_link_libraries(hog_bot_tests PRIVATE hog::app)
endif()
