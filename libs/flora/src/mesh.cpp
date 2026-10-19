#include "trig.hpp"

#include <hog/flora/mesh.hpp>

#include <algorithm>
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

void mesh_card(const Card& card, float scale, TreeMesh& grown) {
    const Vec3 across = card.frame * Vec3{card.width * scale, 0.0F, 0.0F};
    const Vec3 along = card.frame * Vec3{0.0F, 0.0F, card.size * scale};
    const Vec3 normal = card.frame * Vec3{0.0F, 1.0F, 0.0F};
    const auto first = static_cast<std::uint32_t>(grown.leaves.size());
    grown.leaves.push_back({.position = card.centre - (across * 0.5F),
                            .normal = normal,
                            .card = {-0.5F, 0.0F},
                            .seed = card.seed});
    grown.leaves.push_back({.position = card.centre + (across * 0.5F),
                            .normal = normal,
                            .card = {0.5F, 0.0F},
                            .seed = card.seed});
    grown.leaves.push_back({.position = card.centre - (across * 0.5F) + along,
                            .normal = normal,
                            .card = {-0.5F, 1.0F},
                            .seed = card.seed});
    grown.leaves.push_back({.position = card.centre + (across * 0.5F) + along,
                            .normal = normal,
                            .card = {0.5F, 1.0F},
                            .seed = card.seed});
    grown.leaf_indices.insert(grown.leaf_indices.end(),
                              {first, first + 1, first + 2, first + 2, first + 1, first + 3});
}

} // namespace

TreeMesh mesh(const Skeleton& skeleton, const Detail& detail) {
    TreeMesh grown;
    for (const Bough& bough : skeleton.boughs) {
        if (bough.level < detail.wood_levels) {
            mesh_bough(bough, detail, grown);
        }
    }
    const auto card_step = static_cast<std::size_t>(std::max(detail.card_stride, 1));
    for (std::size_t at = 0; at < skeleton.cards.size(); at += card_step) {
        mesh_card(skeleton.cards[at], detail.card_scale, grown);
    }
    return grown;
}

} // namespace hog::flora
