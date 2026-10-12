add_library(hog_sim
    src/calendar.cpp
    src/cell_grid.cpp
    src/ecology.cpp
    src/weather.cpp
    src/weather_heat.cpp
    src/weather_motion.cpp
    src/weather_save.cpp
    src/weather_systems.cpp
    src/weather_water.cpp
    src/weather_wind.cpp)
add_library(hog::sim ALIAS hog_sim)

target_include_directories(hog_sim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_sim PUBLIC hog::core hog::planet hog::jobs PRIVATE tomlplusplus)
hog_target_defaults(hog_sim)

hog_add_tests(hog_sim
    tests/calendar_tests.cpp
    tests/cell_grid_tests.cpp
    tests/ecology_tests.cpp
    tests/weather_tests.cpp)
