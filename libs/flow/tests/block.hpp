#pragma once

#include <hog/flow/region.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace hog::flow::testing {

using globe::BrickSite;
using globe::BrickVoxels;
using globe::Generator;
using globe::Material;
using globe::Site;
using globe::Voxel;
using globe::VoxelStore;
using globe::World;

constexpr std::int64_t full = Voxel::full;
// A server tick's worth of game time.
constexpr double tick = 0.05;

inline const Generator& earth() {
    static const Generator generator{hog::globe::Planet{}};
    return generator;
}

// Columns 24 across and 24 along in the middle of face 0, with a floor: all shaped rock up to
// layer `floor` of `ground`, then open air to the top of the third brick up, or as `shape(i, j,
// layer)` says where given, over the floor. Cell numbers run from the block's own corner.
class Block {
public:
    static constexpr std::uint32_t side = 3 * hog::globe::brick_side;

    explicit Block(int floor, bool hold_sea = false,
                   const std::function<bool(std::uint32_t, std::uint32_t, int)>& shape = {},
                   Material ground = Material::Basalt)
        : store_{earth().layers()}, world_{earth(), store_}, floor_{floor},
          corner_i_{(store_.bricks_across() / 2) * hog::globe::brick_side}, corner_j_{corner_i_} {
        const auto first = (static_cast<std::uint32_t>(floor) - 1) / hog::globe::brick_side;
        for (std::uint32_t bl = first; bl < first + 3; ++bl) {
            for (std::uint32_t bj = 0; bj < 3; ++bj) {
                for (std::uint32_t bi = 0; bi < 3; ++bi) {
                    const BrickSite brick{.face = 0,
                                          .i = (corner_i_ / hog::globe::brick_side) + bi,
                                          .j = (corner_j_ / hog::globe::brick_side) + bj,
                                          .layer = bl};
                    BrickVoxels voxels{};
                    const double band = hog::globe::distance_band(store_.layers());
                    for (int k = 0; k < hog::globe::brick_voxels; ++k) {
                        const Site site = hog::globe::site_in(brick, k);
                        const auto layer = static_cast<int>(site.layer);
                        const bool open =
                            layer >= floor && (!shape || shape(site.i - corner_i_,
                                                               site.j - corner_j_, layer - floor));
                        // The floor's distance within the band, as generated ground has it.
                        // Measured from the highest rock below in the column.
                        int rock = floor - 1;
                        if (shape) {
                            for (int below = floor; below < layer; ++below) {
                                if (!shape(site.i - corner_i_, site.j - corner_j_, below - floor)) {
                                    rock = below;
                                }
                            }
                        }
                        const double to_floor = std::clamp(
                            (layer - rock - 0.5) * store_.layers().thickness, -band, band);
                        Voxel& voxel = voxels[static_cast<std::size_t>(k)];
                        voxel.distance = hog::globe::distance_in_mm(to_floor);
                        if (!open) {
                            voxel.solid = ground;
                            if (layer >= floor) {
                                voxel.distance = -Voxel::far; // a wall the shape raised
                            }
                        }
                    }
                    store_.put(brick, hog::globe::Brick::pack(voxels));
                    bricks_.push_back(brick);
                }
            }
        }
        region_ = std::make_unique<Region>(world_, hold_sea);
        for (const BrickSite brick : bricks_) {
            region_->load(brick);
        }
    }

    [[nodiscard]] Region& region() { return *region_; }
    [[nodiscard]] World& world() { return world_; }
    [[nodiscard]] const VoxelStore& store() const { return store_; }
    [[nodiscard]] const std::vector<BrickSite>& bricks() const { return bricks_; }

    // `layer` counted up from the floor.
    [[nodiscard]] Site at(std::uint32_t i, std::uint32_t j, int layer) const {
        return {.face = 0,
                .i = corner_i_ + i,
                .j = corner_j_ + j,
                .layer = static_cast<std::uint32_t>(floor_ + layer)};
    }
    [[nodiscard]] std::int64_t amount(std::uint32_t i, std::uint32_t j, int layer) {
        return region_->at(at(i, j, layer)).liquid_amount;
    }
    [[nodiscard]] std::int64_t grain(std::uint32_t i, std::uint32_t j, int layer) {
        return region_->at(at(i, j, layer)).grain_amount;
    }
    [[nodiscard]] int distance(std::uint32_t i, std::uint32_t j, int layer) {
        return region_->at(at(i, j, layer)).distance;
    }
    [[nodiscard]] std::int64_t carried(std::uint32_t i, std::uint32_t j) {
        return region_->carried(0, corner_i_ + i, corner_j_ + j).amount;
    }
    // All the liquid up a column.
    [[nodiscard]] std::int64_t column(std::uint32_t i, std::uint32_t j) {
        std::int64_t total = 0;
        for (int layer = 0; layer < 16; ++layer) {
            total += amount(i, j, layer);
        }
        return total;
    }
    void pour(std::uint32_t i, std::uint32_t j, int layer, std::int64_t amount) {
        region_->pour(at(i, j, layer), static_cast<std::uint16_t>(amount));
    }
    void pile(std::uint32_t i, std::uint32_t j, int layer, Material material, std::int64_t amount) {
        region_->pile(at(i, j, layer), material, static_cast<std::uint16_t>(amount));
    }
    void suspend(std::uint32_t i, std::uint32_t j, int layer, Material material,
                 std::int64_t amount) {
        region_->suspend(at(i, j, layer), material, amount);
    }

    // Steps `seconds` at a time while anything changes, up to `most` times; how many it took.
    int settle(int most, double seconds = tick) {
        for (int step = 1; step <= most; ++step) {
            const std::int64_t before = region_->liquid();
            const std::int64_t grain_before = region_->grain();
            const Stepped stepped = region_->step(seconds);
            if (region_->liquid() != before + stepped.from_sea) {
                FAIL("liquid lost or made: " << before << " -> " << region_->liquid());
            }
            if (region_->grain() != grain_before + stepped.eroded) {
                FAIL("grain lost or made: " << grain_before << " -> " << region_->grain());
            }
            if (stepped.changed == 0) {
                return step;
            }
        }
        return most;
    }

private:
    VoxelStore store_;
    World world_;
    int floor_;
    std::uint32_t corner_i_ = 0;
    std::uint32_t corner_j_ = 0;
    std::vector<BrickSite> bricks_;
    std::unique_ptr<Region> region_;
};

// A brick layer whose voxels all stand `above` the sea.
inline int floor_above_sea(double above) {
    const hog::globe::Layers& layers = earth().layers();
    const int layer = layers.layer_at(earth().sea_level().value_or(0.0) + above);
    return ((layer / hog::globe::brick_side) + 1) * hog::globe::brick_side;
}

} // namespace hog::flow::testing
