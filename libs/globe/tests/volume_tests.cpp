#include "bed.hpp"

#include <hog/globe/volume.hpp>
#include <hog/globe/volume_archive.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/voxel.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <iostream>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using hog::globe::Cell;
using hog::globe::Cube;
using hog::globe::Generator;
using hog::globe::Material;
using hog::globe::Site;
using hog::globe::Volume;
using hog::globe::Voxel;
using hog::globe::VoxelTree;
using hog::globe::bed::earth;
using hog::globe::bed::somewhere;

namespace {

Cube full_of(Material material) {
    return {.solid = material, .solid_amount = Voxel::full};
}

// Rock up to `top` across a few tiles, either side of a tile edge, made from the bottom.
Volume rock_to(std::uint32_t top) {
    Volume volume;
    volume.set_floor({.face = 2, .i = 0, .j = 5}, 0);
    volume.set_floor({.face = 2, .i = 64, .j = 5}, 0);
    for (std::uint32_t i = 60; i < 70; ++i) {
        for (std::uint32_t layer = 0; layer <= top; ++layer) {
            volume.set({.face = 2, .i = i, .j = 5, .layer = layer}, full_of(Material::Granite));
        }
    }
    return volume;
}

} // namespace

TEST_CASE("a volume holds no air", "[globe][volume]") {
    Volume volume;
    volume.set_floor({.face = 1, .i = 9, .j = 9}, 0);
    CHECK(volume.at({.face = 1, .i = 9, .j = 9, .layer = 9}) == Cube{});
    volume.set({.face = 1, .i = 9, .j = 9, .layer = 9}, Cube{});
    CHECK(volume.tiles() == 0);
    volume.set({.face = 1, .i = 9, .j = 9, .layer = 9}, full_of(Material::Basalt));
    CHECK(volume.tiles() == 1);
    CHECK(volume.at({.face = 1, .i = 9, .j = 9, .layer = 9}).solid == Material::Basalt);
    volume.remove({.face = 1, .i = 9, .j = 9, .layer = 9});
    CHECK(volume.tiles() == 0);
}

TEST_CASE("dirt is put on the rock as a voxel on top, and taken off by removing it",
          "[globe][volume]") {
    Volume volume = rock_to(70);
    const Site rock{.face = 2, .i = 63, .j = 5, .layer = 70};
    const Site over{.face = 2, .i = 63, .j = 5, .layer = 71};
    REQUIRE(volume.put_on(rock, full_of(Material::Soil)));
    CHECK(volume.at(rock).solid == Material::Granite);
    CHECK(volume.at(over).solid == Material::Soil);
    // Nothing goes on air, or where something already is.
    CHECK_FALSE(volume.put_on({.face = 2, .i = 63, .j = 5, .layer = 80}, full_of(Material::Soil)));
    CHECK_FALSE(volume.put_on(rock, full_of(Material::Clay)));
    volume.remove(over);
    CHECK(volume.at(over) == Cube{});
    CHECK(volume.at(rock).solid == Material::Granite);
}

TEST_CASE("a column's top is its highest voxel holding matter, whatever its tile holds",
          "[globe][volume]") {
    Volume volume = rock_to(70);
    CHECK(volume.top({.face = 2, .i = 63, .j = 5}) == 70U);
    // A voxel high in a tile of its own tops only its own column.
    volume.set({.face = 2, .i = 61, .j = 5, .layer = 200}, full_of(Material::Snow));
    CHECK(volume.top({.face = 2, .i = 61, .j = 5}) == 200U);
    CHECK(volume.top({.face = 2, .i = 62, .j = 5}) == 70U);
    // Warm air over the ground is no matter.
    volume.set({.face = 2, .i = 62, .j = 5, .layer = 90}, Cube{.heat = 3});
    CHECK(volume.top({.face = 2, .i = 62, .j = 5}) == 70U);
    // Where the tile over the ground holds nothing in this column, the one under it says.
    CHECK(volume.top({.face = 2, .i = 60, .j = 5, .layer = 300}) == 70U);
    CHECK_FALSE(volume.top({.face = 2, .i = 59, .j = 5}).has_value());
}

