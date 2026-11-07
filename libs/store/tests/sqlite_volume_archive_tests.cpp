#include "temporary_database.hpp"

#include <hog/globe/cube.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/volume_archive.hpp>
#include <hog/store/sqlite_volume_archive.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

using hog::globe::Cube;
using hog::globe::Material;
using hog::globe::SavedColumn;
using hog::globe::Site;
using hog::globe::Volume;
using hog::store::SqliteVolumeArchive;
using hog::store::testing::query;
using hog::store::testing::tamper;
using hog::store::testing::TemporaryFile;

namespace {

const hog::globe::VolumeOrigin origin{
    .seed = 812'080'951,
    .layers = {.level = 14, .floor = 28'000.0, .thickness = 2.5, .count = 2'228}};

SqliteVolumeArchive opened(const TemporaryFile& file) {
    auto archive = SqliteVolumeArchive::open(file.path());
    REQUIRE(archive);
    return *std::move(archive);
}

// A tile column of rock up to `top`, made from `floor`.
SavedColumn rock(std::uint8_t face, std::uint32_t i, std::uint32_t j, std::uint32_t floor,
                 std::uint32_t top) {
    Volume volume;
    const Site corner{.face = face, .i = i * 64, .j = j * 64};
    volume.set_floor(corner, floor);
    for (std::uint32_t layer = floor; layer <= top; ++layer) {
        volume.set({.face = face, .i = corner.i + 5, .j = corner.j, .layer = layer},
                   {.solid = Material::Basalt, .solid_level = Cube::levels});
    }
    return {.face = face, .i = i, .j = j, .bytes = volume.encode_column(corner)};
}

} // namespace

TEST_CASE("tile columns saved to the world's file load back after it's reopened",
          "[store][volume]") {
    const TemporaryFile file;
    const SavedColumn low = rock(0, 1, 2, 64, 100);
    const SavedColumn last = rock(5, 255, 255, 10, 300);
    {
        SqliteVolumeArchive archive = opened(file);
        REQUIRE(archive.header());
        CHECK(archive.header()->empty());
        REQUIRE(archive.write(hog::globe::volume_header(origin), std::vector{low, last}));
    }
    const SqliteVolumeArchive archive = opened(file);
    const auto header = archive.header();
    REQUIRE(header);
    CHECK(hog::globe::origin_from(*header) == origin);
    CHECK(archive.column(0, 1, 2) == low);
    CHECK(archive.column(5, 255, 255) == last);
    CHECK(archive.column(0, 2, 1) == std::nullopt);

    Volume volume;
    const auto saved = archive.column(5, 255, 255);
    REQUIRE(saved);
    const SavedColumn column = saved->value_or(SavedColumn{});
    REQUIRE_FALSE(column.bytes.empty());
    REQUIRE(volume.decode_column({.face = 5, .i = 255 * 64, .j = 255 * 64}, column.bytes));
    CHECK(volume.floor({.face = 5, .i = 255 * 64, .j = 255 * 64}) == 10U);
    CHECK(volume.at({.face = 5, .i = (255 * 64) + 5, .j = 255 * 64, .layer = 300}).solid ==
          Material::Basalt);
}

TEST_CASE("a tile column written again replaces the one before", "[store][volume]") {
    const TemporaryFile file;
    SqliteVolumeArchive archive = opened(file);
    const std::vector<std::byte> header = hog::globe::volume_header(origin);
    REQUIRE(archive.write(header, std::vector{rock(2, 3, 4, 0, 50)}));
    const SavedColumn again = rock(2, 3, 4, 0, 90);
    REQUIRE(archive.write(header, std::vector{again}));
    CHECK(query(file.path(), "SELECT count(*) FROM volume_columns") == 1);
    CHECK(archive.column(2, 3, 4) == again);
}

TEST_CASE("a volume write that fails leaves the file as it was", "[store][volume]") {
    const TemporaryFile file;
    SqliteVolumeArchive archive = opened(file);
    const std::vector<std::byte> header = hog::globe::volume_header(origin);
    REQUIRE(archive.write(header, std::vector{rock(1, 1, 1, 0, 5)}));
    tamper(file.path(), "CREATE TRIGGER refuse BEFORE INSERT ON volume_columns "
                        "BEGIN SELECT RAISE(ABORT, 'refused'); END;");
    CHECK_FALSE(archive.write(header, std::vector{rock(1, 1, 2, 0, 5), rock(1, 1, 3, 0, 5)}));
    CHECK(query(file.path(), "SELECT count(*) FROM volume_columns") == 1);
    CHECK(archive.column(1, 1, 2) == std::nullopt);
}
