#include <hog/planet/beds.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <vector>

namespace {

using hog::planet::Beds;
using hog::planet::BedSetting;
using hog::planet::CellId;
using hog::planet::fixed_one;
using hog::planet::Rock;

constexpr double radius = 115'000.0;
constexpr int level = 10;
constexpr BedSetting plains{.continental = true, .mountains = 0};

[[nodiscard]] CellId cell(std::uint32_t i, std::uint32_t j) {
    return CellId::from_face_ij(1, i, j, level);
}

// Runs of one rock, in metres, from the basement up through `metres` of beds in one-metre steps.
[[nodiscard]] std::vector<std::int64_t> runs(const Beds& beds, CellId at, std::int64_t metres) {
    const std::int64_t base = beds.basement(at, plains);
    std::vector<std::int64_t> found;
    Rock last = beds.rock_at(at, base, plains);
    std::int64_t run = 0;
    for (std::int64_t up = 0; up < metres; ++up) {
        const Rock rock = beds.rock_at(at, base + (up * fixed_one), plains);
        if (rock != last) {
            found.push_back(run);
            run = 0;
            last = rock;
        }
        ++run;
    }
    return found;
}

} // namespace

TEST_CASE("away from continents the beds are basalt all the way down", "[planet][beds]") {
    const Beds beds{1, radius};
    for (const std::int64_t height : {-3'000, -100, 0, 500, 4'000}) {
        CHECK(beds.rock_at(cell(10, 20), height * fixed_one, {}) == Rock::Basalt);
    }
}

TEST_CASE("sedimentary beds lie on granite, lifted in the belts", "[planet][beds]") {
    const Beds beds{1, radius};
    const BedSetting belt{.continental = true, .mountains = fixed_one};
    for (std::uint32_t i = 0; i < 50; ++i) {
        const CellId at = cell(i * 13, i * 7);
        const std::int64_t base = beds.basement(at, plains);
        CHECK(beds.rock_at(at, base - fixed_one, plains) == Rock::Granite);
        CHECK(beds.rock_at(at, base + fixed_one, plains) != Rock::Granite);
        CHECK(beds.basement(at, belt) > base + (2'000 * fixed_one));
    }
}

TEST_CASE("the beds alternate, tens of metres thick, with every sedimentary rock among them",
          "[planet][beds]") {
    const Beds beds{1, radius};
    std::set<Rock> seen;
    for (std::uint32_t i = 0; i < 20; ++i) {
        const CellId at = cell(i * 31, i * 17);
        const std::vector<std::int64_t> found = runs(beds, at, 2'000);
        CHECK(found.size() >= 8);
        // The first run starts partway into a bed.
        for (std::size_t k = 1; k < found.size(); ++k) {
            CHECK(found[k] >= 40);
        }
        for (std::int64_t up = 0; up < 2'000; up += 10) {
            seen.insert(beds.rock_at(at, beds.basement(at, plains) + (up * fixed_one), plains));
        }
    }
    CHECK(seen == std::set<Rock>{Rock::Sandstone, Rock::Shale, Rock::Limestone});
}

TEST_CASE("a bed carries on to the neighbouring ground", "[planet][beds]") {
    const Beds beds{1, radius};
    int same = 0;
    int pairs = 0;
    for (std::uint32_t i = 0; i < 40; ++i) {
        for (std::int64_t height = 0; height < 1'000; height += 37) {
            const std::uint32_t x = (i * 23) + 5;
            same += beds.rock_at(cell(x, 300), height * fixed_one, plains) ==
                            beds.rock_at(cell(x + 1, 300), height * fixed_one, plains)
                        ? 1
                        : 0;
            ++pairs;
        }
    }
    CHECK(same * 10 >= pairs * 9);
}

TEST_CASE("beds come from the seed", "[planet][beds]") {
    const Beds one{1, radius};
    const Beds again{1, radius};
    const Beds other{2, radius};
    int differ = 0;
    for (std::uint32_t i = 0; i < 30; ++i) {
        for (std::int64_t height = 0; height < 600; height += 50) {
            const Rock rock = one.rock_at(cell(i * 9, i * 4), height * fixed_one, plains);
            CHECK(rock == again.rock_at(cell(i * 9, i * 4), height * fixed_one, plains));
            differ += rock != other.rock_at(cell(i * 9, i * 4), height * fixed_one, plains) ? 1 : 0;
        }
    }
    CHECK(differ > 0);
}
