#include <hog/globe/volume_build.hpp>

#include <hog/globe/world.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace hog::globe {

Volume generate_tiles(const Generator& generator, std::uint8_t face, std::uint32_t i,
                      std::uint32_t j) {
    constexpr int side = Volume::tile_side;
    const Layers& layers = generator.layers();
    const std::vector<Generator::Column> columns = generator.columns(
        {.level = static_cast<std::uint8_t>(layers.level),
         .face = face,
         .i = i * side,
         .j = j * side},
        side, voxel_lod(generator));
    double highest = generator.sea_level().value_or(layers.floor);
    for (const Generator::Column& column : columns) {
        highest = std::max(highest, column.top());
    }
    const int top = std::min(layers.layer_at(highest), layers.count - 1);
    Volume volume;
    for (int k = 0; k * side <= top; ++k) {
        const VoxelTree tile = VoxelTree::build(side, [&](int x, int y, int z) {
            const int layer = (k * side) + z;
            if (layer >= layers.count) {
                return Cube{};
            }
            const Voxel voxel =
                generated_at(columns[(static_cast<std::size_t>(y) * side) + static_cast<std::size_t>(x)],
                             layers, static_cast<std::uint32_t>(layer));
            // What the generator makes is as warm as the place, so it holds no heat over it.
            return cube_of(voxel, voxel.temperature / 8.0);
        });
        volume.put_tile({.face = face, .i = i * side, .j = j * side,
                         .layer = static_cast<std::uint32_t>(k * side)},
                        tile);
    }
    return volume;
}

} // namespace hog::globe
