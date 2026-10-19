#pragma once

#include <hog/core/math.hpp>
#include <hog/flora/species.hpp>

#include <cstdint>
#include <vector>

namespace hog::flora {

// A ring of a branch: where it lies, how it faces (its local z the branch's heading), how wide.
struct Ring {
    Vec3 centre{0.0F};
    Quat frame{1.0F, 0.0F, 0.0F, 0.0F};
    float radius = 0.0F;
};

struct Bough {
    int level = 0;
    int sides = 3; // vertices around each ring, once meshed
    std::vector<Ring> rings;
};

// The needle mass of one twig: two solid blades crossed about its spine, each rolled off flat its
// own way, widest at the base and thinning to nothing by the tip, as pine growth thins outward;
// `seed` streaks its own needles.
struct Frond {
    std::vector<Ring> spine;
    float width = 0.3F;    // half-width at the base, metres
    float tilt = 0.5F;     // radians each blade rolls off flat, opposite ways
    float fullness = 0.0F; // as Species::frond_fullness
    std::uint32_t seed = 0;
};

// One grown individual, rooted at the origin, z up, in metres.
struct Skeleton {
    std::vector<Bough> boughs;
    std::vector<Frond> fronds;
};

// Grows the species from the seed. All randomness is spent growing, so meshing the same skeleton
// at any detail carries the same shape.
[[nodiscard]] Skeleton grow(const Species& species, std::uint32_t seed);

} // namespace hog::flora
