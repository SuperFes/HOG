#include <hog/flow/creep.hpp>
#include <hog/flow/region.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace hog::flow {
namespace {

using globe::Material;
using globe::Site;
using globe::Voxel;

constexpr std::int64_t full = Voxel::full;
// The sides of a column in the order Creep names them: +i, +j, -i, -j.
constexpr std::array<std::pair<int, int>, Creep::sides> around{{{1, 0}, {0, 1}, {-1, 0}, {0, -1}}};

struct Key {
    std::uint8_t face = 0;
    std::uint32_t i = 0;
    std::uint32_t j = 0;
    friend constexpr auto operator<=>(const Key&, const Key&) noexcept = default;
};

// Whether a material lies as a heap and can be moved by creep.
bool loose(Material material) {
    return material != Material::Air && globe::properties(material).repose > 0.0;
}

// Points each side of a column at the one beside it, where that has cover.
void link(std::array<int, Creep::sides>& next, const Key& key,
          const std::map<Key, std::size_t>& index, int level) {
    for (std::size_t side = 0; side < Creep::sides; ++side) {
        const auto over = globe::next_to(
            {.level = static_cast<std::uint8_t>(level), .face = key.face, .i = key.i, .j = key.j},
            around[side].first, around[side].second);
        const auto found =
            over ? index.find(Key{.face = over->face, .i = over->i, .j = over->j}) : index.end();
        if (found != index.end()) {
            next[side] = static_cast<int>(found->second);
        }
    }
}

} // namespace

Stepped Region::age_cover(double years) {
    ++steps_;
    Stepped stepped;
    creep(stepped, years);
    return stepped;
}

struct Region::Covered {
    std::uint8_t face = 0;
    std::uint32_t i = 0;
    std::uint32_t j = 0;
    std::uint32_t first = 0;
    std::uint32_t voxels = 0;
    std::int64_t ground = 0;
    std::int64_t grain = 0;
    std::int64_t bed = 0;
    Material material = Material::Soil;
};

void Region::creep(Stepped& stepped, double years) {
    const globe::Layers& layers = world_->layers();
    const double thickness = layers.thickness;
    const auto metres = [&](std::int64_t units) {
        return static_cast<double>(units) / static_cast<double>(full) * thickness;
    };

    std::vector<Covered> tops;
    std::map<Key, std::size_t> index;
    for (const auto& [stack, layers_held] : stacks_) {
        const auto face = static_cast<std::uint8_t>(stack >> 42U);
        const auto bi = static_cast<std::uint32_t>((stack >> 21U) & 0x1FFFFFU);
        const auto bj = static_cast<std::uint32_t>(stack & 0x1FFFFFU);
        const std::uint32_t low = layers_held.front() * globe::brick_side;
        const std::uint32_t high = ((layers_held.back() + 1) * globe::brick_side) - 1;
        for (std::uint32_t a = 0; a < globe::brick_side; ++a) {
            for (std::uint32_t b = 0; b < globe::brick_side; ++b) {
                const std::uint32_t i = (bi * globe::brick_side) + a;
                const std::uint32_t j = (bj * globe::brick_side) + b;
                if (const std::optional<Covered> made = covered(face, i, j, low, high, stepped)) {
                    index.emplace(Key{.face = face, .i = i, .j = j}, tops.size());
                    tops.push_back(*made);
                }
            }
        }
    }
    // Aging by nothing only asks for the ground it needs.
    if (tops.empty() || years <= 0.0) {
        return;
    }
    const Site base{.face = tops.front().face,
                    .i = tops.front().i,
                    .j = tops.front().j,
                    .layer = tops.front().first};
    const double width = std::sqrt(
        layers.volume(globe::column_of(base, layers), static_cast<int>(base.layer)) / thickness);
    Creep cover{tops.size(), {.cell = width}};
    for (std::size_t n = 0; n < tops.size(); ++n) {
        cover[n].bed = metres(tops[n].bed);
        cover[n].cover = metres(tops[n].ground + tops[n].grain - tops[n].bed);
        link(cover[n].next, Key{.face = tops[n].face, .i = tops[n].i, .j = tops[n].j}, index,
             layers.level);
    }
    std::vector<double> before(tops.size());
    for (std::size_t n = 0; n < tops.size(); ++n) {
        before[n] = cover[n].cover;
    }
    cover.weather(years);
    for (double left = years; left > 1.0e-9;) {
        left -= cover.step(left);
    }

    // Written back as whole units.
    for (std::size_t n = 0; n < tops.size(); ++n) {
        recover(tops[n], cover[n].cover - before[n], stepped);
    }
}

std::optional<Region::Covered> Region::covered(std::uint8_t face, std::uint32_t i, std::uint32_t j,
                                               std::uint32_t low, std::uint32_t high,
                                               Stepped& stepped) {
    // Open to the bottom of what's held: the ground under it isn't, so ask for it.
    ask_under(face, i, j, low, stepped);
    const std::vector<Scan> found = floors(face, i, j, low, high);
    if (found.empty()) {
        return std::nullopt;
    }
    const Scan& top = found.back();
    Covered made{.face = face,
                 .i = i,
                 .j = j,
                 .first = top.surface.first,
                 .voxels = top.surface.open,
                 .ground = top.ground,
                 .grain = top.surface.grain_amount,
                 .bed = top.ground};
    if (loose(top.surface.grain)) {
        made.material = top.surface.grain;
    } else if (top.surface.grain != Material::Air) {
        // Grain that isn't loose, like ice, is not cover to be crept.
        return std::nullopt;
    }
    // Down through the loose solid under the ground, to the first that isn't. What creeps is
    // what's on top: the grain, else the loose ground just under it.
    bool named = loose(top.surface.grain);
    std::int64_t layer = static_cast<std::int64_t>(top.surface.first) - 1;
    while (std::cmp_greater_equal(layer, low)) {
        const Voxel* below =
            voxel({.face = face, .i = i, .j = j, .layer = static_cast<std::uint32_t>(layer)});
        if (below == nullptr || !loose(below->solid)) {
            break;
        }
        if (!named) {
            made.material = below->solid;
            named = true;
        }
        --layer;
    }
    made.bed = std::min((layer + 1) * full, top.ground);
    return made;
}

void Region::recover(const Covered& top, double gained, Stepped& stepped) {
    const double thickness = world_->layers().thickness;
    const Site column{.face = top.face, .i = top.i, .j = top.j, .layer = top.first};
    const std::int64_t change = dithered(gained / thickness * static_cast<double>(full), column, 5);
    if (change == 0) {
        return;
    }
    stepped.crept += std::abs(change);
    // Cover that came is laid as grain, and cover that went is taken from the grain first.
    std::int64_t grain = top.grain + change;
    if (grain < 0) {
        // More left than the grain held: the rest comes off the ground under it.
        const double mm =
            static_cast<double>(-grain) * thickness * 1000.0 / static_cast<double>(full);
        if (const auto cut = static_cast<int>(std::llround(mm)); cut > 0) {
            scour(column, cut);
        }
        grain = 0;
    }
    grain = std::min(grain, std::int64_t{top.voxels} * full);
    fill_grain(column, top.voxels, grain, top.material);
}

} // namespace hog::flow
