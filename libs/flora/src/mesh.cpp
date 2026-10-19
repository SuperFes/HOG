#include "trig.hpp"

#include <hog/flora/mesh.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace hog::flora {

namespace {

// The rings a bough keeps at this stride: the first and last always, so junctions and tips hold.
std::vector<std::size_t> kept_rings(std::size_t rings, int stride) {
    std::vector<std::size_t> kept;
    const auto step = static_cast<std::size_t>(std::max(stride, 1));
    for (std::size_t ring = 0; ring < rings; ring += step) {
        kept.push_back(ring);
    }
    if (kept.back() != rings - 1) {
        kept.push_back(rings - 1);
    }
    return kept;
}

void mesh_bough(const Bough& bough, const Detail& detail, TreeMesh& grown) {
    if (bough.rings.size() < 2) {
        return;
    }
    const int sides =
        std::max(3, static_cast<int>(std::lround(static_cast<float>(bough.sides) * detail.sides)));
    const std::vector<std::size_t> kept = kept_rings(bough.rings.size(), detail.ring_stride);
    const auto first = static_cast<std::uint32_t>(grown.wood.size());
    for (const std::size_t at : kept) {
        const Ring& ring = bough.rings[at];
        for (int side = 0; side < sides; ++side) {
            const float turn = (2.0F * std::numbers::pi_v<float>)*(static_cast<float>(side) /
                                                                   static_cast<float>(sides));
            const Vec3 out = ring.frame * Vec3{turning_cos(turn), turning_sin(turn), 0.0F};
            grown.wood.push_back({.position = ring.centre + (out * ring.radius), .normal = out});
        }
    }
    const auto around = static_cast<std::uint32_t>(sides);
    for (std::uint32_t band = 0; band + 1 < kept.size(); ++band) {
        const std::uint32_t low = first + (band * around);
        const std::uint32_t high = low + around;
        for (std::uint32_t side = 0; side < around; ++side) {
            const std::uint32_t next = (side + 1) % around;
            grown.wood_indices.insert(
                grown.wood_indices.end(),
                {low + side, high + side, low + next, low + next, high + side, high + next});
        }
    }
}

// A twig's foliage as two solid blades crossed about its spine, each rolled off flat its own way:
// dense from above, still a blade from the side. Needles thin evenly from the base, as pine growth
// thins outward; leaves hold full and round off at the tip.
void mesh_frond(const Frond& frond, const Detail& detail, TreeMesh& grown) {
    if (frond.spine.size() < 2) {
        return;
    }
    constexpr Vec3 world_up{0.0F, 0.0F, 1.0F};
    const std::vector<std::size_t> kept = kept_rings(frond.spine.size(), detail.ring_stride);
    const auto first = static_cast<std::uint32_t>(grown.leaves.size());
    const auto last = static_cast<float>(frond.spine.size() - 1);
    const float swing = turning_cos(frond.tilt);
    const float raise = turning_sin(frond.tilt);
    for (const std::size_t at : kept) {
        const Ring& ring = frond.spine[at];
        const float along = static_cast<float>(at) / last;
        const Vec3 heading = ring.frame * world_up;
        Vec3 flat = glm::cross(world_up, heading);
        if (glm::dot(flat, flat) < 1e-6F) {
            // The leader's tip frond points straight up; any side is its flat one.
            flat = glm::cross(Vec3{1.0F, 0.0F, 0.0F}, heading);
        }
        flat = glm::normalize(flat);
        const Vec3 lift = glm::cross(heading, flat);
        const float width =
            frond.width * detail.frond_scale * (1.0F - along) * (1.0F + (frond.fullness * along));
        const std::array<Vec3, 2> blades{(flat * swing) + (lift * raise),
                                         (flat * swing) - (lift * raise)};
        for (const Vec3& blade : blades) {
            const Vec3 normal = glm::normalize(glm::cross(heading, blade));
            grown.leaves.push_back({.position = ring.centre - (blade * width),
                                    .normal = normal,
                                    .card = {-0.5F, along},
                                    .seed = frond.seed});
            grown.leaves.push_back({.position = ring.centre + (blade * width),
                                    .normal = normal,
                                    .card = {0.5F, along},
                                    .seed = frond.seed});
        }
    }
    for (std::uint32_t band = 0; band + 1 < kept.size(); ++band) {
        const std::uint32_t low = first + (band * 4);
        const std::uint32_t high = low + 4;
        for (std::uint32_t blade = 0; blade < 4; blade += 2) {
            grown.leaf_indices.insert(grown.leaf_indices.end(),
                                      {low + blade, high + blade, low + blade + 1, low + blade + 1,
                                       high + blade, high + blade + 1});
        }
    }
}

} // namespace

TreeMesh mesh(const Skeleton& skeleton, const Detail& detail) {
    TreeMesh grown;
    for (const Bough& bough : skeleton.boughs) {
        if (bough.level < detail.wood_levels) {
            mesh_bough(bough, detail, grown);
        }
    }
    const auto frond_step = static_cast<std::size_t>(std::max(detail.frond_stride, 1));
    for (std::size_t at = 0; at < skeleton.fronds.size(); at += frond_step) {
        mesh_frond(skeleton.fronds[at], detail, grown);
    }
    return grown;
}

} // namespace hog::flora
