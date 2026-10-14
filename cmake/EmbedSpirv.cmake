# cmake -DSPIRV=<file.spv> -DHEADER=<file.hpp> -DNAME=<identifier> -P EmbedSpirv.cmake
file(READ "${SPIRV}" bytes HEX)
string(LENGTH "${bytes}" length)
math(EXPR remainder "${length} % 8")
if(length EQUAL 0 OR NOT remainder EQUAL 0)
    message(FATAL_ERROR "${SPIRV} is not whole SPIR-V words")
endif()
# SPIR-V files are little-endian words.
string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1U," words "${bytes}")
string(REGEX REPLACE "(0x[0-9a-f]+U,0x[0-9a-f]+U,0x[0-9a-f]+U,0x[0-9a-f]+U,0x[0-9a-f]+U,0x[0-9a-f]+U,0x[0-9a-f]+U,0x[0-9a-f]+U,)"
       "\\1\n    " words "${words}")
math(EXPR count "${length} / 8")
file(WRITE "${HEADER}" "#pragma once

#include <array>
#include <cstdint>

namespace hog::shaders {

inline constexpr std::array<std::uint32_t, ${count}> ${NAME}{
    ${words}
};

} // namespace hog::shaders
")