TEST_CASE("nothing is put or taken under a tile column's floor", "[globe][volume]") {
    Volume volume;
    const Site deep{.face = 3, .i = 70, .j = 70, .layer = 100};
    CHECK_FALSE(volume.made(deep));
    CHECK_FALSE(volume.set(deep, full_of(Material::Granite)));
    volume.set_floor(deep, 128);
    CHECK_FALSE(volume.made(deep));
    CHECK_FALSE(volume.set(deep, full_of(Material::Granite)));
    CHECK_FALSE(
        volume.put_on({.face = 3, .i = 70, .j = 70, .layer = 127}, full_of(Material::Soil)));
    CHECK(volume.tiles() == 0);
    // The floor is per tile column.
    CHECK_FALSE(volume.made({.face = 3, .i = 64 + 64, .j = 70, .layer = 200}));
    CHECK(volume.made({.face = 3, .i = 64, .j = 127, .layer = 128}));
}

TEST_CASE("a volume comes back from its bytes as it was", "[globe][volume]") {
    Volume volume = rock_to(70);
    REQUIRE(volume.put_on({.face = 2, .i = 64, .j = 5, .layer = 70}, full_of(Material::Clay)));
    const std::vector<std::byte> bytes = volume.encode();
    auto again = Volume::decode(bytes);
    REQUIRE(again);
    CHECK(again->same_as(volume));
    CHECK(again->at({.face = 2, .i = 64, .j = 5, .layer = 71}).solid == Material::Clay);
    std::vector<std::byte> cut = bytes;
    cut.pop_back();
    CHECK_FALSE(Volume::decode(cut));
}

TEST_CASE("a tile column comes back from its bytes, in place of what was there",
          "[globe][volume]") {
    Volume volume = rock_to(70);
    volume.set_floor({.face = 2, .i = 0, .j = 5}, 12);
    const Site column{.face = 2, .i = 63, .j = 5};
    const std::vector<std::byte> bytes = volume.encode_column(column);
    REQUIRE_FALSE(bytes.empty());
    CHECK(Volume{}.encode_column(column).empty());

    Volume other = rock_to(90);
    REQUIRE(other.decode_column(column, bytes));
    CHECK(other.floor(column) == 12U);
    CHECK(other.at({.face = 2, .i = 63, .j = 5, .layer = 70}).solid == Material::Granite);
    CHECK(other.at({.face = 2, .i = 63, .j = 5, .layer = 80}) == Cube{});
    // Its neighbour, another tile column, is left as it was.
    CHECK(other.at({.face = 2, .i = 64, .j = 5, .layer = 80}).solid == Material::Granite);

    std::vector<std::byte> cut = bytes;
    cut.pop_back();
    CHECK_FALSE(other.decode_column(column, cut));
    CHECK(other.floor(column) == 12U);
}

TEST_CASE("a tile column let go is as if never made, its neighbour left as it was",
          "[globe][volume]") {
    Volume volume = rock_to(70);
    const Site column{.face = 2, .i = 63, .j = 5};
    CHECK(volume.columns() ==
          std::vector<Site>{{.face = 2, .i = 0, .j = 0}, {.face = 2, .i = 64, .j = 0}});
    volume.drop_column(column);
    CHECK(volume.columns() == std::vector<Site>{{.face = 2, .i = 64, .j = 0}});
    CHECK_FALSE(volume.floor(column).has_value());
    CHECK_FALSE(volume.made({.face = 2, .i = 63, .j = 5, .layer = 70}));
    CHECK(volume.at({.face = 2, .i = 63, .j = 5, .layer = 70}) == Cube{});
    CHECK(volume.unsaved() == std::vector<Site>{{.face = 2, .i = 64, .j = 0}});
    CHECK(volume.at({.face = 2, .i = 64, .j = 5, .layer = 70}).solid == Material::Granite);
}

