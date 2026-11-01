add_library(hog_flora src/season.cpp)
add_library(hog::flora ALIAS hog_flora)

target_include_directories(hog_flora PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_flora PUBLIC hog::core)
hog_target_defaults(hog_flora)
