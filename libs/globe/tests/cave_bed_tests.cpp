#include "../src/terms.hpp"

#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::globe::Cell;
using hog::globe::Generator;

namespace {

namespace terms = hog::globe::terms;

// Voxel sizes a world may be run at, in metres.
constexpr std::array voxels{0.5, 1.0, 2.5, 5.0};

constexpr int level = 4;
constexpr int run = 12;

// A small planet whose drainage is a row of cells each running on to the next, over one face, and
// another row beside it that drains almost nothing.
struct Bed {
    hog::globe::Planet planet;
    hog::globe::Derived derived;
    Generator::Shape shape;
    terms::Context context;

    Bed(double wet, double dry) : planet{make()}, derived{hog::globe::derive(planet)} {
        const auto count = static_cast<std::size_t>(hog::globe::cells_at(level));
        shape.cut_level = level;
        shape.receiver.resize(count);
        std::iota(shape.receiver.begin(), shape.receiver.end(), 0U);
        shape.catchment.assign(count, 0.0F);
        shape.table.assign(count, 0.0F);
        shape.cut.assign(count, 0.0F);
        for (int k = 0; k < run; ++k) {
            for (const auto [j, area] : {std::pair{5U, wet}, std::pair{9U, dry}}) {
                const auto at = index(k, j);
                shape.receiver[at] = static_cast<std::uint32_t>(index(std::min(k + 1, run - 1), j));
                shape.catchment[at] = static_cast<float>(area);
                shape.table[at] = static_cast<float>(100.0 - (5.0 * k));
            }
        }
        context = {.planet = &planet, .derived = &derived, .shape = &shape};
    }
    Bed(const Bed&) = delete;
    Bed& operator=(const Bed&) = delete;

    static hog::globe::Planet make() {
        auto planet = hog::globe::preset("earth").value_or(hog::globe::Planet{});
        planet.radius = 4'000.0;
        return planet;
    }
    static std::size_t index(int k, std::uint32_t j) {
        return static_cast<std::size_t>(hog::globe::index_of(
            {.level = level, .face = 0, .i = static_cast<std::uint32_t>(2 + k), .j = j}));
    }
    [[nodiscard]] DVec3 middle(int k, std::uint32_t j) const {
        const Cell a{.level = level, .face = 0, .i = static_cast<std::uint32_t>(2 + k), .j = j};
        const Cell b{.level = level, .face = 0, .i = static_cast<std::uint32_t>(3 + k), .j = j};
        return glm::normalize(hog::globe::center(a) + hog::globe::center(b));
    }
    [[nodiscard]] double table(int k) const { return 100.0 - (5.0 * k); }

    // The nearest the passage comes to the point under `direction` at `height` over the radius,
    // searched a few passage widths about it, and how many sampled points lie inside it.
    struct Found {
        double nearest = 1e9;
        std::size_t inside = 0;
        double lowest = 1e9;
        double highest = -1e9;
    };
    [[nodiscard]] Found search(DVec3 direction, double height, double lod) const {
        const DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, direction));
        const DVec3 north = glm::cross(direction, east);
        Found found;
        constexpr int reach = 14;
        for (int a = -reach; a <= reach; ++a) {
            for (int b = -reach; b <= reach; ++b) {
                for (int c = -reach; c <= reach; ++c) {
                    const DVec3 point = (direction * (planet.radius + height + (2.0 * c))) +
                                        (east * (2.0 * a)) + (north * (2.0 * b));
                    const double d = terms::karst_distance(context, glm::normalize(point),
                                                           glm::length(point), lod, false);
                    found.nearest = std::min(found.nearest, d);
                    if (d < 0.0) {
                        ++found.inside;
                        const double up = glm::length(point) - planet.radius;
                        found.lowest = std::min(found.lowest, up);
                        found.highest = std::max(found.highest, up);
                    }
                }
            }
        }
        return found;
    }
};

} // namespace

TEST_CASE("water dissolves a passage along the drainage, at any voxel size", "[globe][caves]") {
    const double voxel = GENERATE(from_range(voxels));
    CAPTURE(voxel);
    const double lod = 2.0 * voxel;
    const Bed bed{1e9, 1e4};
    for (int k = 1; k < run - 2; k += 3) {
        CAPTURE(k);
        const Bed::Found wet = bed.search(bed.middle(k, 5), bed.table(k) - 30.0, lod);
        CHECK(wet.inside > 0);
        // Where almost no water ran, none opened.
        const Bed::Found dry = bed.search(bed.middle(k, 9), bed.table(k) - 30.0, lod);
        CHECK(dry.inside == 0);
        CHECK(dry.nearest > 0.0);
    }
}

TEST_CASE("a passage falls with the water table and widens with its flow", "[globe][caves]") {
    const double lod = 2.0;
    const Bed bed{1e9, 1e4};
    const Bed::Found up = bed.search(bed.middle(1, 5), bed.table(1) - 30.0, lod);
    const Bed::Found down = bed.search(bed.middle(8, 5), bed.table(8) - 30.0, lod);
    REQUIRE(up.inside > 0);
    REQUIRE(down.inside > 0);
    // The grade: 35 m lower at the downstream end, within what the meanders move it.
    CHECK(up.highest - down.highest > 20.0);
    const Bed trickle{2e5, 1e4};
    const Bed::Found thin = trickle.search(trickle.middle(1, 5), trickle.table(1) - 8.0, lod);
    CHECK(thin.inside < up.inside);
}

TEST_CASE("the passages' bound is never more than their distance", "[globe][caves]") {
    const double voxel = GENERATE(from_range(voxels));
    CAPTURE(voxel);
    const double lod = 2.0 * voxel;
    const Bed bed{1e9, 1e4};
    std::size_t over = 0;
    std::size_t near = 0;
    for (int k = 0; k < run - 1; ++k) {
        const DVec3 direction = bed.middle(k, 5);
        for (int step = -40; step <= 40; ++step) {
            const double height = bed.table(k) - 30.0 + (3.0 * step);
            const double radius = bed.planet.radius + height;
            const double d = terms::karst_distance(bed.context, direction, radius, lod, false);
            const double b = terms::karst_distance(bed.context, direction, radius, lod, true);
            over += b > d + 1e-9 ? 1U : 0U;
            near += d < 50.0 ? 1U : 0U;
        }
    }
    CHECK(near > 20);
    CHECK(over == 0);
}
