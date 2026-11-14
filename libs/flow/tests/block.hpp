#pragma once

#include <hog/flow/region.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/world.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <tuple>
#include <vector>

namespace hog::flow::testing {

using globe::BrickSite;
using globe::BrickVoxels;
using globe::Generator;
using globe::Material;
using globe::Site;
using globe::Volume;
using globe::Voxel;
using globe::World;

constexpr std::int64_t full = Voxel::full;
// A server tick's worth of game time.
constexpr double tick = 0.05;

inline const Generator& earth() {
    return hog::testing::small_world();
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
        : world_{earth(), volume_}, floor_{floor},
          corner_i_{(hog::globe::bricks_across(earth().layers()) / 2) * hog::globe::brick_side},
          corner_j_{corner_i_} {
        const auto first = (static_cast<std::uint32_t>(floor) - 1) / hog::globe::brick_side;
        for (std::uint32_t bl = first; bl < first + 3; ++bl) {
            for (std::uint32_t bj = 0; bj < 3; ++bj) {
                for (std::uint32_t bi = 0; bi < 3; ++bi) {
                    bricks_.push_back({.face = 0,
                                       .i = (corner_i_ / hog::globe::brick_side) + bi,
                                       .j = (corner_j_ / hog::globe::brick_side) + bj,
                                       .layer = bl});
                }
            }
        }
        start(shape, ground, hold_sea);
    }

    // A block over a cube edge: the bricks of every column within a brick of the edge column `j`
    // of face 0's last row, on both faces, for the three bricks up from the floor.
    struct Edge {};
    Block(int floor, Edge /*edge*/, std::uint32_t j) : world_{earth(), volume_}, floor_{floor} {
        const int level = earth().layers().level;
        const std::uint32_t last = (std::uint32_t{1} << level) - 1;
        const auto first = (static_cast<std::uint32_t>(floor) - 1) / hog::globe::brick_side;
        std::set<std::tuple<int, std::uint32_t, std::uint32_t>> columns;
        for (int di = -hog::globe::brick_side; di <= hog::globe::brick_side; di += 1) {
            for (int dj = -hog::globe::brick_side; dj <= hog::globe::brick_side; dj += 1) {
                const auto next = hog::globe::next_to(
                    {.level = static_cast<std::uint8_t>(level), .face = 0, .i = last, .j = j}, di,
                    dj);
                if (next) {
                    columns.emplace(next->face, next->i / hog::globe::brick_side,
                                    next->j / hog::globe::brick_side);
                }
            }
        }
        for (const auto& [face, bi, bj] : columns) {
            for (std::uint32_t bl = first; bl < first + 3; ++bl) {
                bricks_.push_back(
                    {.face = static_cast<std::uint8_t>(face), .i = bi, .j = bj, .layer = bl});
            }
        }
        start({}, Material::Basalt, false);
    }
    [[nodiscard]] Region& region() { return *region_; }
    [[nodiscard]] int floor() const { return floor_; }
    [[nodiscard]] World& world() { return world_; }
    [[nodiscard]] const Volume& volume() const { return volume_; }
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
    [[nodiscard]] std::int64_t solid(std::uint32_t i, std::uint32_t j, int layer) {
        return region_->at(at(i, j, layer)).solid_amount;
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
    void pour(std::uint32_t i, std::uint32_t j, int layer, std::int64_t amount,
              std::uint8_t rise = 0, std::uint8_t peak = 0) {
        region_->pour(at(i, j, layer), static_cast<std::uint16_t>(amount), rise, peak);
    }
    void pour_lava(std::uint32_t i, std::uint32_t j, int layer, std::int64_t amount,
                   double kelvin) {
        region_->pour_lava(at(i, j, layer), Material::Basalt, static_cast<std::uint16_t>(amount),
                           kelvin);
    }
    [[nodiscard]] Voxel voxel(std::uint32_t i, std::uint32_t j, int layer) {
        return region_->at(at(i, j, layer));
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
    void start(const std::function<bool(std::uint32_t, std::uint32_t, int)>& shape, Material ground,
               bool hold_sea) {
        for (const BrickSite brick : bricks_) {
            BrickVoxels voxels{};
            for (int k = 0; k < hog::globe::brick_voxels; ++k) {
                const Site site = hog::globe::site_in(brick, k);
                const auto layer = static_cast<int>(site.layer);
                const bool open =
                    layer >= floor_ &&
                    (!shape || shape(site.i - corner_i_, site.j - corner_j_, layer - floor_));
                if (!open) {
                    Voxel& voxel = voxels[static_cast<std::size_t>(k)];
                    voxel.solid = ground;
                    voxel.solid_amount = Voxel::full;
                }
            }
            // Made from the bottom, the rest of each tile column left as air.
            const Site corner = hog::globe::site_in(brick, 0);
            if (!volume_.floor(corner)) {
                volume_.set_floor(corner, 0);
            }
            world_.write(brick, voxels);
        }
        region_ = std::make_unique<Region>(world_, hold_sea);
        for (const BrickSite brick : bricks_) {
            region_->load(brick);
        }
    }

    Volume volume_;
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
