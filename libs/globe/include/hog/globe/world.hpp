#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_store.hpp>

#include <array>
#include <functional>
#include <map>
#include <optional>
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

    // Whether chunks are drawn as the voxels' own boxes, nothing smoothed, in place of the shaped
    // surface. Copies of the world keep the choice.
    void draw_as_boxes(bool on) noexcept { as_boxes_ = on; }
    [[nodiscard]] bool draws_as_boxes() const noexcept { return as_boxes_; }

    [[nodiscard]] Voxel at(Site site) const;
    [[nodiscard]] BrickVoxels brick(BrickSite site) const;

    // The voxels `count` layers up from `bottom` through each of `cells`, every `apart` layers,
    // as they now are, a column after another; `columns` are the generator's through them, at the
    // voxels' lod `apart` times coarser. Their solid amounts are as voxels `apart` layers thick,
    // as coarser detail is drawn from voxels further apart.
    [[nodiscard]] std::vector<Voxel> fill(std::span<const Cell> cells,
                                          std::span<const Generator::Column> columns, int bottom,
                                          int count, int apart = 1) const;

    // The brick's voxels as they now are; held only if they differ from what's generated.
    void write(BrickSite site, const BrickVoxels& voxels);

private:
    const Generator* generator_;
    VoxelStore* store_;
    bool as_boxes_ = false;
};

// The highest layer of the column through `cell` that may hold matter: unchanged voxels are as
// generated, `column` being the generator's through it, so it's over the generated ground or at
// the top of the highest change held there.
[[nodiscard]] int highest_layer(const World& world, Cell cell, const Generator::Column& column);

// The first surface of matter under `from` metres from the centre in the column under
// `direction`, told from the voxels as they now are: shaped matter, or grain filling half a voxel
// or more. None where the column is open to the floor.
[[nodiscard]] std::optional<double> surface_under(const World& world, DVec3 direction,
                                                  double from);
// The highest surface in the column, from the sky's ceiling down.
[[nodiscard]] std::optional<double> top_of(const World& world, DVec3 direction);

// Shaped matter taken out of a ball, or built up in one of `material`; `centre` in metres from
// the planet's centre. Loose matter is left where it is.
void dig(World& world, DVec3 centre, double radius);
void build(World& world, DVec3 centre, double radius, Material material);

// The same for any shape, given as its signed distance in metres (negative inside) at a point in
// metres from the planet's centre. It lies within `reach` of `centre` along the ground and
// `height` up or down.
using Shape = std::function<double(DVec3)>;
void dig_shape(World& world, DVec3 centre, double reach, double height, const Shape& shape);
void build_shape(World& world, DVec3 centre, double reach, double height, const Shape& shape,
                 Material material);

// The voxels of every column within `reach` of a point, across faces where they span them, by
// brick: the indices in each.
// `height` bounds the layers separately from the columns' `reach`, for shapes wide and shallow.
[[nodiscard]] std::map<BrickSite, std::vector<int>> sites_near(const Layers& layers, DVec3 centre,
                                                               double reach, double height = -1.0);
// One voxel taken out of a shape or built into one, given its signed distance to the shape's
// surface (negative inside): digging leaves only the matter outside the shape, building fills
// what's inside it, as amounts of a voxel `thickness` metres thick.
void dig_voxel(Voxel& voxel, double to_shape, double thickness);
void build_voxel(Voxel& voxel, double to_shape, Material material, double thickness);

} // namespace hog::globe
