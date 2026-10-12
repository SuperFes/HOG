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
    src/weather_wind.cpp
    src/wildlife.cpp
    src/wildlife_food.cpp
    src/wildlife_life.cpp
    src/wildlife_motion.cpp
    src/wildlife_save.cpp)
add_library(hog::sim ALIAS hog_sim)

target_include_directories(hog_sim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_sim PUBLIC hog::core hog::planet hog::jobs PRIVATE tomlplusplus)
hog_target_defaults(hog_sim)

hog_add_tests(hog_sim
    tests/calendar_tests.cpp
    tests/cell_grid_tests.cpp
    tests/ecology_tests.cpp
    tests/food_chain_tests.cpp
    tests/weather_tests.cpp
    tests/wildlife_tests.cpp)

if(TARGET hog_sim_tests)
    target_compile_definitions(hog_sim_tests PRIVATE HOG_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endif()
