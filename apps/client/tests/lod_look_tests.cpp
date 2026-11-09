#include "test_planet.hpp"

#include <hog/client/daylight.hpp>
#include <hog/client/flight.hpp>
#include <hog/client/picture.hpp>
#include <hog/client/scene.hpp>
#include <hog/client/tile_meshes.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/land.hpp>
#include <hog/globe/planet.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/projection.hpp>
#include <hog/render/device.hpp>
#include <hog/server/tile_sending.hpp>
#include <hog/terrain/bend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

using hog::client::testing::calendar;

namespace {

constexpr std::uint32_t width = 1600;
constexpr std::uint32_t height = 900;

std::vector<double> luma(const hog::client::Picture& picture) {
    std::vector<double> out(static_cast<std::size_t>(picture.width) * picture.height);
    for (std::size_t n = 0; n < out.size(); ++n) {
        const auto channel = [&](std::size_t c) {
            return static_cast<double>(std::to_integer<int>(picture.texels[(n * 4) + c]));
        };
        out[n] = (0.2126 * channel(0)) + (0.7152 * channel(1)) + (0.0722 * channel(2));
    }
    return out;
}

// Mean difference of two pictures' lightness, each first averaged over `box` texels square: what
// changes in what's seen, rather than in each texel.
double differ(const hog::client::Picture& a, const hog::client::Picture& b, std::uint32_t box) {
    const std::vector<double> la = luma(a);
    const std::vector<double> lb = luma(b);
    double sum = 0.0;
    std::size_t count = 0;
    for (std::uint32_t y = 0; y + box <= a.height; y += box) {
        for (std::uint32_t x = 0; x + box <= a.width; x += box) {
            double here = 0.0;
            for (std::uint32_t dy = 0; dy < box; ++dy) {
                for (std::uint32_t dx = 0; dx < box; ++dx) {
                    const std::size_t at = (static_cast<std::size_t>(y + dy) * a.width) + x + dx;
                    here += la[at] - lb[at];
                }
            }
            sum += std::abs(here) / (box * box);
            ++count;
        }
    }
    return sum / static_cast<double>(std::max<std::size_t>(count, 1));
}

// The tiles cut meshed, all of them: what the scene's to be shown.
hog::client::GroundChanges meshed(hog::client::TileMeshes& meshes,
                                  const hog::server::TileSending::Cut& cut) {
    for (const auto& tile : cut.sent) {
        REQUIRE(meshes.take(tile));
    }
    hog::client::GroundChanges changes = meshes.collect();
    meshes.finish();
    hog::client::GroundChanges more = meshes.collect();
    changes.shown.insert(changes.shown.end(), more.shown.begin(), more.shown.end());
    changes.gone.insert(changes.gone.end(), more.gone.begin(), more.gone.end());
    return changes;
}

// Milliseconds each pass took on the GPU, averaged over frames of the shot.
std::map<std::string, double, std::less<>>
passes_of(hog::render::Device& device, hog::client::Scene& scene, const hog::client::Shot& shot) {
    constexpr int frames = 20;
    std::map<std::string, double, std::less<>> passes;
    for (int frame = 0; frame < frames; ++frame) {
        REQUIRE(hog::client::draw_still(device, scene, shot, {.width = width, .height = height}));
        for (const hog::render::GpuSpan& span : scene.timings()) {
            passes[std::string{span.name}] += span.milliseconds / frames;
        }
    }
    return passes;
}

} // namespace

