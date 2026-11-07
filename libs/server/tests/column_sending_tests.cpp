#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/volume_archive.hpp>
#include <hog/globe/world.hpp>
#include <hog/server/column_sending.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using hog::DVec3;
using hog::globe::Material;
using hog::globe::Site;
using hog::globe::Volume;
using hog::server::ColumnSending;

const hog::globe::Generator& earth() {
    static const hog::globe::Generator generator{hog::globe::Planet{}};
    return generator;
}

constexpr std::uint32_t ground = 100;
constexpr auto side = static_cast<std::uint32_t>(Volume::tile_side);
const hog::net::ConnectionId player{7};

// Every level of every tile column within reach of `centre`, each a floor and a voxel of granite
// on it.
const hog::globe::MemoryVolumeArchive& archive_about(DVec3 centre) {
    static const hog::globe::MemoryVolumeArchive archive = [&] {
        Volume volume;
        for (const Site corner : hog::globe::tile_columns_near(earth().layers(), centre,
                                                               ColumnSending::reach + 500.0)) {
            volume.set_floor(corner, ground);
            volume.set({.face = corner.face, .i = corner.i, .j = corner.j, .layer = ground},
                       {.solid = Material::Granite, .solid_amount = hog::globe::Voxel::full});
        }
        hog::globe::MemoryVolumeArchive made;
        REQUIRE(hog::globe::save_volume(volume, made, hog::globe::origin_of(earth())));
        return made;
    }();
    return archive;
}

DVec3 here() {
    return glm::normalize(DVec3{0.3, 0.4, 0.86}) * earth().planet().radius;
}

Site corner_under(DVec3 at) {
    const hog::globe::Cell cell =
        hog::globe::cell_under(glm::normalize(at), earth().layers().level);
    return {.face = cell.face, .i = cell.i / side * side, .j = cell.j / side * side};
}

} // namespace

TEST_CASE("a tile column is sent at its own level near, one coarser each doubling past that",
          "[server][columns]") {
    CHECK(ColumnSending::level_at(0.0) == 0);
    CHECK(ColumnSending::level_at(999.0) == 0);
    CHECK(ColumnSending::level_at(1000.0) == 1);
    CHECK(ColumnSending::level_at(1999.0) == 1);
    CHECK(ColumnSending::level_at(2000.0) == 2);
    CHECK(ColumnSending::level_at(7999.0) == 3);
    CHECK(ColumnSending::level_at(1.0e9) == hog::globe::coarsest_level);
}

TEST_CASE("a player is sent every tile column in reach once, nearest and finest first",
          "[server][columns]") {
    const hog::globe::MemoryVolumeArchive& archive = archive_about(here());
    // The ground under the player is held, and edited there.
    Volume held;
    const Site under = corner_under(here());
    REQUIRE(hog::globe::load_columns(held, archive, std::vector{under}));
    REQUIRE(held.set({.face = under.face, .i = under.i, .j = under.j, .layer = ground + 1},
                     {.solid = Material::Clay, .solid_amount = hog::globe::Voxel::full}));

    ColumnSending sending{earth().layers()};
    sending.join(player);
    sending.follow(player, here());
    const std::size_t wanted =
        hog::globe::tile_columns_near(earth().layers(), here(), ColumnSending::reach).size();
    CHECK(sending.due(player) == wanted);

    auto first = sending.next(player, std::size_t{1} << 30U, held, archive);
    REQUIRE(first);
    REQUIRE_FALSE(first->columns.empty());
    const hog::protocol::SentColumn& nearest = first->columns.front();
    CHECK(nearest.face == under.face);
    CHECK(nearest.i == under.i / side);
    CHECK(nearest.j == under.j / side);
    CHECK(nearest.level == 0);
    CHECK(nearest.bytes == held.encode_column(under));
    CHECK(std::ranges::is_sorted(first->columns, {}, &hog::protocol::SentColumn::level));

    std::size_t sent = first->columns.size();
    std::uint8_t coarsest = 0;
    while (sending.due(player) > 0) {
        auto more = sending.next(player, std::size_t{1} << 30U, held, archive);
        REQUIRE(more);
        REQUIRE_FALSE(more->columns.empty());
        for (const hog::protocol::SentColumn& column : more->columns) {
            CHECK_FALSE(column.bytes.empty());
            coarsest = std::max(coarsest, column.level);
        }
        sent += more->columns.size();
    }
    CHECK(sent == wanted);
    // Those with ground in reach include some whose middles are just past it.
    CHECK(coarsest == ColumnSending::level_at(ColumnSending::reach));

    // Over the same tile column, nothing more is due; a little way on, only the columns whose
    // level changed.
    sending.follow(player, here());
    CHECK(sending.due(player) == 0);
    const DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, here()));
    sending.follow(player, glm::normalize(here() + (east * 400.0)) * glm::length(here()));
    CHECK(sending.due(player) > 0);
    CHECK(sending.due(player) < wanted / 2);
}

TEST_CASE("a tile column changed is sent again first to those holding it at its own level",
          "[server][columns]") {
    const hog::globe::MemoryVolumeArchive& archive = archive_about(here());
    const Volume none;
    ColumnSending sending{earth().layers()};
    sending.join(player);
    sending.follow(player, here());
    // A little room: one column at a time.
    auto one = sending.next(player, 1, none, archive);
    REQUIRE(one);
    REQUIRE(one->columns.size() == 1);
    const Site under = corner_under(here());
    const std::size_t due = sending.due(player);

    const std::vector<Site> changed{under};
    sending.changed(changed);
    CHECK(sending.due(player) == due + 1);
    auto again = sending.next(player, 1, none, archive);
    REQUIRE(again);
    REQUIRE(again->columns.size() == 1);
    CHECK(again->columns.front().i == under.i / side);
    CHECK(again->columns.front().level == 0);

    sending.leave(player);
    CHECK(sending.due(player) == 0);
    auto gone = sending.next(player, 1, none, archive);
    REQUIRE(gone);
    CHECK(gone->columns.empty());
}
