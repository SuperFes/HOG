#include "../src/terms.hpp"

#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <print>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::globe::Generator;
namespace terms = hog::globe::terms;

namespace {

constexpr double lod = 2.0;

DVec3 direction(std::uint64_t k) {
    return hog::globe::center(hog::globe::cell_at_index(12, (k * 2'654'435'761ULL) % 100'663'296));
}

// The mean of all, of the highest tenth and of the lowest tenth.
void report(std::string_view what, std::vector<double> values) {
    if (values.size() < 10) {
        std::println("  {:<36} too few ({})", what, values.size());
        return;
    }
    std::ranges::sort(values);
    const auto mean = [](std::span<const double> of) {
        double sum = 0.0;
        for (const double value : of) {
            sum += value;
        }
        return sum / static_cast<double>(of.size());
    };
    const std::size_t tenth = values.size() / 10;
    std::println("  {:<36} {:9.1f} {:9.1f} {:9.1f}   ({})", what, mean(values),
                 mean(std::span{values}.last(tenth)), mean(std::span{values}.first(tenth)),
                 values.size());
}

// Highest less lowest ground over a square `side` metres across, 9 samples a side.
double relief_over(const terms::Context& context, DVec3 down, double side) {
    const DVec3 axis = std::abs(down.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 east = glm::normalize(glm::cross(axis, down));
    const DVec3 north = glm::cross(down, east);
    const double radius = context.planet->radius;
    double lowest = 1e30;
    double highest = -1e30;
    for (int b = 0; b < 9; ++b) {
        for (int a = 0; a < 9; ++a) {
            const double x = ((a / 8.0) - 0.5) * side / radius;
            const double y = ((b / 8.0) - 0.5) * side / radius;
            const double height =
                terms::column(context, glm::normalize(down + (x * east) + (y * north)), lod)
                    .height.value;
            lowest = std::min(lowest, height);
            highest = std::max(highest, height);
        }
    }
    return highest - lowest;
}

struct Survey {
    std::vector<double> high_ground;
    std::vector<double> low_ground;
    std::vector<double> belts;
    std::vector<double> belt_relief;
    std::vector<double> plain_relief;
    std::vector<double> cut;
    std::vector<double> rims;
    std::vector<double> floors;
};

// How far gullies cut the ground under `down`, metres: the relief with them less without.
double cut_at(const Generator& world, DVec3 down) {
    const terms::Context context{
        .planet = &world.planet(), .derived = &world.derived(), .shape = &world.shape()};
    hog::globe::Derived dry_derived = world.derived();
    dry_derived.fluvial = false;
    const terms::Context dry{
        .planet = &world.planet(), .derived = &dry_derived, .shape = &world.shape()};
    const terms::Province crust = terms::province(context, down);
    const hog::globe::Noise built = terms::built_up(context, down, crust);
    return terms::relief(dry, down, crust, built, lod).value -
           terms::relief(context, down, crust, built, lod).value;
}

Survey survey(const Generator& world) {
    const terms::Context context{
        .planet = &world.planet(), .derived = &world.derived(), .shape = &world.shape()};
    const double datum = world.shape().datum;
    Survey found;
    for (std::uint64_t k = 0; k < 6'000; ++k) {
        const DVec3 down = direction(k);
        const terms::Column column = terms::column(context, down, lod);
        const double height = column.height.value - datum;
        if (column.high > 0.9) {
            found.high_ground.push_back(height);
        } else if (column.high < 0.1) {
            found.low_ground.push_back(height);
        }
        if (column.built > 0.5) {
            found.belts.push_back(height);
            if (found.belt_relief.size() < 150) {
                found.belt_relief.push_back(
                    relief_over(context, down, 2.5 * world.derived().tallest));
            }
        }
        if (column.built < 0.02 && column.high > 0.9 && found.plain_relief.size() < 150) {
            found.plain_relief.push_back(relief_over(context, down, 2'000.0));
        }
        if (const double cut = world.derived().fluvial ? cut_at(world, down) : 0.0; cut > 0.5) {
            found.cut.push_back(cut);
        }
        const double struck = terms::craters(context, down, lod).value;
        if (struck > 0.5) {
            found.rims.push_back(struck);
        } else if (struck < -0.5) {
            found.floors.push_back(-struck);
        }
    }
    return found;
}

} // namespace

TEST_CASE("how tall each landform stands", "[.survey]") {
    for (const std::string_view name : {"earth", "mars", "venus", "moon"}) {
        const Generator world{hog::globe::preset(name).value_or(hog::globe::Planet{})};
        std::println("{}: radius {:.0f} m, tallest {:.0f} m, isostatic step {:.0f} m", name,
                     world.planet().radius, world.derived().tallest,
                     world.derived().isostatic_step);
        std::println("  {:<36} {:>9} {:>9} {:>9}", "metres", "mean", "top 10%", "low 10%");
        Survey found = survey(world);
        report("high crust (continents), height", std::move(found.high_ground));
        report("low crust (ocean floor), height", std::move(found.low_ground));
        report("mountain belts, height", std::move(found.belts));
        report("mountains, relief over peak spacing", std::move(found.belt_relief));
        report("plains, relief over 2 km", std::move(found.plain_relief));
        report("valleys and gullies, depth cut", std::move(found.cut));
        report("crater rims, height over ground", std::move(found.rims));
        report("crater floors, depth under ground", std::move(found.floors));
    }
}

TEST_CASE("what running water cut from the ground changes smoothly over the drainage's cells",
          "[globe][generator]") {
    auto planet = hog::globe::preset("earth").value_or(hog::globe::Planet{});
    planet.radius = 4'000.0;
    planet.floor_depth = 100.0;
    planet.sky_height = 500.0;
    planet.wear = 0.03;
    const hog::globe::Generator world{planet};
    const terms::Context context{
        .planet = &world.planet(), .derived = &world.derived(), .shape = &world.shape()};
    const double fine = hog::globe::voxel_lod(world);
    const int cut_level = world.shape().cut_level;
    // Cut is what the column's ground lies under the terms' own height.
    const auto cut_at = [&](DVec3 direction) {
        return terms::column(context, direction, fine).height.value -
               (world.column(direction, fine).ground() - world.planet().radius +
                world.shape().datum);
    };
    double worst = 0.0;
    double spread = 0.0;
    for (std::uint64_t k = 0; k < 40; ++k) {
        const hog::globe::Cell cell =
            hog::globe::cell_at_index(cut_level, (k * 2'654'435'761ULL) %
                                                     hog::globe::cells_at(cut_level));
        for (const auto& [di, dj] : {std::pair{1, 0}, std::pair{0, 1}, std::pair{1, 1}}) {
            const auto next = hog::globe::next_to(cell, di, dj);
            if (!next) {
                continue;
            }
            // From one middle to the other, over the edge between, in steps about a metre long.
            const DVec3 from = hog::globe::center(cell);
            const DVec3 to = hog::globe::center(*next);
            const int steps = static_cast<int>(glm::length(to - from) * planet.radius);
            double before = cut_at(from);
            for (int step = 1; step <= steps; ++step) {
                const double share = static_cast<double>(step) / static_cast<double>(steps);
                const double here = cut_at(glm::normalize(from + ((to - from) * share)));
                worst = std::max(worst, std::abs(here - before));
                spread = std::max(spread, std::abs(here - cut_at(from)));
                before = here;
            }
        }
    }
    CAPTURE(worst, spread);
    // The cut differs from cell to cell by metres, but a step of a metre changes it by a small
    // share of that, never jumping at an edge.
    CHECK(spread > 1.0);
    CHECK(worst < 0.02 * spread);
}
