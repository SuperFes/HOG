option(HOG_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(HOG_BUILD_TESTS "Build unit tests" ON)
set(HOG_SANITIZE "" CACHE STRING "Semicolon-separated sanitizers, e.g. address;undefined or thread")
option(HOG_USE_LIBCXX "Build against LLVM libc++ (what macOS ships) instead of the platform default" OFF)

# Global so dependencies share the same standard library ABI.
if(HOG_USE_LIBCXX)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>)
    add_link_options(-stdlib=libc++)
endif()

if(NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(HOG_CCACHE ccache)
    if(HOG_CCACHE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${HOG_CCACHE}")
    endif()
endif()

# Applied to every HOG target; third-party targets keep their own flags.
function(hog_target_defaults target)
    target_compile_features(${target} PUBLIC cxx_std_23)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
        if(HOG_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        target_compile_definitions(${target} PRIVATE _MSVC_STL_HARDENING=1)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast
            -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align
            -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough
            # Clang fuses a*b+c into FMA by default and GCC does not; results must match exactly.
            -ffp-contract=off)
        if(HOG_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
        # Bounds and precondition checks in the standard library, in every build type.
        target_compile_definitions(${target} PRIVATE
            _GLIBCXX_ASSERTIONS
            $<IF:$<CONFIG:Debug>,_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG,_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST>)
    endif()
endfunction()

# Global rather than per-target so dependencies are instrumented too (required for thread).
if(HOG_SANITIZE)
    list(JOIN HOG_SANITIZE "," hog_sanitizers)
    if(MSVC)
        add_compile_options(/fsanitize=${hog_sanitizers})
    else()
        add_compile_options(-fsanitize=${hog_sanitizers} -fno-omit-frame-pointer -fno-sanitize-recover=all)
        add_link_options(-fsanitize=${hog_sanitizers})
    endif()
endif()
