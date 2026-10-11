#include <hog/planet/projection.hpp>
#include <hog/server/spatial_index.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <set>
#include <vector>

using hog::DVec3;
using hog::WorldPosition;
using hog::server::EntityId;
using hog::server::SpatialIndex;

namespace {

constexpr double radius = 115'000.0;

WorldPosition above(DVec3 direction, double altitude = 0.0) {
    return WorldPosition{glm::normalize(direction) * (radius + altitude)};
}

std::set<EntityId> ids(const SpatialIndex& index, hog::planet::CellId cell) {
    std::set<EntityId> found;
    for (const auto& entry : index.in_cell(cell)) {
        found.insert(entry.id);
    }
    return found;
}

} // namespace

TEST_CASE("entities are placed, moved and removed", "[server][spatial]") {
    SpatialIndex index{11};
    const WorldPosition here = above({1.0, 0.2, 0.3});
    const WorldPosition near_here = here + DVec3{0.0, 0.5, 0.0};
    const WorldPosition far = above({-0.3, 1.0, 0.1});

    const auto cell = index.update(1, here);
    CHECK(cell == hog::planet::cell_at(here, 11));
    CHECK(index.update(2, near_here) == cell);
    CHECK(index.update(3, far) != cell);
    CHECK(index.size() == 3);
    CHECK(ids(index, cell) == std::set<EntityId>{1, 2});

    // Altitude keeps an entity in its column.
    CHECK(index.update(1, above({1.0, 0.2, 0.3}, 2'000.0)) == cell);
    CHECK(index.in_cell(cell)[0].position == above({1.0, 0.2, 0.3}, 2'000.0));

    CHECK(index.cell_of(3) == index.update(2, far));
    CHECK(ids(index, cell) == std::set<EntityId>{1});

    index.remove(1);
    index.remove(1);
    CHECK_FALSE(index.cell_of(1));
    CHECK(index.in_cell(cell).empty());
    CHECK(index.size() == 2);
}

TEST_CASE("removal keeps the rest of a cell intact", "[server][spatial]") {
    SpatialIndex index{8};
    const WorldPosition here = above({0.1, 0.1, 1.0});
    for (EntityId id = 0; id < 10; ++id) {
        index.update(id, here + DVec3{static_cast<double>(id) * 0.01, 0.0, 0.0});
    }
    const auto cell = index.cell_of(0).value_or(hog::planet::CellId{});
    REQUIRE(cell.is_valid());
    for (const EntityId id : {3U, 0U, 9U, 5U}) {
        index.remove(id);
    }
    CHECK(ids(index, cell) == std::set<EntityId>{1, 2, 4, 6, 7, 8});
    // Every survivor can still be found and moved.
    for (const EntityId id : {1U, 2U, 4U, 6U, 7U, 8U}) {
        index.update(id, above({-1.0, 0.0, 0.0}));
    }
    CHECK(index.in_cell(cell).empty());
}

TEST_CASE("radius queries match a brute-force search", "[server][spatial]") {
    // A cube corner, a face edge and the middle of a face.
    const DVec3 center_direction =
        GENERATE(DVec3{1.0, 1.0, 1.0}, DVec3{1.0, 1e-4, 0.3}, DVec3{0.1, -0.2, 1.0});
    const WorldPosition center = above(center_direction, 50.0);
    std::mt19937_64 random{7};
    std::uniform_real_distribution<double> offset{-2'000.0, 2'000.0};
    std::uniform_real_distribution<double> altitude{-500.0, 500.0};

    SpatialIndex index{11};
    std::vector<WorldPosition> positions;
    for (EntityId id = 0; id < 3'000; ++id) {
        const DVec3 moved = center.metres() + DVec3{offset(random), offset(random), offset(random)};
        positions.push_back(above(moved, altitude(random)));
        index.update(id, positions.back());
    }

    for (const double query : {0.0, 30.0, 150.0, 900.0}) {
        std::set<EntityId> expected;
        for (EntityId id = 0; id < positions.size(); ++id) {
            const DVec3 between = positions[id] - center;
            if (glm::dot(between, between) <= query * query) {
                expected.insert(id);
            }
        }
        std::set<EntityId> found;
        index.for_each_near(center, query, [&](const SpatialIndex::Entry& entry) {
            CHECK(found.insert(entry.id).second);
        });
        INFO("radius " << query);
        CHECK(found == expected);
        CHECK((query < 900.0 || expected.size() > 50));
    }
}

TEST_CASE("spatial indexes reject bad input", "[server][spatial]") {
    const hog::testing::AssertionGuard guard;
    using hog::testing::AssertionFailed;
    CHECK_THROWS_AS(SpatialIndex(-1), AssertionFailed);
    CHECK_THROWS_AS(SpatialIndex(hog::planet::max_level + 1), AssertionFailed);

    const SpatialIndex index{11};
    CHECK_THROWS_AS(static_cast<void>(index.in_cell(hog::planet::CellId::from_face(0))),
                    AssertionFailed);
}
