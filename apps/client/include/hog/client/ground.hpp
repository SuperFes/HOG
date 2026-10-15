#pragma once

#include <hog/core/result.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/render/terrain.hpp>
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

// Each biome's material, in Biome's order, averaging to its colour in the palette.
[[nodiscard]] std::vector<terrain::MaterialImage> biome_materials();

// Slots on the GPU that patches are held in, as render::TerrainPool hands them out.
template <typename Pool>
concept SlotPool =
    requires(Pool& pool, std::uint32_t slot, std::span<const terrain::PatchVertex> vertices) {
        { pool.acquire() } -> std::same_as<std::optional<std::uint32_t>>;
        pool.release(slot);
        { pool.stage(slot, slot, vertices) } -> std::same_as<Result<>>;
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