TEST_CASE("a tile column halved is half as fine, its floor counted in its taller voxels",
          "[globe][volume]") {
    Volume volume = rock_to(70);
    volume.set_floor({.face = 2, .i = 0, .j = 5}, 12);
    REQUIRE(volume.column({.face = 2, .i = 63, .j = 5}));
    const hog::globe::TileColumn column =
        volume.column({.face = 2, .i = 63, .j = 5}).value_or(hog::globe::TileColumn{});
    CHECK(column.floor == 12);
    REQUIRE(column.tiles.size() == 2);
    CHECK(column.encode() == volume.encode_column({.face = 2, .i = 63, .j = 5}));
    CHECK_FALSE(volume.column({.face = 2, .i = 300, .j = 5}));

    const hog::globe::TileColumn half = column.halved();
    CHECK(half.floor == 6);
    REQUIRE(half.tiles.size() == 2);
    CHECK(half.tiles[1].first == 1);
    CHECK(half.tiles[1].second.side() == Volume::tile_side / 2);
    // Rock to layer 70 is up to 35 of the halved; i 60 to 63 is 30 to 31.
    CHECK(half.tiles[1].second.at(30, 2, 35 - 32).solid == Material::Granite);
    CHECK(half.tiles[1].second.at(30, 2, 36 - 32) == Cube{});
    CHECK(half.tiles[1].second.at(29, 2, 0) == Cube{});

    const auto back = hog::globe::TileColumn::decode(half.encode(), Volume::tile_side / 2);
    REQUIRE(back);
    CHECK(back->floor == 6);
    CHECK(back->tiles[1].second.same_as(half.tiles[1].second));
    CHECK_FALSE(hog::globe::TileColumn::decode(half.encode(), Volume::tile_side));
}

TEST_CASE("a volume tells the bricks changed since a revision, each once", "[globe][volume]") {
    Volume volume = rock_to(70);
    const std::uint64_t before = volume.revision();
    const Site a{.face = 2, .i = 60, .j = 5, .layer = 71};
    const Site b{.face = 2, .i = 61, .j = 5, .layer = 71};
    const Site c{.face = 2, .i = 69, .j = 5, .layer = 71};
    volume.set(a, full_of(Material::Clay));
    volume.set(c, full_of(Material::Clay));
    const std::uint64_t middle = volume.revision();
    // A brick changed again after a follower last looked is told again.
    volume.set(b, full_of(Material::Clay));
    volume.set(a, full_of(Material::Soil));
    CHECK(volume.changed_since(before) ==
          std::vector{hog::globe::brick_of(a), hog::globe::brick_of(c)});
    CHECK(volume.changed_since(middle) == std::vector{hog::globe::brick_of(a)});
    // Setting what's already there changes nothing; nor does what's refused under the floor.
    const std::uint64_t now = volume.revision();
    volume.set(a, full_of(Material::Soil));
    volume.set({.face = 2, .i = 60, .j = 900, .layer = 1}, full_of(Material::Soil));
    CHECK(volume.revision() == now);
    CHECK(volume.changed_since(now).empty());
}

TEST_CASE("a volume tells the tile columns not saved since they changed", "[globe][volume]") {
    Volume volume = rock_to(70);
    CHECK(volume.unsaved() ==
          std::vector<Site>{{.face = 2, .i = 0, .j = 0}, {.face = 2, .i = 64, .j = 0}});
    volume.saved({.face = 2, .i = 10, .j = 20});
    volume.saved({.face = 2, .i = 64, .j = 0});
    CHECK(volume.unsaved().empty());
    volume.set({.face = 2, .i = 65, .j = 5, .layer = 71}, full_of(Material::Clay));
    CHECK(volume.unsaved() == std::vector<Site>{{.face = 2, .i = 64, .j = 0}});
    // Put back from its saved bytes, it's as saved.
    const std::vector<std::byte> bytes = volume.encode_column({.face = 2, .i = 64, .j = 0});
    REQUIRE(volume.decode_column({.face = 2, .i = 64, .j = 0}, bytes));
    CHECK(volume.unsaved().empty());
}

TEST_CASE("a voxel's heat is told over the open air's warmth where it is", "[globe][volume]") {
    const hog::globe::Planet& planet = earth().planet();
    const hog::globe::Layers& layers = earth().layers();
    const auto at = [&](hog::DVec3 direction, int layer) {
        const Cell cell = hog::globe::cell_under(glm::normalize(direction), layers.level);
        return hog::globe::place_temperature(
            planet, layers,
            {.face = cell.face, .i = cell.i, .j = cell.j, .layer = static_cast<std::uint32_t>(layer)});
    };
    const hog::DVec3 equator{1.0, 0.0, 0.0};
    const hog::DVec3 north{0.1, 0.0, 1.0};
    const int sea = layers.layer_at(planet.radius);
    // Colder up high and towards a pole, and no warmer under the sea than at it.
    CHECK(at(equator, sea + 400) < at(equator, sea));
    CHECK(at(north, sea) < at(equator, sea));
    CHECK(at(equator, sea - 20) == Catch::Approx(at(equator, sea)));
    const Cube warm{.solid = Material::Basalt, .solid_amount = Voxel::full, .heat = 3};
    const double baseline = at(equator, sea);
    CHECK(hog::globe::kelvin(hog::globe::voxel_of(warm, baseline).temperature) ==
          Catch::Approx(baseline + (3 * Cube::heat_step)).margin(0.125));
}

