#include <hog/server/tick_loop.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using hog::GameTime;
using hog::MonotonicClock;
using hog::server::LoopConfig;
using hog::server::TickClock;
using hog::server::TickLoop;

namespace {

// Real time that moves only when the loop sleeps or a test pushes it on.
struct ManualTimer {
    MonotonicClock::time_point time;

    [[nodiscard]] MonotonicClock::time_point now() const { return time; }
    void sleep_until(MonotonicClock::time_point when) { time = std::max(time, when); }
};

struct Ran {
    std::uint64_t tick;
    MonotonicClock::duration real;
    GameTime game;
    hog::GameDuration step;
};

Ran ran_at(const ManualTimer& timer, const TickClock& clock) {
    return {.tick = clock.tick().value(),
            .real = timer.now().time_since_epoch(),
            .game = clock.now(),
            .step = clock.step()};
}

} // namespace

TEST_CASE("the loop runs one tick per period", "[server][loop]") {
    ManualTimer timer;
    TickLoop loop{timer, LoopConfig{}};
    std::vector<Ran> ran;
    loop.run([&](const TickClock& clock) {
        ran.push_back(ran_at(timer, clock));
        if (ran.size() == 10) {
            loop.stop();
        }
    });

    REQUIRE(ran.size() == 10);
    for (std::uint64_t i = 0; i < ran.size(); ++i) {
        CHECK(ran[i].tick == i);
        CHECK(ran[i].real == i * 50ms);
        CHECK(ran[i].game == GameTime{i * 50ms});
        CHECK(ran[i].step == 50ms);
    }
    CHECK(loop.clock().tick() == hog::Tick{10});
    CHECK(loop.stats().ticks == 10);
    CHECK(loop.stats().slipped == 0);
}

TEST_CASE("a slow tick is caught up in a burst and the rest dropped", "[server][loop]") {
    ManualTimer timer;
    TickLoop loop{timer, LoopConfig{.max_burst = 2}};
    std::vector<Ran> ran;
    loop.run([&](const TickClock& clock) {
        ran.push_back(ran_at(timer, clock));
        if (clock.tick() == hog::Tick{2}) {
            timer.time += 300ms;
        }
        if (ran.size() == 7) {
            loop.stop();
        }
    });

    // Tick 2 ends at 400 ms, when ticks up to the one due then are late: two run at once and
    // four are dropped, while game time still moves one step per tick.
    const std::vector<MonotonicClock::duration> expected{0ms,   50ms,  100ms, 400ms,
                                                         400ms, 450ms, 500ms};
    REQUIRE(ran.size() == expected.size());
    for (std::uint64_t i = 0; i < ran.size(); ++i) {
        CHECK(ran[i].real == expected[i]);
        CHECK(ran[i].game == GameTime{i * 50ms});
    }
    CHECK(loop.stats().slipped == 4);
    CHECK(loop.stats().longest_step == 300ms);
    CHECK(loop.stats().last_step == 0ms);
}

TEST_CASE("a rate change applies from the next tick", "[server][loop]") {
    ManualTimer timer;
    TickLoop loop{timer, LoopConfig{}};
    std::vector<Ran> ran;
    loop.run([&](const TickClock& clock) {
        ran.push_back(ran_at(timer, clock));
        if (clock.tick() == hog::Tick{1}) {
            loop.set_rate(10);
        }
        if (ran.size() == 4) {
            loop.stop();
        }
    });

    REQUIRE(ran.size() == 4);
    CHECK(ran[1].step == 50ms);
    CHECK(ran[2].real == 100ms);
    CHECK(ran[2].game == GameTime{100ms});
    CHECK(ran[2].step == 100ms);
    CHECK(ran[3].real == 200ms);
    CHECK(loop.clock().rate() == 10);
}

TEST_CASE("a resumed loop carries on from its tick and time", "[server][loop]") {
    ManualTimer timer;
    TickLoop loop{timer, LoopConfig{.start = GameTime{1h}, .first_tick = hog::Tick{500}}};
    std::vector<Ran> ran;
    loop.run([&](const TickClock& clock) {
        ran.push_back(ran_at(timer, clock));
        if (ran.size() == 2) {
            loop.stop();
        }
    });
    REQUIRE(ran.size() == 2);
    CHECK(ran[0].tick == 500);
    CHECK(ran[0].game == GameTime{1h});
    CHECK(ran[1].tick == 501);
    CHECK(ran[1].game == GameTime{1h + 50ms});
}

