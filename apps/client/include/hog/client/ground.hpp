#pragma once

#include <hog/core/result.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/render/forest.hpp>
#include <hog/render/grass.hpp>
#include <hog/render/terrain.hpp>
#include <hog/render/trees.hpp>
#include <hog/terrain/material.hpp>
#include <hog/terrain/stream.hpp>

#include <array>
#include <concepts>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace hog::client {

using BiomePalette = std::array<Vec3, planet::biome_count>;

// Each biome as it looks from above in daylight, the seabed as it would without the sea over it.
[[nodiscard]] BiomePalette biome_palette();

// How each biome's ground looks up close, averaging to its colour in the palette.
[[nodiscard]] std::array<terrain::Look, planet::biome_count> biome_looks();

// Each biome's material from its look, in Biome's order.
[[nodiscard]] std::vector<terrain::MaterialImage> biome_materials();

// Slots on the GPU that patches are held in, as render::TerrainPool hands them out.
template <typename Pool>
concept SlotPool =
    requires(Pool& pool, std::uint32_t slot, std::span<const terrain::PatchVertex> vertices) {
        { pool.acquire() } -> std::same_as<std::optional<std::uint32_t>>;
        pool.release(slot);
        { pool.stage(slot, slot, vertices) } -> std::same_as<Result<>>;
    };

// How much grass is grown, on ground how fine.
struct GrassBudget {
    // The lawn: every biome's blades, at full density to lawn_near, thinning with distance squared
    // beyond, and shrunk away by lawn_far.
    double lawn_near = render::grass_near;
    double lawn_far = render::grass_far;
    double finest = 0.75; // metres between a patch's vertices at most, so only fine ground grows
    double density = render::GrassTier{}.density; // blades a square metre up close
    // The tall swaths seen far off: coarser ground grows only the tall biomes' blades.
    double tall_coarsest = 6.0; // metres between vertices at most for the far swaths
    double tall_density = render::swath_tier.density; // blades a square metre, held to tall_hold
    double tall_hold = render::swath_tier.hold; // metres, past which it falls with distance squared
    double tall_far = render::swath_tier.far;   // metres the tall swaths reach
    double bush_far = 260.0;                    // metres the bushes reach
    // The trees, standing far beyond the rest and on far coarser ground; the ground wears their
    // canopy's colour beyond.
    double tree_coarsest = 24.0;         // metres between vertices at most for the trees
    double tree_far = render::trees_far; // metres the trees reach
    // The grown forest drawn near the eye, inside which the far stamps hold off.
    double forest_coarsest = 12.0;          // metres between vertices at most for grown trees
    double forest_full = 80.0;              // metres the fullest detail reaches
    double forest_mid = 150.0;              // metres the middle detail reaches
    double forest_far = render::forest_far; // metres the coarsest reaches, where stamps take over

    [[nodiscard]] render::GrassTier lawn() const noexcept {
        return {.density = density, .hold = lawn_near, .far = lawn_far};
    }
    [[nodiscard]] render::GrassTier swaths() const noexcept {
        return {.density = tall_density, .hold = tall_hold, .far = tall_far};
    }

    friend bool operator==(const GrassBudget&, const GrassBudget&) = default;
};

// Which slot holds each patch a stream holds.
class GroundSlots {
public:
    // Frees the slots of the patches the stream let go and stages those arriving into theirs, for
    // `frame`'s uploads; a patch baked again goes into the slot it's in.
    template <SlotPool Pool>
    Result<> take(Pool& pool, std::uint32_t frame, const terrain::StreamFrame& streamed);

    // The stream's patches as drawn from their slots, all of which must be held.
    [[nodiscard]] std::vector<render::TerrainDraw>
    draws(std::span<const terrain::StreamedPatch> drawn) const;

    [[nodiscard]] std::optional<std::uint32_t> slot(planet::CellId cell) const;
    [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }

private:
    std::map<planet::CellId, std::uint32_t> slots_;
};

// The grass grown on the drawn patches: the near lawn on ground fine enough and close enough,
// and the far swaths of tall grass on coarser ground, fewer blades a cell the further they lie.
// Patches without slots are passed over.
[[nodiscard]] std::vector<render::GrassDraw>
grass_draws(const GroundSlots& slots, std::span<const terrain::StreamedPatch> drawn,
            WorldPosition eye, double planet_radius, const GrassBudget& budget = {});

template <SlotPool Pool>
Result<> GroundSlots::take(Pool& pool, std::uint32_t frame, const terrain::StreamFrame& streamed) {
    for (const planet::CellId cell : streamed.evicted) {
        const auto held = slots_.find(cell);
        if (held != slots_.end()) {
            pool.release(held->second);
            slots_.erase(held);
        }
    }
    for (const terrain::Patch& patch : streamed.arrived) {
        auto held = slots_.find(patch.cell);
        if (held == slots_.end()) {
            const std::optional<std::uint32_t> free = pool.acquire();
            if (!free) {
                return make_error(ErrorCode::OutOfRange,
                                  std::format("no slot left for patch {}", patch.cell));
            }
            held = slots_.emplace(patch.cell, *free).first;
        }
        if (auto staged = pool.stage(frame, held->second, patch.vertices); !staged) {
            return staged;
        }
    }
    return {};
}

} // namespace hog::client