TEST_CASE("a volume archive keeps tile columns and what they were made from", "[globe][volume]") {
    hog::globe::MemoryVolumeArchive archive;
    REQUIRE(archive.header());
    CHECK(archive.header()->empty());
    CHECK(hog::globe::archived_origin(archive) == std::nullopt);
    const hog::globe::VolumeOrigin origin = hog::globe::origin_of(earth());
    const std::vector<std::byte> header = hog::globe::volume_header(origin);
    const hog::globe::SavedColumn saved{
        .face = 4, .i = 7, .j = 9, .bytes = rock_to(3).encode_column({.face = 2, .i = 60, .j = 5})};
    REQUIRE(archive.write(header, std::span{&saved, 1}));
    CHECK(archive.header() == header);
    CHECK(hog::globe::origin_from(*archive.header()) == origin);
    CHECK(hog::globe::archived_origin(archive) == origin);
    CHECK(archive.column(4, 7, 9, 0) == saved);
    CHECK(archive.column(4, 7, 10, 0) == std::nullopt);
    CHECK(archive.column(4, 7, 9, 1) == std::nullopt);

    std::vector<std::byte> other = header;
    other.front() = std::byte{99};
    CHECK_FALSE(hog::globe::origin_from(other));
    other = header;
    other.pop_back();
    CHECK_FALSE(hog::globe::origin_from(other));
    REQUIRE(archive.write(other, {}));
    CHECK_FALSE(hog::globe::archived_origin(archive));
}

TEST_CASE("tile columns are loaded where the archive holds them, and saved a few at a time",
          "[globe][volume]") {
    const hog::globe::VolumeOrigin origin = hog::globe::origin_of(earth());
    hog::globe::MemoryVolumeArchive archive;
    Volume made = rock_to(70);
    REQUIRE(hog::globe::save_volume(made, archive, origin) == 2U);

    const Site here{.face = 2, .i = 0, .j = 0};
    const Site there{.face = 2, .i = 64, .j = 0};
    const Site never{.face = 2, .i = 128, .j = 0};
    Volume held;
    const std::vector<Site> asked{here, never};
    CHECK(hog::globe::load_columns(held, archive, asked) == std::vector<Site>{here});
    CHECK(held.at({.face = 2, .i = 63, .j = 5, .layer = 70}).solid == Material::Granite);
    CHECK(held.columns() == std::vector<Site>{here});
    CHECK(held.unsaved().empty());
    // What's held already isn't read again over what it holds.
    REQUIRE(held.put_on({.face = 2, .i = 63, .j = 5, .layer = 70}, full_of(Material::Clay)));
    CHECK(hog::globe::load_columns(held, archive, asked)->empty());
    CHECK(held.at({.face = 2, .i = 63, .j = 5, .layer = 71}).solid == Material::Clay);

    const std::vector<Site> other{there};
    CHECK(hog::globe::save_columns(held, archive, origin, other) == 0U);
    const std::vector<Site> mine{here};
    CHECK(hog::globe::save_columns(held, archive, origin, mine) == 1U);
    CHECK(held.unsaved().empty());
    Volume again;
    REQUIRE(hog::globe::load_columns(again, archive, mine));
    CHECK(again.at({.face = 2, .i = 63, .j = 5, .layer = 71}).solid == Material::Clay);
}

