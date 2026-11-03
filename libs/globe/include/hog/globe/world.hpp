#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_store.hpp>

#include <array>
#include <map>
#include <span>
#include <vector>

namespace hog::globe {

using BrickVoxels = std::array<Voxel, brick_voxels>;

// A brick's voxels as generated, its columns taken together.
[[nodiscard]] BrickVoxels generated(const Generator& generator, BrickSite site);

// The world as it now is: what the store holds, or else what's generated.
class World {
public:
    // Precondition: the store stands on the generator's layers.
    World(const Generator& generator, VoxelStore& store);

    [[nodiscard]] const Generator& generator() const noexcept { return *generator_; }
    [[nodiscard]] const VoxelStore& store() const noexcept { return *store_; }

    [[nodiscard]] Voxel at(Site site) const;
    [[nodiscard]] BrickVoxels brick(BrickSite site) const;

    // The voxels `count` layers up from `bottom` through each of `cells`, every `apart` layers,
    // as they now are, a column after another; `columns` are the generator's through them, at the
    // voxels' lod `apart` times coarser. Their distances are told in `apart` millimetres, so
    // where generated they're kept within a band `apart` times as wide, as coarser detail is
    // drawn from voxels further apart.
    [[nodiscard]] std::vector<Voxel> fill(std::span<const Cell> cells,
                                          std::span<const Generator::Column> columns, int bottom,
                                          int count, int apart = 1) const;

    // The brick's voxels as they now are; held only if they differ from what's generated.
    void write(BrickSite site, const BrickVoxels& voxels);

private:
    const Generator* generator_;
    VoxelStore* store_;
};

// Shaped matter taken out of a ball, or built up in one of `material`; `centre` in metres from
// the planet's centre. Loose matter is left where it is.
void dig(World& world, DVec3 centre, double radius);
void build(World& world, DVec3 centre, double radius, Material material);

// The voxels of every column within `reach` of a point, across faces where they span them, by
// brick: the indices in each.
[[nodiscard]] std::map<BrickSite, std::vector<int>> sites_near(const Layers& layers, DVec3 centre,
                                                               double reach);
// One voxel taken out of a shape or built into one, given its signed distance to the shape's
// surface (negative inside, within the band) and the way out of the shape there, or none.
void dig_voxel(Voxel& voxel, double to_shape, DVec3 outward);
void build_voxel(Voxel& voxel, double to_shape, DVec3 outward, Material material);

} // namespace hog::globe