// Draws the land cut for each spot with its finest tiles reaching out to where a voxel at a
// level's far edge spans so many pixels, on the real GPU: what each costs, how much of the picture
// changes from the finest, and how much it shimmers when turned by half a pixel.
TEST_CASE("the land's detail against what it costs to draw", "[.profile]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const auto planet = hog::globe::load_planet(HOG_SOURCE_DIR "/config/planet.toml");
    REQUIRE(planet);
    const hog::globe::Generator generator{*planet};
    const double radius = planet->body.radius;
    auto created = hog::render::Device::create({});
    if (!created) {
        SKIP("no Vulkan device: " << created.error().message);
    }
    hog::render::Device& device = **created;
    hog::jobs::JobSystem jobs;
    const hog::globe::EachOf each = [&](std::size_t count,
                                        const std::function<void(std::size_t)>& body) {
        jobs.parallel_for(static_cast<std::uint32_t>(count), 1,
                          [&](hog::jobs::Range range, std::uint32_t /*thread*/) {
                              for (std::uint32_t n = range.begin; n < range.end; ++n) {
                                  body(n);
                              }
                          });
    };
    hog::globe::Land land{generator};
    constexpr double degree = std::numbers::pi / 180.0;
    // Radians a texel spans at the middle of the picture.
    const double texel = 2.0 * std::tan(hog::client::vertical_fov / 2.0) / height;
    const double voxel = generator.layers().thickness;
    struct Spot {
        const char* name;
        double latitude;
        double longitude;
        double up;
        double heading;
        double pitch;
    };
    for (const Spot& spot : {Spot{.name = "lake",
                                  .latitude = 2.511136,
                                  .longitude = 127.904199,
                                  .up = 1'809.0,
                                  .heading = -121.8,
                                  .pitch = -66.0},
                             Spot{.name = "shore",
                                  .latitude = 10.882248,
                                  .longitude = 85.555264,
                                  .up = 100.0,
                                  .heading = -10.7,
                                  .pitch = -21.5},
                             Spot{.name = "skyline",
                                  .latitude = 2.511136,
                                  .longitude = 127.904199,
                                  .up = 30.0,
                                  .heading = -121.8,
                                  .pitch = -6.0}}) {
        const hog::DVec3 up =
            hog::planet::direction_at(spot.latitude * degree, spot.longitude * degree);
        const double ground =
            hog::globe::ground_below(generator, up, hog::globe::voxel_lod(generator))
                .value_or(radius);
        const hog::DVec3 eye = up * (ground + spot.up);
        const hog::GameTime date = hog::to_game_time(
            hog::WallClock::time_point{std::chrono::sys_days{std::chrono::year{2026} / 6 / 21}});
        const auto shot_at = [&](double heading) {
            const hog::client::Flight flight = hog::client::turned_to(
                hog::client::start_flight(up, radius, ground + spot.up - radius,
                                          hog::planet::north_pole),
                heading, spot.pitch * degree);
            return hog::client::Shot{
                .camera = hog::client::flight_camera(flight, hog::client::vertical_fov),
                .bend = hog::terrain::bend_from(flight.eye, radius, ground - radius),
                .time = hog::client::start_time(calendar(), date, up, 0.5),
                .seconds = 0.0,
                .companion = std::nullopt};
        };
        std::optional<hog::client::Picture> finest;
        for (const double pixels : {2.0, 3.0, 4.0, 6.0, 8.0, 12.0}) {
            const double reach = voxel / (pixels * texel);
            const hog::server::TileSending sending{reach};
            auto scene = hog::client::Scene::create(device, generator.planet(), jobs, calendar(),
                                                    hog::client::still_format, 1);
            REQUIRE(scene);
            hog::client::TileMeshes meshes{generator.planet(), generator.layers(), jobs};
            auto started = std::chrono::steady_clock::now();
            const hog::server::TileSending::Cut cut = sending.cut(eye, {}, land, each);
            const double cutting =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            started = std::chrono::steady_clock::now();
            hog::client::GroundChanges changes = meshed(meshes, cut);
            const double meshing =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::size_t quads = 0;
            for (const auto& shown : changes.shown) {
                quads += (shown.ground->indices.size() + shown.water->indices.size()) / 6;
            }
            scene->show_ground(std::move(changes));
            scene->set_ground_state(meshes.covered(eye, 8'000.0), false);

            const auto still = hog::client::draw_still(
                device, *scene, shot_at(spot.heading * degree), {.width = width, .height = height});
            REQUIRE(still);
            double gpu = 0.0;
            std::string costs;
            for (const auto& [name, milliseconds] :
                 passes_of(device, *scene, shot_at(spot.heading * degree))) {
                gpu += milliseconds;
                costs += std::format(" {} {:.1f}", name, milliseconds);
            }
            const auto turned = hog::client::draw_still(
                device, *scene, shot_at((spot.heading * degree) + (texel / 2.0)),
                {.width = width, .height = height});
            REQUIRE(turned);
            if (!finest) {
                finest = *still;
            }
            WARN(
                std::format("{} {:>4.1f} px ({:>4.0f} m): {:>5} tiles, {:>9} quads, cut {:5.1f} s, "
                            "meshed {:5.1f} s, gpu {:6.2f} ms, changed {:5.2f}, shimmer {:5.2f};{}",
                            spot.name, pixels, reach, cut.sent.size(), quads, cutting, meshing, gpu,
                            differ(*finest, *still, 8), differ(*still, *turned, 1), costs));
            REQUIRE(hog::client::save_picture(
                std::filesystem::path{where} /
                    std::format("lod_{}_{:02.0f}px.png", spot.name, pixels),
                *still));
        }
    }
}
