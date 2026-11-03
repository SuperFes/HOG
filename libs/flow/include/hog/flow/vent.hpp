#pragma once

#include <hog/globe/material.hpp>
#include <hog/globe/voxel.hpp>

namespace hog::flow {

// A place where magma reaches the surface: a volcano's throat, a fissure, a pool in a cavern fed
// from below. It does nothing but give molten rock at the rate its stage says; where that goes
// is the flow's.
enum class Stage : std::uint8_t {
    Dormant,  // gives nothing
    Active,   // a lake or a steady seep
    Erupting, // pouring out
};

struct Vent {
    globe::Site site;
    globe::Material rock = globe::Material::Basalt;
    double kelvin = 1'650.0;
    // Cubic metres a second in each stage.
    double active_rate = 0.0;
    double erupting_rate = 0.0;
    // Each cycle of `period` seconds, `eruption` seconds of it are an eruption and `active` the
    // seconds after that it still seeps; the cycle is shifted by `phase` seconds. All zero is a
    // vent that is always `Active`.
    double period = 0.0;
    double eruption = 0.0;
    double active = 0.0;
    double phase = 0.0;
};

// The stage the vent is in at `clock` seconds, the same each time it's asked.
[[nodiscard]] Stage stage_at(const Vent& vent, double clock);

// Cubic metres a second the vent gives at `clock`.
[[nodiscard]] double rate_at(const Vent& vent, double clock);

} // namespace hog::flow
