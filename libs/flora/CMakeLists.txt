add_library(hog_flora
    src/bushes.cpp
    src/fates.cpp
    src/mesh.cpp
    src/skeleton.cpp
    src/season.cpp
    src/species.cpp
    src/stand.cpp)
add_library(hog::flora ALIAS hog_flora)

target_include_directories(hog_flora PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# hog_planet by its plain name: libs/planet is added after libs/flora, and only a plain target
# may be named before it is defined.
target_link_libraries(hog_flora PUBLIC hog::core hog_planet)
hog_target_defaults(hog_flora)
# The same seed must grow the same plant, bit for bit, on every platform: no fused contractions,
# and src/trig.hpp keeps libm out of the growing.
target_compile_options(hog_flora PRIVATE -ffp-contract=off)

hog_add_tests(hog_flora
    tests/bush_tests.cpp
    tests/flora_tests.cpp)
if(TARGET hog_flora_tests)
    target_compile_options(hog_flora_tests PRIVATE -ffp-contract=off)
endif()
