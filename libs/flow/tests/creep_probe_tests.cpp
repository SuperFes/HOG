#include <hog/flow/simulation.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/world.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <print>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace hog::globe;

constexpr std::uint32_t side = 96;

// The highest ground of a spread of samples over the planet, as a direction.
hog::DVec3 highest_peak(const Generator& generator) {
    hog::DVec3 peak{0.0, 0.0, 1.0};
    double highest = 0.0;
    for (int k = 0; k < 40000; ++k) {
        const double z = 1.0 - (((2.0 * k) + 1.0) / 40000.0);
        const double r = std::sqrt(1.0 - (z * z));
        const hog::DVec3 d{r * std::cos(2.399963 * k), r * std::sin(2.399963 * k), z};
        if (const double ground = generator.column(d, 200.0).ground(); ground > highest) {
            highest = ground;
            peak = d;
        }
    }
    return peak;
}

// Metres of loose matter, shaped or grain, up a column about its ground layer.
double cover_at(const World& world, const Cell& column, int ground, double thickness) {
    double metres = 0.0;
    for (int layer = std::max(ground - 10, 0); layer <= ground + 8; ++layer) {
        const auto voxel = world.at({.face = column.face,
                                     .i = column.i,
                                     .j = column.j,
                                     .layer = static_cast<std::uint32_t>(layer)});
        if (voxel.solid != Material::Air && properties(voxel.solid).repose > 0.0) {
            metres += static_cast<double>(voxel.solid_amount) / Voxel::full * thickness;
        }
        if (voxel.grain != Material::Air && properties(voxel.grain).repose > 0.0) {
            metres += static_cast<double>(voxel.grain_amount) / Voxel::full * thickness;
        }
    }
    return metres;
}

// Writes the cover as a grey image into HOG_LOOK_DIR, where that is set.
void write_cover(const std::vector<double>& cover, const char* label) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing sets the environment while tests run
    if (const char* dir = std::getenv("HOG_LOOK_DIR")) {
        std::ofstream out{std::string{dir} + "/cover_" + label + ".pgm", std::ios::binary};
        out << "P5\n" << side - 16 << " " << side - 16 << "\n255\n";
        for (const double metres : cover) {
            out.put(static_cast<char>(std::min(255.0, metres * 12.0)));
        }
    }
}

// Loose cover over the ground of a patch of the highest mountain, before and after the cover
// creeps. Hidden: for looking at.
TEST_CASE("probe: cover on a mountain after creep", "[.creep_probe]") {
    const Generator& generator = hog::testing::small_world();
    const Layers& layers = generator.layers();
    const hog::DVec3 peak = highest_peak(generator);
    const Cell centre = cell_under(peak, layers.level);
    const std::uint32_t i0 = centre.i - (side / 2);
    const std::uint32_t j0 = centre.j - (side / 2);
    std::set<BrickSite> bricks;
    std::vector<int> grounds;
    for (std::uint32_t y = 0; y < side; ++y) {
        for (std::uint32_t x = 0; x < side; ++x) {
            const Cell cell{.level = centre.level, .face = centre.face, .i = i0 + x, .j = j0 + y};
            const int ground = layers.layer_at(generator.column(center(cell), 1.0).ground());
            grounds.push_back(ground);
            for (int layer = std::max(ground - 10, 0); layer <= ground + 8; layer += brick_side) {
                bricks.insert(brick_of({.face = cell.face,
                                        .i = cell.i,
                                        .j = cell.j,
                                        .layer = static_cast<std::uint32_t>(layer)}));
            }
            bricks.insert(brick_of({.face = cell.face,
                                    .i = cell.i,
                                    .j = cell.j,
                                    .layer = static_cast<std::uint32_t>(ground + 8)}));
        }
    }
    hog::globe::MadeWorld made{generator};
    World& world = made.world();
    made.cover(peak);
    hog::flow::Simulation simulation{world};
    const std::vector<BrickSite> list(bricks.begin(), bricks.end());

    const auto measure = [&](const char* label) {
        std::vector<double> cover;
        for (std::uint32_t y = 8; y + 8 < side; ++y) {
            for (std::uint32_t x = 8; x + 8 < side; ++x) {
                const Cell column{
                    .level = centre.level, .face = centre.face, .i = i0 + x, .j = j0 + y};
                cover.push_back(cover_at(world, column, grounds[(y * side) + x], layers.thickness));
            }
        }
        write_cover(cover, label);
        std::ranges::sort(cover);
        const auto pick = [&](double q) {
            return cover[static_cast<std::size_t>(q * static_cast<double>(cover.size() - 1))];
        };
        std::println("{}: cover p10 {:.2f} p50 {:.2f} p90 {:.2f} p99 {:.2f} max {:.2f} m", label,
                     pick(0.1), pick(0.5), pick(0.9), pick(0.99), cover.back());
    };
    measure("before");
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing sets the environment while tests run
    const char* asked_rounds = std::getenv("HOG_ROUNDS");
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing sets the environment while tests run
    const char* asked_years = std::getenv("HOG_YEARS");
    const int rounds = asked_rounds != nullptr ? std::stoi(asked_rounds) : 10;
    const double years = asked_years != nullptr ? std::stod(asked_years) : 50.0;
    for (int round = 0; round < rounds; ++round) {
        simulation.age_cover(list, years);
    }
    measure("after");
}

} // namespace
