#include <hog/core/assert.hpp>
#include <hog/globe/world.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hog::globe {
namespace {

// Its distance in `apart` millimetres.
Voxel generated_at(const Generator::Column& column, const Layers& layers, std::uint32_t layer,
                   int apart = 1) {
    Sample sample = column.sample(layers.middle(static_cast<int>(layer)));
    sample.distance /= apart;
    return voxel_from(sample, distance_band(layers));
}

} // namespace

BrickVoxels generated(const Generator& generator, BrickSite site) {
    const Layers& layers = generator.layers();
    const std::vector<Generator::Column> columns =
        generator.columns({.level = static_cast<std::uint8_t>(layers.level),
                           .face = site.face,
                           .i = site.i * brick_side,
                           .j = site.j * brick_side},
                          brick_side, voxel_lod(generator));
    BrickVoxels voxels{};
    for (int index = 0; index < brick_voxels; ++index) {
        const Site at = site_in(site, index);
        const auto column = static_cast<std::size_t>((index % (brick_side * brick_side)));
        voxels[static_cast<std::size_t>(index)] = generated_at(columns[column], layers, at.layer);
    }
    return voxels;
}

World::World(const Generator& generator, VoxelStore& store)
    : generator_{&generator}, store_{&store} {
    HOG_ASSERT(store.layers() == generator.layers(), "the store stands on other layers");
}

Voxel World::at(Site site) const {
    if (const auto held = store_->at(site)) {
        return *held;
    }
    const Layers& layers = generator_->layers();
    return generated_at(generator_->columns(column_of(site, layers), 1, voxel_lod(*generator_))[0],
                        layers, site.layer);
}

BrickVoxels World::brick(BrickSite site) const {
    if (const Brick* held = store_->brick(site)) {
        BrickVoxels voxels{};
        held->unpack(voxels);
        return voxels;
    }
    return generated(*generator_, site);
}

std::vector<Voxel> World::fill(std::span<const Cell> cells,
                               std::span<const Generator::Column> columns, int bottom, int count,
                               int apart) const {
    const Layers& layers = generator_->layers();
    HOG_ASSERT(cells.size() == columns.size(), "a column for each cell");
    HOG_ASSERT(apart > 0 && bottom >= 0 && count >= 0 &&
                   (count == 0 || bottom + ((count - 1) * apart) < layers.count),
               "layers out of range");
    std::vector<Voxel> voxels(cells.size() * static_cast<std::size_t>(count));
    std::unordered_map<std::uint64_t, BrickVoxels> held;
    for (std::size_t k = 0; k < cells.size(); ++k) {
        HOG_ASSERT(std::cmp_equal(cells[k].level, layers.level), "cells of the voxels' level");
        // The voxels' run up the column, `apart` layers between each.
        const auto voxel_at = [&](int step) -> Voxel& {
            return voxels[(k * static_cast<std::size_t>(count)) + static_cast<std::size_t>(step)];
        };
        const auto layer_of = [&](int step) { return bottom + (step * apart); };
        for (int step = 0; step < count;) {
            const Site site{.face = cells[k].face,
                            .i = cells[k].i,
                            .j = cells[k].j,
                            .layer = static_cast<std::uint32_t>(layer_of(step))};
            const BrickSite in = brick_of(site);
            // The steps whose layers lie in the same brick.
            int end = step + 1;
            while (end < count && std::cmp_less(layer_of(end), (in.layer + 1) * brick_side)) {
                ++end;
            }
            const Brick* stored = store_->brick(in);
            if (stored == nullptr) {
                for (; step < end; ++step) {
                    voxel_at(step) = generated_at(
                        columns[k], layers, static_cast<std::uint32_t>(layer_of(step)), apart);
                }
                continue;
            }
            const auto [found, added] = held.try_emplace(brick_key(in));
            if (added) {
                stored->unpack(found->second);
            }
            for (; step < end; ++step) {
                const Site at{.face = site.face,
                              .i = site.i,
                              .j = site.j,
                              .layer = static_cast<std::uint32_t>(layer_of(step))};
                Voxel& voxel = voxel_at(step);
                voxel = found->second[static_cast<std::size_t>(index_in_brick(at))];
                voxel.distance = distance_in_mm(metres(voxel.distance) / apart);
            }
        }
    }
    return voxels;
}

