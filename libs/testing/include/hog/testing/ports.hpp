#pragma once

#include <cstdint>

namespace hog::testing {

// The ports tests listen on: below Windows' dynamic range, where most of the blocks it reserves
// lie.
inline constexpr std::uint16_t lowest_port = 20'000;
inline constexpr std::uint16_t highest_port = 48'999;

// How many ports to try before giving up.
inline constexpr int port_tries = 20;

// A port to try listening on, drawn apart from the last, since Windows reserves ports in blocks
// that a run of neighbours could fall wholly inside.
[[nodiscard]] std::uint16_t random_port();

} // namespace hog::testing
