#include "terms.hpp"

#include <algorithm>
#include <cmath>

namespace hog::globe::terms {
namespace {

// Passages wind some tens of metres between bends, a few metres wide, and run twice as far
// along as up or down: water dissolves its way along beds and toward the water table.
constexpr double tunnel_wavelength = 90.0;
constexpr double tunnel_radius = 3.5;
constexpr double stretch = 2.0;

// Chambers open where dissolution pooled, rarely.
constexpr double chamber_wavelength = 160.0;
constexpr double chamber_threshold = 0.6;

// Caves gather in districts a couple of kilometres across, where water found its way in.
constexpr double district_wavelength = 2'000.0;

// Dissolution needs water moving: it fades a kilometre down. Lava tubes lie within tens of
// metres of the flows' tops.
constexpr double karst_depth = 1'000.0;
constexpr double tube_depth = 30.0;

// How readily each rock holds open passages.
double openness(Material rock, double depth, bool watered) {
    switch (rock) {
    case Material::Limestone:
        return watered ? 1.0 : 0.0;
    case Material::Shale:
    case Material::Sandstone:
        return watered ? 0.25 : 0.0;
    case Material::Basalt:
        // Lava tubes, on any world with flows.
        return 1.0 - smoothstep((depth - tube_depth) / tube_depth);
    case Material::Granite:
    case Material::Anorthosite:
        return 0.1;
    default:
        return 0.0;
    }
}

// The field's gradient against the point itself, given its gradient against the stretched point.
DVec3 unstretched(DVec3 gradient, DVec3 direction, double tangent_scale) {
    const double along = glm::dot(gradient, direction);
    return ((gradient - (along * direction)) * tangent_scale) + ((stretch * along) * direction);
}

} // namespace

double cave_distance(const Context& context, DVec3 direction, double radius, Material rock,
                     double depth, double lod) noexcept {
    const Planet& planet = *context.planet;
    const bool watered = planet.volatiles != Volatile::None;
    const double open =
        openness(rock, depth, watered) * (1.0 - smoothstep((depth - karst_depth) / karst_depth));
    if (open <= 0.0) {
        return cave_reach;
    }
    // Stretched across the radius, so passages run more along than up.
    const double lifted = planet.radius + (stretch * (radius - planet.radius));
    const DVec3 at = direction * lifted;
    const double tangent_scale = lifted / radius;

    const Noise district =
        fbm(direction * planet.radius,
            {.wavelength = district_wavelength, .gain = 0.5, .finest = district_wavelength / 2.0},
            0.0, context.seed(Term::CaveDistricts));
    const double presence = open * smoothstep((district.value + 0.1) / 0.4);
    const double size = tunnel_radius * presence;
    // Passages too narrow for the detail asked for close up smoothly.
    const double shown = size * smoothstep((size / lod) - 1.0);
    if (shown <= 0.0 && rock != Material::Limestone) {
        return cave_reach;
    }

    const Octaves winding{
        .wavelength = tunnel_wavelength, .gain = 0.3, .finest = tunnel_wavelength / 8.0};
    const Noise a = fbm(at, winding, lod, context.seed(Term::CaveA));
    const Noise b = fbm(at, winding, lod, context.seed(Term::CaveB));
    const double across_a =
        a.value / std::max(glm::length(unstretched(a.gradient, direction, tangent_scale)), 1e-9);
    const double across_b =
        b.value / std::max(glm::length(unstretched(b.gradient, direction, tangent_scale)), 1e-9);
    double distance =
        shown > 0.0 ? std::sqrt((across_a * across_a) + (across_b * across_b)) - shown : cave_reach;

    if (rock == Material::Limestone && watered) {
        const Noise pool = fbm(
            at, {.wavelength = chamber_wavelength, .gain = 0.5, .finest = chamber_wavelength / 8.0},
            lod, context.seed(Term::Chambers));
        const double steep =
            std::max(glm::length(unstretched(pool.gradient, direction, tangent_scale)), 1e-9);
        // Fewer where passages are fewer: none where there are none.
        const double threshold = chamber_threshold + ((1.0 - presence) * 0.6);
        distance = std::min(distance, (threshold - pool.value) / steep);
    }
    return std::min(distance, cave_reach);
}

} // namespace hog::globe::terms