TEST_CASE("the generator's voxels are made into the volume once, its air left out",
          "[globe][volume]") {
    const Generator& generator = earth();
    const hog::globe::Layers& layers = generator.layers();
    const Cell cell = hog::globe::cell_under(somewhere(3), layers.level);
    const auto i = cell.i / Volume::tile_side;
    const auto j = cell.j / Volume::tile_side;
    const Volume volume = hog::globe::generate_tiles(generator, cell.face, i, j);
    REQUIRE(volume.tiles() > 0);
    const std::vector<Generator::Column> columns =
        generator.columns({.level = static_cast<std::uint8_t>(layers.level),
                           .face = cell.face,
                           .i = i * Volume::tile_side,
                           .j = j * Volume::tile_side},
                          Volume::tile_side, hog::globe::voxel_lod(generator));
    const Site corner{.face = cell.face, .i = i * Volume::tile_side, .j = j * Volume::tile_side};
    // The tile from `bottom`, as building from every one of its voxels gives it, made from
    // `floor` up.
    const auto every = [&](std::uint32_t bottom, std::uint32_t floor) {
        return VoxelTree::build(Volume::tile_side, [&](int x, int y, int z) {
            const std::uint32_t layer = bottom + static_cast<std::uint32_t>(z);
            if (layer < floor) {
                return Cube{};
            }
            const hog::globe::Voxel made =
                hog::globe::generated_at(columns[(static_cast<std::size_t>(y) * Volume::tile_side) +
                                                 static_cast<std::size_t>(x)],
                                         layers, layer);
            return hog::globe::cube_of(made, made.temperature / 8.0);
        });
    };
    const auto tile_from = [](std::uint32_t layer) {
        return layer / Volume::tile_side * Volume::tile_side;
    };

    // Made from `made_depth` under the lowest ground.
    double lowest = columns.front().ground();
    for (const Generator::Column& each : columns) {
        lowest = std::min(lowest, each.ground());
    }
    const std::uint32_t floor = volume.floor(corner).value_or(0);
    CHECK(std::cmp_equal(floor, layers.layer_at(lowest) - hog::globe::made_depth));
    REQUIRE(floor > Volume::tile_side);
    const Site under{.face = cell.face, .i = corner.i, .j = corner.j, .layer = floor - 1};
    CHECK_FALSE(volume.made(under));
    CHECK(volume.made({.face = cell.face, .i = corner.i, .j = corner.j, .layer = floor}));

    // The tile the ground lies in, and the one the floor lies in.
    const auto ground_tile =
        tile_from(static_cast<std::uint32_t>(layers.layer_at(columns.front().ground())));
    for (const std::uint32_t bottom : {ground_tile, tile_from(floor)}) {
        CAPTURE(bottom);
        const VoxelTree* built =
            volume.tile_at({.face = cell.face, .i = corner.i, .j = corner.j, .layer = bottom});
        REQUIRE(built != nullptr);
        const VoxelTree expected = every(bottom, floor);
        CHECK(built->same_as(expected));
        CHECK(built->leaves() < expected.unshared_nodes());
    }

    const auto matches_world = [&](const Volume& made_volume) {
        for (std::uint32_t a = 0; a < 64; a += 21) {
            for (std::uint32_t layer = 0; std::cmp_less(layer, layers.count); layer += 37) {
                const Site site{.face = cell.face,
                                .i = (i * Volume::tile_side) + a,
                                .j = (j * Volume::tile_side) + 63 - a,
                                .layer = layer};
                if (!made_volume.made(site)) {
                    continue;
                }
                const hog::globe::Voxel made = hog::globe::generated_at(
                    generator.columns(hog::globe::column_of(site, layers), 1,
                                      hog::globe::voxel_lod(generator))[0],
                    layers, site.layer);
                CAPTURE(a, layer);
                CHECK(made_volume.at(site) == hog::globe::cube_of(made, made.temperature / 8.0));
            }
        }
    };
    matches_world(volume);

    // Dug toward, a tile's height more is made under it as it would have been, and what was
    // made is kept.
    Volume deeper = volume;
    const Site edited{.face = cell.face, .i = corner.i, .j = corner.j, .layer = floor + 1};
    REQUIRE(deeper.set(edited, full_of(Material::Clay)));
    REQUIRE(hog::globe::deepen(deeper, generator, cell.face, i, j));
    const std::uint32_t lower = floor - Volume::tile_side;
    CHECK(deeper.floor(corner) == lower);
    CHECK(deeper.made(under));
    CHECK(deeper.at(edited).solid == Material::Clay);
    deeper.set(edited, volume.at(edited));
    for (const std::uint32_t bottom : {tile_from(lower), tile_from(floor)}) {
        CAPTURE(bottom);
        const VoxelTree* built =
            deeper.tile_at({.face = cell.face, .i = corner.i, .j = corner.j, .layer = bottom});
        REQUIRE(built != nullptr);
        CHECK(built->same_as(every(bottom, lower)));
    }
    matches_world(deeper);
}

