#include <hog/core/assert.hpp>
#include <hog/globe/world.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace hog::globe {
namespace {

// One voxel standing for those of `layers` together: each matter as much of it as they hold on
// average, of the kind most of it is; the rest as the lowest has it.
Voxel averaged(std::span<const Voxel> layers) {
    Voxel out = layers.front();
    const auto share = [&](Material Voxel::* kind, std::uint16_t Voxel::* amount) {
        std::uint32_t total = 0;
        const Voxel* most = &layers.front();
        for (const Voxel& voxel : layers) {
            total += voxel.*amount;
            most = voxel.*amount > most->*amount ? &voxel : most;
        }
        const auto count = static_cast<std::uint32_t>(layers.size());
        out.*amount = static_cast<std::uint16_t>((total + (count / 2)) / count);
        out.*kind = out.*amount == 0 ? Material::Air : most->*kind;
    };
    share(&Voxel::solid, &Voxel::solid_amount);
    share(&Voxel::grain, &Voxel::grain_amount);
    share(&Voxel::liquid, &Voxel::liquid_amount);
    return out;
}

} // namespace

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

std::uint32_t bricks_across(const Layers& layers) noexcept {
    return (std::uint32_t{1} << static_cast<unsigned>(layers.level)) / brick_side;
}

std::uint32_t bricks_up(const Layers& layers) noexcept {
    return static_cast<std::uint32_t>((layers.count + brick_side - 1) / brick_side);
}

bool in_layers(const Layers& layers, BrickSite site) noexcept {
    return site.face < 6 && site.i < bricks_across(layers) && site.j < bricks_across(layers) &&
           site.layer < bricks_up(layers);
}

World::World(const Generator& generator, Volume& volume)
    : generator_{&generator}, planet_{&generator.planet()}, layers_{generator.layers()},
      volume_{&volume} {}

World::World(const Planet& planet, const Layers& layers, Volume& volume)
    : planet_{&planet}, layers_{layers}, volume_{&volume} {}

const Generator& World::generator() const noexcept {
    HOG_ASSERT(generator_ != nullptr, "a world made with a generator");
    return *generator_;
}

void World::column(Site base, int bottom, int count, int apart, std::span<Voxel> out,
                   std::size_t stride) const {
    const Planet& planet = *planet_;
    const Layers& layers = layers_;
    const DVec3 up = center(column_of(base, layers));
    const std::optional<std::uint32_t> lowest = volume_->floor(base);
    if (!lowest && tiles_ != nullptr) {
        held_column(base, up, bottom, count, apart, out, stride);
        return;
    }
    constexpr auto side = static_cast<std::uint32_t>(Volume::tile_side);
    const auto x = static_cast<int>(base.i % side);
    const auto y = static_cast<int>(base.j % side);
    const VoxelTree* tile = nullptr;
    std::optional<std::uint32_t> tile_k;
    for (int step = 0; step < count; ++step) {
        const auto layer = static_cast<std::uint32_t>(bottom + (step * apart));
        const std::uint32_t at = layer;
        Cube cube;
        if (lowest && at < *lowest) {
            // Under a column's floor nothing is made yet: deep ground, taken as the continents'
            // basement.
            cube = {.solid = Material::Granite, .solid_amount = Voxel::full};
        } else {
            if (tile_k != at / side) {
                tile_k = at / side;
                tile = volume_->tile_at(
                    {.face = base.face, .i = base.i, .j = base.j, .layer = *tile_k * side});
            }
            if (tile != nullptr) {
                cube = tile->at(x, y, static_cast<int>(at % side));
            }
        }
        out[static_cast<std::size_t>(step) * stride] =
            voxel_of(cube, place_temperature(planet, up, layers.middle(static_cast<int>(layer))));
    }
}

