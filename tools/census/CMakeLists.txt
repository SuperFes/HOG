add_library(hog_census src/census.cpp)
add_library(hog::census ALIAS hog_census)

target_include_directories(hog_census PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_census PUBLIC hog::globe)
hog_target_defaults(hog_census)

add_executable(hog_census_cli src/main.cpp)
set_target_properties(hog_census_cli PROPERTIES OUTPUT_NAME hog-census)
target_link_libraries(hog_census_cli PRIVATE hog::census hog::jobs)
hog_target_defaults(hog_census_cli)

hog_add_tests(hog_census tests/census_tests.cpp)
