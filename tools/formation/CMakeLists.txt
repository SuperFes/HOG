add_library(hog_formation_lab src/pictures.cpp)
add_library(hog::formation_lab ALIAS hog_formation_lab)

target_include_directories(hog_formation_lab PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_formation_lab PUBLIC hog::formation)
hog_target_defaults(hog_formation_lab)

add_executable(hog_formation_cli src/main.cpp)
set_target_properties(hog_formation_cli PROPERTIES OUTPUT_NAME hog-formation)
target_link_libraries(hog_formation_cli PRIVATE hog::formation_lab)
hog_target_defaults(hog_formation_cli)

hog_add_tests(hog_formation_lab tests/pictures_tests.cpp)
