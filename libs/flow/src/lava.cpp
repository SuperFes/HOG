#include <hog/flow/lava.hpp>

#include <algorithm>
#include <cmath>

namespace hog::flow {
namespace {

// Lava runs freely from just over its melting point and has set stiff this far under it.
constexpr double runny_above = 50.0;
constexpr double stiff_below = 200.0;
// Watts a square metre of a glowing surface loses for each kelvin over the air's, radiation
// and the air's own movement together.
constexpr double surface_loss = 120.0;

} // namespace

double lava_mobility(globe::Material rock, double kelvin) {
    const double melts = globe::properties(rock).melts;
    if (melts <= 0.0) {
        return 0.0;
    }
    const double t = std::clamp(
        (kelvin - (melts - stiff_below)) / (stiff_below + runny_above), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

bool is_molten(globe::Material rock, double kelvin) {
    const double melts = globe::properties(rock).melts;
    return melts > 0.0 && kelvin >= melts;
}

double cooled(globe::Material rock, double kelvin, double ambient, double open, double thickness,
              double seconds) {
    const globe::Properties& properties = globe::properties(rock);
    const double loss = surface_loss * open;
    if (loss <= 0.0 || thickness <= 0.0 || kelvin <= ambient) {
        return kelvin;
    }
    const double settle = properties.density * properties.heat_capacity * thickness / loss;
    return ambient + (kelvin - ambient) * std::exp(-seconds / settle);
}

} // namespace hog::flow
