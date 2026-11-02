add_library(hog_weather
    src/atmosphere.cpp
    src/field.cpp
    src/grid.cpp
    src/moisture.cpp
    src/stencil.cpp
    src/wind.cpp)
add_library(hog::weather ALIAS hog_weather)

target_include_directories(hog_weather PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_weather PUBLIC hog::core hog::globe)
hog_target_defaults(hog_weather)

hog_add_tests(hog_weather
    tests/atmosphere_tests.cpp
    tests/field_tests.cpp
    tests/grid_tests.cpp
    tests/moisture_tests.cpp
    tests/stencil_tests.cpp
    tests/surface_tests.cpp
    tests/wind_tests.cpp)
target_link_libraries(hog_weather_tests PRIVATE hog::sim)
