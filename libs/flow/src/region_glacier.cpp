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
constexpr std::array<std::pair<int, int>, Glacier::sides> around{
    {{1, 0}, {0, 1}, {-1, 0}, {0, -1}}};
// A drop this far, metres, under a glacier's edge is one icicles hang from.
constexpr double icicle_drop = 3.0;

struct Key {
    std::uint8_t face = 0;
    std::uint32_t i = 0;
    std::uint32_t j = 0;
    friend constexpr auto operator<=>(const Key&, const Key&) noexcept = default;
};

// Each column's top ground and the ice on it.
struct Iced {
    Key key;
    std::uint32_t first = 0;
    std::uint32_t voxels = 0;
    std::int64_t ground = 0;
    std::int64_t ice = 0;
};

// The column across `side` of one, over the cube's edges onto the next face.
std::optional<Key> beside(int level, const Key& key, std::size_t side) {
    const auto next = globe::next_to(
        {.level = static_cast<std::uint8_t>(level), .face = key.face, .i = key.i, .j = key.j},
        around[side].first, around[side].second);
    return next ? std::optional<Key>{Key{.face = next->face, .i = next->i, .j = next->j}}
                : std::nullopt;
}

// Adds the columns of the held bricks `held` with ice lying in the open.
void add_open_ice(const auto& held, std::map<Key, int>& columns) {
    for (const auto& [brick_key, brick] : held) {
        const BrickSite at = globe::brick_at_key(brick_key);
        for (int k = 0; k < globe::brick_voxels; ++k) {
            const Voxel& voxel = brick.voxels[static_cast<std::size_t>(k)];
            if (voxel.grain_amount > 0 && voxel.grain == Material::Ice &&
                voxel.solid == Material::Air) {
                const Site site = globe::site_in(at, k);
                columns.try_emplace(Key{.face = site.face, .i = site.i, .j = site.j}, 0);
            }
        }
    }
}

// Adds every column of the stacks of bricks held.
void add_held(const std::unordered_map<std::uint64_t, std::vector<std::uint32_t>>& stacks,
              std::map<Key, int>& columns) {
    for (const auto& [stack, layers_held] : stacks) {
        const auto face = static_cast<std::uint8_t>(stack >> 42U);
        const auto bi = static_cast<std::uint32_t>((stack >> 21U) & 0x1FFFFFU);
        const auto bj = static_cast<std::uint32_t>(stack & 0x1FFFFFU);
        for (std::uint32_t a = 0; a < globe::brick_side; ++a) {
            for (std::uint32_t b = 0; b < globe::brick_side; ++b) {
                columns.try_emplace(Key{.face = face,
                                        .i = (bi * globe::brick_side) + a,
                                        .j = (bj * globe::brick_side) + b},
                                    0);
            }
        }
    }
}

// Adds the columns beside each of `columns`.
void add_beside(std::map<Key, int>& columns, int level) {
    std::vector<Key> iced;
    iced.reserve(columns.size());
    for (const auto& [key, unused] : columns) {
        iced.push_back(key);
    }
    for (const Key& key : iced) {
        for (std::size_t side = 0; side < Glacier::sides; ++side) {
            if (const auto next = beside(level, key, side)) {
                columns.try_emplace(*next, 0);
            }
        }
    }
}

// Points each side of a column at the one beside it, where that has a top.
void link(std::array<int, Glacier::sides>& next, const Key& key,
          const std::map<Key, std::size_t>& index, int level) {
    for (std::size_t side = 0; side < Glacier::sides; ++side) {
        const auto over = beside(level, key, side);
        const auto found = over ? index.find(*over) : index.end();
        if (found != index.end()) {
            next[side] = static_cast<int>(found->second);
        }
    }
}

