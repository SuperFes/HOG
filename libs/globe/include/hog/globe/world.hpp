#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_store.hpp>

#include <array>

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

} // namespace hog::globe
