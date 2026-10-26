#include "test_planet.hpp"

#include <hog/client/bushes.hpp>
#include <hog/client/scene.hpp>
#include <hog/flora/bushes.hpp>
#include <hog/flora/fates.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/projection.hpp>
#include <hog/render/bushes.hpp>
#include <hog/terrain/patch.hpp>

#include <catch2/catch_test_macros.hpp>
#include <test_device.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <numbers>
#include <optional>
#include <vector>

using hog::client::bush_tile_level;
using hog::client::tile_bushes;
using hog::client::testing::world;
using hog::render::BushAnchor;
using hog::render::testing::test_device;

namespace {

hog::WorldPosition origin_of(hog::planet::CellId tile) {
    return hog::WorldPosition{hog::planet::cell_center(tile) * world().geology().planet_radius()};
}

// The bush tiles about the temperate woods nearest the start, nearest first, that anchor at least
// `least` bushes, up to `wanted` of them.
std::vector<hog::planet::CellId> bushy_tiles(
    const hog::client::BiomePalette& palette, std::size_t least = 1, std::size_t wanted = 1,
    hog::DVec3 from = hog::client::testing::nearest_biome(hog::planet::Biome::TemperateForest)) {
    const hog::WorldPosition middle{from * world().geology().planet_radius()};
    const hog::planet::FaceIJ at = hog::planet::cell_at(middle, bush_tile_level).to_face_ij();
    std::vector<hog::planet::CellId> found;
    for (std::uint32_t ring = 0; ring < 24 && found.size() < wanted; ++ring) {
        for (std::uint32_t i = at.i - ring; i <= at.i + ring && found.size() < wanted; ++i) {
            for (std::uint32_t j = at.j - ring; j <= at.j + ring && found.size() < wanted; ++j) {
                if (std::max(i, at.i) - std::min(i, at.i) != ring &&
                    std::max(j, at.j) - std::min(j, at.j) != ring) {
                    continue;
                }
                const hog::planet::CellId tile =
                    hog::planet::CellId::from_face_ij(at.face, i, j, bush_tile_level);
                if (tile_bushes(world(), tile, origin_of(tile), palette).size() >= least) {
                    found.push_back(tile);
                }
            }
        }
    }
    return found;
}

hog::planet::Biome biome_at(hog::DVec3 point) {
    return world().state(hog::planet::cell_at(glm::normalize(point))).biome;
}

// Bush tiles about the temperate woods nearest the start where a biome that grows bushes meets
// another, up to
// `wanted`.
std::vector<hog::planet::CellId> edge_tiles(std::size_t wanted) {
    const double radius = world().geology().planet_radius();
    const hog::WorldPosition middle{
        hog::client::testing::nearest_biome(hog::planet::Biome::TemperateForest) * radius};
    const hog::planet::FaceIJ at = hog::planet::cell_at(middle, bush_tile_level).to_face_ij();
    const auto bushy = [](hog::planet::Biome biome) {
        return hog::flora::bushes_of(biome).height > 0.0F;
    };
    std::vector<hog::planet::CellId> found;
    constexpr std::uint32_t reach = 160;
    constexpr std::uint32_t side = 1U << bush_tile_level;
    const std::uint32_t first_i = at.i > reach ? at.i - reach : 0;
    const std::uint32_t first_j = at.j > reach ? at.j - reach : 0;
    for (std::uint32_t i = first_i; i < std::min(at.i + reach, side) && found.size() < wanted;
         i += 3) {
        for (std::uint32_t j = first_j; j < std::min(at.j + reach, side) && found.size() < wanted;
             j += 3) {
            const hog::planet::CellId tile =
                hog::planet::CellId::from_face_ij(at.face, i, j, bush_tile_level);
            const std::array<hog::DVec3, 4> corners = hog::planet::cell_vertices(tile);
            const hog::planet::Biome west = biome_at(corners[0]);
            const hog::planet::Biome east = biome_at(corners[2]);
            if (west != east && (bushy(west) || bushy(east))) {
                found.push_back(tile);
            }
        }
    }
    return found;
}

// The biome whose leaves a bush wears.
std::optional<hog::planet::Biome> worn(const BushAnchor& bush,
                                       const hog::client::BiomePalette& palette) {
    for (int biome = 0; biome < hog::planet::biome_count; ++biome) {
        const std::uint32_t leaves = hog::render::packed_leaves(
            hog::render::leaf_colour(palette.at(static_cast<std::size_t>(biome))),
            hog::render::kind_of(bush.colour));
        if (leaves == bush.colour) {
            return static_cast<hog::planet::Biome>(biome);
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("anchored bushes are bound to their spots, the same every build", "[client][bushes]") {
    const hog::client::BiomePalette palette = hog::client::biome_palette();
    const std::vector<hog::planet::CellId> found = bushy_tiles(palette, 20);
    REQUIRE(!found.empty());
    const hog::planet::CellId tile = found.front();
    const hog::WorldPosition origin = origin_of(tile);
    const std::vector<BushAnchor> first = tile_bushes(world(), tile, origin, palette);
    const std::vector<BushAnchor> again = tile_bushes(world(), tile, origin, palette);
    REQUIRE(first.size() == again.size());
    REQUIRE(std::memcmp(first.data(), again.data(), first.size() * sizeof(BushAnchor)) == 0);

    // Every bush stands within its tile, on dry ground, near upright, no taller than its kind.
    const double radius = world().geology().planet_radius();
    const double span = (std::numbers::pi / 2.0) * radius /
                        static_cast<double>(std::uint32_t{1} << bush_tile_level);
    const hog::DVec3 up = glm::normalize(origin.metres());
    for (const BushAnchor& bush : first) {
        const hog::DVec3 along =
            glm::dvec3{bush.position} - (up * glm::dot(glm::dvec3{bush.position}, up));
        CHECK(glm::length(along) < span * 1.5);
        CHECK(bush.height > 0.0F);
        CHECK(bush.height < 1.8F);
        CHECK(bush.width > 0.0F);
        CHECK(glm::dot(glm::dvec3{bush.normal}, up) > 0.3);
        const hog::DVec3 root = origin.metres() + hog::DVec3{bush.position};
        CHECK(glm::length(root) > radius + 0.29);
        CHECK(worn(bush, palette).has_value());
    }
}

TEST_CASE("the open sea anchors no bushes", "[client][bushes]") {
    const hog::client::BiomePalette palette = hog::client::biome_palette();
    // A face's middle out at sea, its tile's ground deep under water all over.
    std::optional<hog::planet::CellId> sea;
    const std::uint32_t middle = 1U << (bush_tile_level - 1);
    for (int face = 0; face < hog::planet::face_count && !sea; ++face) {
        const hog::planet::CellId tile =
            hog::planet::CellId::from_face_ij(face, middle, middle, bush_tile_level);
        const auto state = world().state(hog::planet::cell_at(hog::planet::cell_center(tile)));
        if (state.biome == hog::planet::Biome::Ocean && state.elevation < 0) {
            sea = tile;
        }
    }
    REQUIRE(sea);
    const hog::planet::CellId open = sea.value_or(hog::planet::CellId{});
    CHECK(tile_bushes(world(), open, origin_of(open), palette).empty());
}

TEST_CASE("bushes stray a little way past their biome's edge, and no further", "[client][bushes]") {
    const hog::client::BiomePalette palette = hog::client::biome_palette();
    const double reach = hog::terrain::biome_blend_reach;
    std::size_t bushes = 0;
    std::size_t strays = 0;
    std::size_t unfound = 0;
    const std::vector<hog::planet::CellId> edges = edge_tiles(16);
    REQUIRE(edges.size() > 4);
    for (const hog::planet::CellId tile : edges) {
        const hog::WorldPosition origin = origin_of(tile);
        const hog::DVec3 up = glm::normalize(origin.metres());
        const hog::DVec3 east = glm::normalize(glm::cross(hog::DVec3{0.0, 0.0, 1.0}, up));
        const hog::DVec3 north = glm::cross(up, east);
        for (const BushAnchor& bush : tile_bushes(world(), tile, origin, palette)) {
            ++bushes;
            const hog::DVec3 root = origin.metres() + hog::DVec3{bush.position};
            const std::optional<hog::planet::Biome> worn_as = worn(bush, palette);
            REQUIRE(worn_as);
            const hog::planet::Biome wears = worn_as.value_or(hog::planet::Biome::Ocean);
            if (wears == biome_at(root)) {
                continue;
            }
            ++strays;
            // Its biome lies within the blending reach of it: a little further allowed, the
            // reach being measured roughly across the face.
            bool near = false;
            for (int a = -6; a <= 6 && !near; ++a) {
                for (int b = -6; b <= 6 && !near; ++b) {
                    const hog::DVec3 off =
                        ((east * static_cast<double>(a)) + (north * static_cast<double>(b))) *
                        (reach * 1.5 / 6.0);
                    near = biome_at(root + off) == wears;
                }
            }
            unfound += near ? 0U : 1U;
        }
    }
    INFO(bushes << " bushes, " << strays << " strays, " << unfound << " far from their biome");
    CHECK(strays > 0);
    CHECK(strays < bushes / 3);
    CHECK(unfound == 0);
}

TEST_CASE("burned ground anchors no bushes; fallen woods crowd them in", "[client][bushes]") {
    const hog::client::BiomePalette palette = hog::client::biome_palette();
    const std::vector<hog::planet::CellId> found = bushy_tiles(palette, 20, 8);
    REQUIRE(!found.empty());
    const hog::flora::Fates black{
        0, std::vector<hog::flora::Fate>(6, {.living = 0.0F, .established = 0.0F, .scar = 1.0F})};
    const hog::flora::Fates fallen{
        0, std::vector<hog::flora::Fate>(6, {.living = 0.0F, .established = 0.0F, .dead = 0.0F})};
    std::size_t untouched = 0;
    std::size_t crowded = 0;
    for (const hog::planet::CellId tile : found) {
        const hog::WorldPosition origin = origin_of(tile);
        CHECK(tile_bushes(world(), tile, origin, palette, black).empty());
        untouched += tile_bushes(world(), tile, origin, palette).size();
        crowded += tile_bushes(world(), tile, origin, palette, fallen).size();
    }
    CHECK(crowded > untouched);
}

TEST_CASE("bush tiles are kept round the eye, built again in place when their woods change",
          "[client][bushes][gpu]") {
    const auto device = test_device();
    auto pool =
        hog::render::AnchorPool::create(*device, {.tiles = 512, .frames = 1, .uploads = 512});
    REQUIRE(pool);
    const hog::client::BiomePalette palette = hog::client::biome_palette();
    const double radius = world().geology().planet_radius();
    const std::vector<hog::planet::CellId> found = bushy_tiles(palette, 20);
    REQUIRE(!found.empty());
    const hog::DVec3 up = hog::planet::cell_center(found.front());
    const double ground = hog::client::ground_under(world(), hog::WorldPosition{up * radius});
    const hog::WorldPosition standing{up * (radius + ground + 2.0)};
    hog::client::BushAnchors anchors{palette};
    REQUIRE(anchors.update(*pool, 0, world(), standing, 120.0, 512));
    REQUIRE(anchors.update(*pool, 0, world(), standing, 120.0, 512));
    REQUIRE(anchors.settled());
    const std::size_t tiles = anchors.size();
    const std::uint32_t free = pool->free_slots();
    const std::size_t before = hog::client::anchors_in(anchors.draws({.eye = standing}, 120.0));
    CHECK(before > 0);
    // Nearer, fewer tiles are drawn.
    CHECK(hog::client::anchors_in(anchors.draws({.eye = standing}, 20.0)) < before);

    // Burned black over the eye's ground: still drawn as it was, then built again bare.
    const auto burned = std::make_shared<const hog::flora::Fates>(
        0, std::vector<hog::flora::Fate>(6, {.living = 0.0F, .established = 0.0F, .scar = 1.0F}));
    const std::vector<hog::planet::CellId> changed{found.front().parent(0)};
    anchors.fare(burned, changed);
    CHECK(hog::client::anchors_in(anchors.draws({.eye = standing}, 120.0)) == before);
    REQUIRE(anchors.update(*pool, 0, world(), standing, 120.0, 512));
    REQUIRE(anchors.update(*pool, 0, world(), standing, 120.0, 512));
    CHECK(anchors.settled());
    CHECK(anchors.size() == tiles);
    CHECK(pool->free_slots() == free);
    CHECK(hog::client::anchors_in(anchors.draws({.eye = standing}, 120.0)) == 0);
}

TEST_CASE("bush tiles built on the job pool come out as those built in place",
          "[client][bushes][gpu]") {
    const auto device = test_device();
    auto in_place =
        hog::render::AnchorPool::create(*device, {.tiles = 512, .frames = 1, .uploads = 512});
    auto pooled =
        hog::render::AnchorPool::create(*device, {.tiles = 512, .frames = 1, .uploads = 512});
    REQUIRE(in_place);
    REQUIRE(pooled);
    const hog::client::BiomePalette palette = hog::client::biome_palette();
    const double radius = world().geology().planet_radius();
    const std::vector<hog::planet::CellId> found = bushy_tiles(palette, 20);
    REQUIRE(!found.empty());
    const hog::DVec3 up = hog::planet::cell_center(found.front());
    const double ground = hog::client::ground_under(world(), hog::WorldPosition{up * radius});
    const hog::WorldPosition standing{up * (radius + ground + 2.0)};

    hog::client::BushAnchors built{palette};
    REQUIRE(built.update(*in_place, 0, world(), standing, 100.0, 512));
    hog::jobs::JobSystem jobs{3};
    hog::client::BushAnchors background{palette, &jobs};
    // A few tiles a call, landing as they finish.
    int calls = 0;
    do {
        REQUIRE(background.update(*pooled, 0, world(), standing, 100.0, 2));
        background.finish();
        ++calls;
    } while (!background.settled() && calls < 500);
    CHECK(calls > 2);
    CHECK(background.settled());
    CHECK(background.size() == built.size());
    CHECK(hog::client::anchors_in(background.draws({.eye = standing}, 100.0)) ==
          hog::client::anchors_in(built.draws({.eye = standing}, 100.0)));
}

TEST_CASE("bush tiles on high ground are drawn round an eye standing among them",
          "[client][bushes][gpu]") {
    const auto device = test_device();
    auto pool =
        hog::render::AnchorPool::create(*device, {.tiles = 512, .frames = 1, .uploads = 512});
    REQUIRE(pool);
    const hog::client::BiomePalette palette = hog::client::biome_palette();
    const double radius = world().geology().planet_radius();
    // Well over the sea, so where a tile's laid out from is far below its bushes.
    std::optional<hog::planet::CellId> high;
    double ground = 0.0;
    const hog::DVec3 upland =
        hog::client::testing::nearest_place([](const hog::planet::CellState& state) {
            return state.biome == hog::planet::Biome::TemperateForest &&
                   state.elevation > 150 * hog::planet::fixed_one;
        });
    for (const hog::planet::CellId found : bushy_tiles(palette, 20, 200, upland)) {
        ground = hog::client::ground_under(
            world(), hog::WorldPosition{hog::planet::cell_center(found) * radius});
        if (ground > 80.0) {
            high = found;
            break;
        }
    }
    REQUIRE(high);
    const hog::planet::CellId tile = high.value_or(hog::planet::CellId{});
    const hog::DVec3 up = hog::planet::cell_center(tile);
    const hog::WorldPosition standing{up * (radius + ground + 2.0)};
    hog::client::BushAnchors anchors{palette};
    REQUIRE(anchors.update(*pool, 0, world(), standing, 120.0, 512));
    REQUIRE(anchors.update(*pool, 0, world(), standing, 120.0, 512));
    REQUIRE(anchors.settled());
    // Looking level along the ground, as from a walk.
    const hog::DVec3 level = glm::normalize(glm::cross(up, hog::DVec3{0.0, 0.0, 1.0}));
    const hog::render::Camera camera{
        .eye = standing, .forward = level, .up = up, .vertical_fov = 1.0};
    const hog::client::Sight sight = hog::client::sight_of(camera, 16.0 / 9.0);
    const std::vector<hog::render::StandDraw> drawn = anchors.draws(sight, 120.0);
    INFO("ground " << ground << " m");
    CHECK(std::ranges::any_of(drawn, [&](const hog::render::StandDraw& stand) {
        return glm::length(stand.origin - origin_of(tile)) < 1.0;
    }));
    // Of all those round about, those ahead are drawn, out to as far as they're drawn.
    std::size_t ahead = 0;
    for (const hog::render::StandDraw& stand : drawn) {
        ahead += glm::dot(stand.origin - standing, level) > 30.0 ? 1U : 0U;
    }
    CHECK(ahead > 3);
}