TEST_CASE("another thread can stop a loop paced to real time", "[server][loop]") {
    hog::server::SteadyTimer timer;
    TickLoop loop{timer, LoopConfig{.rate = hog::server::max_rate}};
    std::atomic<int> ticks{0};
    std::thread runner{[&] { loop.run([&](const TickClock&) { ++ticks; }); }};
    while (ticks.load() < 3) {
        std::this_thread::sleep_for(1ms);
    }
    loop.stop();
    runner.join();
    CHECK(loop.stats().ticks >= 3);
}

TEST_CASE("a speed change applies from the next tick", "[server][loop]") {
    ManualTimer timer;
    TickLoop loop{timer, LoopConfig{}};
    std::vector<Ran> ran;
    loop.run([&](const TickClock& clock) {
        ran.push_back(ran_at(timer, clock));
        if (clock.tick() == hog::Tick{1}) {
            loop.set_scale(600);
            CHECK(loop.scale() == 600);
        }
        if (ran.size() == 4) {
            loop.stop();
        }
    });

    REQUIRE(ran.size() == 4);
    CHECK(ran[1].step == 50ms);
    CHECK(ran[2].real == 100ms);
    CHECK(ran[2].game == GameTime{100ms});
    CHECK(ran[2].step == 30s);
    CHECK(ran[3].game == GameTime{100ms + 30s});
}

TEST_CASE("a paused loop keeps its pace but not its clock", "[server][loop]") {
    ManualTimer timer;
    TickLoop loop{timer, LoopConfig{}};
    std::vector<Ran> ran;
    std::vector<Ran> idled;
    loop.run(
        [&](const TickClock& clock) {
            ran.push_back(ran_at(timer, clock));
            if (ran.size() == 2) {
                loop.pause();
            }
            if (ran.size() == 3) {
                loop.stop();
            }
        },
        [&](const TickClock& clock) {
            CHECK(loop.paused());
            idled.push_back(ran_at(timer, clock));
            if (idled.size() == 3) {
                loop.resume();
            }
        });

    REQUIRE(idled.size() == 3);
    for (std::size_t i = 0; i < idled.size(); ++i) {
        CHECK(idled[i].tick == 2);
        CHECK(idled[i].game == GameTime{100ms});
        CHECK(idled[i].real == (i + 2) * 50ms);
    }
    REQUIRE(ran.size() == 3);
    CHECK(ran[2].tick == 2);
    CHECK(ran[2].game == GameTime{100ms});
    CHECK(ran[2].real == 250ms);
}

TEST_CASE("stepping a paused loop runs ticks back to back", "[server][loop]") {
    ManualTimer timer;
    TickLoop loop{timer, LoopConfig{}};
    loop.pause();
    std::vector<Ran> ran;
    std::vector<Ran> idled;
    loop.run(
        [&](const TickClock& clock) {
            ran.push_back(ran_at(timer, clock));
            timer.time += 10ms;
        },
        [&](const TickClock& clock) {
            idled.push_back(ran_at(timer, clock));
            if (idled.size() == 1) {
                loop.step_ticks(3);
            }
            if (idled.size() == 2) {
                loop.stop();
            }
        });

    REQUIRE(ran.size() == 3);
    for (std::uint64_t i = 0; i < ran.size(); ++i) {
        CHECK(ran[i].tick == i);
        CHECK(ran[i].real == i * 10ms);
    }
    REQUIRE(idled.size() == 2);
    CHECK(idled[1].tick == 3);
    // The pace starts over once the steps are done, rather than counting them as late.
    CHECK(idled[1].real == 30ms);
    CHECK(loop.stats().slipped == 0);
}

TEST_CASE("catching up runs sped up until the target, then at the live speed", "[server][loop]") {
    ManualTimer timer;
    TickLoop loop{timer, LoopConfig{}};
    std::vector<Ran> ran;
    loop.run([&](const TickClock& clock) {
        ran.push_back(ran_at(timer, clock));
        if (ran.size() == 1) {
            loop.catch_up_to(GameTime{1h}, 100);
        }
        if (clock.now() >= GameTime{1h}) {
            loop.stop();
        }
    });

    REQUIRE(ran.size() > 2);
    CHECK(std::ranges::all_of(ran.begin() + 1, ran.end() - 1,
                              [](const Ran& tick) { return tick.real == 0ms; }));
    CHECK(ran.back().game <= GameTime{1h});
    CHECK(ran.back().game > GameTime{1h} - 50ms);
    CHECK(loop.clock().scale() == hog::server::default_time_scale);
}
