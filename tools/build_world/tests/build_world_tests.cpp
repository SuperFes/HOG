#include <hog/build_world/build_world.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/volume_archive.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/jobs/job_system.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

using hog::build_world::Area;
using hog::globe::Generator;
using hog::globe::MemoryVolumeArchive;
using hog::globe::Volume;

namespace {

const Generator& planet() {
    static const Generator generator{hog::globe::Planet{}};
    return generator;
}

} // namespace

TEST_CASE("an area about a place holds it and stays on its side of the planet", "[build_world]") {
    const Generator& generator = planet();
    const std::uint32_t side = hog::globe::tiles_across(generator);
    REQUIRE(side > 8);
    for (const hog::DVec3 direction :
         {hog::DVec3{0.3, -0.2, 0.93}, hog::DVec3{1.0, 1.0, 1.0}, hog::DVec3{1.0, -1.0, -1.0}}) {
        const Area area = hog::globe::around(generator, direction, 6);
        const hog::globe::Cell cell = hog::globe::cell_under(direction, generator.layers().level);
        CAPTURE(direction.x, direction.y, direction.z);
        CHECK(area.face == cell.face);
        CHECK(area.across == 6);
        CHECK(area.i + area.across <= side);
        CHECK(area.j + area.across <= side);
        CHECK(cell.i / Volume::tile_side >= area.i);
        CHECK(cell.i / Volume::tile_side < area.i + area.across);
        CHECK(cell.j / Volume::tile_side >= area.j);
        CHECK(cell.j / Volume::tile_side < area.j + area.across);
    }
    CHECK(hog::globe::around(generator, {0.0, 0.0, 1.0}, side * 2).across == side);
    const std::vector<Area> all = hog::build_world::whole(generator);
    REQUIRE(all.size() == 6);
    for (std::uint8_t face = 0; face < 6; ++face) {
        CHECK(all[face] == Area{.face = face, .i = 0, .j = 0, .across = side});
    }
}

TEST_CASE("a build writes each tile column once, as the generator makes it", "[build_world]") {
    const Generator& generator = planet();
    MemoryVolumeArchive archive;
    hog::jobs::JobSystem jobs{2};
    const Area area = hog::globe::around(generator, {0.3, -0.2, 0.93}, 2);
    std::vector<std::size_t> reported;
    const auto built = hog::build_world::build(generator, archive, jobs, {area}, 3,
                                               [&](std::size_t done, std::size_t total) {
                                                   CHECK(total == 4);
                                                   reported.push_back(done);
                                               });
    REQUIRE(built);
    CHECK(built->made == 4);
    CHECK(built->skipped == 0);
    CHECK(built->bytes > 0);
    CHECK(reported == std::vector<std::size_t>{3, 4});
    CHECK(hog::globe::origin_from(*archive.header()) == hog::globe::origin_of(generator));
    for (std::uint32_t j = area.j; j < area.j + 2; ++j) {
        for (std::uint32_t i = area.i; i < area.i + 2; ++i) {
            const auto saved = archive.column(area.face, i, j);
            REQUIRE(saved);
            const hog::globe::SavedColumn column = saved->value_or(hog::globe::SavedColumn{});
            REQUIRE_FALSE(column.bytes.empty());
            const hog::globe::Site corner{
                .face = area.face, .i = i * Volume::tile_side, .j = j * Volume::tile_side};
            CHECK(column.bytes ==
                  hog::globe::generate_tiles(generator, area.face, i, j).encode_column(corner));
        }
    }

    // Built again, nothing is made twice.
    const auto again = hog::build_world::build(generator, archive, jobs, {area});
    REQUIRE(again);
    CHECK(again->made == 0);
    CHECK(again->skipped == 4);
}

TEST_CASE("a build refuses a world made from another planet", "[build_world]") {
    const Generator& generator = planet();
    MemoryVolumeArchive archive;
    hog::globe::VolumeOrigin other = hog::globe::origin_of(generator);
    ++other.seed;
    REQUIRE(archive.write(hog::globe::volume_header(other), {}));
    hog::jobs::JobSystem jobs{1};
    const Area area = hog::globe::around(generator, {0.3, -0.2, 0.93}, 1);
    CHECK_FALSE(hog::build_world::build(generator, archive, jobs, {area}));
    CHECK(archive.column(area.face, area.i, area.j) == std::nullopt);
}
