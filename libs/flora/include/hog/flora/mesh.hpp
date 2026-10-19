#pragma once

#include <hog/core/math.hpp>
#include <hog/flora/skeleton.hpp>

#include <cstdint>
#include <vector>

namespace hog::flora {

struct WoodVertex {
    Vec3 position{0.0F};
    Vec3 normal{0.0F};
};

struct LeafVertex {
    Vec3 position{0.0F};
    Vec3 normal{0.0F};
    glm::vec2 card{0.0F}; // -0.5 to 0.5 across the frond, 0 at its spine; 0 to 1 along it
    std::uint32_t seed = 0;
};

// How much of the skeleton a mesh keeps; coarser for the farther levels of detail.
struct Detail {
    int ring_stride = 1;      // every nth ring kept, the first and last always
    float sides = 1.0F;       // of each ring's vertices kept, never below 3
    int frond_stride = 1;     // every nth frond kept
    float frond_scale = 1.0F; // kept fronds widened to cover for those dropped
    // Branching levels meshed as wood; twigs past it are left to their fronds to cover.
    int wood_levels = 8;
};

// The branches as open tapered tubes, the foliage as solid tapered blades, one a twig.
struct TreeMesh {
    std::vector<WoodVertex> wood;
    std::vector<std::uint32_t> wood_indices;
    std::vector<LeafVertex> leaves;
    std::vector<std::uint32_t> leaf_indices;
};

// Meshes the skeleton; deterministic, spending no randomness, so every detail of one skeleton
// keeps the same silhouette.
[[nodiscard]] TreeMesh mesh(const Skeleton& skeleton, const Detail& detail = {});

} // namespace hog::flora