namespace {

std::vector<Generator::Column> columns_under(const Generator& generator, std::uint8_t face,
                                             std::uint32_t i, std::uint32_t j) {
    return generator.columns({.level = static_cast<std::uint8_t>(generator.layers().level),
                              .face = face,
                              .i = i * Volume::tile_side,
                              .j = j * Volume::tile_side},
                             Volume::tile_side, hog::globe::voxel_lod(generator));
}

// The first made voxel of the tile column at (`face`, `i`, `j`), in tiles, that isn't as the
// generator makes it, up to a little over its ground or the sea.
std::optional<Site> first_wrong(const Volume& volume, const Generator& generator,
                                const std::vector<Generator::Column>& columns, std::uint8_t face,
                                std::uint32_t i, std::uint32_t j) {
    const hog::globe::Layers& layers = generator.layers();
    const double sea = generator.sea_level().value_or(layers.floor);
    const std::uint32_t lowest =
        volume.floor({.face = face, .i = i * Volume::tile_side, .j = j * Volume::tile_side})
            .value_or(0);
    for (std::uint32_t y = 0; y < 64; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) {
            const Generator::Column& column = columns[(y * 64) + x];
            const auto top = static_cast<std::uint32_t>(
                std::min(layers.layer_at(std::max(column.top(), sea)) + 2, layers.count - 1));
            for (std::uint32_t layer = lowest; layer <= top; ++layer) {
                const hog::globe::Voxel made = hog::globe::generated_at(column, layers, layer);
                const Site site{.face = face,
                                .i = (i * Volume::tile_side) + x,
                                .j = (j * Volume::tile_side) + y,
                                .layer = layer};
                if (volume.at(site) != hog::globe::cube_of(made, made.temperature / 8.0)) {
                    return site;
                }
            }
        }
    }
    return std::nullopt;
}

double since(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

} // namespace

TEST_CASE("the working world's tile columns, timed and sized", "[.time]") {
    auto planet = hog::globe::load_planet(HOG_SOURCE_DIR "/config/planet.toml");
    REQUIRE(planet);
    const Generator generator{*planet};
    const hog::globe::Layers& layers = generator.layers();
    const double sea = generator.sea_level().value_or(layers.floor);
    // Land and sea apart.
    struct Sum {
        double seconds = 0.0;
        double caves = 0.0;
        std::size_t tiles = 0;
        std::size_t bytes = 0;
        std::size_t count = 0;
    };
    Sum land;
    Sum water;
    for (std::uint64_t n = 0; n < 24; ++n) {
        const Cell cell = hog::globe::cell_under(somewhere(n), layers.level);
        const auto i = cell.i / Volume::tile_side;
        const auto j = cell.j / Volume::tile_side;
        const std::vector<Generator::Column> columns = columns_under(generator, cell.face, i, j);
        Sum& sum = columns.front().ground() > sea ? land : water;
        auto started = std::chrono::steady_clock::now();
        const Volume volume = hog::globe::generate_tiles(generator, cell.face, i, j);
        sum.seconds += since(started);
        const double floor = layers.bottom(static_cast<int>(
            volume
                .floor({.face = cell.face, .i = i * Volume::tile_side, .j = j * Volume::tile_side})
                .value_or(0)));
        started = std::chrono::steady_clock::now();
        for (const Generator::Column& each : columns) {
            static_cast<void>(each.caves(floor, each.ground(), layers.thickness / 2.0));
        }
        sum.caves += since(started);
        // Every made voxel as the generator makes it, on the first few.
        if (n < 6) {
            const std::optional<Site> wrong =
                first_wrong(volume, generator, columns, cell.face, i, j);
            CAPTURE(n, wrong.value_or(Site{}).i, wrong.value_or(Site{}).j,
                    wrong.value_or(Site{}).layer);
            CHECK_FALSE(wrong);
        }
        sum.tiles += volume.tiles();
        sum.bytes += volume.encode().size();
        ++sum.count;
    }
    for (const auto& [name, sum] : {std::pair{"land", land}, std::pair{"sea", water}}) {
        if (sum.count > 0) {
            const auto count = static_cast<double>(sum.count);
            std::cout << std::format("{}, {} tile columns: {:.3f} s (caves {:.3f} s), {} tiles, "
                                     "{} bytes each\n",
                                     name, sum.count, sum.seconds / count, sum.caves / count,
                                     sum.tiles / sum.count, sum.bytes / sum.count);
        }
    }
}
