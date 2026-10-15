#include <hog/client/frame_times.hpp>
#include <hog/render/gpu_timer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <vector>

using Catch::Matchers::WithinAbs;
using hog::client::FrameTimes;
using hog::client::Timing;
using hog::render::GpuSpan;

TEST_CASE("frame times start empty", "[client][frame_times]") {
    const FrameTimes times;
    CHECK(times.frames() == 0);
    CHECK(times.cpu().name == "frame");
    CHECK(times.cpu().mean == 0.0);
    CHECK(times.cpu().worst == 0.0);
    CHECK(times.gpu().empty());
    CHECK(times.history().empty());
}

TEST_CASE("frame times average the frames held, and know the worst", "[client][frame_times]") {
    FrameTimes times;
    for (const double seconds : {0.010, 0.030, 0.020}) {
        times.add(seconds, {});
    }
    CHECK(times.frames() == 3);
    CHECK_THAT(times.cpu().mean, WithinAbs(20.0, 1e-9));
    CHECK_THAT(times.cpu().worst, WithinAbs(30.0, 1e-9));
    const std::vector<float> history = times.history();
    REQUIRE(history.size() == 3);
    CHECK_THAT(history[0], WithinAbs(10.0, 1e-4));
    CHECK_THAT(history[2], WithinAbs(20.0, 1e-4));
    // Without any timed on the GPU.
    CHECK(times.gpu().empty());
}

TEST_CASE("frame times hold only the last of them", "[client][frame_times]") {
    FrameTimes times{1.0};
    for (int frame = 0; frame < 5; ++frame) {
        times.add(0.4, {});
    }
    CHECK(times.frames() == 2);
    // The newest stays, however long it took.
    times.add(3.0, {});
    CHECK(times.frames() == 1);
    CHECK_THAT(times.cpu().mean, WithinAbs(3'000.0, 1e-9));
}

TEST_CASE("frame times sum each pass the GPU timed, and all of them", "[client][frame_times]") {
    FrameTimes times;
    const std::vector<GpuSpan> first{{.name = "sky", .milliseconds = 1.0},
                                     {.name = "ground", .milliseconds = 4.0}};
    // The sea newly drawn, and before the ground.
    const std::vector<GpuSpan> second{{.name = "sky", .milliseconds = 3.0},
                                      {.name = "sea", .milliseconds = 2.0},
                                      {.name = "ground", .milliseconds = 2.0}};
    times.add(0.016, first);
    times.add(0.016, {});
    times.add(0.016, second);
    const std::vector<Timing> gpu = times.gpu();
    REQUIRE(gpu.size() == 4);
    CHECK(gpu[0].name == "sky");
    CHECK_THAT(gpu[0].mean, WithinAbs(2.0, 1e-9));
    CHECK_THAT(gpu[0].worst, WithinAbs(3.0, 1e-9));
    CHECK(gpu[1].name == "sea");
    CHECK_THAT(gpu[1].mean, WithinAbs(2.0, 1e-9));
    CHECK(gpu[2].name == "ground");
    CHECK_THAT(gpu[2].mean, WithinAbs(3.0, 1e-9));
    CHECK_THAT(gpu[2].worst, WithinAbs(4.0, 1e-9));
    // Over the two frames timed.
    CHECK(gpu[3].name == "GPU");
    CHECK_THAT(gpu[3].mean, WithinAbs(6.0, 1e-9));
    CHECK_THAT(gpu[3].worst, WithinAbs(7.0, 1e-9));
}