void World::held_column(Site base, DVec3 up, int bottom, int count, int apart, std::span<Voxel> out,
                        std::size_t stride) const {
    constexpr auto side = static_cast<std::int64_t>(Volume::tile_side);
    std::optional<HeldTile> held;
    std::int64_t from = 0; // the layers the tile read spans
    std::int64_t to = 0;
    for (int step = 0; step < count; ++step) {
        const auto layer = static_cast<std::uint32_t>(bottom + (step * apart));
        if (std::cmp_less(layer, from) || std::cmp_greater_equal(layer, to)) {
            held = tiles_->finest_over({.level = 0,
                                        .face = base.face,
                                        .i = base.i / Volume::tile_side,
                                        .j = base.j / Volume::tile_side,
                                        .k = layer / Volume::tile_side});
            const unsigned level = held ? held->key.level : 0U;
            const std::int64_t k = held ? held->key.k : layer / side;
            from = (k * side) << level;
            to = ((k + 1) * side) << level;
        }
        Cube cube;
        if (held) {
            const unsigned level = held->key.level;
            const auto place = [&](std::uint32_t at, std::uint32_t tile) {
                return static_cast<int>((at >> level) - (tile * Volume::tile_side));
            };
            cube = held->tree->at(place(base.i, held->key.i), place(base.j, held->key.j),
                                  place(layer, held->key.k));
        }
        out[static_cast<std::size_t>(step) * stride] = voxel_of(
            cube, place_temperature(*planet_, up, layers_.middle(static_cast<int>(layer))));
    }
}

const VoxelTree* World::tile(TileKey key) const {
    if (const VoxelTree* found = volume_->tile(key)) {
        return found;
    }
    // In a column made, what isn't held is air over its floor and rock under it.
    if (key.level == 0) {
        const Site site{.face = key.face,
                        .i = key.i * Volume::tile_side,
                        .j = key.j * Volume::tile_side,
                        .layer = key.k * Volume::tile_side};
        if (const std::optional<std::uint32_t> floor = volume_->floor(site)) {
            static const VoxelTree air{Volume::tile_side};
            static const VoxelTree rock{Volume::tile_side,
                                        {.solid = Material::Granite, .solid_amount = Voxel::full}};
            return site.layer >= *floor ? &air : &rock;
        }
    }
    return tiles_ != nullptr ? tiles_->tile(key) : nullptr;
}

std::optional<HeldTile> World::tile_over(TileKey key) const {
    const auto corner = [&](std::uint32_t at) { return (at << key.level) * Volume::tile_side; };
    const Site site{
        .face = key.face, .i = corner(key.i), .j = corner(key.j), .layer = corner(key.k)};
    // In a column made, nothing coarser stands for what isn't held: it's open air, or deep rock.
    if (key.level == 0 && volume_->floor(site)) {
        const VoxelTree* found = volume_->tile(key);
        return found != nullptr ? std::optional{HeldTile{.key = key, .tree = found}} : std::nullopt;
    }
    if (const auto over = volume_->finest_over(site, key.level)) {
        TileKey found = key;
        for (std::uint8_t level = key.level; level < over->first; ++level) {
            found = found.parent();
        }
        return HeldTile{.key = found, .tree = over->second};
    }
    return tiles_ != nullptr ? tiles_->finest_over(key) : std::nullopt;
}

Voxel World::at(Site site) const {
    Voxel voxel;
    column(site, static_cast<int>(site.layer), 1, 1, std::span{&voxel, 1}, 1);
    return voxel;
}

BrickVoxels World::brick(BrickSite site) const {
    BrickVoxels voxels{};
    constexpr std::size_t layer_stride = static_cast<std::size_t>(brick_side) * brick_side;
    for (int index = 0; index < brick_side * brick_side; ++index) {
        column(site_in(site, index), static_cast<int>(site.layer) * brick_side, brick_side, 1,
               std::span{voxels}.subspan(static_cast<std::size_t>(index)), layer_stride);
    }
    return voxels;
}

std::vector<Voxel> World::fill(std::span<const Cell> cells, int bottom, int count,
                               int apart) const {
    const Layers& layers = layers_;
    HOG_ASSERT(apart > 0 && bottom >= 0 && count >= 0 &&
                   (count == 0 || bottom + ((count - 1) * apart) < layers.count),
               "layers out of range");
    std::vector<Voxel> voxels(cells.size() * static_cast<std::size_t>(count));
    for (std::size_t k = 0; k < cells.size(); ++k) {
        HOG_ASSERT(std::cmp_equal(cells[k].level, layers.level), "cells of the voxels' level");
        if (count == 0) {
            continue;
        }
        const std::span<Voxel> out = std::span{voxels}.subspan(k * static_cast<std::size_t>(count),
                                                               static_cast<std::size_t>(count));
        const Site base{.face = cells[k].face, .i = cells[k].i, .j = cells[k].j};
        if (apart == 1) {
            column(base, bottom, count, 1, out, 1);
            continue;
        }
        const int every = std::min(count * apart, layers.count - bottom);
        std::vector<Voxel> each(static_cast<std::size_t>(every));
        column(base, bottom, every, 1, each, 1);
        for (int step = 0; step < count; ++step) {
            const int from = step * apart;
            out[static_cast<std::size_t>(step)] = averaged(
                std::span{each}.subspan(static_cast<std::size_t>(from),
                                        static_cast<std::size_t>(std::min(apart, every - from))));
        }
    }
    return voxels;
}

