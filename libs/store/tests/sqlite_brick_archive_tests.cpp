#include "temporary_database.hpp"

#include <hog/globe/brick.hpp>
#include <hog/globe/brick_archive.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/store/sqlite_brick_archive.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using hog::globe::Brick;
using hog::globe::BrickSite;
using hog::globe::Material;
using hog::globe::Voxel;
using hog::globe::VoxelStore;
using hog::store::SqliteBrickArchive;
using hog::store::testing::query;
using hog::store::testing::tamper;
using hog::store::testing::TemporaryFile;

namespace {

constexpr hog::globe::Layers layers{
    .level = 18, .floor = 111'000.0, .thickness = 0.5, .count = 28'000};

Brick wet(std::uint16_t amount) {
    std::array<Voxel, hog::globe::brick_voxels> voxels{};
    for (std::size_t index = 0; index < voxels.size(); ++index) {
        voxels[index] = {.distance = static_cast<std::int16_t>(index),
                         .liquid = Material::Water,
                         .liquid_amount = static_cast<std::uint16_t>(amount + (index % 3))};
    }
    return Brick::pack(voxels);
}

SqliteBrickArchive opened(const TemporaryFile& file) {
    auto archive = SqliteBrickArchive::open(file.path());
    REQUIRE(archive);
    return *std::move(archive);
}

} // namespace

TEST_CASE("bricks saved to the world's file load back after it's reopened", "[store][bricks]") {
    const TemporaryFile file;
    VoxelStore store{layers};
    const BrickSite last_face{.face = 5, .i = 32'767, .j = 32'767, .layer = 3'499};
    store.put({.face = 0, .i = 1, .j = 2, .layer = 3}, wet(100));
    store.put(last_face, wet(9'000));
    store.put({.face = 2, .i = 0, .j = 0, .layer = 0}, Brick{Voxel{.solid = Material::Granite}});
    {
        SqliteBrickArchive archive = opened(file);
        REQUIRE(hog::globe::save(store, archive) == 3);
    }
    const SqliteBrickArchive archive = opened(file);
    const auto loaded = hog::globe::load(layers, archive);
    REQUIRE(loaded);
    CHECK(loaded->sites() == store.sites());
    for (const BrickSite& site : store.sites()) {
        CHECK(*loaded->brick(site) == *store.brick(site));
    }
}

TEST_CASE("a save to the file rewrites only changed bricks", "[store][bricks]") {
    const TemporaryFile file;
    SqliteBrickArchive archive = opened(file);
    VoxelStore store{layers};
    for (std::uint32_t k = 0; k < 10; ++k) {
        store.put({.face = 1, .i = k, .j = 0, .layer = 900}, wet(50));
    }
    REQUIRE(hog::globe::save(store, archive));
    store.erase({.face = 1, .i = 3, .j = 0, .layer = 900});
    store.put({.face = 1, .i = 4, .j = 0, .layer = 900}, wet(70));
    CHECK(hog::globe::save(store, archive) == 2);
    CHECK(query(file.path(), "SELECT count(*) FROM bricks") == 9);
    const auto loaded = hog::globe::load(layers, archive);
    REQUIRE(loaded);
    CHECK(loaded->size() == 9);
    CHECK(*loaded->brick({.face = 1, .i = 4, .j = 0, .layer = 900}) == wet(70));
}

TEST_CASE("a brick write that fails leaves the file as it was", "[store][bricks]") {
    const TemporaryFile file;
    SqliteBrickArchive archive = opened(file);
    VoxelStore store{layers};
    store.put({.face = 4, .i = 8, .j = 8, .layer = 8}, wet(1));
    REQUIRE(hog::globe::save(store, archive));
    // Refuses any further brick, after the header has been rewritten.
    tamper(file.path(), "CREATE TRIGGER refuse BEFORE INSERT ON bricks "
                        "BEGIN SELECT RAISE(ABORT, 'refused'); END;");
    store.put({.face = 4, .i = 9, .j = 8, .layer = 8}, wet(2));
    CHECK_FALSE(hog::globe::save(store, archive));
    // Still to be saved.
    CHECK(store.changed().size() == 1);
    CHECK(query(file.path(), "SELECT count(*) FROM bricks") == 1);
    CHECK(query(file.path(), "SELECT count(*) FROM brick_header") == 1);
}
