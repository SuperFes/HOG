#include <hog/core/math.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/volume.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <random>
#include <vector>

using hog::planet::CellId;
using hog::planet::Contents;
using hog::planet::Material;
using hog::planet::Volume;
using hog::planet::VolumeCell;

namespace {

constexpr Contents air{.material = Material::Air, .temperature = 250.0F};
constexpr Contents rock{.material = Material::Granite, .temperature = 400.0F};
constexpr Contents water{.material = Material::Water, .temperature = 280.0F};

[[nodiscard]] VolumeCell random_cell(std::mt19937_64& random, int levels, int shell_levels) {
    std::uniform_int_distribution<int> face(0, hog::planet::face_count - 1);
    const int level = std::uniform_int_distribution<int>(0, levels)(random);
    const int shell_level = std::uniform_int_distribution<int>(0, shell_levels)(random);
    std::uniform_int_distribution<std::uint32_t> across(0, (1U << level) - 1);
    std::uniform_int_distribution<std::uint32_t> outward(0, (1U << shell_level) - 1);
    const CellId column = CellId::from_face_ij(face(random), across(random), across(random), level);
    return VolumeCell::from(column, shell_level, outward(random));
}

// Of the whole volume, as a share of one face's; exact while levels stay low.
[[nodiscard]] double measure(VolumeCell cell) {
    return std::ldexp(1.0, -(2 * cell.column().level()) - cell.shell_level());
}

// Of the one leaf holding `cell`; an invalid material if none does.
[[nodiscard]] Contents contents_of(const Volume& volume, VolumeCell cell) {
    constexpr Contents none{.material = static_cast<Material>(255), .temperature = 0.0F};
    return volume.leaf_holding(cell).value_or(Volume::Leaf{.cell = {}, .contents = none}).contents;
}

[[nodiscard]] std::map<Material, double> measures(const Volume& volume) {
    std::map<Material, double> by_material;
    volume.for_each_leaf([&](VolumeCell cell, const Contents& contents) {
        by_material[contents.material] += measure(cell);
    });
    return by_material;
}

} // namespace

TEST_CASE("a new volume is six leaves of one thing", "[planet][volume]") {
    const Volume volume{air};
    CHECK(volume.leaf_count() == 6);
    const VolumeCell deep = VolumeCell::from(CellId::from_face_ij(3, 100, 7, 9), 12, 1000);
    const auto held = volume.leaf_holding(deep);
    REQUIRE(held.has_value());
    const Volume::Leaf leaf = held.value_or(Volume::Leaf{});
    CHECK(leaf.cell == VolumeCell::from(CellId::from_face(3)));
    CHECK(leaf.contents == air);
}

TEST_CASE("filling a cell splits only down to it", "[planet][volume]") {
    Volume volume{air};
    const VolumeCell cell = VolumeCell::from(CellId::from_face_ij(1, 2, 3, 2), 3, 5);
    volume.set(cell, rock);
    // Two splits across and three outward.
    CHECK(volume.leaf_count() == 6 + (2 * 3) + 3);
    const auto held = volume.leaf_holding(cell);
    REQUIRE(held.has_value());
    const Volume::Leaf leaf = held.value_or(Volume::Leaf{});
    CHECK(leaf.cell == cell);
    CHECK(leaf.contents == rock);
    CHECK(contents_of(volume, VolumeCell::from(cell.column(), 3, 4)) == air);

    // Spanning the filled cell and air beside it, no one leaf holds it.
    CHECK_FALSE(volume.leaf_holding(VolumeCell::from(cell.column(), 2, 2)));
}

TEST_CASE("filling a cell with what's already there changes nothing", "[planet][volume]") {
    Volume volume{air};
    volume.set(VolumeCell::from(CellId::from_face_ij(0, 5, 5, 4), 6, 20), air);
    CHECK(volume.leaf_count() == 6);
}

