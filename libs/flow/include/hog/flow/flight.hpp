#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/material.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace hog::flow {

// A lump of grain in motion: knocked off an edge, falling, bouncing or sliding down a slope. It
// is the region's, not the voxels': it lands in a column's grain when it comes to rest.
struct Parcel {
    std::uint32_t id = 0;
    DVec3 at{};       // metres from the planet's centre
    DVec3 velocity{}; // metres a second
    globe::Material material = globe::Material::Air;
    std::int64_t amount = 0; // of Voxel::full, as grain is counted
    double age = 0.0;        // seconds in motion
    // Metres across a boulder, the rock of a single block rolling on; none for loose grain.
    double radius = 0.0;
};

// The surface under a parcel.
struct Ground {
    double radius = 0.0; // metres from the centre, grain on it included
    DVec3 normal{};      // unit, out of the surface
    double holds = 0.0;  // tangent of the steepest slope that keeps it
};

// How a parcel moves, by the same friction that holds a heap: gravity pulls it along the radius;
// on the ground it rebounds by its material's bounce, loses speed to friction as the ground holds
// it, and slides on while the slope is steeper than that, until it stops.
class Flight {
public:
    // Seconds in motion after which a parcel is taken to have stopped wherever it is.
    static constexpr double longest = 30.0;
    // A rebound slower than this, in metres a second, is a rest.
    static constexpr double dead = 0.5;

    enum class Fate : std::uint8_t { Flying, Landed };

    // Moves `parcel` `seconds` on. `ground(at)` gives the surface under a point, or none where
    // there is nothing to land on, which stops the parcel where it was. `reach` bounds how far a
    // parcel goes between looks at the ground.
    template <typename Query>
    static Fate advance(Parcel& parcel, double seconds, double gravity, double reach,
                        const Query& ground);
};

template <typename Query>
Flight::Fate Flight::advance(Parcel& parcel, double seconds, double gravity, double reach,
                             const Query& ground) {
    const double bounce = globe::properties(parcel.material).bounce;
    const double speed = glm::length(parcel.velocity) + (gravity * seconds);
    const int slices = std::clamp(static_cast<int>(std::ceil(speed * seconds / reach)), 1, 64);
    const double h = seconds / slices;

    for (int slice = 0; slice < slices; ++slice) {
        parcel.age += h;
        const DVec3 up = glm::normalize(parcel.at);
        const DVec3 pull = -up * gravity;
        parcel.velocity += pull * h;
        const DVec3 next = parcel.at + (parcel.velocity * h);
        const std::optional<Ground> below = ground(next);
        if (!below) {
            return Fate::Landed;
        }
        if (glm::length(next) > below->radius) {
            parcel.at = next;
            continue;
        }

        parcel.at = glm::normalize(next) * below->radius;
        const DVec3& n = below->normal;
        const double into = glm::dot(parcel.velocity, n);
        DVec3 along = parcel.velocity - (n * into);
        double off = 0.0;
        double grip = below->holds;
        // Pressing in at the pace gravity alone gives in a slice is resting, not an impact.
        if (-into > dead) {
            off = -into * bounce;
            if (off < dead) {
                off = 0.0;
            }
            // The impact's share of friction: Coulomb's, from the momentum pressed into it.
            const double dragged = grip * (1.0 + bounce) * -into;
            const double slow = glm::length(along);
            along = slow > dragged ? along * ((slow - dragged) / slow) : DVec3{};
        }
        // Resting on it, the weight on the surface is the pull's part along the normal.
        const double weight = std::max(-glm::dot(pull, n), 0.0);
        const DVec3 slide = pull - (n * glm::dot(pull, n));
        if (off == 0.0) {
            const double slow = glm::length(along);
            const double drag = grip * weight * h;
            along = slow > drag ? along * ((slow - drag) / slow) : DVec3{};
            if (glm::length(along) == 0.0 && glm::length(slide) <= grip * weight) {
                parcel.velocity = {};
                return Fate::Landed;
            }
        }
        parcel.velocity = along + (n * off);
    }
    return parcel.age >= longest ? Fate::Landed : Fate::Flying;
}

} // namespace hog::flow
