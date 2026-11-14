add_library(hog_formation
    src/bedrock.cpp
    src/climate.cpp
    src/fields.cpp
    src/ice.cpp
    src/lakes.cpp
    src/landforms.cpp
    src/drifts.cpp
    src/ores.cpp
    src/plates.cpp
    src/regimes.cpp
    src/rivers.cpp
    src/sediment.cpp
    src/shores.cpp
    src/smooth.cpp
    src/soil.cpp
    src/sphere.cpp
    src/world.cpp)
add_library(hog::formation ALIAS hog_formation)

target_include_directories(hog_formation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_formation PUBLIC hog::core)
hog_target_defaults(hog_formation)

hog_add_tests(hog_formation
    tests/bedrock_tests.cpp
    tests/climate_tests.cpp
    tests/fields_tests.cpp
    tests/ice_tests.cpp
    tests/lakes_tests.cpp
    tests/landforms_tests.cpp
    tests/drifts_tests.cpp
    tests/ores_tests.cpp
    tests/plates_tests.cpp
    tests/regimes_tests.cpp
    tests/rivers_tests.cpp
    tests/sediment_tests.cpp
    tests/shores_tests.cpp
    tests/smooth_tests.cpp
    tests/soil_tests.cpp
    tests/sphere_tests.cpp)
