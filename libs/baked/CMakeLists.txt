find_package(Threads REQUIRED)

set(lmdb_dir "${lmdb_SOURCE_DIR}/libraries/liblmdb")
add_library(lmdb STATIC "${lmdb_dir}/mdb.c" "${lmdb_dir}/midl.c")
target_include_directories(lmdb SYSTEM PUBLIC "${lmdb_dir}")
target_link_libraries(lmdb PUBLIC Threads::Threads)
if(NOT MSVC)
    # Its own sources aren't held to our warnings.
    target_compile_options(lmdb PRIVATE -w)
endif()

add_library(lmdbxx INTERFACE)
target_include_directories(lmdbxx SYSTEM INTERFACE "${lmdbxx_SOURCE_DIR}")
target_link_libraries(lmdbxx INTERFACE lmdb)

add_library(hog_baked
    src/node.cpp
    src/page.cpp
    src/store.cpp)
add_library(hog::baked ALIAS hog_baked)

target_include_directories(hog_baked PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_baked PUBLIC hog::globe PRIVATE lmdbxx)
hog_target_defaults(hog_baked)

hog_add_tests(hog_baked
    tests/node_tests.cpp
    tests/page_tests.cpp
    tests/store_tests.cpp)