void World::write(BrickSite site, const BrickVoxels& voxels) {
    const Planet& planet = *planet_;
    const Layers& layers = layers_;
    for (int index = 0; index < brick_side * brick_side; ++index) {
        const Site base = site_in(site, index);
        const DVec3 up = center(column_of(base, layers));
        for (int up_by = 0; up_by < brick_side; ++up_by) {
            const int at = index + (up_by * brick_side * brick_side);
            const Site there = site_in(site, at);
            volume_->set(there,
                         cube_of(voxels[static_cast<std::size_t>(at)],
                                 place_temperature(planet, up,
                                                   layers.middle(static_cast<int>(there.layer)))));
        }
    }
}

std::optional<int> highest_layer(const World& world, Cell cell) {
    const Site site{.face = cell.face, .i = cell.i, .j = cell.j, .layer = 0};
    std::optional<std::uint32_t> top = world.volume().top(site);
    if (!top && !world.volume().floor(site) && world.tiles() != nullptr) {
        top = world.tiles()->top(site, static_cast<std::uint32_t>(world.layers().count));
    }
    if (!top) {
        return std::nullopt;
    }
    return std::min(static_cast<int>(*top), world.layers().count - 1);
}

std::optional<double> surface_under(const World& world, DVec3 direction, double from) {
    const Layers& layers = world.layers();
    const Cell cell = cell_under(direction, layers.level);
    const std::optional<int> highest = highest_layer(world, cell);
    if (!highest) {
        return std::nullopt;
    }
    const int start = std::min(layers.layer_at(from), *highest);
    const auto in_ground = [](const Voxel& voxel) {
        return voxel.solid != Material::Air || voxel.grain_amount >= Voxel::full / 2;
    };
    const auto share = [](std::uint16_t amount) { return amount / double{Voxel::full}; };
    constexpr int stretch = 32;
    Voxel above{};
    for (int top = start; top >= 0; top -= stretch) {
        const int bottom = std::max(0, top - stretch + 1);
        const std::vector<Voxel> voxels = world.fill(std::span{&cell, 1}, bottom, top - bottom + 1);
        for (int layer = top; layer >= bottom; --layer) {
            const Voxel& voxel = voxels[static_cast<std::size_t>(layer - bottom)];
            if (!in_ground(voxel)) {
                above = voxel;
                continue;
            }
            double surface =
                voxel.solid != Material::Air
                    ? layers.middle(layer) + ((share(voxel.solid_amount) - 0.5) * layers.thickness)
                    : layers.bottom(layer) + (share(voxel.grain_amount) * layers.thickness);
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
    return surface_under(world, direction, world.layers().ceiling());
}

double ground_height(const World& world, DVec3 at) {
    return top_of(world, glm::normalize(at)).value_or(world.layers().floor) - world.planet().radius;
}

std::map<BrickSite, std::vector<int>> sites_near(const Layers& layers, DVec3 centre, double reach,
                                                 double height) {
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

std::vector<Site> tile_columns_near(const Layers& layers, DVec3 centre, double reach) {
    constexpr auto side = static_cast<std::uint32_t>(Volume::tile_side);
    // A tile column is a cell this many levels coarser than a voxel's.
    constexpr int coarser = std::countr_zero(side);
    const double from = glm::length(centre);
    const DVec3 direction = centre / from;
    // No tile column's ground is further than this from its middle.
    const double slack = Volume::tile_side * layers.thickness;
    std::set<Cell> seen{cell_under(direction, layers.level - coarser)};
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
    }
    std::vector<Site> corners;
    corners.reserve(seen.size());
    for (const Cell& cell : seen) {
        corners.push_back({.face = cell.face, .i = cell.i * side, .j = cell.j * side});
    }
    std::ranges::sort(corners);
    return corners;
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
    const Layers& layers = world.layers();
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
    const double thickness = world.layers().thickness;
    reshape(world, centre, reach, height, shape,
            [thickness](Voxel& voxel, double to_shape) { dig_voxel(voxel, to_shape, thickness); });
}

void build_shape(World& world, DVec3 centre, double reach, double height, const Shape& shape,
                 Material material) {
    const double thickness = world.layers().thickness;
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