TEST_CASE("filling a cell drops the leaves it covers", "[planet][volume]") {
    Volume volume{air};
    const CellId column = CellId::from_face_ij(2, 1, 0, 1);
    volume.set(VolumeCell::from(column.child(3), 5, 9), rock);
    volume.set(VolumeCell::from(column.child(0), 4, 2), water);
    volume.set(VolumeCell::from(column), rock);
    CHECK(contents_of(volume, VolumeCell::from(column)) == rock);
    volume.settle();
    // Face 2 is now a quarter rock under three of air.
    CHECK(volume.leaf_count() == 6 + 3);
}

TEST_CASE("settled leaves merge back to the fewest", "[planet][volume]") {
    Volume volume{air};
    const VolumeCell cell = VolumeCell::from(CellId::from_face_ij(4, 9, 6, 4), 7, 77);
    volume.set(cell, water);
    volume.set(cell, air);
    CHECK(volume.leaf_count() > 6);
    CHECK(volume.merge(cell.shell_parent()));
    volume.settle();
    CHECK(volume.leaf_count() == 6);
}

TEST_CASE("the volume holds what was last put in each place", "[planet][volume]") {
    std::mt19937_64 random{17};
    const std::vector<Contents> choices{air, rock, water};
    Volume volume{air};
    std::vector<std::pair<VolumeCell, Contents>> history;
    for (int n = 0; n < 300; ++n) {
        const VolumeCell cell = random_cell(random, 4, 6);
        const Contents contents = choices[static_cast<std::size_t>(n) % choices.size()];
        volume.set(cell, contents);
        history.emplace_back(cell, contents);
    }
    const auto expected = [&](VolumeCell sample) {
        Contents last = air;
        for (const auto& [cell, contents] : history) {
            last = cell.contains(sample) ? contents : last;
        }
        return last;
    };
    for (int n = 0; n < 3000; ++n) {
        const VolumeCell sample = random_cell(random, 8, 10);
        const auto leaf = volume.leaf_holding(sample);
        // A sample coarser than the leaves it spans has no one answer.
        if (leaf.has_value()) {
            REQUIRE(leaf.value_or(Volume::Leaf{}).contents == expected(sample));
        }
    }
}

TEST_CASE("leaves tile the volume and settling keeps every material", "[planet][volume]") {
    std::mt19937_64 random{19};
    Volume volume{air};
    for (int n = 0; n < 400; ++n) {
        volume.set(random_cell(random, 5, 8), n % 2 == 0 ? rock : water);
    }
    for (int n = 0; n < 400; ++n) {
        volume.set(random_cell(random, 5, 8), air);
    }
    const auto before = measures(volume);
    double whole = 0.0;
    for (const auto& [material, share] : before) {
        whole += share;
    }
    CHECK(whole == 6.0);

    const std::size_t leaves = volume.leaf_count();
    volume.settle();
    CHECK(volume.leaf_count() <= leaves);
    CHECK(measures(volume) == before);

    std::size_t counted = 0;
    volume.for_each_leaf([&](VolumeCell, const Contents&) { ++counted; });
    CHECK(counted == volume.leaf_count());
}

TEST_CASE("nothing settled can merge further", "[planet][volume]") {
    std::mt19937_64 random{23};
    Volume volume{air};
    for (int n = 0; n < 200; ++n) {
        volume.set(random_cell(random, 4, 6), n % 3 == 0 ? air : rock);
    }
    volume.settle();
    std::vector<VolumeCell> leaves;
    volume.for_each_leaf([&](VolumeCell cell, const Contents&) { leaves.push_back(cell); });
    for (const VolumeCell leaf : leaves) {
        if (!leaf.column().is_face()) {
            REQUIRE_FALSE(volume.merge(leaf.across_parent()));
        }
        if (leaf.shell_level() > 0) {
            REQUIRE_FALSE(volume.merge(leaf.shell_parent()));
        }
    }
}

namespace {

constexpr double radius = 1'000'000.0;

[[nodiscard]] bool solid(const Contents& contents) {
    return contents.material != Material::Air;
}

// Rock everywhere below the planet's radius.
[[nodiscard]] Volume ground() {
    Volume volume{air};
    for (int face = 0; face < hog::planet::face_count; ++face) {
        volume.set(VolumeCell::from(CellId::from_face(face), 1, 0), rock);
    }
    return volume;
}

} // namespace

