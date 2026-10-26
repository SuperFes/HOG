#include <hog/planet/beds.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
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

// `count` beds in turn from the basement up.
[[nodiscard]] std::vector<hog::planet::Bed> beds_up(const Beds& beds, CellId at, int count) {
    const auto column = beds.column(at, plains);
    std::vector<hog::planet::Bed> found;
    std::int64_t height = column.basement;
    for (int k = 0; k < count; ++k) {
        const auto bed = beds.bed_at(column, height);
        if (!bed) {
            break;
        }
        found.push_back(*bed);
        height = bed->top;
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
        CHECK(runs(beds, at, 2'000).size() >= 2);
        for (std::int64_t up = 0; up < 2'000; up += 10) {
            seen.insert(beds.rock_at(at, beds.basement(at, plains) + (up * fixed_one), plains));
        }
    }
    CHECK(seen == std::set<Rock>{Rock::Sandstone, Rock::Shale, Rock::Limestone});
}

TEST_CASE("one rock runs through several beds, a formation", "[planet][beds]") {
    const Beds beds{1, radius};
    int same = 0;
    int pairs = 0;
    for (std::uint32_t i = 0; i < 40; ++i) {
        const std::vector<hog::planet::Bed> found = beds_up(beds, cell(i * 23, i * 11), 40);
        for (std::size_t k = 1; k < found.size(); ++k) {
            same += found[k].rock == found[k - 1].rock ? 1 : 0;
            ++pairs;
        }
    }
    CHECK(same * 2 > pairs);
}

TEST_CASE("sandstone and limestone mostly meet through shale", "[planet][beds]") {
    const Beds beds{1, radius};
    int across = 0;
    int contacts = 0;
    for (std::uint32_t i = 0; i < 40; ++i) {
        const std::vector<hog::planet::Bed> found = beds_up(beds, cell(i * 23, i * 11), 40);
        for (std::size_t k = 1; k < found.size(); ++k) {
            if (found[k].rock != found[k - 1].rock) {
                across += found[k].rock != Rock::Shale && found[k - 1].rock != Rock::Shale ? 1 : 0;
                ++contacts;
            }
        }
    }
    CHECK(contacts > 100);
    CHECK(across * 6 < contacts);
}

TEST_CASE("beds thin and thicken across the land, some pinching out", "[planet][beds]") {
    const Beds beds{1, radius};
    std::int64_t thinnest = std::numeric_limits<std::int64_t>::max();
    std::int64_t thickest = 0;
    int pinched = 0;
    for (std::uint32_t i = 0; i < 40; ++i) {
        const std::vector<hog::planet::Bed> found = beds_up(beds, cell(i * 23, i * 11), 40);
        for (std::size_t k = 1; k < found.size(); ++k) {
            thinnest = std::min(thinnest, found[k].top - found[k].base);
            thickest = std::max(thickest, found[k].top - found[k].base);
            pinched += found[k].index > found[k - 1].index + 1 ? 1 : 0;
        }
    }
    CHECK(thinnest < 15 * fixed_one);
    CHECK(thickest > 140 * fixed_one);
    CHECK(pinched > 0);
}

TEST_CASE("a bed's rock changes sideways, tonguing into the next", "[planet][beds]") {
    const Beds beds{1, radius};
    int changed = 0;
    int compared = 0;
    for (std::uint32_t i = 0; i < 20; ++i) {
        std::map<std::int64_t, Rock> near;
        for (const hog::planet::Bed& bed : beds_up(beds, cell(i * 37, 200), 30)) {
            near.emplace(bed.index, bed.rock);
        }
        // Some ten kilometres along.
        for (const hog::planet::Bed& bed : beds_up(beds, cell((i * 37) + 60, 200), 30)) {
            const auto there = near.find(bed.index);
            if (there != near.end()) {
                changed += there->second != bed.rock ? 1 : 0;
                ++compared;
            }
        }
    }
    CHECK(changed > 0);
    CHECK(changed * 3 < compared);
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

TEST_CASE("a bed holds the height asked about, one bed running into the next, counted upwards",
          "[planet][beds]") {
    const Beds beds{1, radius};
    const auto column = beds.column(cell(30, 40), plains);
    CHECK_FALSE(beds.bed_at(column, column.basement - fixed_one));
    CHECK_FALSE(beds.bed_at(beds.column(cell(30, 40), {}), 0));
    REQUIRE(beds.bed_at(column, column.basement));
    hog::planet::Bed bed = beds.bed_at(column, column.basement).value_or(hog::planet::Bed{});
    for (int k = 0; k < 40; ++k) {
        CHECK(bed.top > bed.base);
        CHECK(bed.top - bed.base <= 160 * fixed_one);
        for (const std::int64_t height : {bed.base, (bed.base + bed.top) / 2, bed.top - 1}) {
            if (height >= column.basement) {
                CHECK(beds.rock_at(column, height) == bed.rock);
                REQUIRE(beds.bed_at(column, height));
                CHECK(beds.bed_at(column, height).value_or(hog::planet::Bed{}).base == bed.base);
            }
        }
        REQUIRE(beds.bed_at(column, bed.top));
        const hog::planet::Bed next = beds.bed_at(column, bed.top).value_or(hog::planet::Bed{});
        REQUIRE(next.base == bed.top);
        CHECK(next.index > bed.index);
        bed = next;
    }
}

TEST_CASE("a ledge leaves bed boundaries where they are, benched about them", "[planet][beds]") {
    const hog::planet::Bed bed{
        .base = 100 * fixed_one, .top = 180 * fixed_one, .rock = Rock::Limestone};
    const auto at = [&](double metres, std::int64_t strength) {
        return hog::planet::ledge(bed, std::llround(metres * fixed_one), strength);
    };
    for (const double metres : {100.0, 117.0, 140.0, 163.5, 179.9}) {
        CHECK(at(metres, 0) == std::llround(metres * fixed_one));
    }
    CHECK(at(100.0, fixed_one) == 100 * fixed_one);
    CHECK(at(140.0, fixed_one) == 140 * fixed_one);
    CHECK(at(180.0, fixed_one) == 180 * fixed_one);
    // Never folding back on itself, flat at the boundaries and steepest midway.
    std::int64_t last = at(100.0, fixed_one);
    double flattest = 10.0;
    double steepest = 0.0;
    for (int step = 1; step <= 800; ++step) {
        const std::int64_t now = at(100.0 + (step / 10.0), fixed_one);
        REQUIRE(now >= last);
        const double slope = hog::planet::fixed_to_double(now - last) / 0.1;
        if (step <= 40 || step > 760) {
            flattest = std::min(flattest, slope);
        } else if (step > 380 && step <= 420) {
            steepest = std::max(steepest, slope);
        }
        last = now;
    }
    CHECK(flattest < 0.05);
    CHECK(steepest > 1.8);
    CHECK(steepest < 1.9);
}

namespace {

// Tangent directions at `center`, along the x axis and across it.
struct Tangents {
    hog::DVec3 along;
    hog::DVec3 across;
};

[[nodiscard]] Tangents tangents(hog::DVec3 center) {
    const hog::DVec3 axis{1.0, 0.0, 0.0};
    const hog::DVec3 along = glm::normalize(axis - (center * glm::dot(center, axis)));
    return {.along = along, .across = glm::cross(center, along)};
}

// Fixed-point metres the layering shifts between `center` and `metres` away along `toward`.
[[nodiscard]] std::int64_t shift_change(const Beds& beds, const BedSetting& setting,
                                        hog::DVec3 center, hog::DVec3 toward, double metres) {
    const auto at = [&](hog::DVec3 direction) {
        return beds.column(hog::planet::cell_at(direction, 14), setting).shift;
    };
    return std::abs(at(glm::normalize(center + (toward * (metres / radius)))) - at(center));
}

} // namespace

TEST_CASE("belts tilt their beds steeply along the grain, thrusting them; the plains barely",
          "[planet][beds]") {
    const Beds beds{1, radius};
    const BedSetting belt{.continental = true, .mountains = fixed_one, .grain = {fixed_one, 0, 0}};
    constexpr double step = 50.0;
    std::vector<double> belt_dips;
    std::vector<double> plain_dips;
    std::vector<std::int64_t> told_dips;
    double along = 0.0;
    double across = 0.0;
    int thrusts = 0;
    for (int i = 0; i < 400; ++i) {
        const hog::DVec3 center =
            glm::normalize(hog::DVec3{0.3 + (0.001 * i), 1.0, 0.2 + (0.0013 * (i % 37))});
        const Tangents ways = tangents(center);
        const std::int64_t across_change = shift_change(beds, belt, center, ways.across, step);
        const std::int64_t along_change = shift_change(beds, belt, center, ways.along, step);
        // A jump of more than a 60 degree slope over the step is a thrust, not the beds' dip.
        if (static_cast<double>(across_change) > 1.7 * step * fixed_one) {
            ++thrusts;
        } else {
            belt_dips.push_back(std::atan(static_cast<double>(across_change) / (step * fixed_one)) *
                                180.0 / std::numbers::pi);
        }
        across += static_cast<double>(across_change);
        along += static_cast<double>(along_change);
        told_dips.push_back(beds.dip(hog::planet::cell_at(center, 14), belt));
        const BedSetting plain{.continental = true, .grain = {fixed_one, 0, 0}};
        CHECK(beds.dip(hog::planet::cell_at(center, 14), plain) == 0);
        plain_dips.push_back(
            std::atan(static_cast<double>(shift_change(beds, plain, center, ways.across, step)) /
                      (step * fixed_one)) *
            180.0 / std::numbers::pi);
    }
    std::ranges::sort(belt_dips);
    std::ranges::sort(plain_dips);
    const double belt_median = belt_dips.at(belt_dips.size() / 2);
    const double plain_median = plain_dips.at(plain_dips.size() / 2);
    INFO("belt " << belt_median << " degrees, plains " << plain_median << " degrees, " << thrusts
                 << " thrusts, along " << along << " across " << across);
    CHECK(belt_median > 20.0);
    CHECK(belt_median < 60.0);
    CHECK(plain_median < 3.0);
    // As told, about as steep as measured: over 20 degrees, mostly.
    std::ranges::sort(told_dips);
    CHECK(told_dips.at(told_dips.size() / 2) > fixed_one * 36 / 100);
    CHECK(thrusts > 0);
    // Folds and thrusts run with the grain, so the layering changes far less along it.
    CHECK(along * 2.0 < across);
}

TEST_CASE("a volcano's lava buries the beds above its base", "[planet][beds]") {
    const Beds beds{1, radius};
    const BedSetting volcano{.continental = true, .lava = 500 * fixed_one};
    const auto column = beds.column(cell(100, 200), volcano);
    CHECK(beds.rock_at(column, 600 * fixed_one) == Rock::Basalt);
    CHECK(beds.rock_at(column, 2'000 * fixed_one) == Rock::Basalt);
    CHECK_FALSE(beds.bed_at(column, 600 * fixed_one).has_value());
    CHECK(beds.rock_at(column, 400 * fixed_one) != Rock::Basalt);
    CHECK(beds.bed_at(column, 400 * fixed_one).has_value());
}
