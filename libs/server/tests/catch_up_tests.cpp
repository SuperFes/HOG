#include <hog/server/catch_up.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

using namespace std::chrono_literals;
using hog::GameDuration;
using hog::GameTime;
using hog::Tick;
using hog::server::catch_up;
using hog::server::TickClock;

namespace {

struct Ran {
    GameTime start;
    GameDuration step;
};

// Every tick must start where the one before ended.
void check_continuous(const std::vector<Ran>& ran, GameTime from) {
    GameTime expected = from;
    for (const Ran& tick : ran) {
        REQUIRE(tick.start == expected);
        expected = tick.start + tick.step;
    }
}

} // namespace

TEST_CASE("a current world has nothing to catch up", "[server][catch_up]") {
    TickClock clock{GameTime{1h}, hog::server::idle_rate, 1, Tick{7}};
    std::uint64_t steps = 0;
    const auto stats = catch_up(
        clock, 100, [] { return GameTime{1h + 49ms}; }, [&](const TickClock&) { ++steps; });
    CHECK(stats.ticks == 0);
    CHECK(steps == 0);
    CHECK(clock.tick() == Tick{7});
}

TEST_CASE("a short gap is caught up at the live scale", "[server][catch_up]") {
    TickClock clock{GameTime{}, hog::server::idle_rate, 1};
    std::vector<Ran> ran;
    const auto stats = catch_up(
        clock, 1'000, [] { return GameTime{10s + 20ms}; },
        [&](const TickClock& at) { ran.push_back({.start = at.now(), .step = at.step()}); });
    CHECK(stats.ticks == 200);
    CHECK(stats.largest_scale == 1);
    CHECK(clock.now() == GameTime{10s});
    CHECK(clock.tick() == Tick{200});
    check_continuous(ran, GameTime{});
}

TEST_CASE("a long gap is caught up in a bounded number of ticks", "[server][catch_up]") {
    const GameTime from{1'000h};
    const GameTime target = from + 24h * 30 + 123ms;
    TickClock clock{from, hog::server::idle_rate, 1, Tick{5'000}};
    std::vector<Ran> ran;
    const auto stats = catch_up(
        clock, 1'000, [&] { return target; },
        [&](const TickClock& at) { ran.push_back({.start = at.now(), .step = at.step()}); });
    CHECK(stats.largest_scale > 1);
    // A coarse pass, then at most a budget of finer ticks for each smaller remainder.
    CHECK(stats.ticks < 3'000);
    CHECK(target - clock.now() < clock.step());
    CHECK(clock.now() <= target);
    CHECK(clock.scale() == 1);
    CHECK(clock.tick() == Tick{5'000 + stats.ticks});
    check_continuous(ran, from);
}

TEST_CASE("time spent catching up is caught up too", "[server][catch_up]") {
    TickClock clock{GameTime{}, hog::server::idle_rate, 1};
    // Each tick costs 2 ms of real time, far faster than the 50 ms it simulates.
    GameTime real{1h};
    std::vector<Ran> ran;
    catch_up(
        clock, 500, [&] { return real; },
        [&](const TickClock& at) {
            ran.push_back({.start = at.now(), .step = at.step()});
            real += 2ms;
        });
    CHECK(real - clock.now() < clock.step());
    check_continuous(ran, GameTime{});
}

TEST_CASE("catching up needs a budget", "[server][catch_up]") {
    const hog::testing::AssertionGuard guard;
    TickClock clock{GameTime{}, hog::server::idle_rate, 1};
    CHECK_THROWS_AS(catch_up(
                        clock, 0, [] { return GameTime{1h}; }, [](const TickClock&) {}),
                    hog::testing::AssertionFailed);
}
