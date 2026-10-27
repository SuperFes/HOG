#include <hog/planet/bake.hpp>
#include <hog/planet/elevation.hpp>
#include <hog/planet/projection.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using hog::planet::LandformBake;

namespace {

LandformBake sample() {
    return {.level = 3,
            .heights = {0, -5, 1'234'567'890'123},
            .change = {7, 0, -1},
            .drained = {1, 2, 3},
            .ground = {-9'000'000'000'000, 4, 5},
            .soil = {0, 1, 65'536},
            .risen = {3, 0, 1'048'576},
            .carved = {0, 12, 0}};
}

// A world coarse enough to evolve in a moment, keeping its landform in `bakes`.
hog::planet::PlanetConfig small(const std::filesystem::path& bakes) {
    return {.seed = 3,
            .ocean_fraction = 0.6,
            .radius = 50'000.0,
            .erosion_cell = 2'000.0,
            .bakes = bakes};
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

TEST_CASE("one claims a world's bake, and the rest wait for it", "[planet][bake]") {
    const hog::testing::TemporaryDirectory directory;
    const hog::planet::BakeName name{.world = 5, .code = 6};
    const hog::planet::ClaimTiming quick{.beat = std::chrono::milliseconds{20},
                                         .abandoned = std::chrono::milliseconds{2'000},
                                         .poll = std::chrono::milliseconds{10}};
    std::optional<hog::planet::BakeClaim> first{std::in_place, directory.path(), name,
                                                std::function<void()>{}, quick};
    REQUIRE(first->held());

    std::atomic<int> told{0};
    std::atomic<bool> done{false};
    std::atomic<bool> held{true};
    std::atomic<bool> found{false};
    std::thread second{[&] {
        const hog::planet::BakeClaim claim{directory.path(), name, [&] { ++told; }, quick};
        held = claim.held();
        found = std::filesystem::exists(hog::planet::bake_path(directory.path(), name));
        done = true;
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    CHECK_FALSE(done);
    REQUIRE(hog::planet::write_bake(directory.path(), name, sample()));
    first.reset();
    second.join();
    // Let go only once the bake is kept, and then never claimed again.
    CHECK(told == 1);
    CHECK(found);
    CHECK_FALSE(held);
}

TEST_CASE("a held claim stays alive, while one let go or left unmarked is claimed again",
          "[planet][bake]") {
    const hog::testing::TemporaryDirectory directory;
    const hog::planet::ClaimTiming quick{.beat = std::chrono::milliseconds{20},
                                         .abandoned = std::chrono::milliseconds{300},
                                         .poll = std::chrono::milliseconds{10}};
    const hog::planet::BakeName name{.world = 5, .code = 6};
    {
        const hog::planet::BakeClaim claim{directory.path(), name, {}, quick};
        REQUIRE(claim.held());
        // Marked alive while held, however long the bake takes.
        const auto path =
            hog::planet::bake_path(directory.path(), name).replace_extension(".claim");
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() -
                                                   std::chrono::hours{1});
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        // Compared before checking, as libc++'s file clock counts in a type Catch can't print.
        const bool marked =
            std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(path) <
            std::chrono::seconds{10};
        CHECK(marked);
    }
    const hog::planet::BakeClaim again{directory.path(), name, {}, quick};
    REQUIRE(again.held());

    // Another's, left by a process that died before marking it again.
    const hog::planet::BakeName other{.world = 7, .code = 6};
    const auto left = hog::planet::bake_path(directory.path(), other).replace_extension(".claim");
    std::filesystem::create_directory(left);
    std::filesystem::last_write_time(left, std::filesystem::file_time_type::clock::now() -
                                               std::chrono::seconds{1});
    CHECK(hog::planet::BakeClaim{directory.path(), other, {}, quick}.held());
}
