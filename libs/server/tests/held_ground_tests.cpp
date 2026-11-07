#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/volume_archive.hpp>
#include <hog/globe/world.hpp>
#include <hog/server/held_ground.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

using hog::DVec3;
using hog::globe::Material;
using hog::globe::Site;
using hog::globe::Volume;
using hog::server::HeldGround;

const hog::globe::Generator& earth() {
    static const hog::globe::Generator generator{hog::globe::Planet{}};
    return generator;
}

constexpr std::uint32_t ground = 100;

// An archive of tile columns over all the ground within `metres` of `centre`, each a floor and a
// voxel of granite on it.
hog::globe::MemoryVolumeArchive archive_about(DVec3 centre, double metres) {
    Volume volume;
    for (const Site corner : hog::globe::tile_columns_near(earth().layers(), centre, metres)) {
        volume.set_floor(corner, ground);
        volume.set({.face = corner.face, .i = corner.i, .j = corner.j, .layer = ground},
                   {.solid = Material::Granite, .solid_amount = hog::globe::Voxel::full});
    }
    hog::globe::MemoryVolumeArchive archive;
    REQUIRE(hog::globe::save_volume(volume, archive, hog::globe::origin_of(earth())));
    return archive;
}

DVec3 east_of(DVec3 centre, double metres) {
    const double radius = glm::length(centre);
    const DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, centre));
    return glm::normalize(centre + (east * metres)) * radius;
}

bool includes(const std::vector<Site>& outer, const std::vector<Site>& inner) {
    return std::ranges::includes(outer, inner);
}

} // namespace

TEST_CASE("the ground near the players is read as they come near, and saved and let go once "
          "well away",
          "[server][held]") {
    const hog::globe::Layers& layers = earth().layers();
    const DVec3 here = glm::normalize(DVec3{0.3, 0.4, 0.86}) * earth().planet().radius;
    const DVec3 there = east_of(here, 3000.0);
    hog::globe::MemoryVolumeArchive archive = archive_about(here, 6000.0);
    Volume volume;
    HeldGround held{volume, archive, hog::globe::origin_of(earth())};

    const std::vector<DVec3> one{here};
    CHECK(held.moved(one));
    auto changed = held.keep(one, {});
    REQUIRE(changed);
    const std::vector<Site> reached =
        hog::globe::tile_columns_near(layers, here, HeldGround::reach);
    CHECK(changed->loaded == reached);
    CHECK(changed->dropped.empty());
    CHECK(volume.columns() == reached);
    CHECK_FALSE(held.moved(one));

    // An edit far behind is saved before it's let go, and read again as edited.
    const Site edited{.face = reached.front().face,
                      .i = reached.front().i,
                      .j = reached.front().j,
                      .layer = ground + 1};
    REQUIRE(volume.set(edited, {.solid = Material::Clay, .solid_amount = hog::globe::Voxel::full}));
    // And one pinned is kept, near or not.
    const std::vector<Site> pinned{reached.back()};
    const std::vector<DVec3> moved{there};
    CHECK(held.moved(moved));
    changed = held.keep(moved, pinned);
    REQUIRE(changed);
    CHECK(includes(volume.columns(),
                   hog::globe::tile_columns_near(layers, there, HeldGround::reach)));
    std::vector<Site> allowed =
        hog::globe::tile_columns_near(layers, there, HeldGround::reach + HeldGround::slack);
    allowed.push_back(reached.back());
    std::ranges::sort(allowed);
    CHECK(includes(allowed, volume.columns()));
    CHECK(std::ranges::binary_search(volume.columns(), reached.back()));
    CHECK(std::ranges::binary_search(changed->dropped, reached.front()));
    CHECK(volume.unsaved().empty());
    Volume again;
    const std::vector<Site> asked{reached.front()};
    REQUIRE(hog::globe::load_columns(again, archive, asked));
    CHECK(again.at(edited).solid == Material::Clay);

    // With no one about, all is let go.
    changed = held.keep({}, {});
    REQUIRE(changed);
    CHECK(volume.columns().empty());
}

TEST_CASE("a tile column's footprint is a brick a column, and its bricks give its corner back",
          "[server][held]") {
    const Site corner{.face = 3, .i = 128, .j = 64};
    const std::vector<hog::globe::BrickSite> bricks = hog::globe::brick_footprint(corner);
    CHECK(bricks.size() == 64);
    CHECK(HeldGround::corners_of(bricks) == std::vector<Site>{corner});
    CHECK(std::ranges::all_of(bricks, [](const hog::globe::BrickSite& brick) {
        return brick.layer == 0 && brick.i >= 16 && brick.i < 24 && brick.j >= 8 && brick.j < 16;
    }));
}
