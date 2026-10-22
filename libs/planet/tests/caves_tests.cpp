#include <hog/core/hash.hpp>
#include <hog/planet/caves.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/testing/quick.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_adapters.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::planet::Cave;
using hog::planet::Caves;
using hog::planet::CellId;
using hog::planet::fixed_one;
using hog::planet::Geology;
using hog::planet::Passage;
using hog::planet::Rock;

namespace {

constexpr double radius = 115'000.0;

hog::planet::PlanetConfig planet(std::uint64_t seed) {
    return {.seed = seed, .ocean_fraction = 0.7, .radius = radius};
}

const Geology& geology(std::uint64_t seed) {
    static const Geology first{planet(1)};
    static const Geology second{planet(2)};
    return seed == 1 ? first : second;
}

// Karst tiles scattered over the planet.
std::vector<CellId> tiles(const Caves& caves, std::uint64_t count) {
    const int level = caves.karst_level();
    const std::uint64_t side = std::uint64_t{1} << level;
    std::vector<CellId> cells;
    cells.reserve(count);
    for (std::uint64_t sample = 0; sample < count; ++sample) {
        cells.push_back(CellId::from_face_ij(
            static_cast<int>(hog::hash_values(sample, 0) % hog::planet::face_count),
            static_cast<std::uint32_t>(hog::hash_values(sample, 1) % side),
            static_cast<std::uint32_t>(hog::hash_values(sample, 2) % side), level));
    }
    return cells;
}

// The finest cell over a point, and the fixed-point depth of the point below its surface.
struct Spot {
    CellId cell;
    std::int64_t depth = 0;
};

Spot spot_at(const Caves& caves, DVec3 point) {
    const CellId cell = hog::planet::cell_at(glm::normalize(point), 20);
    const double height = glm::length(point) - radius;
    const std::int64_t surface = caves.geology().terrain().elevation(cell);
    return {.cell = cell, .depth = surface - std::llround(height * static_cast<double>(fixed_one))};
}

// Sea-cave tiles over and around each starting island.
std::vector<CellId> island_coasts(const Caves& caves) {
    std::vector<CellId> cells;
    for (const auto& island : caves.geology().terrain().archipelago().islands()) {
        const hog::planet::TangentFrame frame = hog::planet::tangent_frame(island.center);
        const auto steps = static_cast<int>(island.radius * 2.0 / 150.0);
        for (int east = -steps; east <= steps; ++east) {
            for (int north = -steps; north <= steps; ++north) {
                const DVec3 offset = ((frame.east * static_cast<double>(east)) +
                                      (frame.north * static_cast<double>(north))) *
                                     (150.0 / radius);
                cells.push_back(hog::planet::cell_at(glm::normalize(island.center + offset),
                                                     caves.sea_level()));
            }
        }
    }
    std::ranges::sort(cells);
    const auto [first, last] = std::ranges::unique(cells);
    cells.erase(first, last);
    return cells;
}

} // namespace

TEST_CASE("a passage holds what lies within its radius there", "[planet][caves]") {
    const Passage passage{.from = DVec3{0.0, 0.0, 0.0},
                          .to = DVec3{10.0, 0.0, 0.0},
                          .from_radius = 1.0,
                          .to_radius = 3.0,
                          .cave = Cave::Karst};
    CHECK(hog::planet::clearance_at(passage, DVec3{0.0, 0.5, 0.0}) == 1.0);
    CHECK_THAT(hog::planet::clearance_at(passage, DVec3{5.0, 1.9, 0.0}).value_or(0.0),
               Catch::Matchers::WithinAbs(2.0, 1e-12));
    CHECK_FALSE(hog::planet::clearance_at(passage, DVec3{5.0, 2.1, 0.0}));
    CHECK(hog::planet::clearance_at(passage, DVec3{12.0, 0.0, 0.0}) == 3.0);
    CHECK_FALSE(hog::planet::clearance_at(passage, DVec3{-1.5, 0.0, 0.0}));
    const Passage point{.from = DVec3{1.0}, .to = DVec3{1.0}, .from_radius = 2.0};
    CHECK(hog::planet::clearance_at(point, DVec3{1.0, 2.0, 1.0}) == 2.0);
}

TEST_CASE("every cave has a distinct name", "[planet][caves]") {
    std::set<std::string_view> names;
    for (int cave = 0; cave < hog::planet::cave_count; ++cave) {
        names.insert(to_string(static_cast<Cave>(cave)));
    }
    CHECK(names.size() == hog::planet::cave_count);
    CHECK_FALSE(names.contains("unknown"));
}

