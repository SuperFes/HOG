#include <hog/core/hash.hpp>
#include <hog/flora/placement.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string_view>
#include <utility>

namespace hog::flora {
namespace {

DVec3 unit_vector(DVec3 v) noexcept {
    return v / std::sqrt(glm::dot(v, v));
}

double roll(std::uint64_t hash, std::uint64_t die) noexcept {
    return unit_of(hash_combine(hash, die));
}

// Where a kind has no species yet, the kinds tried in its place.
constexpr std::array<Kind, kinds> stand_ins{Kind::Deciduous, Kind::Needleleaf, Kind::Evergreen,
                                            Kind::Wetland,   Kind::Dry,        Kind::Palm};

std::optional<std::uint32_t> species_of(Kind kind, double dice, const Planting& planting) {
    const auto pick = [&](Kind wanted) -> std::optional<std::uint32_t> {
        std::uint32_t count = 0;
        for (const Planting::Species& s : planting.species) {
            count += s.kind == wanted && s.variants > 0 ? 1U : 0U;
        }
        if (count == 0) {
            return std::nullopt;
        }
        auto nth = static_cast<std::uint32_t>(dice * count);
        for (std::uint32_t i = 0; i < planting.species.size(); ++i) {
            if (planting.species[i].kind == wanted && planting.species[i].variants > 0 &&
                nth-- == 0) {
                return i;
            }
        }
        return std::nullopt;
    };
    if (const auto own = pick(kind)) {
        return own;
    }
    for (const Kind other : stand_ins) {
        if (const auto borrowed = pick(other)) {
            return borrowed;
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<Kind> kind_of(std::string_view species) noexcept {
    // The tree lab's species (prototypes/trees/species).
    constexpr std::array<std::pair<std::string_view, Kind>, 9> known{{
        {"spruce", Kind::Needleleaf},
        {"pine", Kind::Needleleaf},
        {"larch", Kind::Needleleaf},
        {"oak", Kind::Deciduous},
        {"beech", Kind::Deciduous},
        {"birch", Kind::Deciduous},
        {"eucalyptus", Kind::Evergreen},
        {"willow", Kind::Wetland},
        {"coconut", Kind::Palm},
    }};
    for (const auto& [name, kind] : known) {
        if (name == species) {
            return kind;
        }
    }
    return std::nullopt;
}

Planting planting_of(std::span<const Species> species, std::uint32_t most_variants) {
    Planting planting;
    for (const Species& one : species) {
        const auto kind = kind_of(one.name);
        const auto variants =
            kind ? std::min(most_variants, static_cast<std::uint32_t>(one.variants.size())) : 0U;
        std::uint32_t impostors = 0;
        while (impostors < variants && one.variants[impostors].impostor) {
            ++impostors;
        }
        planting.species.push_back(
            {.kind = kind.value_or(Kind::Deciduous), .variants = variants, .impostors = impostors});
    }
    return planting;
}

Across across_of(DVec3 up) noexcept {
    const DVec3 helper = std::abs(up.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 east = unit_vector(glm::cross(helper, up));
    return {.east = east, .north = glm::cross(up, east)};
}

std::vector<Spot> spots(DVec3 middle, double side, DVec3 up, std::uint64_t node_hash,
                        double spacing) {
    // A cube's shadow down any direction lies within its bounding sphere's.
    const double width = side * std::numbers::sqrt3;
    const auto cells = static_cast<std::uint32_t>(std::ceil(width / spacing));
    const double cell = width / cells;
    const Across across = across_of(up);
    std::vector<Spot> out;
    out.reserve(static_cast<std::size_t>(cells) * cells);
    for (std::uint32_t j = 0; j < cells; ++j) {
        for (std::uint32_t i = 0; i < cells; ++i) {
            const std::uint64_t hash = hash_values(node_hash, i, j);
            const double east = ((i + roll(hash, 0)) * cell) - (width / 2.0);
            const double north = ((j + roll(hash, 1)) * cell) - (width / 2.0);
            out.push_back(
                {.at = middle + (across.east * east) + (across.north * north), .hash = hash});
        }
    }
    return out;
}

std::optional<Planted> plant(const Spot& spot, const Woods& woods,
                             const Planting& planting) noexcept {
    const std::uint64_t hash = spot.hash;
    if (roll(hash, 2) >= woods.cover) {
        return std::nullopt;
    }
    double total = 0.0;
    for (const double share : woods.shares) {
        total += share;
    }
    if (total <= 0.0) {
        return std::nullopt;
    }
    const double kind_dice = roll(hash, 3) * total;
    std::size_t kind = 0;
    for (double below = woods.shares.at(0); kind + 1 < kinds && below <= kind_dice;) {
        below += woods.shares.at(++kind);
    }
    const auto species = species_of(static_cast<Kind>(kind), roll(hash, 4), planting);
    if (!species) {
        return std::nullopt;
    }
    const Planting::Species& chosen = planting.species[*species];
    return Planted{
        .species = *species,
        .variant = static_cast<std::uint32_t>(roll(hash, 5) * chosen.variants),
        .turn = static_cast<float>(roll(hash, 6)),
        .scale = static_cast<float>(0.85 + (0.3 * roll(hash, 7))),
        .lean =
            Vec2{static_cast<float>(roll(hash, 8) - 0.5), static_cast<float>(roll(hash, 9) - 0.5)} *
            0.08F,
        .tint = static_cast<float>((2.0 * roll(hash, 10)) - 1.0),
        .season = static_cast<std::uint32_t>(hash_combine(hash, 11)),
    };
}

} // namespace hog::flora
