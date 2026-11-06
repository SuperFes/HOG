#include <hog/flow/region.hpp>
#include <hog/flow/runoff.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

namespace hog::flow {
namespace {

using globe::Material;
using globe::Site;
using globe::Voxel;

constexpr std::int64_t full = Voxel::full;
// The sides of a column in the order Runoff names them: +i, +j, -i, -j.
constexpr std::array<std::pair<int, int>, Runoff::sides> around{{{1, 0}, {0, 1}, {-1, 0}, {0, -1}}};

struct Key {
    std::uint8_t face = 0;
    std::uint32_t i = 0;
    std::uint32_t j = 0;
    friend constexpr auto operator<=>(const Key&, const Key&) noexcept = default;
};

// The lowest column with no neighbour held across `side`.
std::optional<std::size_t> lowest_on_edge(const Runoff& runoff, std::size_t side) {
    std::optional<std::size_t> lowest;
    for (std::size_t n = 0; n < runoff.size(); ++n) {
        if (runoff[n].next.at(side) == Runoff::none &&
            (!lowest || runoff[n].ground < runoff[*lowest].ground)) {
            lowest = n;
        }
    }
    return lowest;
}

void add_springs(Runoff& runoff, const std::map<Key, std::size_t>& index,
                 const std::vector<Region::Spring>& springs) {
    for (const Region::Spring& spring : springs) {
        if (const auto found = index.find(Key{.face = spring.face, .i = spring.i, .j = spring.j});
            found != index.end()) {
            runoff[found->second].inflow += spring.discharge;
        }
    }
}

} // namespace

std::vector<Region::Top> Region::tops() {
    const globe::Layers& layers = world_->store().layers();
    std::vector<Top> found;
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
                const std::vector<Scan> floors_up = floors(face, i, j, low, high);
                if (floors_up.empty()) {
                    continue;
                }
                const Scan& top = floors_up.back();
                const auto units = static_cast<double>(top.ground + top.surface.grain_amount);
                found.push_back(
                    {.face = face,
                     .i = i,
                     .j = j,
                     .first = top.surface.first,
                     .open = top.surface.open,
                     .ground = layers.floor + (units / static_cast<double>(full) * layers.thickness)});
            }
        }
    }
    return found;
}

void Region::lay_water(const std::vector<Top>& tops,
                       const std::function<double(std::size_t)>& surface, Stepped& stepped) {
    const globe::Layers& layers = world_->store().layers();
    for (std::size_t n = 0; n < tops.size(); ++n) {
        const double level = surface(n);
        for (std::uint32_t k = 0; k < tops[n].open; ++k) {
            const Site site{
                .face = tops[n].face, .i = tops[n].i, .j = tops[n].j, .layer = tops[n].first + k};
            const Voxel* here = voxel(site);
            if (here == nullptr) {
                break;
            }
            const double share = std::clamp(
                (level - layers.bottom(static_cast<int>(site.layer))) / layers.thickness, 0.0, 1.0);
            const std::int64_t taken =
                here->grain_amount + (here->solid == Material::Air ? here->solid_amount : 0);
            const auto amount = static_cast<std::uint16_t>(std::clamp<std::int64_t>(
                std::llround(share * static_cast<double>(full)) - taken, 0, full - taken));
            if (amount != here->liquid_amount || (amount > 0 && here->liquid != liquid_)) {
                stepped.changed += std::abs(std::int64_t{amount} - here->liquid_amount);
                pour(site, amount);
            }
        }
    }
}

Stepped Region::settle_water(const Rainfall& rainfall) {
    ++steps_;
    Stepped stepped;
    const globe::Layers& layers = world_->store().layers();
    const std::optional<double> sea = world_->generator().sea_level();
    const std::vector<Top> held_tops = tops();
    if (held_tops.empty()) {
        return stepped;
    }
    std::map<Key, std::size_t> index;
    for (std::size_t n = 0; n < held_tops.size(); ++n) {
        index.emplace(Key{.face = held_tops[n].face, .i = held_tops[n].i, .j = held_tops[n].j}, n);
    }
    const auto cell_of = [&](const Top& top) {
        return globe::Cell{
            .level = static_cast<std::uint8_t>(layers.level), .face = top.face, .i = top.i, .j = top.j};
    };
    const auto index_beside = [&](const Top& top, std::size_t side) {
        const auto next = globe::next_to(cell_of(top), around[side].first, around[side].second);
        const auto found =
            next ? index.find(Key{.face = next->face, .i = next->i, .j = next->j}) : index.end();
        return found != index.end() ? static_cast<int>(found->second) : Runoff::none;
    };
    const Top& first = held_tops.front();
    const double width = std::sqrt(
        layers.volume(cell_of(first), static_cast<int>(first.first)) / layers.thickness);
    Runoff runoff{held_tops.size(), {.cell = width, .evaporation = rainfall.evaporation}};
    // Water leaves over the held ground's edge, but not back up the side a river comes in by.
    const std::size_t closed = rainfall.river ? rainfall.river->side : Runoff::sides;
    for (std::size_t n = 0; n < held_tops.size(); ++n) {
        Runoff::Node& node = runoff[n];
        node.ground = held_tops[n].ground;
        node.rain = rainfall.rain ? rainfall.rain(globe::center(cell_of(held_tops[n]))) : 0.0;
        bool edge = false;
        for (std::size_t side = 0; side < Runoff::sides; ++side) {
            node.next.at(side) = index_beside(held_tops[n], side);
            edge = edge || (node.next.at(side) == Runoff::none && side != closed);
        }
        node.outlet = edge || (sea && node.ground < *sea);
    }
    if (rainfall.river) {
        if (const auto entry = lowest_on_edge(runoff, closed)) {
            runoff[*entry].inflow += rainfall.river->discharge;
        }
    }
    add_springs(runoff, index, rainfall.springs);
    runoff.settle();
    // The sea's own water is left as it is.
    lay_water(held_tops,
              [&](std::size_t n) {
                  return sea && held_tops[n].ground < *sea ? 0.0 : runoff.surface(n);
              },
              stepped);
    return stepped;
}

} // namespace hog::flow
