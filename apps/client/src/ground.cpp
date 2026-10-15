#include <hog/client/ground.hpp>
#include <hog/core/assert.hpp>
#include <hog/mapgen/photo.hpp>

#include <cstddef>

namespace hog::client {

render::BiomePalette biome_palette() {
    render::BiomePalette palette{};
    for (std::size_t biome = 0; biome < palette.size(); ++biome) {
        palette.at(biome) = Vec3(mapgen::true_colour({.biome = static_cast<planet::Biome>(biome)}));
    }
    return palette;
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

} // namespace hog::client