TEST_CASE("karst runs through wet limestone near the water table", "[planet][caves]") {
    const Caves caves{geology(1)};
    Caves::Lookup lookup{caves};
    int passages = 0;
    int sinkholes = 0;
    int walkable = 0;
    int squeezes = 0;
    for (const CellId tile : tiles(caves, 3'000)) {
        for (const Passage& passage : lookup.passages_near(tile)) {
            if (passage.cave == Cave::Sinkhole) {
                ++sinkholes;
                // A funnel from a shallow junction up through the ground and out.
                const Spot top = spot_at(caves, passage.to);
                CHECK(top.depth < 0);
                CHECK(passage.to_radius >= 5.0);
                continue;
            }
            if (passage.cave != Cave::Karst) {
                continue;
            }
            ++passages;
            walkable += passage.from_radius >= hog::planet::walk_clearance ? 1 : 0;
            squeezes += passage.from_radius < hog::planet::crawl_clearance ? 1 : 0;
            REQUIRE(passage.from_radius >= 0.15);
            REQUIRE(passage.from_radius <= 6.0);
            // One end is a junction in the limestone; the other may be a middle that wandered.
            const auto in_limestone = [&](DVec3 end) {
                const Spot at = spot_at(caves, end);
                const hog::planet::Strata strata = caves.geology().strata(at.cell);
                return strata.sediment == Rock::Limestone && at.depth >= strata.cover_thickness &&
                       at.depth <= strata.bedrock_depth();
            };
            CHECK((in_limestone(passage.from) || in_limestone(passage.to)));
        }
    }
    INFO(passages << " passages, " << walkable << " walkable, " << squeezes << " squeezes");
    CHECK(passages > 100);
    CHECK(sinkholes > 0);
    // Most are too tight to walk, and plenty too tight even to crawl.
    CHECK(walkable < passages / 2);
    CHECK(walkable > passages / 10);
    CHECK(squeezes > passages / 5);
}

TEST_CASE("neighbouring tiles agree on the passage between them", "[planet][caves]") {
    const Caves caves{geology(1)};
    Caves::Lookup lookup{caves};
    int shared = 0;
    for (const CellId tile : tiles(caves, 3'000)) {
        const std::vector<Passage> mine = lookup.passages_near(tile);
        if (mine.empty()) {
            continue;
        }
        for (const CellId neighbor : hog::planet::edge_neighbors(tile)) {
            const std::vector<Passage> theirs = lookup.passages_near(neighbor);
            for (const Passage& passage : mine) {
                const bool found = std::ranges::any_of(theirs, [&](const Passage& other) {
                    return other.from == passage.from && other.to == passage.to;
                });
                shared += found ? 1 : 0;
            }
        }
    }
    CHECK(shared > 0);
}

TEST_CASE("caves are found where their passages run", "[planet][caves]") {
    const Caves caves{geology(1)};
    Caves::Lookup lookup{caves};
    int found = 0;
    for (const CellId tile : tiles(caves, 3'000)) {
        for (const Passage& passage : lookup.passages_near(tile)) {
            if (passage.cave != Cave::Karst || passage.from_radius < 1.5) {
                continue;
            }
            const Spot at = spot_at(caves, passage.from);
            if (caves.geology().strata(at.cell).rock_at(at.depth) != Rock::Limestone) {
                continue;
            }
            const auto hit = caves.cave_at(at.cell, at.depth);
            REQUIRE(hit);
            const hog::planet::CaveHit got = hit.value_or(hog::planet::CaveHit{});
            // A sinkhole may rise from the junction too.
            CHECK(got.cave != Cave::LavaTube);
            CHECK(got.cave != Cave::SeaCave);
            CHECK(got.clearance >= hog::planet::walk_clearance);
            // Deep in the bedrock below, there is nothing.
            CHECK_FALSE(caves.cave_at(at.cell, at.depth + (1'000 * fixed_one)));
            ++found;
        }
    }
    CHECK(found > 10);
}

TEST_CASE("lava tubes run downhill from each vent, a few metres down", "[planet][caves]") {
    const std::uint64_t seed = GENERATE(take(hog::testing::cases(2), values({1U, 2U})));
    const Caves caves{geology(seed)};
    const auto chambers = caves.geology().chambers();
    std::vector<int> per_chamber(chambers.size(), 0);
    int to_the_sea = 0;
    int pinched = 0;
    int stretches = 0;
    for (const auto& tube : caves.lava_tubes()) {
        REQUIRE(tube.chamber < chambers.size());
        ++per_chamber[tube.chamber];
        REQUIRE_FALSE(tube.passages.empty());
        // From the vent.
        const DVec3 start = glm::normalize(tube.passages.front().from);
        CHECK(glm::length(start - chambers[tube.chamber].center) < 1e-9);
        std::int64_t last = std::numeric_limits<std::int64_t>::max();
        for (const Passage& passage : tube.passages) {
            CHECK(passage.cave == Cave::LavaTube);
            const Spot at = spot_at(caves, passage.from);
            CHECK(at.depth >= (3 * fixed_one) - (fixed_one / 2));
            CHECK(at.depth <= (15 * fixed_one) + (fixed_one / 2));
            const std::int64_t ground = caves.geology().terrain().elevation(at.cell);
            CHECK(ground - (fixed_one / 2) <= last);
            last = ground;
            CHECK(glm::length(passage.from - tube.middle) <= tube.reach);
            CHECK(glm::length(passage.to - tube.middle) <= tube.reach);
            pinched += passage.from_radius < hog::planet::crawl_clearance ? 1 : 0;
            ++stretches;
        }
        // Closing off at the far end.
        CHECK(tube.passages.back().to_radius < hog::planet::crawl_clearance);
        const Spot end = spot_at(caves, tube.passages.back().to);
        to_the_sea += caves.geology().terrain().elevation(end.cell) <= 0 ? 1 : 0;
    }
    for (const int count : per_chamber) {
        CHECK(count >= 1);
        CHECK(count <= 4);
    }
    INFO(to_the_sea << " of " << caves.lava_tubes().size() << " reach the sea");
    CHECK(to_the_sea * 2 >= static_cast<int>(caves.lava_tubes().size()));
    CHECK(pinched > 0);
    CHECK(pinched * 3 < stretches);
}

TEST_CASE("lava tubes are found in the basalt they run through", "[planet][caves]") {
    const Caves caves{geology(1)};
    Caves::Lookup lookup{caves};
    int found = 0;
    for (const auto& tube : caves.lava_tubes()) {
        for (const Passage& passage : tube.passages) {
            if (passage.from_radius < 2.0) {
                continue;
            }
            const Spot at = spot_at(caves, passage.from);
            if (caves.geology().strata(at.cell).rock_at(at.depth) != Rock::Basalt) {
                continue;
            }
            const auto hit = lookup.cave_at(at.cell, at.depth);
            REQUIRE(hit);
            const hog::planet::CaveHit got = hit.value_or(hog::planet::CaveHit{});
            CHECK(got.cave == Cave::LavaTube);
            CHECK(got.clearance >= hog::planet::walk_clearance);
            CHECK_FALSE(lookup.cave_at(at.cell, at.depth + (50 * fixed_one)));
            ++found;
        }
    }
    CHECK(found > 10);
}

TEST_CASE("sea caves run inland from cliffs at the water's edge", "[planet][caves]") {
    const std::uint64_t seed = GENERATE(take(hog::testing::cases(2), values({1U, 2U})));
    const Caves caves{geology(seed)};
    Caves::Lookup lookup{caves};
    const hog::planet::Elevation& terrain = caves.geology().terrain();
    std::set<std::pair<double, double>> seen;
    int found = 0;
    int entered = 0;
    for (const CellId tile : island_coasts(caves)) {
        for (const Passage& passage : lookup.passages_near(tile)) {
            if (passage.cave != Cave::SeaCave ||
                !seen.insert({passage.from.x, passage.from.y}).second) {
                continue;
            }
            ++found;
            // From a mouth in the sea, about level with it, to a crack inland.
            CHECK(terrain.elevation(hog::planet::cell_at(glm::normalize(passage.from), 18)) <= 0);
            CHECK(terrain.elevation(hog::planet::cell_at(glm::normalize(passage.to), 18)) > 0);
            for (const DVec3 end : {passage.from, passage.to}) {
                CHECK_THAT(glm::length(end) - radius, Catch::Matchers::WithinAbs(1.0, 1e-6));
            }
            CHECK(passage.from_radius >= 3.0);
            CHECK(passage.to_radius < hog::planet::crawl_clearance);
            const double length = glm::length(passage.to - passage.from);
            CHECK(length >= 20.0);
            CHECK(length <= 140.0);
            // Somewhere along it, inside the cliff, there is room.
            for (int step = 0; step <= 20; ++step) {
                const double along = step / 20.0;
                const Spot at =
                    spot_at(caves, passage.from + ((passage.to - passage.from) * along));
                if (at.depth <= 2 * fixed_one) {
                    continue;
                }
                const auto hit = lookup.cave_at(at.cell, at.depth);
                if (hit && hit->cave == Cave::SeaCave) {
                    ++entered;
                    break;
                }
            }
        }
    }
    INFO(found << " sea caves, " << entered << " entered");
    CHECK(found > 0);
    CHECK(entered * 2 >= found);
}

TEST_CASE("only limestone holds karst", "[planet][caves]") {
    const Caves caves{geology(1)};
    Caves::Lookup lookup{caves};
    for (const CellId tile : tiles(caves, 1'000)) {
        const CellId cell = tile.child(1).child(2);
        const hog::planet::Strata strata = caves.geology().strata(cell);
        if (strata.sediment == Rock::Limestone) {
            continue;
        }
        for (const std::int64_t depth : {0, 5, 20, 80}) {
            CHECK_FALSE(lookup.cave_at(cell, depth * fixed_one));
        }
    }
}

TEST_CASE("caves depend on the seed", "[planet][caves]") {
    const auto summary = [](const Geology& geology) {
        const Caves caves{geology};
        Caves::Lookup lookup{caves};
        std::uint64_t hash = 0;
        for (const CellId tile : tiles(caves, 1'500)) {
            for (const Passage& passage : lookup.passages_near(tile)) {
                hash = hog::hash_values(hash, std::bit_cast<std::uint64_t>(passage.from.x),
                                        std::bit_cast<std::uint64_t>(passage.to_radius));
            }
        }
        return hash;
    };
    const std::uint64_t first = summary(geology(1));
    CHECK(summary(geology(1)) == first);
    CHECK(summary(geology(2)) != first);
}
