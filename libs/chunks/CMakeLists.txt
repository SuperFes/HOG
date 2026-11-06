# What of the planet's surface a viewer is shown, chosen and meshed a chunk at a time round an eye,
# coarser further off: for a server to send or a viewer to draw.
add_library(hog_chunks src/globe_stream.cpp src/mesh_codec.cpp)
add_library(hog::chunks ALIAS hog_chunks)

target_include_directories(hog_chunks PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_chunks PUBLIC hog::core hog::globe hog::jobs hog::protocol hog::terrain)
hog_target_defaults(hog_chunks)

hog_add_tests(hog_chunks tests/lod_error_tests.cpp tests/mesh_codec_tests.cpp)