TEST_CASE("a ray straight down stops on the ground", "[planet][volume]") {
    const Volume volume = ground();
    const hog::WorldPosition from{0.0, 0.0, radius + 1000.0};
    const auto hit =
        hog::planet::cast_ray(volume, radius, from, hog::DVec3{0.0, 0.0, -1.0}, 5000.0, solid);
    REQUIRE(hit.has_value());
    const hog::planet::RayHit found = hit.value_or(hog::planet::RayHit{});
    CHECK(found.leaf.contents == rock);
    CHECK(std::abs(found.distance - 1000.0) < 0.01);
}

TEST_CASE("a ray from space starts where it enters the volume", "[planet][volume]") {
    const Volume volume = ground();
    const hog::WorldPosition from{3.0 * radius, 0.0, 0.0};
    const auto hit = hog::planet::cast_ray(volume, radius, from, hog::DVec3{-1.0, 0.0, 0.0},
                                           4.0 * radius, solid);
    REQUIRE(hit.has_value());
    CHECK(std::abs(hit.value_or(hog::planet::RayHit{}).distance - (2.0 * radius)) < 0.01);
}

TEST_CASE("a ray with nothing in its way stops nowhere", "[planet][volume]") {
    const Volume volume = ground();
    const hog::WorldPosition from{0.0, radius + 1000.0, 0.0};
    CHECK_FALSE(hog::planet::cast_ray(volume, radius, from, hog::DVec3{0.0, 1.0, 0.0}, 1.0e6, solid)
                    .has_value());
    CHECK_FALSE(
        hog::planet::cast_ray(volume, radius, from, hog::DVec3{0.0, -1.0, 0.0}, 500.0, solid)
            .has_value());
    CHECK_FALSE(hog::planet::cast_ray(volume, radius, hog::WorldPosition{0.0, 3.0 * radius, 0.0},
                                      hog::DVec3{1.0, 0.0, 0.0}, 1.0e7, solid)
                    .has_value());
}

TEST_CASE("rays stop where small steps would first find something", "[planet][volume]") {
    std::mt19937_64 random{37};
    const CellId middle = hog::planet::cell_at(hog::DVec3{0.0, 0.0, 1.0}, 10);
    Volume volume{air};
    std::uniform_int_distribution<std::uint32_t> nearby(0, 6);
    const auto [face, i, j, level] = middle.to_face_ij();
    for (int n = 0; n < 60; ++n) {
        const CellId column =
            CellId::from_face_ij(face, i - 3 + nearby(random), j - 3 + nearby(random), level);
        volume.set(VolumeCell::from(column, 14, 8189 + nearby(random)), rock);
    }
    std::uniform_real_distribution<double> lateral(-2000.0, 2000.0);
    std::uniform_real_distribution<double> slope(-1.0, 1.0);
    constexpr double step = 0.5;
    constexpr double reach = 4000.0;
    int found = 0;
    for (int n = 0; n < 200; ++n) {
        const hog::WorldPosition from{lateral(random), lateral(random), radius + 60.0};
        const hog::DVec3 direction = glm::normalize(hog::DVec3{slope(random), slope(random), -0.3});
        const auto hit = hog::planet::cast_ray(volume, radius, from, direction, reach, solid);
        double stepped = 0.0;
        while (stepped <= reach) {
            const auto leaf = hog::planet::leaf_at(volume, radius, from + (direction * stepped));
            if (leaf.has_value() && solid(leaf.value_or(Volume::Leaf{}).contents)) {
                break;
            }
            stepped += step;
        }
        if (stepped <= reach) {
            ++found;
            REQUIRE(hit.has_value());
            const double distance = hit.value_or(hog::planet::RayHit{}).distance;
            REQUIRE(distance <= stepped);
            REQUIRE(stepped - distance < step + 0.01);
        }
    }
    CHECK(found > 20);
}
