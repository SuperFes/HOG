#include <hog/planet/bake.hpp>
#include <hog/planet/elevation.hpp>
#include <hog/planet/projection.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using hog::planet::LandformBake;

namespace {

LandformBake sample() {
    return {.level = 3,
            .heights = {0, -5, 1'234'567'890'123},
            .change = {7, 0, -1},
            .drained = {1, 2, 3},
            .ground = {-9'000'000'000'000, 4, 5}};
}

// A world small enough to evolve in a moment, keeping its landform in `bakes`.
hog::planet::PlanetConfig small(const std::filesystem::path& bakes) {
    return {.seed = 3, .ocean_fraction = 0.6, .radius = 50'000.0, .bakes = bakes};
}

// The name of the one bake kept in `directory`, read from its file's.
hog::planet::BakeName only_bake(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> found;
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        found.push_back(entry.path());
    }
    REQUIRE(found.size() == 1);
    const std::string name = found.front().stem().string();
    REQUIRE(name.size() == 42);
    REQUIRE(name.starts_with("landform-"));
    return {.world = std::stoull(name.substr(9, 16), nullptr, 16),
            .code = std::stoull(name.substr(26, 16), nullptr, 16)};
}

} // namespace

TEST_CASE("a bake reads back as written", "[planet][bake]") {
    const hog::testing::TemporaryDirectory directory;
    const hog::planet::BakeName name{.world = 42, .code = 7};
    REQUIRE(hog::planet::write_bake(directory.path() / "nested", name, sample()));
    CHECK(hog::planet::read_bake(directory.path() / "nested", name) == sample());
}

TEST_CASE("a bake by other code, cut short or missing reads as none", "[planet][bake]") {
    const hog::testing::TemporaryDirectory directory;
    const hog::planet::BakeName name{.world = 42, .code = 7};
    CHECK_FALSE(hog::planet::read_bake(directory.path(), name));
    REQUIRE(hog::planet::write_bake(directory.path(), name, sample()));
    CHECK_FALSE(hog::planet::read_bake(directory.path(), {.world = 42, .code = 8}));
    CHECK_FALSE(hog::planet::read_bake(directory.path(), {.world = 43, .code = 7}));

    // Renamed as another's, it's still told apart by what it holds.
    std::filesystem::copy_file(hog::planet::bake_path(directory.path(), name),
                               hog::planet::bake_path(directory.path(), {.world = 42, .code = 9}));
    CHECK_FALSE(hog::planet::read_bake(directory.path(), {.world = 42, .code = 9}));

    const auto path = hog::planet::bake_path(directory.path(), name);
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 1);
    CHECK_FALSE(hog::planet::read_bake(directory.path(), name));
    {
        std::ofstream file{path, std::ios::binary | std::ios::trunc};
        file << "not a bake";
    }
    CHECK_FALSE(hog::planet::read_bake(directory.path(), name));
}

TEST_CASE("a bake replaces the world's bakes by other code, and only those", "[planet][bake]") {
    const hog::testing::TemporaryDirectory directory;
    REQUIRE(hog::planet::write_bake(directory.path(), {.world = 1, .code = 1}, sample()));
    REQUIRE(hog::planet::write_bake(directory.path(), {.world = 2, .code = 1}, sample()));
    REQUIRE(hog::planet::write_bake(directory.path(), {.world = 1, .code = 2}, sample()));
    CHECK_FALSE(
        std::filesystem::exists(hog::planet::bake_path(directory.path(), {.world = 1, .code = 1})));
    CHECK(hog::planet::read_bake(directory.path(), {.world = 1, .code = 2}));
    CHECK(hog::planet::read_bake(directory.path(), {.world = 2, .code = 1}));
}

TEST_CASE("a bake whose fields differ in length is never written", "[planet][bake]") {
    const hog::testing::TemporaryDirectory directory;
    LandformBake bake = sample();
    bake.ground.pop_back();
    CHECK_FALSE(hog::planet::write_bake(directory.path(), {.world = 1, .code = 1}, bake));
    CHECK(std::filesystem::is_empty(directory.path()));
}

TEST_CASE("a world evolves its landform once, then builds it from the bake", "[planet][bake]") {
    const hog::testing::TemporaryDirectory directory;
    std::vector<std::int64_t> elevations;
    std::vector<hog::planet::CellId> cells;
    for (int at = 0; at < 64; ++at) {
        const double t = at / 64.0;
        cells.push_back(
            hog::planet::cell_at(glm::normalize(hog::DVec3{1.0 - t, t - 0.5, (2.0 * t) - 0.7})));
    }
    {
        const hog::planet::Elevation terrain{small(directory.path())};
        for (const auto cell : cells) {
            elevations.push_back(terrain.elevation(cell));
        }
    }
    const hog::planet::BakeName name = only_bake(directory.path());
    LandformBake bake = hog::planet::read_bake(directory.path(), name).value_or(LandformBake{});
    REQUIRE_FALSE(bake.heights.empty());

    {
        const hog::planet::Elevation same{small(directory.path())};
        for (std::size_t at = 0; at < cells.size(); ++at) {
            REQUIRE(same.elevation(cells[at]) == elevations[at]);
        }
    }
    // Changed where it's kept, so the world can only take the change by reading the bake.
    bake.heights.front() += 1;
    REQUIRE(hog::planet::write_bake(directory.path(), name, bake));
    const hog::planet::Elevation changed{small(directory.path())};
    CHECK(changed.landform().heights().front() == bake.heights.front());
}

TEST_CASE("a damaged bake is evolved again and replaced", "[planet][bake]") {
    const hog::testing::TemporaryDirectory directory;
    std::vector<std::int64_t> heights;
    {
        const hog::planet::Elevation terrain{small(directory.path())};
        const auto worn = terrain.landform().heights();
        heights.assign(worn.begin(), worn.end());
    }
    const hog::planet::BakeName name = only_bake(directory.path());
    const auto path = hog::planet::bake_path(directory.path(), name);
    std::filesystem::resize_file(path, std::filesystem::file_size(path) / 2);
    const hog::planet::Elevation terrain{small(directory.path())};
    const auto again = terrain.landform().heights();
    CHECK(std::vector<std::int64_t>(again.begin(), again.end()) == heights);
    CHECK(hog::planet::read_bake(directory.path(), name).value_or(LandformBake{}).heights ==
          heights);
}
