#include <hog/server/tick_clock.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace std::chrono_literals;
using hog::GameTime;
using hog::Tick;
using hog::server::TickClock;

TEST_CASE("game time runs at the time scale", "[server][tick]") {
    TickClock clock{GameTime{1h}, hog::server::idle_rate, 20};
    CHECK(clock.tick() == Tick{0});
    CHECK(clock.now() == GameTime{1h});
    CHECK(clock.step() == 1s);

    for (int i = 0; i < 20; ++i) {
        clock.advance();
    }
    CHECK(clock.tick() == Tick{20});
    CHECK(clock.now() == GameTime{1h + 20s});
}

TEST_CASE("rates that do not divide a second never drift", "[server][tick]") {
    TickClock clock{GameTime{}, hog::server::combat_rate, 1};
    hog::GameDuration total{};
    for (int i = 0; i < 30 * 3'600; ++i) {
        total += clock.step();
        clock.advance();
    }
    CHECK(clock.now() == GameTime{1h});
    CHECK(total == 1h);
}

TEST_CASE("a rate change keeps game time continuous", "[server][tick]") {
    TickClock clock{GameTime{}, hog::server::idle_rate, 1};
    for (int i = 0; i < 25; ++i) {
        clock.advance();
    }
    const GameTime before = clock.now();
    clock.set_rate(hog::server::combat_rate);
    CHECK(clock.now() == before);
    CHECK(clock.rate() == hog::server::combat_rate);

    for (int i = 0; i < 30; ++i) {
        clock.advance();
    }
    CHECK(clock.tick() == Tick{55});
    CHECK(clock.now() == before + 1s);

    clock.set_rate(hog::server::idle_rate);
    CHECK(clock.step() == 50ms);
}

TEST_CASE("a resumed clock carries on from its tick", "[server][tick]") {
    TickClock clock{GameTime{1h}, hog::server::idle_rate, 1, Tick{1'000}};
    CHECK(clock.tick() == Tick{1'000});
    CHECK(clock.now() == GameTime{1h});
    clock.advance();
    CHECK(clock.tick() == Tick{1'001});
    CHECK(clock.now() == GameTime{1h + 50ms});
}

TEST_CASE("a scale change keeps game time continuous", "[server][tick]") {
    TickClock clock{GameTime{}, hog::server::idle_rate, 1};
    for (int i = 0; i < 3; ++i) {
        clock.advance();
    }
    clock.set_scale(1'200);
    CHECK(clock.now() == GameTime{150ms});
    CHECK(clock.scale() == 1'200);
    CHECK(clock.step() == 1min);
    clock.advance();
    clock.set_scale(1);
    CHECK(clock.now() == GameTime{1min + 150ms});
    CHECK(clock.step() == 50ms);
}

TEST_CASE("tick clocks reject bad input", "[server][tick]") {
    const hog::testing::AssertionGuard guard;
    using hog::testing::AssertionFailed;
    CHECK_THROWS_AS(TickClock(GameTime{}, 0, 1), AssertionFailed);
    CHECK_THROWS_AS(TickClock(GameTime{}, hog::server::max_rate + 1, 1), AssertionFailed);
    CHECK_THROWS_AS(TickClock(GameTime{}, 20, 0), AssertionFailed);

    TickClock clock{GameTime{}, 20, 1};
    CHECK_THROWS_AS(clock.set_rate(0), AssertionFailed);
    CHECK_THROWS_AS(clock.set_scale(0), AssertionFailed);
}
