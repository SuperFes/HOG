#pragma once

#include <hog/globe/globe.hpp>
#include <hog/globe/settling.hpp>

#include <cstdint>

namespace hog::globe {

// How a planet sorts itself by weight. Wherever a hexel presses on a lighter one under it harder
// than that one bears, it sinks through it, and the energy of the fall heats them both, weakening
// what lies deeper in turn. Rock that melts lets go of its water and gas. It runs until nothing
// more sinks, then lets the surface settle. Returns how many times a hexel sank a shell.
std::uint64_t differentiate(Globe& globe, const Gravity& gravity);

} // namespace hog::globe
