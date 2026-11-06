#include <hog/chunks/globe_stream.hpp>
#include <hog/chunks/mesh_codec.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/globe/world.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/protocol/messages.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <numbers>
#include <vector>

using namespace hog;

// Prints, for each detail a falloff gives, how far the chunks' vertices are from the true ground
// and how big that looks on screen, to judge where detail is let go too soon.
TEST_CASE("how far off the ground each level of detail is, in pixels, to look at",
          "[.look][chunks]") {
    const globe::Generator generator{globe::Planet{}};
    globe::VoxelStore store{generator.layers()};
    const globe::World world{generator, store};
    jobs::JobSystem jobs{3};
    const double radius = generator.planet().radius;
    const DVec3 up = globe::center(globe::cell_at_index(7, 4'321));
    const DVec3 eye = up * (radius + chunks::globe_ground(generator, up) + 2.0);
    // A 1080-line picture across a 60 degree view.
    const double pixels_per_radian = 1080.0 / (2.0 * std::tan(std::numbers::pi / 6.0));

    for (const double slope : {1.0, 1.5, 2.0}) {
        const double ratio = 3.0;
        chunks::GlobeStream stream{world, jobs, {.ratio = ratio, .least = 1'000.0, .slope = slope}};
        stream.follow(eye);
        const auto changes = stream.finish();
        struct Spread {
            std::vector<double> error;   // pixels
            std::vector<double> spacing; // pixels between the chunk's columns
            std::size_t chunks = 0;
        };
        std::map<int, Spread> by_detail;
        for (const auto& shown : changes.shown) {
            const globe::Cell chunk = chunks::globe_cell(shown.key);
            const int detail = globe::detail_of(chunk, generator.layers());
            Spread& spread = by_detail[detail];
            ++spread.chunks;
            const DVec3 origin = shown.ground->origin.metres();
            // The nearest the chunk comes to the eye, and how wide its columns are.
            double nearest = 1e30;
            for (const terrain::BlockVertex& vertex : shown.ground->vertices) {
                const DVec3 at = origin + DVec3{vertex.position};
                nearest = std::min(nearest, glm::length(at - eye));
                const DVec3 direction = glm::normalize(at);
                const auto truth =
                    globe::ground_below(generator, direction, globe::voxel_lod(generator));
                if (!truth || std::abs(glm::length(at) - *truth) > 40.0) {
                    continue; // caves, sides and the sea: not the ground's surface
                }
                const double distance = std::max(glm::length(at - eye), 1.0);
                spread.error.push_back(std::abs(glm::length(at) - *truth) / distance *
                                       pixels_per_radian);
            }
            const double column = generator.layers().thickness * globe::apart_at(detail);
            spread.spacing.push_back(column / std::max(nearest, 1.0) * pixels_per_radian);
        }
        std::printf("ratio %.1f slope %.1f: %zu chunks\n", ratio, slope, changes.shown.size());
        for (auto& [detail, spread] : by_detail) {
            std::ranges::sort(spread.error);
            std::ranges::sort(spread.spacing);
            const auto at = [](const std::vector<double>& values, double share) {
                return values.empty() ? 0.0
                                      : values[static_cast<std::size_t>(
                                            share * static_cast<double>(values.size() - 1))];
            };
            std::printf(
                "  detail %d: %4zu chunks, nearest column spacing %5.1f px (median of nearest), "
                "error median %5.2f px, 95%% %5.2f px, worst %6.2f px\n",
                detail, spread.chunks, at(spread.spacing, 0.5), at(spread.error, 0.5),
                at(spread.error, 0.95), at(spread.error, 1.0));
        }
    }
}

// Prints how long the chunks round a mountain take to mesh, and to ready for sending, to look at.
TEST_CASE("how long the chunks round a mountain take to mesh, to look at", "[.look][chunks]") {
    const globe::Generator generator{globe::Planet{}};
    globe::VoxelStore store{generator.layers()};
    const globe::World world{generator, store};
    constexpr double degree = std::numbers::pi / 180.0;
    const DVec3 up{glm::normalize(DVec3{std::cos(27.927653 * degree) * std::cos(-2.869545 * degree),
                                        std::cos(27.927653 * degree) * std::sin(-2.869545 * degree),
                                        std::sin(27.927653 * degree)})};
    const DVec3 eye = up * (generator.planet().radius + chunks::globe_ground(generator, up) + 3.0);
    const auto seconds = [](auto from) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
    };
    for (const std::uint32_t threads : {4U, 8U, 16U, 32U}) {
        jobs::JobSystem jobs{threads};
        chunks::GlobeStream stream{world, jobs, {.ratio = 3.0, .least = 1'000.0, .slope = 1.0}};
        const auto began = std::chrono::steady_clock::now();
        stream.follow(eye);
        const auto changes = stream.finish();
        const double meshed = seconds(began);
        const auto converting = std::chrono::steady_clock::now();
        std::size_t bytes = 0;
        for (const auto& shown : changes.shown) {
            const auto parts = chunks::to_parts(shown.key, *shown.ground, *shown.water);
            if (parts) {
                for (const protocol::Chunk& part : *parts) {
                    bytes += protocol::encode(part).size();
                }
            }
        }
        const double converted = seconds(converting);
        std::printf(
            "%2u threads: %zu chunks meshed in %.1f s (%.0f a second); readying them to send "
            "takes %.1f s more, one after another (%.1f ms each, %.1f MB in all)\n",
            threads, changes.shown.size(), meshed,
            static_cast<double>(changes.shown.size()) / meshed, converted,
            1'000.0 * converted /
                static_cast<double>(std::max<std::size_t>(changes.shown.size(), 1)),
            static_cast<double>(bytes) / 1e6);
    }
}
