#pragma once

#include <hog/globe/material.hpp>

#include <cstdint>

namespace hog::flow {

// What a voxel's surroundings do to it, each from 0 to 1.
struct Exposure {
    double open = 0.0;   // share of its faces on open space; buried matter doesn't weather
    double damp = 0.0;   // wetness of it and what touches it
    double freeze = 0.0; // nearness of its temperature to freezing, where thaw and frost alternate
};

// Share of a voxel's wholeness lost each second to the weather, rust, rot or whatever else takes
// that material; 0 for matter that doesn't wear (loose ground, water, air).
[[nodiscard]] double wearing_rate(globe::Material material, const Exposure& exposure);

// `condition` after `seconds` at `rate`. The loss is fractional, so what is lost is rounded
// up or down by `chance` (0 to 1) and the long-run loss is right however small each step.
[[nodiscard]] std::uint8_t worn(std::uint8_t condition, double rate, double seconds, double chance);

// A voxel's own softness from 0.2 to 2.6 times a typical one, the same each time for `key`: a
// stone has flaws and a plank has knots, so wear starts in patches and not as an even fade.
[[nodiscard]] double softness(std::uint64_t key);

} // namespace hog::flow
