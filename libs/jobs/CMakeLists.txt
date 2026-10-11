find_package(Threads REQUIRED)

add_library(enkiTS STATIC "${enkiTS_SOURCE_DIR}/src/TaskScheduler.cpp")
target_include_directories(enkiTS SYSTEM PUBLIC "${enkiTS_SOURCE_DIR}/src")
target_link_libraries(enkiTS PUBLIC Threads::Threads)

add_library(hog_jobs
    src/job_system.cpp)
add_library(hog::jobs ALIAS hog_jobs)

target_include_directories(hog_jobs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hog_jobs PUBLIC hog::core enkiTS)
hog_target_defaults(hog_jobs)

hog_add_tests(hog_jobs
    tests/job_system_tests.cpp)