// Icicles hang from where the glacier ends at a drop.
void hang_icicles(const Glacier& glacier, const std::vector<Iced>& tops, Stepped& stepped) {
    for (const Glacier::Hang& hang : glacier.hangs(icicle_drop)) {
        const Iced& top = tops[hang.node];
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

} // namespace

bool Region::lay_ice(std::uint8_t face, std::uint32_t i, std::uint32_t j, double metres) {
    const auto stack =
        stacks_.find((std::uint64_t{face} << 42U) | (std::uint64_t{i / globe::brick_side} << 21U) |
                     (j / globe::brick_side));
    if (stack == stacks_.end()) {
        return false;
    }
    const std::uint32_t low = stack->second.front() * globe::brick_side;
    const std::uint32_t high = ((stack->second.back() + 1) * globe::brick_side) - 1;
    const std::vector<Scan> found = floors(face, i, j, low, high);
    if (found.empty()) {
        return false;
    }
    const globe::Surface& top = found.back().surface;
    const double units =
        std::max(metres, 0.0) / world_->layers().thickness * static_cast<double>(full);
    fill_grain(
        {.face = face, .i = i, .j = j, .layer = top.first}, top.open,
        std::min(static_cast<std::int64_t>(std::llround(units)), std::int64_t{top.open} * full),
        Material::Ice);
    return true;
}

Stepped Region::age_ice(double years) {
    ++steps_;
    Stepped stepped;
    glaciate(stepped, years);
    return stepped;
}

void Region::glaciate(Stepped& stepped, double years) {
    const globe::Layers& layers = world_->layers();
    const double thickness = layers.thickness;

    // The columns with ice lying in the open, and those beside them for it to flow into.
    std::map<Key, int> columns;
    add_open_ice(held_, columns);
    if (ice_balance_) {
        add_held(stacks_, columns);
    }
    if (columns.empty()) {
        return;
    }
    add_beside(columns, layers.level);

    std::vector<Iced> tops;
    std::map<Key, std::size_t> index;
    for (const auto& [key, unused] : columns) {
        const std::vector<Scan> floors_here = column_floors(key.face, key.i, key.j, stepped);
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
    const double width = std::sqrt(
        layers.volume(globe::column_of(base, layers), static_cast<int>(base.layer)) / thickness);
    Glacier glacier{tops.size(), {.cell = width}};
    for (std::size_t n = 0; n < tops.size(); ++n) {
        glacier[n].rock = metres(tops[n].ground);
        glacier[n].ice = metres(tops[n].ice);
        link(glacier[n].next, tops[n].key, index, layers.level);
    }
    if (ice_balance_) {
        for (std::size_t n = 0; n < tops.size(); ++n) {
            const double change = ice_balance_({.level = static_cast<std::uint8_t>(layers.level),
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

    // Written back as whole units, to what each column has room for, rounded along the columns'
    // running total from one draw: each column rounds up as often as its fraction says, and
    // together they keep the glacier's total.
    const double offset = drawn(base, 3);
    double total = 0.0;
    std::int64_t given = 0;
    for (std::size_t n = 0; n < tops.size(); ++n) {
        const Site column{.face = tops[n].key.face,
                          .i = tops[n].key.i,
                          .j = tops[n].key.j,
                          .layer = tops[n].first};
        total += glacier[n].ice / thickness * static_cast<double>(full);
        const auto whole = static_cast<std::int64_t>(std::floor(total + offset));
        const auto units = std::min(whole - given, std::int64_t{tops[n].voxels} * full);
        given = whole;
        set_ice(column, tops[n].voxels, bed[n] - glacier[n].rock, tops[n].ice, units, stepped);
    }
    hang_icicles(glacier, tops, stepped);
}

void Region::set_ice(Site column, std::uint32_t voxels, double worn, std::int64_t before,
                     std::int64_t units, Stepped& stepped) {
    // The bed under ice is worn down, in millimetres dithered so slow wear still happens.
    if (worn > 0.0) {
        if (const std::int64_t mm = dithered(worn * 1000.0, column, 4); mm > 0) {
            scour(column, static_cast<int>(mm));
        }
    }
    if (units == before) {
        return;
    }
    stepped.glaciated += std::abs(units - before);
    fill_grain(column, voxels, units, Material::Ice);
}

} // namespace hog::flow
