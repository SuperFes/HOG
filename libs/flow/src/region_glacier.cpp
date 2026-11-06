#include <hog/flow/glacier.hpp>
#include <hog/flow/region.hpp>

#include <algorithm>
#include <cmath>
#include <map>

namespace hog::flow {
namespace {

using globe::BrickSite;
using globe::Material;
using globe::Site;
using globe::Voxel;

constexpr std::int64_t full = Voxel::full;
// The sides of a glacier's column in the order Glacier names them: +i, +j, -i, -j.
constexpr std::array<std::pair<int, int>, Glacier::sides> around{{{1, 0}, {0, 1}, {-1, 0}, {0, -1}}};
// A drop this far, metres, under a glacier's edge is one icicles hang from.
constexpr double icicle_drop = 3.0;

struct Key {
    std::uint8_t face = 0;
    std::uint32_t i = 0;
    std::uint32_t j = 0;
    friend constexpr auto operator<=>(const Key&, const Key&) noexcept = default;
};

} // namespace

Stepped Region::age_ice(double years) {
    ++steps_;
    Stepped stepped;
    glaciate(stepped, years);
    return stepped;
}

void Region::glaciate(Stepped& stepped, double years) {
    const globe::Layers& layers = world_->store().layers();
    const double thickness = layers.thickness;

    // The columns with ice lying in the open, and those beside them for it to flow into.
    std::map<Key, int> columns;
    for (const auto& [brick_key, brick] : held_) {
        const BrickSite at = globe::brick_at_key(brick_key);
        for (int k = 0; k < globe::brick_voxels; ++k) {
            const Voxel& voxel = brick.voxels[static_cast<std::size_t>(k)];
            if (voxel.grain_amount > 0 && voxel.grain == Material::Ice &&
                voxel.solid == Material::Air) {
                const Site site = globe::site_in(at, k);
                columns.try_emplace(Key{site.face, site.i, site.j}, 0);
            }
        }
    }
    if (ice_balance_) {
        for (const auto& [stack, layers_held] : stacks_) {
            const auto face = static_cast<std::uint8_t>(stack >> 42U);
            const auto bi = static_cast<std::uint32_t>((stack >> 21U) & 0x1FFFFFU);
            const auto bj = static_cast<std::uint32_t>(stack & 0x1FFFFFU);
            for (std::uint32_t a = 0; a < globe::brick_side; ++a) {
                for (std::uint32_t b = 0; b < globe::brick_side; ++b) {
                    columns.try_emplace(
                        Key{face, (bi * globe::brick_side) + a, (bj * globe::brick_side) + b}, 0);
                }
            }
        }
    }
    if (columns.empty()) {
        return;
    }
    const std::vector<Key> iced = [&] {
        std::vector<Key> keys;
        for (const auto& [key, unused] : columns) {
            keys.push_back(key);
        }
        return keys;
    }();
    const auto beside = [&](const Key& key, std::size_t side) -> std::optional<Key> {
        const auto next = globe::next_to({.level = static_cast<std::uint8_t>(layers.level),
                                          .face = key.face,
                                          .i = key.i,
                                          .j = key.j},
                                         around[side].first, around[side].second);
        return next ? std::optional<Key>{Key{next->face, next->i, next->j}} : std::nullopt;
    };
    for (const Key& key : iced) {
        for (std::size_t side = 0; side < Glacier::sides; ++side) {
            if (const auto next = beside(key, side)) {
                columns.try_emplace(*next, 0);
            }
        }
    }

    // Each column's top ground and the ice on it.
    struct Top {
        Key key;
        std::uint32_t first = 0;
        std::uint32_t voxels = 0;
        std::int64_t ground = 0;
        std::int64_t ice = 0;
    };
    std::vector<Top> tops;
    std::map<Key, std::size_t> index;
    for (const auto& [key, unused] : columns) {
        const auto stack = stacks_.find((std::uint64_t{key.face} << 42U) |
                                        (std::uint64_t{key.i / globe::brick_side} << 21U) |
                                        (key.j / globe::brick_side));
        if (stack == stacks_.end()) {
            continue;
        }
        const std::uint32_t low = stack->second.front() * globe::brick_side;
        const std::uint32_t high = ((stack->second.back() + 1) * globe::brick_side) - 1;
        // Open to the bottom of what's held: the ground under it isn't, so ask for it.
        const Voxel* bottom = voxel({key.face, key.i, key.j, low});
        if (bottom != nullptr && bottom->solid == Material::Air) {
            if (const auto missing =
                    unheld_below(globe::middle_of({key.face, key.i, key.j, low}, layers))) {
                stepped.wanted.push_back(*missing);
            }
        }
        const std::vector<Scan> floors_here = floors(key.face, key.i, key.j, low, high);
        if (floors_here.empty()) {
            continue;
        }
        const Scan& top = floors_here.back();
        const bool iced_here = top.surface.grain == Material::Ice;
        index.emplace(key, tops.size());
        tops.push_back({.key = key,
                        .first = top.surface.first,
                        .voxels = top.surface.open,
                        .ground = top.ground,
                        .ice = iced_here ? std::int64_t{top.surface.grain_amount} : 0});
    }
    // Aging by nothing only asks for the ground it needs.
    if (tops.empty() || years <= 0.0) {
        return;
    }

    const auto metres = [&](std::int64_t units) {
        return static_cast<double>(units) / static_cast<double>(full) * thickness;
    };
    const Site base{.face = tops.front().key.face,
                    .i = tops.front().key.i,
                    .j = tops.front().key.j,
                    .layer = tops.front().first};
    const double width =
        std::sqrt(layers.volume(globe::column_of(base, layers), static_cast<int>(base.layer)) /
                  thickness);
    Glacier glacier{tops.size(), {.cell = width}};
    for (std::size_t n = 0; n < tops.size(); ++n) {
        glacier[n].rock = metres(tops[n].ground);
        glacier[n].ice = metres(tops[n].ice);
        for (std::size_t side = 0; side < Glacier::sides; ++side) {
            const auto over = beside(tops[n].key, side);
            const auto found = over ? index.find(*over) : index.end();
            if (found != index.end()) {
                glacier[n].next[side] = static_cast<int>(found->second);
            }
        }
    }
    if (ice_balance_) {
        for (std::size_t n = 0; n < tops.size(); ++n) {
            const double change = ice_balance_(
                {.level = static_cast<std::uint8_t>(layers.level),
                 .face = tops[n].key.face,
                 .i = tops[n].key.i,
                 .j = tops[n].key.j},
                layers.floor + glacier[n].rock + glacier[n].ice);
            glacier[n].ice = std::max(glacier[n].ice + (change * years), 0.0);
        }
    }
    std::vector<double> bed(tops.size());
    for (std::size_t n = 0; n < tops.size(); ++n) {
        bed[n] = glacier[n].rock;
    }
    for (double left = years; left > 1.0e-9;) {
        left -= glacier.step(left);
    }

    // Written back as whole units, to what each column has room for.
    for (std::size_t n = 0; n < tops.size(); ++n) {
        const Site column{.face = tops[n].key.face,
                           .i = tops[n].key.i,
                           .j = tops[n].key.j,
                           .layer = tops[n].first};
        // The bed under ice is worn down, in millimetres dithered so slow wear still happens.
        if (const double worn = bed[n] - glacier[n].rock; worn > 0.0) {
            if (const std::int64_t mm = dithered(worn * 1000.0, column, 4); mm > 0) {
                scour(column, static_cast<int>(mm));
            }
        }
        const auto units = std::min(
            dithered(glacier[n].ice / thickness * static_cast<double>(full), column, 3),
            std::int64_t{tops[n].voxels} * full);
        if (units == tops[n].ice) {
            continue;
        }
        stepped.glaciated += std::abs(units - tops[n].ice);
        fill_grain(column, tops[n].voxels, units, Material::Ice);
    }
    for (const Glacier::Hang& hang : glacier.hangs(icicle_drop)) {
        const Top& top = tops[hang.node];
        stepped.icicles.push_back(
            {.face = top.key.face,
             .i = top.key.i,
             .j = top.key.j,
             .layer = top.first + static_cast<std::uint32_t>(
                                      std::max<std::int64_t>(0, (top.ice + full - 1) / full)),
             .side = static_cast<std::uint8_t>(hang.side),
             .drop = hang.drop});
    }
}

} // namespace hog::flow
