#include <hog/replication/timeline.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace hog;
using namespace hog::replication;
using namespace std::chrono_literals;

namespace {

constexpr MonotonicClock::time_point start{std::chrono::hours{1}};

// Matt, `x` metres along.
EntityStates matt_at(double x) {
    return {{.id = protocol::NetId{1},
             .at = protocol::Millimetres{static_cast<std::int64_t>(x * 1'000.0), 0, 0},
             .name = "Matt"}};
}

// Where the timeline shows Matt along, or -1 when it doesn't.
double matt(const Timeline& timeline, MonotonicClock::time_point now) {
    const std::vector<Shown> shown = timeline.at(now);
    return shown.size() == 1 ? shown.front().at.metres().x : -1.0;
}

} // namespace

TEST_CASE("a timeline shows nothing before a snapshot, then the first as it is",
          "[replication][timeline]") {
    Timeline timeline;
    CHECK(timeline.at(start).empty());
    timeline.add(1, start, matt_at(5.0));
    CHECK(matt(timeline, start) == Catch::Approx(5.0));
    CHECK(timeline.at(start).front().name == "Matt");
    // With nothing to go on, it carries nothing on.
    CHECK(matt(timeline, start + 1s) == Catch::Approx(5.0));
}

TEST_CASE("a timeline shows others between the snapshots around a moment a little past",
          "[replication][timeline]") {
    Timeline timeline;
    timeline.add(1, start, matt_at(0.0));
    timeline.add(2, start + 50ms, matt_at(1.0));
    // Tick 1 is shown 100 ms after it came.
    CHECK(matt(timeline, start + 100ms) == Catch::Approx(0.0));
    CHECK(matt(timeline, start + 125ms) == Catch::Approx(0.5));
    CHECK(matt(timeline, start + 150ms) == Catch::Approx(1.0));
}

TEST_CASE("a timeline moves others evenly whenever their snapshots come",
          "[replication][timeline]") {
    // Matt goes a metre a tick; 3 comes late, with 4, and 6 is lost.
    struct Coming {
        std::uint32_t tick;
        std::chrono::milliseconds at;
    };
    const std::vector<Coming> comings{{.tick = 1, .at = 0ms},   {.tick = 2, .at = 50ms},
                                      {.tick = 3, .at = 160ms}, {.tick = 4, .at = 160ms},
                                      {.tick = 5, .at = 200ms}, {.tick = 7, .at = 300ms},
                                      {.tick = 8, .at = 350ms}};
    Timeline timeline;
    std::size_t next = 0;
    double last = 0.0;
    for (auto now = 100ms; now <= 400ms; now += 10ms) {
        while (next < comings.size() && comings[next].at <= now) {
            timeline.add(comings[next].tick, start + comings[next].at,
                         matt_at(static_cast<double>(comings[next].tick)));
            ++next;
        }
        const double shown = matt(timeline, start + now);
        if (now > 100ms) {
            CHECK(shown - last == Catch::Approx(0.2));
        }
        last = shown;
    }
}

TEST_CASE("a timeline carries others on a little when a snapshot is late, then holds them",
          "[replication][timeline]") {
    Timeline timeline;
    timeline.add(1, start, matt_at(0.0));
    timeline.add(2, start + 50ms, matt_at(1.0));
    CHECK(matt(timeline, start + 200ms) == Catch::Approx(2.0));
    CHECK(matt(timeline, start + 250ms) == Catch::Approx(3.0));
    CHECK(matt(timeline, start + 2s) == Catch::Approx(3.0));
    // Once it comes, they're shown where it has them.
    timeline.add(3, start + 2s, matt_at(2.0));
    CHECK(matt(timeline, start + 2s + 100ms) == Catch::Approx(2.0));
}

TEST_CASE("a timeline shows others come and go with the snapshot they do it in",
          "[replication][timeline]") {
    Timeline timeline;
    timeline.add(1, start, {});
    timeline.add(2, start + 50ms, matt_at(4.0));
    timeline.add(3, start + 100ms, {});
    CHECK(timeline.at(start + 140ms).empty());
    CHECK(matt(timeline, start + 160ms) == Catch::Approx(4.0));
    CHECK(timeline.at(start + 200ms).empty());
}

TEST_CASE("a timeline ignores a snapshot no newer than the last", "[replication][timeline]") {
    Timeline timeline;
    timeline.add(2, start, matt_at(1.0));
    timeline.add(2, start, matt_at(9.0));
    timeline.add(1, start, matt_at(9.0));
    CHECK(matt(timeline, start + 1s) == Catch::Approx(1.0));
}
