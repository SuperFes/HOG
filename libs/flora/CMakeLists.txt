add_library(hog_flora
    src/mesh.cpp
    src/skeleton.cpp
    src/species.cpp)
add_library(hog::flora ALIAS hog_flora)

target_include_directories(hog_flora PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_flora PUBLIC hog::core)
hog_target_defaults(hog_flora)
# The same seed must grow the same plant, bit for bit, on every platform: no fused contractions,
# and src/trig.hpp keeps libm out of the growing.
target_compile_options(hog_flora PRIVATE -ffp-contract=off)

hog_add_tests(hog_flora
    tests/flora_tests.cpp)
if(TARGET hog_flora_tests)
    target_compile_options(hog_flora_tests PRIVATE -ffp-contract=off)
endif()
