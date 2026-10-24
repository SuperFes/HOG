#include <hog/core/assert.hpp>
#include <hog/terrain/sea.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>

namespace hog::terrain {

namespace {

// Past the horizon, so the chords of the last ring don't leave sky showing under it.
constexpr double past_horizon = 1.05;

} // namespace

SeaDisc sea_disc(int rings, int spokes) {
    HOG_ASSERT(rings >= 1 && spokes >= 3, "a disc needs a ring and three spokes");
    HOG_ASSERT((static_cast<std::int64_t>(rings) * spokes) <
                   std::numeric_limits<std::uint16_t>::max(),
               "too many corners for 16-bit indices");
    SeaDisc disc;
    disc.vertices.reserve((static_cast<std::size_t>(rings) * static_cast<std::size_t>(spokes)) + 1);
    disc.vertices.push_back({});
    for (int ring = 1; ring <= rings; ++ring) {
        for (int spoke = 0; spoke < spokes; ++spoke) {
            const double angle = 2.0 * std::numbers::pi * spoke / spokes;
            disc.vertices.push_back({.out = static_cast<float>(static_cast<double>(ring) / rings),
                                     .towards = Vec2{DVec2{std::cos(angle), std::sin(angle)}}});
        }
    }

    const auto at = [&](int ring, int spoke) {
        return static_cast<std::uint16_t>(1 + ((ring - 1) * spokes) + (spoke % spokes));
    };
    disc.indices.reserve(std::size_t{3} * static_cast<std::size_t>(spokes) *
                         static_cast<std::size_t>((2 * rings) - 1));
    for (int spoke = 0; spoke < spokes; ++spoke) {
        disc.indices.insert(disc.indices.end(), {0, at(1, spoke), at(1, spoke + 1)});
    }
    for (int ring = 1; ring < rings; ++ring) {
        for (int spoke = 0; spoke < spokes; ++spoke) {
            const std::uint16_t inner = at(ring, spoke);
            const std::uint16_t inner_next = at(ring, spoke + 1);
            const std::uint16_t outer = at(ring + 1, spoke);
            const std::uint16_t outer_next = at(ring + 1, spoke + 1);
            disc.indices.insert(disc.indices.end(),
                                {inner, outer, outer_next, inner, outer_next, inner_next});
        }
    }
    return disc;
}

SeaReach sea_reach(const Bend& bend, double water) {
    // Under the sea, the surface overhead is drawn as if from just above it.
    const double above = std::max(bend.eye_height, 1.0);
    const double horizon =
        bend.faked *
        std::atan2(std::sqrt((2.0 * bend.faked * above) + (above * above)), bend.faked);
    return {.nearest = std::max(bend.eye_height - water, 1.0), .furthest = horizon * past_horizon};
}

double sea_along(const SeaReach& reach, double out) {
    return reach.nearest * (std::pow((reach.furthest / reach.nearest) + 1.0, out) - 1.0);
}

DVec3 sea_from_eye(const Bend& bend, double along, DVec3 towards) {
    // On the faked sphere, as far round from under the eye as the ground is along the true one.
    const double turned = along / bend.faked;
    const double half = std::sin(turned / 2.0);
    return (bend.up * ((-2.0 * bend.faked * half * half) - bend.eye_height)) +
           (towards * (bend.faked * std::sin(turned)));
}

double seabed_showing(double eye, double sea, double deepest, double shallowest, double nearest,
                      double farthest, const Optics& water) {
    if (eye <= sea || shallowest >= sea) {
        return 1.0;
    }
    // A line of sight meets every sphere round the centre the more steeply the nearer it passes
    // the centre, and meets the ground beneath no more steeply than it does the sea's surface.
    // Lines to ground further round pass the centre further off, up to where they first dip below
    // its depth; lines to deeper ground nearer.
    const auto passing = [&](double round) {
        round = std::min(round, std::numbers::pi);
        const double half = std::sin(round / 2.0);
        const double apart =
            std::sqrt(((eye - deepest) * (eye - deepest)) + (4.0 * eye * deepest * half * half));
        return eye * deepest * std::sin(round) / apart;
    };
    const double closest = std::min(passing(nearest), passing(farthest));
    const double facing = std::sqrt(std::max(0.0, 1.0 - ((closest / sea) * (closest / sea))));
    const double reflected =
        water_facing_reflection + ((1.0 - water_facing_reflection) * std::pow(1.0 - facing, 5.0));
    const DVec3 lost = (water.dimming + (water.hiding / std::max(facing, water_least_facing))) *
                       (sea - shallowest);
    return (1.0 - reflected) * std::exp(-std::min({lost.x, lost.y, lost.z}));
}

} // namespace hog::terrain
