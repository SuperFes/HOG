#include <hog/baked/wants_table.hpp>
#include <hog/grid/key.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using hog::baked::WantsTable;
using hog::grid::Key;

TEST_CASE("the wants table holds what it's last given and is read by another opening",
          "[baked][wants]") {
    const hog::testing::TemporaryDirectory directory;
    auto table = WantsTable::open(directory.path() / "wants");
    REQUIRE(table.has_value());
    CHECK(table->read().value().empty());

    const Key near{.depth = 9, .x = 300, .y = 2, .z = 511};
    const Key far{.depth = 3, .x = 1, .y = 7, .z = 0};
    const std::vector<WantsTable::Want> first{
        {.node = near, .depth = 12, .rank = {.round = 4, .priority = 0.5}},
        {.node = far, .depth = 8, .rank = {.round = 4, .priority = 0.25}},
        {.node = near, .depth = 14, .rank = {.round = 4, .priority = 0.75}}};
    REQUIRE(table->replace(first).has_value());
    const auto read = table->read();
    REQUIRE(read.has_value());
    // In Z order: the far node's first.
    CHECK(*read == std::vector<WantsTable::Want>{first[1], first[0], first[2]});

    // Given again, what's gone goes and what's changed changes.
    const std::vector<WantsTable::Want> second{
        {.node = near, .depth = 12, .rank = {.round = 5, .priority = 0.5}},
        {.node = near, .depth = 12, .rank = {.round = 6, .priority = 0.6}}};
    REQUIRE(table->replace(second).has_value());
    CHECK(table->read().value() == std::vector<WantsTable::Want>{second[1]});

    // A table opened anew over the same directory reads the same.
    table = WantsTable::open(directory.path() / "wants");
    REQUIRE(table.has_value());
    CHECK(table->read().value() == std::vector<WantsTable::Want>{second[1]});
    REQUIRE(table->replace({}).has_value());
    CHECK(table->read().value().empty());
}
