find_package(Threads REQUIRED)

set(lmdb_dir "${lmdb_SOURCE_DIR}/libraries/liblmdb")
add_library(lmdb STATIC "${lmdb_dir}/mdb.c" "${lmdb_dir}/midl.c")
target_include_directories(lmdb SYSTEM PUBLIC "${lmdb_dir}")
target_link_libraries(lmdb PUBLIC Threads::Threads)
if(NOT MSVC)
    # Its own sources aren't held to our warnings.
    target_compile_options(lmdb PRIVATE -w)
endif()

# Its header alone: the repository's VERSION file would stand in for <version> on a filesystem
# that ignores case.
set(lmdbxx_include "${CMAKE_CURRENT_BINARY_DIR}/lmdbxx")
configure_file("${lmdbxx_SOURCE_DIR}/lmdb++.h" "${lmdbxx_include}/lmdb++.h" COPYONLY)
add_library(lmdbxx INTERFACE)
target_include_directories(lmdbxx SYSTEM INTERFACE "${lmdbxx_include}")
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
