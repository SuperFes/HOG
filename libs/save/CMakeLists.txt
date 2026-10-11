add_library(hog_save
    src/memory_snapshot_store.cpp
    src/snapshot.cpp
    src/snapshot_store.cpp)
add_library(hog::save ALIAS hog_save)

target_include_directories(hog_save PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_save PUBLIC hog::core)
hog_target_defaults(hog_save)

hog_add_tests(hog_save
    tests/snapshot_store_tests.cpp
    tests/snapshot_tests.cpp)
