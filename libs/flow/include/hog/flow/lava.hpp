#pragma once

#include <hog/globe/material.hpp>

namespace hog::flow {

// Rock hotter than this shines and is still cooling; below it, it is taken to have cooled off.
inline constexpr double red_hot_kelvin = 600.0;

// Molten rock is the rock's own material above its melting point; these tell how it behaves
// while it is hot and cooling.

// How freely `rock` at `kelvin` runs, 0 stiff to 1 running: it thickens as it cools and stops
// flowing a little below its melting point.
[[nodiscard]] double lava_mobility(globe::Material rock, double kelvin);

// Whether `rock` at `kelvin` is still molten, running or not.
[[nodiscard]] bool is_molten(globe::Material rock, double kelvin);

// The temperature of a voxel `thickness` metres deep of `rock` after `seconds` of heat lost to air
// at `ambient`, from the `open` share (0 to 1) of its faces on open space; buried lava keeps its
// heat.
[[nodiscard]] double cooled(globe::Material rock, double kelvin, double ambient, double open,
                            double thickness, double seconds);

// Heat conducted between two neighbouring voxels of `rock`, each `thickness` metres deep, over
// `seconds`: the hotter gives to the cooler, none lost.
void conduct(globe::Material rock, double& below, double& above, double thickness, double seconds);

} // namespace hog::flow
