#include <hog/physics/space.hpp>
#include <hog/server/move_check.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <optional>
#include <utility>

using namespace hog;
using namespace hog::server;
using namespace std::chrono_literals;

namespace {

constexpr double radius = 115'000.0;
constexpr MonotonicClock::time_point start{};

// Metres along +x from the centre, `y` east and `z` north.
WorldPosition at(double x, double y = 0.0, double z = 0.0) {
    return WorldPosition{radius + x, y, z};
}

// A square 100 m across, `x` metres over the surface, facing up or down.
physics::Mesh square(double x, bool facing_up) {
    physics::Mesh mesh{.origin = at(x),
                       .vertices = {{0.0F, -50.0F, -50.0F},
                                    {0.0F, 50.0F, -50.0F},
                                    {0.0F, 50.0F, 50.0F},
                                    {0.0F, -50.0F, 50.0F}},
                       .triangles = {{0, 1, 2}, {0, 2, 3}}};
    if (!facing_up) {
        for (auto& triangle : mesh.triangles) {
            std::swap(triangle[1], triangle[2]);
        }
    }
    return mesh;
}

struct Ground {
    Ground() { REQUIRE(space.add(square(0.0, true))); }

    physics::Space space;
};

} // namespace

TEST_CASE("moves at a walk are taken", "[server][moves]") {
    const Ground ground;
    MoveCheck check{at(0.0), start};
    auto now = start;
    for (int step = 1; step <= 20; ++step) {
        now += 50ms;
        CHECK(check.judge(at(0.0, 0.3 * step), now, ground.space) == Verdict::Taken);
    }
    CHECK(check.at() == at(0.0, 6.0));
}

TEST_CASE("moves too far for the time are refused, leaving the player where they were",
          "[server][moves]") {
    const Ground ground;
    MoveCheck check{at(0.0), start};
    CHECK(check.judge(at(0.0, 20.0), start + 50ms, ground.space) == Verdict::TooFast);
    CHECK(check.at() == at(0.0));
    // Running speed, kept up, is fine; a little more, kept up, isn't.
    auto now = start + 50ms;
    double east = 0.0;
    for (int step = 0; step < 100; ++step) {
        now += 50ms;
        east += 0.3;
        REQUIRE(check.judge(at(0.0, east), now, ground.space) == Verdict::Taken);
    }
    Verdict last = Verdict::Taken;
    for (int step = 0; step < 100 && last == Verdict::Taken; ++step) {
        now += 50ms;
        east += 0.4;
        last = check.judge(at(0.0, east), now, ground.space);
    }
    CHECK(last == Verdict::TooFast);
}

TEST_CASE("moves held up on the way may arrive at once, up to a burst", "[server][moves]") {
    const Ground ground;
    const MoveRules rules;
    MoveCheck check{at(0.0), start, rules};
    const auto now = start + 2s;
    double east = 0.0;
    while (east + 1.0 <= rules.fastest * rules.burst) {
        east += 1.0;
        REQUIRE(check.judge(at(0.0, east), now, ground.space) == Verdict::Taken);
    }
    CHECK(check.judge(at(0.0, east + 1.0), now, ground.space) == Verdict::TooFast);
}

TEST_CASE("falling costs nothing", "[server][moves]") {
    const Ground ground;
    MoveCheck check{at(40.0), start};
    CHECK(check.judge(at(0.0), start + 50ms, ground.space) == Verdict::Taken);
}

TEST_CASE("feet under the ground are refused, unless only just", "[server][moves]") {
    const Ground ground;
    MoveCheck check{at(0.0), start};
    CHECK(check.judge(at(-1.0), start + 1s, ground.space) == Verdict::UnderGround);
    CHECK(check.at() == at(0.0));
    CHECK(check.judge(at(-0.2), start + 2s, ground.space) == Verdict::Taken);
}

TEST_CASE("standing under an overhang is fine", "[server][moves]") {
    Ground ground;
    REQUIRE(ground.space.add(square(3.0, false)));
    MoveCheck check{at(0.0), start};
    CHECK(check.judge(at(0.0, 1.0), start + 1s, ground.space) == Verdict::Taken);
}

TEST_CASE("climbing past what the slope holds is refused once a jump's worth is spent",
          "[server][moves]") {
    const Ground ground;
    const MoveRules rules;
    const Grip gentle = [](WorldPosition) { return std::optional<double>{0.3}; };
    MoveCheck check{at(0.0), start, rules};
    // A rise that the slope allows costs nothing, however often.
    auto now = start;
    double east = 0.0;
    double up = 0.0;
    for (int step = 0; step < 40; ++step) {
        now += 50ms;
        east += 0.3;
        up += 0.3 * std::tan(0.29);
        REQUIRE(check.judge(at(up, east), now, ground.space, gentle) == Verdict::Taken);
    }
    // A jump's rise is taken once, but a wall climbed at the same pace is not.
    now += 50ms;
    CHECK(check.judge(at(up + 1.0, east), now, ground.space, gentle) == Verdict::Taken);
    Verdict last = Verdict::Taken;
    for (int step = 0; step < 100 && last == Verdict::Taken; ++step) {
        now += 50ms;
        up += 1.0;
        last = check.judge(at(up + 1.0, east), now, ground.space, gentle);
    }
    CHECK(last == Verdict::TooSteep);
}

TEST_CASE("a steeper grip lets a steeper climb through", "[server][moves]") {
    const Ground ground;
    const Grip steep = [](WorldPosition) { return std::optional<double>{0.87}; };
    const Grip flat = [](WorldPosition) { return std::optional<double>{0.1}; };
    MoveCheck on_steep{at(0.0), start};
    MoveCheck on_flat{at(0.0), start};
    const auto now = start + 1s;
    // 2 m up for 1 m along is beyond either's allowance at once; 1 m up for 1 m along is not.
    CHECK(on_steep.judge(at(1.0, 1.0), now, ground.space, steep) == Verdict::Taken);
    CHECK(on_steep.judge(at(2.0, 2.0), now, ground.space, steep) == Verdict::Taken);
    CHECK(on_flat.judge(at(1.4, 1.0), now, ground.space, flat) == Verdict::Taken);
    CHECK(on_flat.judge(at(2.8, 2.0), now, ground.space, flat) == Verdict::TooSteep);
}
