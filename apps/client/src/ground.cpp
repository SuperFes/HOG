#include <hog/client/ground.hpp>
#include <hog/core/assert.hpp>
#include <hog/mapgen/photo.hpp>
#include <hog/planet/projection.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <set>

namespace hog::client {

BiomePalette biome_palette() {
    BiomePalette palette{};
    for (std::size_t biome = 0; biome < palette.size(); ++biome) {
        palette.at(biome) = Vec3(mapgen::true_colour({.biome = static_cast<planet::Biome>(biome)}));
    }
    // Wet sand; the water's colour is drawn over it by depth.
    constexpr float wet = 0.7F;
    palette.at(static_cast<std::size_t>(planet::Biome::Ocean)) =
        palette.at(static_cast<std::size_t>(planet::Biome::Beach)) * wet;
    return palette;
}

std::array<terrain::Look, planet::biome_count> biome_looks() {
    // The accent as a tint of the biome's colour, how much ground it covers, then the grain and
    // the mottling.
    struct Variation {
        planet::Biome biome;
        DVec3 accent;
        double share;
        double grain;
        double mottle;
    };
    constexpr auto vary = [](planet::Biome biome, DVec3 accent, double share, double grain,
                             double mottle) {
        return Variation{
            .biome = biome, .accent = accent, .share = share, .grain = grain, .mottle = mottle};
    };
    using planet::Biome;
    constexpr std::array variations{
        // Ripples of darker, wetter sand.
        vary(Biome::Ocean, {0.7, 0.7, 0.7}, 0.2, 0.25, 0.15),
        vary(Biome::Beach, {1.2, 1.2, 1.15}, 0.3, 0.3, 0.1),
        vary(Biome::Ice, {0.85, 0.95, 1.05}, 0.3, 0.06, 0.08),
        // Lichen and bare stone.
        vary(Biome::Tundra, {1.2, 1.0, 0.8}, 0.4, 0.35, 0.25),
        // Needles and moss.
        vary(Biome::BorealForest, {0.6, 0.7, 0.6}, 0.35, 0.4, 0.3),
        // Leaf litter.
        vary(Biome::TemperateForest, {1.5, 1.0, 0.6}, 0.3, 0.4, 0.3),
        // Dry tufts.
        vary(Biome::Grassland, {1.4, 1.1, 0.7}, 0.25, 0.35, 0.2),
        // Bare dirt between the bushes.
        vary(Biome::Shrubland, {1.4, 1.1, 0.9}, 0.4, 0.35, 0.3),
        vary(Biome::Desert, {1.1, 1.0, 0.9}, 0.3, 0.2, 0.15),
        vary(Biome::Savanna, {1.3, 1.15, 0.8}, 0.35, 0.35, 0.25),
        vary(Biome::TropicalRainforest, {0.7, 0.9, 0.7}, 0.35, 0.4, 0.3),
        // Paler stone among the darker.
        vary(Biome::Alpine, {1.2, 1.2, 1.25}, 0.35, 0.45, 0.35),
        // Standing water.
        vary(Biome::Wetland, {0.6, 0.7, 0.8}, 0.35, 0.3, 0.25),
        vary(Biome::Lake, {0.7, 0.7, 0.7}, 0.3, 0.25, 0.2)};
    static_assert(variations.size() == planet::biome_count);

    const BiomePalette palette = biome_palette();
    std::array<terrain::Look, planet::biome_count> looks{};
    for (const Variation& variation : variations) {
        const auto biome = static_cast<std::size_t>(variation.biome);
        const DVec3 colour{palette.at(biome)};
        looks.at(biome) = {.colour = colour,
                           .accent = glm::min(colour * variation.accent, DVec3{1.0}),
                           .accent_share = variation.share,
                           .grain = variation.grain,
                           .mottle = variation.mottle,
                           .seed = biome + 1};
    }
    return looks;
}

std::vector<terrain::MaterialImage> biome_materials() {
    // A texel about 1.5 cm across over the four metres a material spans.
    constexpr int size = 256;
    std::vector<terrain::MaterialImage> materials;
    for (const terrain::Look& look : biome_looks()) {
        materials.push_back(terrain::material_image(look, size));
    }
    return materials;
}

std::vector<render::TerrainDraw>
GroundSlots::draws(std::span<const terrain::StreamedPatch> drawn) const {
    std::vector<render::TerrainDraw> draws;
    draws.reserve(drawn.size());
    for (const terrain::StreamedPatch& patch : drawn) {
        const std::optional<std::uint32_t> held = slot(patch.cell);
        HOG_ASSERT(held, "drawing a patch that isn't held");
        draws.push_back(
            {.slot = *held, .origin = patch.origin, .coarser_edges = patch.coarser_edges});
    }
    return draws;
}

std::optional<std::uint32_t> GroundSlots::slot(planet::CellId cell) const {
    const auto held = slots_.find(cell);
    if (held == slots_.end()) {
        return std::nullopt;
    }
    return held->second;
}

namespace {

// The same on every machine, so everyone grows the same field.
std::uint32_t grass_seed(planet::CellId cell) noexcept {
    std::uint64_t mixed = cell.raw();
    mixed ^= mixed >> 30U;
    mixed *= 0xbf58'476d'1ce4'e5b9;
    mixed ^= mixed >> 27U;
    mixed *= 0x94d0'49bb'1331'11eb;
    mixed ^= mixed >> 31U;
    return static_cast<std::uint32_t>(mixed);
}

} // namespace

std::vector<render::GrassDraw> grass_draws(const GroundSlots& slots,
                                           std::span<const terrain::StreamedPatch> drawn,
                                           WorldPosition eye, double planet_radius,
                                           const GrassBudget& budget) {
    std::vector<render::GrassDraw> draws;
    const DVec3 up = glm::normalize(eye.metres());
    for (const terrain::StreamedPatch& patch : drawn) {
        // A face spans a quarter of the circumference, so a cell at this level this much of it.
        const double spacing =
            (std::numbers::pi / 2.0) * planet_radius /
            static_cast<double>(std::uint64_t{1} << static_cast<unsigned>(patch.cell.level())) /
            terrain::patch_cells;
        if (spacing > budget.tall_coarsest) {
            continue;
        }
        // Distance level with the ground, generously: an origin lies at sea level under its
        // patch, maybe a span aside of its nearest ground, while each blade fades by how far it
        // truly is.
        const double span = spacing * terrain::patch_cells;
        const DVec3 away = patch.origin - eye;
        const double distance = std::max(0.0, glm::length(away - (up * glm::dot(away, up))) - span);
        const std::optional<std::uint32_t> held = slots.slot(patch.cell);
        if (!held) {
            continue;
        }
        const auto grow = [&](double density, double fade_far, bool tall_only, std::uint32_t seed) {
            const auto blades =
                static_cast<std::uint32_t>(std::lround(density * spacing * spacing));
            if (blades == 0) {
                return;
            }
            draws.push_back({.slot = *held,
                             .origin = patch.origin,
                             .spacing = spacing,
                             .blades_per_cell = std::min(blades, render::most_blades_per_cell),
                             .seed = seed,
                             .fade_far = fade_far,
                             .tall_only = tall_only});
        };
        // The lawn: every biome's blades, on fine ground near the eye.
        if (spacing <= budget.finest && distance < render::grass_far) {
            const double fade = std::min(
                (render::grass_far - distance) / (render::grass_far - render::grass_near), 1.0);
            grow(budget.density * fade, render::grass_far, false, grass_seed(patch.cell));
        }
        // The far swaths: tall biomes only, their stand thinning with the square of distance,
        // seeded apart from the lawn so the two don't grow twin blades where they overlap.
        if (distance < budget.tall_far) {
            const double middle = distance + (span * 0.5);
            const double held_to = budget.tall_hold / std::max(middle, budget.tall_hold);
            grow(budget.tall_density * held_to * held_to, budget.tall_far, true,
                 grass_seed(patch.cell) ^ 0x7a11'f1e1U);
        }
    }
    return draws;
}

namespace {

// A patch keyed onto a fixed lattice, for bushes and trees alike.
struct LatticeStand {
    planet::CellId cell; // the keyed patch: the drawn one, or its ancestor at the lattice's level
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WorldPosition origin = {};
    std::uint32_t slot = 0;
    std::uint32_t face = 0;
    std::uint32_t lattice_i = 0;
    std::uint32_t lattice_j = 0;
    int shift = 0;
    double spacing = 0.0;  // metres between the patch's vertices
    double distance = 0.0; // metres to the patch's nearest ground, generously
};

// Keys the patch onto the fixed lattice of `lattice_level`, if its ground is fine enough to
// carry it and close enough to reach. The lattice keys a patch's grid cells together or apart by
// whole powers of two; a patch too coarse for even paired lattice cells can't carry it, and one
// finer than the lattice is keyed through its ancestor at the lattice's own level, whose patch
// stays held under the fine ground drawn over it — siblings key to the same stand, so callers
// must pass duplicates over.
std::optional<LatticeStand> lattice_stand(const GroundSlots& slots,
                                          const terrain::StreamedPatch& patch, WorldPosition eye,
                                          DVec3 up, double planet_radius, int lattice_level,
                                          double coarsest, double reach) {
    planet::CellId cell = patch.cell;
    WorldPosition origin = patch.origin;
    if (cell.level() > lattice_level) {
        cell = cell.parent(lattice_level);
        origin = WorldPosition{planet::cell_center(cell) * planet_radius};
    }
    const int level = cell.level();
    const int shift = level + terrain::patch_depth - lattice_level;
    if (shift < -1) {
        return std::nullopt;
    }
    const double spacing = (std::numbers::pi / 2.0) * planet_radius /
                           static_cast<double>(std::uint64_t{1} << static_cast<unsigned>(level)) /
                           terrain::patch_cells;
    if (spacing > coarsest) {
        return std::nullopt;
    }
    const double span = spacing * terrain::patch_cells;
    const DVec3 away = origin - eye;
    const double distance = std::max(0.0, glm::length(away - (up * glm::dot(away, up))) - span);
    if (distance >= reach) {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> held = slots.slot(cell);
    if (!held) {
        return std::nullopt;
    }
    const planet::FaceIJ at = cell.to_face_ij();
    const auto spread = static_cast<unsigned>(lattice_level - level);
    return LatticeStand{.cell = cell,
                        .origin = origin,
                        .slot = *held,
                        .face = static_cast<std::uint32_t>(at.face),
                        .lattice_i = static_cast<std::uint32_t>(at.i) << spread,
                        .lattice_j = static_cast<std::uint32_t>(at.j) << spread,
                        .shift = shift,
                        .spacing = spacing,
                        .distance = distance};
}

} // namespace

std::vector<render::BushDraw> bush_draws(const GroundSlots& slots,
                                         std::span<const terrain::StreamedPatch> drawn,
                                         WorldPosition eye, double planet_radius,
                                         const GrassBudget& budget) {
    std::vector<render::BushDraw> draws;
    const DVec3 up = glm::normalize(eye.metres());
    std::set<planet::CellId> keyed;
    for (const terrain::StreamedPatch& patch : drawn) {
        const std::optional<LatticeStand> stand =
            lattice_stand(slots, patch, eye, up, planet_radius, render::bush_lattice_level,
                          budget.tall_coarsest, budget.tall_far);
        if (!stand || !keyed.insert(stand->cell).second) {
            continue;
        }
        draws.push_back({.slot = stand->slot,
                         .origin = stand->origin,
                         .face = stand->face,
                         .lattice_i = stand->lattice_i,
                         .lattice_j = stand->lattice_j,
                         .shift = stand->shift,
                         .fade_far = budget.tall_far});
    }
    return draws;
}

std::vector<render::TreeDraw> tree_draws(const GroundSlots& slots,
                                         std::span<const terrain::StreamedPatch> drawn,
                                         WorldPosition eye, double planet_radius,
                                         const GrassBudget& budget) {
    std::vector<render::TreeDraw> draws;
    const DVec3 up = glm::normalize(eye.metres());
    std::set<planet::CellId> keyed;
    for (const terrain::StreamedPatch& patch : drawn) {
        const std::optional<LatticeStand> stand =
            lattice_stand(slots, patch, eye, up, planet_radius, render::tree_lattice_level,
                          budget.tree_coarsest, budget.tree_far);
        if (!stand || !keyed.insert(stand->cell).second) {
            continue;
        }
        // Where the grown forest covers this patch, its stamps keep to the distance beyond.
        const double hold_off = stand->spacing <= budget.forest_coarsest ? render::forest_far : 0.0;
        draws.push_back({.slot = stand->slot,
                         .origin = stand->origin,
                         .face = stand->face,
                         .lattice_i = stand->lattice_i,
                         .lattice_j = stand->lattice_j,
                         .shift = stand->shift,
                         .fade_far = budget.tree_far,
                         .hold_off = hold_off});
    }
    return draws;
}

std::vector<render::ForestDraw> forest_draws(const GroundSlots& slots,
                                             std::span<const terrain::StreamedPatch> drawn,
                                             WorldPosition eye, double planet_radius,
                                             const GrassBudget& budget) {
    std::vector<render::ForestDraw> draws;
    const DVec3 up = glm::normalize(eye.metres());
    std::set<planet::CellId> keyed;
    for (const terrain::StreamedPatch& patch : drawn) {
        const std::optional<LatticeStand> stand =
            lattice_stand(slots, patch, eye, up, planet_radius, render::tree_lattice_level,
                          budget.forest_coarsest, render::forest_far);
        if (!stand || !keyed.insert(stand->cell).second) {
            continue;
        }
        int detail = 2;
        if (stand->distance < budget.forest_full) {
            detail = 0;
        } else if (stand->distance < budget.forest_mid) {
            detail = 1;
        }
        draws.push_back({.slot = stand->slot,
                         .origin = stand->origin,
                         .face = stand->face,
                         .lattice_i = stand->lattice_i,
                         .lattice_j = stand->lattice_j,
                         .shift = stand->shift,
                         .fade_far = render::forest_far,
                         .detail = detail});
    }
    return draws;
}

} // namespace hog::client
