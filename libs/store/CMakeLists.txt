find_package(Threads REQUIRED)

add_library(sqlite3 STATIC "${sqlite3_SOURCE_DIR}/sqlite3.c")
target_include_directories(sqlite3 SYSTEM PUBLIC "${sqlite3_SOURCE_DIR}")
# Defaults recommended by SQLite; extensions are never loaded, so no libdl.
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_SHARED_CACHE
    SQLITE_USE_ALLOCA)
target_link_libraries(sqlite3 PUBLIC Threads::Threads)

add_library(hog_store
    src/database.cpp
    src/sqlite_change_store.cpp
    src/sqlite_player_store.cpp
    src/sqlite_snapshot_store.cpp)
add_library(hog::store ALIAS hog_store)

target_include_directories(hog_store PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_store PUBLIC hog::planet hog::save PRIVATE sqlite3)
hog_target_defaults(hog_store)

hog_add_tests(hog_store
    tests/database_tests.cpp
    tests/sqlite_change_store_tests.cpp
    tests/sqlite_player_store_tests.cpp
    tests/sqlite_snapshot_store_tests.cpp)

if(TARGET hog_store_tests)
    # Tests reach into the database to damage it.
    target_link_libraries(hog_store_tests PRIVATE sqlite3)
endif()
