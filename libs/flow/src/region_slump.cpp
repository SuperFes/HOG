#include <hog/flow/region.hpp>
#include <hog/flow/slump.hpp>
#include <hog/globe/surface.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

namespace hog::flow {
namespace {

using globe::BrickSite;
using globe::Material;
using globe::Site;
using globe::Voxel;

constexpr std::int64_t full = Voxel::full;
// Passes of the rule a step makes, and the most voxels up a column grain is given room in.
constexpr int passes = 4;
constexpr std::uint32_t tallest = 64;
constexpr std::array<std::pair<int, int>, Slump::sides> around{
    {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}}};

struct Key {
    std::uint8_t face = 0;
    std::uint32_t i = 0;
    std::uint32_t j = 0;
    friend constexpr auto operator<=>(const Key&, const Key&) noexcept = default;
};

// A column's top ground and the grain lying on it.
struct Profile {
    std::uint32_t first = 0; // the lowest voxel the grain may fill
    std::int64_t ground = 0;
    std::int64_t grain = 0;
    std::int64_t room = 0;
    std::int64_t rise = 0;
    Material material = Material::Air;
    std::uint32_t voxels = 0; // how many up from `first` hold room
};

} // namespace

void Region::slump(Stepped& stepped) {
    const globe::Layers& layers = world_->store().layers();
    const double thickness = layers.thickness;
    const double gravity = world_->generator().planet().gravity;

    // The columns with grain in the open.
    std::map<Key, int> columns;
    for (const auto& [brick_key, brick] : held_) {
        const BrickSite at = globe::brick_at_key(brick_key);
        for (int k = 0; k < globe::brick_voxels; ++k) {
            const Voxel& voxel = brick.voxels[static_cast<std::size_t>(k)];
            if (voxel.grain_amount > 0 && voxel.solid == Material::Air) {
                const Site site = globe::site_in(at, k);
                columns.try_emplace(Key{site.face, site.i, site.j}, 0);
            }
        }
    }
    if (columns.empty()) {
        return;
    }
    // And those beside them, which the grain may slide into.
    const std::vector<Key> grained = [&] {
        std::vector<Key> keys;
        for (const auto& [key, unused] : columns) {
            keys.push_back(key);
        }
        return keys;
    }();
    for (const Key& key : grained) {
        for (const auto& [di, dj] : around) {
            if (const auto next = beside({key.face, key.i, key.j, 0}, di, dj)) {
                columns.try_emplace(Key{next->face, next->i, next->j}, 0);
            }
        }
    }

    // Each column's ground and grain, where it has a top surface to lie on.
    std::vector<Key> keys;
    std::vector<Profile> profiles;
    for (auto& [key, node] : columns) {
        const auto stack = stacks_.find(
            (std::uint64_t{key.face} << 42U) | (std::uint64_t{key.i / globe::brick_side} << 21U) |
            (key.j / globe::brick_side));
        if (stack == stacks_.end()) {
            node = Slump::none;
            continue;
        }
        // Down from the top brick, through bricks held without a gap, to the highest shaped
        // matter with open voxel over it.
        const auto lookup = [&](std::uint32_t layer) {
            return voxel({key.face, key.i, key.j, layer});
        };
        const std::optional<globe::Surface> surface = globe::surface_below(
            ((stack->second.back() + 1) * globe::brick_side) - 1, globe::any_depth, tallest,
            lookup);
        if (!surface) {
            node = Slump::none;
            continue;
        }

        Profile profile{.first = surface->first,
                        .grain = surface->grain_amount,
                        .room = std::int64_t{surface->open} * full,
                        .material = surface->grain,
                        .voxels = surface->open};
        const Site base{.face = key.face, .i = key.i, .j = key.j, .layer = surface->first};
        // The shaped surface lies below the voxel's middle by its distance, along the normal.
        double offset = 0.0;
        if (surface->bottom.distance != Voxel::far) {
            const DVec3 normal = globe::unpacked_normal(surface->bottom.normal);
            const DVec3 up = glm::normalize(globe::middle_of(base, layers));
            const double lean = std::max(glm::dot(normal, up), 0.3);
            offset = globe::metres(surface->bottom.distance) / thickness / lean;
        }
        profile.ground = static_cast<std::int64_t>(std::llround(
            (static_cast<double>(surface->first) + 0.5 - offset) * static_cast<double>(full)));
        if (surface->grain != Material::Air) {
            const double width = std::sqrt(layers.volume(globe::column_of(base, layers),
                                                         static_cast<int>(surface->first)) /
                                           thickness);
            profile.rise = static_cast<std::int64_t>(
                std::llround(surface->holds(thickness, gravity) * width / thickness *
                             static_cast<double>(full)));
        }
        node = static_cast<int>(keys.size());
        keys.push_back(key);
        profiles.push_back(profile);
    }

    Slump rule{keys.size()};
    for (std::size_t n = 0; n < keys.size(); ++n) {
        Slump::Node& node = rule[n];
        node.ground = profiles[n].ground;
        node.grain = profiles[n].grain;
        node.rise = profiles[n].rise;
        node.room = profiles[n].room;
        for (std::size_t s = 0; s < Slump::sides; ++s) {
            const auto next = columns.find(Key{.face = keys[n].face,
                                               .i = keys[n].i + static_cast<std::uint32_t>(
                                                                    around[s].first),
                                               .j = keys[n].j + static_cast<std::uint32_t>(
                                                                    around[s].second)});
            // A neighbour beside the face's edge or past what's held is no column to slide to.
            // Any drop is one, however deep: grain pours into pits and over cliffs.
            if (next != columns.end() && next->second != Slump::none) {
                node.next[s] = next->second;
            }
        }
    }
    std::int64_t moved = 0;
    for (int pass = 0; pass < passes; ++pass) {
        const std::int64_t now = rule.step();
        if (now == 0) {
            break;
        }
        moved += now;
    }
    if (moved == 0) {
        return;
    }
    stepped.slumped += moved;

    // Write each column's grain back, filling its voxels from the ground up. A column that gains
    // grain where it had none takes the material of the heaviest neighbour that had some.
    for (std::size_t n = 0; n < keys.size(); ++n) {
        const std::int64_t depth = rule[n].grain;
        if (depth == profiles[n].grain) {
            continue;
        }
        Material material = profiles[n].material;
        if (material == Material::Air) {
            std::int64_t most = 0;
            for (std::size_t s = 0; s < Slump::sides; ++s) {
                const int next = rule[n].next[s];
                if (next != Slump::none && profiles[static_cast<std::size_t>(next)].grain > most) {
                    most = profiles[static_cast<std::size_t>(next)].grain;
                    material = profiles[static_cast<std::size_t>(next)].material;
                }
            }
        }
        std::int64_t left = depth;
        for (std::uint32_t up = 0; up < profiles[n].voxels; ++up) {
            const Site site{.face = keys[n].face,
                            .i = keys[n].i,
                            .j = keys[n].j,
                            .layer = profiles[n].first + up};
            Voxel* here = voxel(site);
            const std::int64_t amount = std::min(left, full);
            left -= amount;
            if (here->grain_amount != amount ||
                (amount > 0 && here->grain != material)) {
                here->grain = amount > 0 ? material : Material::Air;
                here->grain_amount = static_cast<std::uint16_t>(amount);
                held(globe::brick_of(site))->dirty = true;
            }
        }
    }
}

} // namespace hog::flow
