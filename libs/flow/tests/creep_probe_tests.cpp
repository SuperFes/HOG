#include <hog/flow/simulation.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace hog::globe;

// Loose cover over the ground of a patch of the highest mountain, before and after the cover
// creeps. Hidden: for looking at.
TEST_CASE("probe: cover on a mountain after creep", "[.creep_probe]") {
    const Generator generator{Planet{}};
    const Layers& layers = generator.layers();
    hog::DVec3 peak{0.0, 0.0, 1.0};
    double highest = 0.0;
    for (int k = 0; k < 40000; ++k) {
        const double z = 1.0 - ((2.0 * k + 1.0) / 40000.0);
        const double r = std::sqrt(1.0 - (z * z));
        const hog::DVec3 d{r * std::cos(2.399963 * k), r * std::sin(2.399963 * k), z};
        if (const double ground = generator.column(d, 200.0).ground(); ground > highest) {
            highest = ground;
            peak = d;
        }
    }
    constexpr std::uint32_t side = 96;
    const Cell centre = cell_under(peak, layers.level);
    const std::uint32_t i0 = centre.i - (side / 2);
    const std::uint32_t j0 = centre.j - (side / 2);
    std::set<BrickSite> bricks;
    std::vector<int> grounds;
    for (std::uint32_t y = 0; y < side; ++y) {
        for (std::uint32_t x = 0; x < side; ++x) {
            const Cell cell{centre.level, centre.face, i0 + x, j0 + y};
            const int ground = layers.layer_at(generator.column(center(cell), 1.0).ground());
            grounds.push_back(ground);
            for (int layer = std::max(ground - 10, 0); layer <= ground + 8; layer += brick_side) {
                bricks.insert(brick_of({cell.face, cell.i, cell.j, static_cast<std::uint32_t>(layer)}));
            }
            bricks.insert(brick_of({cell.face, cell.i, cell.j, static_cast<std::uint32_t>(ground + 8)}));
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
                double metres = 0.0;
                const int ground = grounds[(y * side) + x];
                for (int layer = std::max(ground - 10, 0); layer <= ground + 8; ++layer) {
                    const auto voxel = world.at({centre.face, i0 + x, j0 + y, static_cast<std::uint32_t>(layer)});
                    if (voxel.solid != Material::Air && properties(voxel.solid).repose > 0.0) {
                        metres += static_cast<double>(voxel.solid_amount) / Voxel::full * layers.thickness;
                    }
                    if (voxel.grain != Material::Air && properties(voxel.grain).repose > 0.0) {
                        metres += static_cast<double>(voxel.grain_amount) / Voxel::full * layers.thickness;
                    }
                }
                cover.push_back(metres);
            }
        }
        if (const char* dir = std::getenv("HOG_LOOK_DIR")) {
            std::ofstream out{std::string{dir} + "/cover_" + label + ".pgm", std::ios::binary};
            out << "P5\n" << side - 16 << " " << side - 16 << "\n255\n";
            for (const double metres : cover) {
                out.put(static_cast<char>(std::min(255.0, metres * 12.0)));
            }
        }
        std::ranges::sort(cover);
        const auto pick = [&](double q) { return cover[static_cast<std::size_t>(q * static_cast<double>(cover.size() - 1))]; };
        std::printf("%s: cover p10 %.2f p50 %.2f p90 %.2f p99 %.2f max %.2f m\n", label, pick(0.1),
                    pick(0.5), pick(0.9), pick(0.99), cover.back());
    };
    measure("before");
    const int rounds = std::getenv("HOG_ROUNDS") != nullptr ? std::atoi(std::getenv("HOG_ROUNDS")) : 10;
    for (int round = 0; round < rounds; ++round) {
        simulation.age_cover(list, std::getenv("HOG_YEARS") != nullptr ? std::atof(std::getenv("HOG_YEARS")) : 50.0);
    }
    measure("after");
}

} // namespace
