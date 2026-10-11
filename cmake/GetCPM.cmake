# Downloads a pinned CPM.cmake (https://github.com/cpm-cmake/CPM.cmake, MIT) and includes it.
set(CPM_DOWNLOAD_VERSION 0.43.2)
set(CPM_HASH_SUM 49a3bef91ceb65bb66d57255e12d1ffd22abc2f6408fa9fe4534c544a2f232aa)

# One shared source cache per checkout so every preset reuses the same downloads.
if(NOT CPM_SOURCE_CACHE AND NOT DEFINED ENV{CPM_SOURCE_CACHE})
    set(CPM_SOURCE_CACHE "${PROJECT_SOURCE_DIR}/.cache/cpm" CACHE PATH "CPM source cache")
endif()

set(CPM_DOWNLOAD_LOCATION "${PROJECT_SOURCE_DIR}/.cache/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")

if(NOT EXISTS "${CPM_DOWNLOAD_LOCATION}")
    file(DOWNLOAD
        "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake"
        "${CPM_DOWNLOAD_LOCATION}"
        EXPECTED_HASH SHA256=${CPM_HASH_SUM})
endif()

include("${CPM_DOWNLOAD_LOCATION}")
