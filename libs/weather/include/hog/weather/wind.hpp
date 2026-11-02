#pragma once

#include <hog/globe/planet.hpp>
#include <hog/weather/field.hpp>
#include <hog/weather/stencil.hpp>

namespace hog::weather {

// Warm air rises and cold sinks, the spinning planet turns moving air aside, and near the ground
// air is slowed; the air's warmth is drawn back towards what its surface would give it, fast
// near the ground and over weeks above. Over `seconds`.
void accelerate(Field& field, const Stencil& stencil, const globe::Planet& planet, double seconds);

// How far a wind is from flowing without piling air up or thinning it: the root mean square of the
// divergence of its mass flux over the open air, kg/m³/s. Rising air thins, so it is the mass
// that must balance, not the volume.
[[nodiscard]] double divergence(const Field& field, const Stencil& stencil,
                                const globe::Planet& planet);

// Takes the divergence out of the wind's mass flux with the pressure that does it, found by
// conjugate gradients, stopping once the divergence is a `tolerance` share of what it was or after
// `iterations`. Air doesn't cross the ground. Returns the iterations it took.
int project(Field& field, const Stencil& stencil, const globe::Planet& planet,
            double tolerance = 1e-3, int iterations = 200);

// Carries everything the air holds, and the wind itself, along the wind over `seconds`, taking
// each cell's value from where its air came from.
// A parcel keeps the temperature it would have brought down to the ground, so rising air cools.
void advect(Field& field, const Stencil& stencil, const globe::Planet& planet, double seconds);

// One step of the air: forces, pressure, then transport.
void step(Field& field, const Stencil& stencil, const globe::Planet& planet, double seconds);

} // namespace hog::weather
