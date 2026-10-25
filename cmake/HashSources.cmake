# Writes OUTPUT, a header naming a hash of the contents of FILES (separated by |) as
# planet_source_hash, so what was made by that code can tell it apart from what other code made.
string(REPLACE "|" ";" files "${FILES}")
list(SORT files)
set(hashes "")
foreach(file IN LISTS files)
    file(SHA256 "${file}" hash)
    string(APPEND hashes "${hash}")
endforeach()
string(SHA256 combined "${hashes}")
string(SUBSTRING "${combined}" 0 16 short)
file(WRITE "${OUTPUT}" "#pragma once

#include <cstdint>

namespace hog::planet {

inline constexpr std::uint64_t planet_source_hash = 0x${short}ULL;

} // namespace hog::planet
")
