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

Voxel generated_at(const Generator::Column& column, const Layers& layers, std::uint32_t layer,
                   int apart) {
    return voxel_from(column.sample(layers.middle(static_cast<int>(layer))),
                      layers.thickness * apart);
}

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
                voxel.solid_amount = amount_at(distance_of(voxel.solid_amount, layers.thickness),
                                               layers.thickness * apart);
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

int highest_layer(const World& world, Cell cell, const Generator::Column& column) {
    const Layers& layers = world.store().layers();
    int highest = layers.layer_at(column.top()) + 1;
    const Site at{.face = cell.face, .i = cell.i, .j = cell.j, .layer = 0};
    if (const auto held = world.store().highest_held(brick_of(at))) {
        highest = std::max(highest, ((static_cast<int>(*held) + 1) * brick_side) - 1);
    }
    return std::min(highest, layers.count - 1);
}

std::optional<double> surface_under(const World& world, DVec3 direction, double from) {
    const Generator& generator = world.generator();
    const Layers& layers = generator.layers();
    const Cell cell = cell_under(direction, layers.level);
    const std::vector<Generator::Column> columns =
        generator.columns(std::span{&cell, 1}, voxel_lod(generator));
    const int start = std::min(layers.layer_at(from), highest_layer(world, cell, columns[0]));
    const auto in_ground = [](const Voxel& voxel) {
        return voxel.solid != Material::Air || voxel.grain_amount >= Voxel::full / 2;
    };
    const auto share = [](std::uint16_t amount) { return amount / double{Voxel::full}; };
    constexpr int stretch = 32;
    Voxel above{};
    for (int top = start; top >= 0; top -= stretch) {
        const int bottom = std::max(0, top - stretch + 1);
        const std::vector<Voxel> voxels = world.fill(std::span{&cell, 1}, columns, bottom,
                                                     top - bottom + 1);
        for (int layer = top; layer >= bottom; --layer) {
            const Voxel& voxel = voxels[static_cast<std::size_t>(layer - bottom)];
            if (!in_ground(voxel)) {
                above = voxel;
                continue;
            }
            double surface = voxel.solid != Material::Air
                                 ? layers.middle(layer) +
                                       ((share(voxel.solid_amount) - 0.5) * layers.thickness)
                                 : layers.bottom(layer) + (share(voxel.grain_amount) *
                                                           layers.thickness);
            // Grain piled in the open voxel over it stands on it.
            if (above.grain_amount > 0) {
                surface = std::max(surface, layers.bottom(layer + 1) +
                                                (share(above.grain_amount) * layers.thickness));
            }
            return surface;
        }
    }
    return std::nullopt;
}

std::optional<double> top_of(const World& world, DVec3 direction) {
    return surface_under(world, direction, world.generator().layers().ceiling());
}

std::map<BrickSite, std::vector<int>> sites_near(const Layers& layers, DVec3 centre,
                                                 double reach, double height) {
    const double from = glm::length(centre);
    const double up = height < 0.0 ? reach : height;
    const int lowest = std::max(0, layers.layer_at(from - up));
    const int highest = std::min(layers.count - 1, layers.layer_at(from + up));
    std::map<BrickSite, std::vector<int>> near;
    if (lowest > highest) {
        return near;
    }
    // Columns out from the one under the middle, while their middles are near enough; a cell
    // is never wider than this slack.
    const DVec3 direction = centre / from;
    const double slack = 8.0 * layers.thickness;
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

void dig_voxel(Voxel& voxel, double to_shape, double thickness) {
    const std::uint16_t left = amount_at(-to_shape, thickness);
    if (left < voxel.solid_amount) {
        voxel.solid_amount = left;
        if (left < Voxel::full / 2) {
            voxel.solid = Material::Air;
        }
    }
}

void build_voxel(Voxel& voxel, double to_shape, Material material, double thickness) {
    const std::uint16_t filled = amount_at(to_shape, thickness);
    if (filled > voxel.solid_amount) {
        voxel.solid_amount = filled;
        if (filled >= Voxel::full / 2) {
            voxel.solid = material;
            voxel.condition = Voxel::sound;
        }
    }
}

namespace {

// Each voxel a shape's surface passes within reach of, given its distance to that surface: a
// voxel's amount tells a surface only within half its thickness, across its diagonal at most.
void reshape(World& world, DVec3 centre, double reach, double height, const Shape& shape,
             const auto& change) {
    const Layers& layers = world.store().layers();
    const double band = layers.thickness;
    for (const auto& [brick, indices] : sites_near(layers, centre, reach + band, height + band)) {
        BrickVoxels voxels = world.brick(brick);
        for (const int index : indices) {
            const double to_shape = shape(middle_of(site_in(brick, index), layers));
            if (to_shape <= band) {
                change(voxels[static_cast<std::size_t>(index)], to_shape);
            }
        }
        world.write(brick, voxels);
    }
}

Shape ball(DVec3 centre, double radius) {
    return [centre, radius](DVec3 at) { return glm::length(at - centre) - radius; };
}

} // namespace

void dig_shape(World& world, DVec3 centre, double reach, double height, const Shape& shape) {
    const double thickness = world.generator().layers().thickness;
    reshape(world, centre, reach, height, shape, [thickness](Voxel& voxel, double to_shape) {
        dig_voxel(voxel, to_shape, thickness);
    });
}

void build_shape(World& world, DVec3 centre, double reach, double height, const Shape& shape,
                 Material material) {
    const double thickness = world.generator().layers().thickness;
    reshape(world, centre, reach, height, shape, [&](Voxel& voxel, double to_shape) {
        build_voxel(voxel, to_shape, material, thickness);
    });
}

void dig(World& world, DVec3 centre, double radius) {
    dig_shape(world, centre, radius, radius, ball(centre, radius));
}

void build(World& world, DVec3 centre, double radius, Material material) {
    build_shape(world, centre, radius, radius, ball(centre, radius), material);
}

} // namespace hog::globe
