#include <hog/core/time.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <type_traits>

using namespace std::chrono_literals;

static_assert(!std::is_convertible_v<hog::GameTime, hog::WallClock::time_point>);
static_assert(!std::is_convertible_v<hog::WallClock::time_point, hog::GameTime>);
static_assert(!std::is_convertible_v<hog::MonotonicClock::time_point, hog::GameTime>);
static_assert(!std::is_convertible_v<std::uint64_t, hog::Tick>);
static_assert(!std::is_convertible_v<hog::Tick, std::uint64_t>);
static_assert(!std::is_convertible_v<hog::TickDelta, hog::Tick>);
static_assert(!std::is_convertible_v<std::int64_t, hog::TickDelta>);

TEST_CASE("ticks and deltas follow point and vector arithmetic", "[core][time]") {
    constexpr hog::Tick start{100};
    constexpr hog::TickDelta step{30};

    STATIC_CHECK((start + step).value() == 130);
    STATIC_CHECK((start - step).value() == 70);
    STATIC_CHECK((start + step) - start == step);
    STATIC_CHECK(start - (start + step) == -step);
    STATIC_CHECK(step + step == hog::TickDelta{60});
    STATIC_CHECK(start < start + step);

    hog::Tick tick{};
    ++tick;
    CHECK(tick == hog::Tick{1});
}

TEST_CASE("a schedule maps ticks to game time exactly", "[core][time]") {
    const hog::GameTime epoch{1'000'000us};
    const hog::TickSchedule schedule{epoch, 50ms};

    CHECK(schedule.time_at(hog::Tick{0}) == epoch);
    CHECK(schedule.time_at(hog::Tick{20}) == epoch + 1s);
    CHECK(schedule.tick_at(epoch + 1s) == hog::Tick{20});
    CHECK(schedule.tick_at(epoch + 1s + 49ms) == hog::Tick{20});
    CHECK(schedule.tick_at(epoch + 1s + 50ms) == hog::Tick{21});
}

TEST_CASE("time_at and tick_at round-trip over a long run", "[core][time]") {
    // 30 Hz does not divide a second evenly in microseconds; the schedule stays exact anyway.
    const hog::TickSchedule schedule{hog::GameTime{}, 33'333us};
    for (std::uint64_t t = 0; t < 10'000'000; t += 9'973) {
        const hog::Tick tick{t};
        REQUIRE(schedule.tick_at(schedule.time_at(tick)) == tick);
    }
}

TEST_CASE("game time counts from the Unix epoch", "[core][time]") {
    using namespace std::chrono;
    const sys_days day = 2026y / October / 11;
    CHECK(hog::to_game_time(hog::WallClock::time_point{day + 90min + 7us}) ==
          hog::GameTime{(day + 90min + 7us).time_since_epoch()});
    CHECK(hog::to_game_time(hog::WallClock::time_point{}) == hog::GameTime{});
}

TEST_CASE("schedules reject bad input", "[core][time]") {
    const hog::testing::AssertionGuard guard;
    CHECK_THROWS_AS(hog::TickSchedule(hog::GameTime{}, 0us), hog::testing::AssertionFailed);

    const hog::TickSchedule schedule{hog::GameTime{10s}, 50ms};
    CHECK_THROWS_AS(schedule.tick_at(hog::GameTime{9s}), hog::testing::AssertionFailed);
}