void World::write(BrickSite site, const BrickVoxels& voxels) {
    if (voxels == generated(*generator_, site)) {
        store_->erase(site);
    } else {
        store_->put(site, Brick::pack(voxels));
    }
}

std::map<BrickSite, std::vector<int>> sites_near(const Layers& layers, DVec3 centre,
                                                 double reach) {
    const double from = glm::length(centre);
    const int lowest = std::max(0, layers.layer_at(from - reach));
    const int highest = std::min(layers.count - 1, layers.layer_at(from + reach));
    std::map<BrickSite, std::vector<int>> near;
    if (lowest > highest) {
        return near;
    }
    // Columns out from the one under the middle, while their middles are near enough; a cell
    // is never wider than this slack.
    const DVec3 direction = centre / from;
    const double slack = 2.0 * distance_band(layers);
    std::set<Cell> seen{cell_under(direction, layers.level)};
    std::vector<Cell> open{*seen.begin()};
    while (!open.empty()) {
        const Cell cell = open.back();
        open.pop_back();
        const CellNeighbors beside = sides(cell);
        for (const Cell next : beside.all()) {
            if (glm::length((center(next) * from) - centre) <= reach + slack &&
                seen.insert(next).second) {
                open.push_back(next);
            }
        }
        for (int layer = lowest; layer <= highest; ++layer) {
            const Site site{.face = cell.face,
                            .i = cell.i,
                            .j = cell.j,
                            .layer = static_cast<std::uint32_t>(layer)};
            near[brick_of(site)].push_back(index_in_brick(site));
        }
    }
    return near;
}

void dig_voxel(Voxel& voxel, double to_shape, DVec3 outward) {
    const std::int16_t out = distance_in_mm(-to_shape);
    if (out > voxel.distance) {
        voxel.distance = out;
        voxel.normal = packed_normal(-outward);
        if (out > 0) {
            voxel.solid = Material::Air;
        }
    }
}

void build_voxel(Voxel& voxel, double to_shape, DVec3 outward, Material material) {
    const std::int16_t in = distance_in_mm(to_shape);
    if (in < voxel.distance) {
        voxel.distance = in;
        voxel.normal = packed_normal(outward);
        if (in < 0) {
            voxel.solid = material;
            voxel.condition = Voxel::sound;
        }
    }
}

namespace {

// Each voxel in the ball or within the band of its surface, given its distance to that surface
// in the band and, near it, the way out of the ball.
void reshape(World& world, DVec3 centre, double radius, const auto& change) {
    const Layers& layers = world.store().layers();
    const double band = distance_band(layers);
    for (const auto& [brick, indices] : sites_near(layers, centre, radius + band)) {
        BrickVoxels voxels = world.brick(brick);
        for (const int index : indices) {
            const DVec3 out = middle_of(site_in(brick, index), layers) - centre;
            const double to_ball = glm::length(out) - radius;
            if (to_ball <= band) {
                const bool near = std::abs(to_ball) < band && glm::length(out) > 0.0;
                Voxel& voxel = voxels[static_cast<std::size_t>(index)];
                change(voxel, std::max(to_ball, -band), near ? glm::normalize(out) : DVec3{0.0});
                sync_solid_amount(voxel, layers.thickness);
            }
        }
        world.write(brick, voxels);
    }
}

} // namespace

void dig(World& world, DVec3 centre, double radius) {
    reshape(world, centre, radius, [](Voxel& voxel, double to_ball, DVec3 outward) {
        dig_voxel(voxel, to_ball, outward);
    });
}

void build(World& world, DVec3 centre, double radius, Material material) {
    reshape(world, centre, radius, [&](Voxel& voxel, double to_ball, DVec3 outward) {
        build_voxel(voxel, to_ball, outward, material);
    });
}

} // namespace hog::globe
