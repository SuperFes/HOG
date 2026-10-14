# Slang shaders compiled to SPIR-V at build time and embedded in the binary.
#
# The compiler is a pinned release, so every machine builds the same SPIR-V; HOG_SLANGC overrides
# it with another slangc.
set(HOG_SLANGC "" CACHE FILEPATH "slangc to compile shaders with; empty fetches the pinned release")

if(NOT HOG_SLANGC)
    set(hog_slang_version 2026.19)
    string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" hog_host_cpu)
    if(hog_host_cpu MATCHES "^(x86_64|amd64)$")
        set(hog_host_cpu x86_64)
    elseif(hog_host_cpu MATCHES "^(aarch64|arm64)$")
        set(hog_host_cpu aarch64)
    endif()
    if(CMAKE_HOST_WIN32)
        set(hog_slang_host windows-${hog_host_cpu})
    elseif(CMAKE_HOST_APPLE)
        set(hog_slang_host macos-${hog_host_cpu})
    else()
        set(hog_slang_host linux-${hog_host_cpu})
    endif()
    CPMAddPackage(
        NAME slang
        URL "https://github.com/shader-slang/slang/releases/download/v${hog_slang_version}/slang-${hog_slang_version}-${hog_slang_host}.tar.gz"
        DOWNLOAD_ONLY YES)
    find_program(hog_slangc_found slangc PATHS "${slang_SOURCE_DIR}/bin" NO_DEFAULT_PATH REQUIRED)
    set(HOG_SLANGC "${hog_slangc_found}" CACHE FILEPATH "" FORCE)
endif()

set(hog_embed_spirv "${CMAKE_CURRENT_LIST_DIR}/EmbedSpirv.cmake")

# hog_add_shaders(<target> <name>.slang...): each shader becomes
# <build>/shaders/include/hog/shaders/<name>.hpp, holding `hog::shaders::<name>` as SPIR-V words.
# Every entry point keeps its own name.
function(hog_add_shaders target)
    set(include_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders/include")
    set(headers)
    foreach(source IN LISTS ARGN)
        get_filename_component(name "${source}" NAME_WE)
        get_filename_component(source_path "${source}" ABSOLUTE)
        set(spirv "${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.spv")
        set(header "${include_dir}/hog/shaders/${name}.hpp")
        add_custom_command(
            OUTPUT "${header}" "${spirv}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
            COMMAND "${HOG_SLANGC}" "${source_path}" -target spirv -fvk-use-entrypoint-name
                    -matrix-layout-column-major -o "${spirv}" -depfile "${spirv}.d"
            COMMAND "${CMAKE_COMMAND}" -DSPIRV=${spirv} -DHEADER=${header} -DNAME=${name}
                    -P "${hog_embed_spirv}"
            DEPENDS "${source_path}" "${hog_embed_spirv}"
            DEPFILE "${spirv}.d"
            COMMENT "Compiling shader ${name}"
            VERBATIM)
        list(APPEND headers "${header}")
    endforeach()
    target_sources(${target} PRIVATE ${headers})
    target_include_directories(${target} PRIVATE "${include_dir}")
endfunction()
