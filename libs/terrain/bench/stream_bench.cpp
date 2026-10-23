#include <hog/jobs/job_system.hpp>
#include <hog/planet/projection.hpp>
#include <hog/terrain/baker.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/selection.hpp>
#include <hog/terrain/stream.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace {

constexpr double radius = 115'000.0;

} // namespace

TEST_CASE("selecting and streaming the ground", "[benchmark]") {
    const hog::planet::World world{hog::planet::PlanetConfig{.seed = 1, .radius = radius}};
    // As many as the profiled scenes have.
    hog::jobs::JobSystem jobs{4};
    hog::terrain::PatchBaker baker{world, jobs};
    hog::terrain::PatchStream stream;

    // Standing on the islands, looking along the ground, as the client's window sees it.
    const hog::DVec3 up = glm::normalize(world.geology().terrain().archipelago().center());
    const double ground =
        hog::terrain::bake_patch(world, hog::planet::cell_at(up, 12)).heights.highest;
    const hog::WorldPosition eye{up * (radius + ground + 1.7)};
    hog::terrain::Viewpoint view{.eye = eye,
                                 .forward =
                                     glm::normalize(glm::cross(up, hog::DVec3{0.0, 0.0, 1.0})),
                                 .up = up,
                                 .vertical_fov = 1.0,
                                 .aspect = 1'600.0 / 900.0,
                                 .viewport_height = 900.0};
    view.bend = hog::terrain::bend_from(eye, radius, ground);
    const hog::terrain::SelectionSettings settings{.only_in_view = false, .seabed_unseen = 0.002};

    const auto select = [&](hog::jobs::JobSystem* pool) {
        return hog::terrain::select_patches(view, radius, settings, stream.known_ground(), pool);
    };
    // Until every bake the view asks for is held.
    for (bool settled = false; !settled;) {
        for (hog::terrain::Patch& patch : baker.finish()) {
            stream.baked(std::move(patch));
        }
        const hog::terrain::StreamFrame frame = stream.update(select(&jobs));
        for (const hog::planet::CellId cell : frame.bakes) {
            baker.start(cell);
        }
        settled = frame.bakes.empty() && frame.arrived.empty() && baker.in_flight() == 0;
    }
    const std::vector<hog::terrain::SelectedPatch> selected = select(&jobs);
    REQUIRE_FALSE(selected.empty());

    BENCHMARK("select") {
        return select(nullptr);
    };
    BENCHMARK("select on the pool") {
        return select(&jobs);
    };
    BENCHMARK("stream") {
        return stream.update(selected);
    };

    // As between frames, once drawing has filled the caches with other things.
    std::vector<std::uint64_t> other(8U << 20U);
    const auto cool = [&] {
        std::uint64_t sum = 0;
        for (std::uint64_t& value : other) {
            sum += ++value;
        }
        return sum;
    };
    BENCHMARK_ADVANCED("select, cold")(Catch::Benchmark::Chronometer meter) {
        (void)cool();
        meter.measure([&] { return select(nullptr); });
    };
    BENCHMARK_ADVANCED("select on the pool, cold")(Catch::Benchmark::Chronometer meter) {
        (void)cool();
        meter.measure([&] { return select(&jobs); });
    };
    BENCHMARK_ADVANCED("stream, cold")(Catch::Benchmark::Chronometer meter) {
        (void)cool();
        meter.measure([&] { return stream.update(selected); });
    };
}
