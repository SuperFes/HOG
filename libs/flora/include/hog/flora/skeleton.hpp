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

// A foliage card on a twig: rooted at `centre`, spanning its frame's local x across and z along,
// facing local y; `seed` cuts its own silhouette.
struct Card {
    Vec3 centre{0.0F};
    Quat frame{1.0F, 0.0F, 0.0F, 0.0F};
    float size = 1.0F;  // along the twig
    float width = 1.0F; // across it
    std::uint32_t seed = 0;
};

// One grown individual, rooted at the origin, z up, in metres.
struct Skeleton {
    std::vector<Bough> boughs;
    std::vector<Card> cards;
};

// Grows the species from the seed. All randomness is spent growing, so meshing the same skeleton
// at any detail carries the same shape.
[[nodiscard]] Skeleton grow(const Species& species, std::uint32_t seed);

} // namespace hog::flora
