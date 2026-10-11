# The implementation, compiled once in C without HOG's warnings.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/stb_image_write.c"
    "#define STB_IMAGE_WRITE_IMPLEMENTATION\n#include <stb_image_write.h>\n")
add_library(stb_image_write STATIC "${CMAKE_CURRENT_BINARY_DIR}/stb_image_write.c")
target_include_directories(stb_image_write SYSTEM PUBLIC "${stb_SOURCE_DIR}")
# Writes through callbacks only, so stdio's file functions are never needed.
target_compile_definitions(stb_image_write PUBLIC STBI_WRITE_NO_STDIO)

add_library(hog_mapgen
    src/options.cpp
    src/render.cpp)
add_library(hog::mapgen ALIAS hog_mapgen)

target_include_directories(hog_mapgen PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_mapgen PUBLIC hog::planet hog::jobs PRIVATE stb_image_write)
hog_target_defaults(hog_mapgen)

add_executable(hog_mapgen_cli src/main.cpp)
set_target_properties(hog_mapgen_cli PROPERTIES OUTPUT_NAME hog-mapgen)
target_link_libraries(hog_mapgen_cli PRIVATE hog::mapgen)
hog_target_defaults(hog_mapgen_cli)

hog_add_tests(hog_mapgen
    tests/options_tests.cpp
    tests/render_tests.cpp)
