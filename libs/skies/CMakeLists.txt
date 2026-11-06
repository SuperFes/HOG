# Keeps the planet's air stepping as game time passes, for a server to send or a viewer to draw
# alone. Knows nothing of connections.
add_library(hog_skies src/weather_keeper.cpp)
add_library(hog::skies ALIAS hog_skies)

target_include_directories(hog_skies PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_skies
    PUBLIC hog::core hog::globe hog::jobs hog::planet hog::protocol hog::save hog::sim hog::waves
        hog::weather)
hog_target_defaults(hog_skies)

hog_add_tests(hog_skies tests/weather_keeper_tests.cpp)
