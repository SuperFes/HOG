#include <hog/server/ground_bake.hpp>

#include <hog/core/bytes.hpp>
#include <hog/flow/simulation.hpp>
#include <hog/globe/cell.hpp>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <utility>

namespace hog::server {
namespace {

constexpr double hours_a_year = 365.25 * 24.0;

// Tiles across a face.
std::uint32_t tiles_across(const globe::VoxelStore& store) {
    return std::max<std::uint32_t>(1, store.bricks_across() / GroundBake::tile_side);
}

} // namespace

GroundBake::Tile GroundBake::tile_at(const globe::VoxelStore& store, DVec3 up) {
    const globe::Layers& layers = store.layers();
    const globe::Cell cell = globe::cell_under(up, layers.level);
    const std::uint32_t across = globe::brick_side * tile_side;
    const std::uint32_t last = tiles_across(store) - 1;
    return {.face = cell.face,
            .i = std::min(cell.i / across, last),
            .j = std::min(cell.j / across, last)};
}

std::vector<GroundBake::Tile> GroundBake::due(const globe::VoxelStore& store,
                                              std::span<const DVec3> places, int reach) const {
    const auto across = static_cast<std::int64_t>(tiles_across(store));
    std::vector<Tile> found;
    std::set<Tile> seen;
    const auto consider = [&](const Tile& home, int di, int dj) {
        const std::int64_t i = std::int64_t{home.i} + di;
        const std::int64_t j = std::int64_t{home.j} + dj;
        // Tiles past the face's edge wait for a place on the face beside.
        if (i < 0 || j < 0 || i >= across || j >= across) {
            return;
        }
        const Tile tile{
            .face = home.face, .i = static_cast<std::uint32_t>(i), .j = static_cast<std::uint32_t>(j)};
        if (!baked(tile) && seen.insert(tile).second) {
            found.push_back(tile);
        }
    };
    for (int ring = 0; ring <= reach; ++ring) {
        for (const DVec3 up : places) {
            const Tile home = tile_at(store, up);
            // The ring's edge: its top and bottom rows whole, its sides between.
            for (int d = -ring; d <= ring; ++d) {
                consider(home, d, -ring);
                if (ring > 0) {
                    consider(home, d, ring);
                }
            }
            for (int d = 1 - ring; d < ring; ++d) {
                consider(home, -ring, d);
                consider(home, ring, d);
            }
        }
    }
    return found;
}

std::vector<globe::BrickSite> GroundBake::bricks_of(const globe::World& world, Tile tile) {
    const globe::Generator& generator = world.generator();
    const globe::Layers& layers = generator.layers();
    const globe::VoxelStore& store = world.store();
    std::vector<globe::BrickSite> bricks;
    for (std::uint32_t a = 0; a < tile_side; ++a) {
        for (std::uint32_t b = 0; b < tile_side; ++b) {
            const std::uint32_t bi = (tile.i * tile_side) + a;
            const std::uint32_t bj = (tile.j * tile_side) + b;
            if (bi >= store.bricks_across() || bj >= store.bricks_across()) {
                continue;
            }
            // The stack's middle column tells where its ground is, give or take its slopes.
            const globe::Cell cell{.level = static_cast<std::uint8_t>(layers.level),
                                   .face = tile.face,
                                   .i = (bi * globe::brick_side) + (globe::brick_side / 2),
                                   .j = (bj * globe::brick_side) + (globe::brick_side / 2)};
            const std::vector<globe::Generator::Column> columns =
                generator.columns(std::span{&cell, 1}, globe::voxel_lod(generator));
            const int highest = globe::highest_layer(world, cell, columns[0]);
            const double ground =
                globe::surface_under(world, globe::center(cell), layers.top(highest))
                    .value_or(layers.floor);
            const int low = std::max(0, layers.layer_at(ground) - (2 * globe::brick_side));
            const int high = std::min(layers.count - 1, highest + globe::brick_side);
            for (int layer = low / globe::brick_side; layer <= high / globe::brick_side; ++layer) {
                bricks.push_back({.face = tile.face,
                                  .i = bi,
                                  .j = bj,
                                  .layer = static_cast<std::uint32_t>(layer)});
            }
        }
    }
    return bricks;
}

std::vector<GroundBake::Baked> GroundBake::held_in(const globe::VoxelStore& store, Tile tile) {
    std::vector<Baked> held;
    for (std::uint32_t a = 0; a < tile_side; ++a) {
        for (std::uint32_t b = 0; b < tile_side; ++b) {
            const globe::BrickSite stack{
                .face = tile.face, .i = (tile.i * tile_side) + a, .j = (tile.j * tile_side) + b};
            if (stack.i >= store.bricks_across() || stack.j >= store.bricks_across()) {
                continue;
            }
            for (const std::uint32_t layer : store.layers_held(stack)) {
                globe::BrickSite site = stack;
                site.layer = layer;
                held.push_back({.site = site, .brick = *store.brick(site)});
            }
        }
    }
    return held;
}

std::vector<GroundBake::Baked> GroundBake::bake(const globe::Generator& generator,
                                                const std::vector<Baked>& held, Tile tile,
                                                const RainRecord& rain, double evaporation) {
    globe::VoxelStore store{generator.layers()};
    for (const Baked& one : held) {
        if (one.brick) {
            store.put(one.site, *one.brick);
        }
    }
    store.clear_changed();
    globe::World world{generator, store};
    flow::Simulation flow{world};
    static_cast<void>(flow.settle_water(
        bricks_of(world, tile),
        {.rain = [&](DVec3 up) { return rain.at(up); }, .inflow = {}, .evaporation = evaporation}));
    std::vector<Baked> written;
    for (const globe::BrickSite site : store.changed()) {
        const globe::Brick* brick = store.brick(site);
        written.push_back({.site = site,
                           .brick = brick != nullptr ? std::optional{*brick} : std::nullopt});
    }
    return written;
}

std::vector<std::byte> GroundBake::saved() const {
    ByteWriter out;
    out.write_varint(baked_.size());
    for (const Tile& tile : baked_) {
        out.write(tile.face);
        out.write_varint(tile.i);
        out.write_varint(tile.j);
    }
    return std::move(out).take();
}

Result<void> GroundBake::restore(std::span<const std::byte> bytes) {
    ByteReader in{bytes};
    const auto count = in.read_varint();
    if (!count) {
        return std::unexpected(count.error());
    }
    std::set<Tile> read;
    for (std::uint64_t k = 0; k < *count; ++k) {
        const auto face = in.read<std::uint8_t>();
        const auto i = in.read_varint();
        const auto j = in.read_varint();
        if (!face || !i || !j) {
            return make_error(ErrorCode::Parse, "baked tiles cut short");
        }
        if (*face >= 6) {
            return make_error(ErrorCode::Parse, std::format("no face {}", *face));
        }
        read.insert({.face = *face,
                     .i = static_cast<std::uint32_t>(*i),
                     .j = static_cast<std::uint32_t>(*j)});
    }
    baked_ = std::move(read);
    return {};
}

RainRecord::RainRecord(int level, double otherwise)
    : level_{level}, otherwise_{otherwise},
      sum_(static_cast<std::size_t>(globe::cells_at(level)), 0.0),
      hours_(static_cast<std::size_t>(globe::cells_at(level)), 0.0) {}

void RainRecord::add_cell(std::size_t cell, double mm_an_hour, double hours) {
    sum_[cell] += mm_an_hour * hours;
    hours_[cell] += hours;
}

DVec3 RainRecord::middle_of(std::size_t cell) const {
    return globe::center(globe::cell_at_index(level_, cell));
}

double RainRecord::at(DVec3 up) const {
    const auto cell = static_cast<std::size_t>(globe::index_of(globe::cell_under(up, level_)));
    if (hours_[cell] <= 0.0) {
        return otherwise_;
    }
    return sum_[cell] / hours_[cell] * hours_a_year / 1'000.0;
}

} // namespace hog::server
