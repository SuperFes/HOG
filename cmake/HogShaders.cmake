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

# Every shader is compiled with these, after its source and the target's shaders directory.
set(hog_slang_arguments -target spirv -fvk-use-entrypoint-name -matrix-layout-column-major)

# hog_add_shaders(<target> [CATALOGUE] <name>.slang...): each shader becomes
# <build>/shaders/include/hog/shaders/<name>.hpp, holding `hog::shaders::<name>` as SPIR-V words.
# Every entry point keeps its own name. Modules are imported from beside the shader or from the
# target's shaders directory. CATALOGUE also writes hog/shaders/catalogue.hpp, saying where each
# came from and how it was compiled, so it can be compiled again while running.
function(hog_add_shaders target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "CATALOGUE" "" "")
    set(include_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders/include")
    set(shaders_dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
    set(headers)
    set(catalogue_includes)
    set(catalogue_sources)
    foreach(source IN LISTS arg_UNPARSED_ARGUMENTS)
        get_filename_component(name "${source}" NAME_WE)
        get_filename_component(source_path "${source}" ABSOLUTE)
        set(spirv "${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.spv")
        set(header "${include_dir}/hog/shaders/${name}.hpp")
        add_custom_command(
            OUTPUT "${header}" "${spirv}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
            COMMAND "${HOG_SLANGC}" "${source_path}" -I "${shaders_dir}" ${hog_slang_arguments}
                    -o "${spirv}" -depfile "${spirv}.d"
            COMMAND "${CMAKE_COMMAND}" -DSPIRV=${spirv} -DHEADER=${header} -DNAME=${name}
                    -P "${hog_embed_spirv}"
            DEPENDS "${source_path}" "${hog_embed_spirv}"
            DEPFILE "${spirv}.d"
            COMMENT "Compiling shader ${name}"
            VERBATIM)
        list(APPEND headers "${header}")
        string(APPEND catalogue_includes "#include <hog/shaders/${name}.hpp>\n")
        string(APPEND catalogue_sources
               "    {.name = \"${name}\",\n     .source = R\"hog(${source_path})hog\",\n"
               "     .depends = R\"hog(${spirv}.d)hog\",\n     .embedded = ${name}},\n")
    endforeach()
    target_sources(${target} PRIVATE ${headers})
    target_include_directories(${target} PRIVATE "${include_dir}")
    if(arg_CATALOGUE)
        set(arguments "-I" "${shaders_dir}" ${hog_slang_arguments})
        set(catalogue_arguments)
        foreach(argument IN LISTS arguments)
            string(APPEND catalogue_arguments "    R\"hog(${argument})hog\",\n")
        endforeach()
        list(LENGTH arguments argument_count)
        list(LENGTH arg_UNPARSED_ARGUMENTS shader_count)
        file(GENERATE OUTPUT "${include_dir}/hog/shaders/catalogue.hpp" CONTENT "#pragma once

#include <hog/render/shader_watch.hpp>
${catalogue_includes}
#include <array>
#include <string_view>

namespace hog::shaders {

inline constexpr std::array<std::string_view, ${argument_count}> arguments{
${catalogue_arguments}};

inline constexpr std::array<render::ShaderSource, ${shader_count}> sources{{
${catalogue_sources}}};

inline constexpr render::ShaderCatalogue catalogue{
    .compiler = {.slangc = R\"hog(${HOG_SLANGC})hog\", .arguments = arguments},
    .shaders = sources,
    .scratch = R\"hog(${CMAKE_CURRENT_BINARY_DIR}/shaders/reloaded)hog\"};

} // namespace hog::shaders
")
    endif()
endfunction()
