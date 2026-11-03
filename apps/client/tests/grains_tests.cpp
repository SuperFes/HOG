#include <hog/client/grains.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <chrono>

namespace {

using hog::MonotonicClock;
using hog::client::GrainField;
using namespace hog::protocol;
using namespace std::chrono_literals;

constexpr std::int64_t radius_mm = 6'000'000'000;

Grains told(std::uint32_t tick, std::int16_t fall_cm = 0, std::uint32_t amount = 32'768) {
    return Grains{.tick = tick,
                  .flying = {{.id = NetId{1},
                              .at = Millimetres{0, radius_mm + 10'000, 0},
                              .velocity = {0, fall_cm, 0},
                              .material = 4,
                              .amount = amount}}};
}

} // namespace

TEST_CASE("grain told as flying is carried on between messages, not held where it was",
          "[client][grains]") {
    GrainField field;
    const auto start = MonotonicClock::now();
    field.receive(told(1, -500), start);
    const auto first = field.capsules(start, 1.0, 9.81);
    const auto later = field.capsules(start + 100ms, 1.0, 9.81);
    REQUIRE(first.size() == 1);
    REQUIRE(later.size() == 1);
    // 5 m/s down for a tenth of a second, and gravity's half-square on top.
    CHECK(glm::length(first[0].feet.metres()) - glm::length(later[0].feet.metres()) ==
          Catch::Approx(0.5 + (0.5 * 9.81 * 0.01)).margin(1e-6));
}

TEST_CASE("grain is carried on only a short while with no word of it", "[client][grains]") {
    GrainField field;
    const auto start = MonotonicClock::now();
    field.receive(told(1, -500), start);
    const auto soon = field.capsules(start + 400ms, 1.0, 9.81);
    const auto long_after = field.capsules(start + 5s, 1.0, 9.81);
    CHECK(glm::length(soon[0].feet.metres()) == Catch::Approx(glm::length(long_after[0].feet.metres())));
}

TEST_CASE("word older than what is held is ignored, and an empty list clears the grain",
          "[client][grains]") {
    GrainField field;
    const auto start = MonotonicClock::now();
    field.receive(told(5), start);
    field.receive(Grains{.tick = 4, .flying = {}}, start);
    CHECK(field.size() == 1);
    field.receive(Grains{.tick = 6, .flying = {}}, start);
    CHECK(field.size() == 0);
}

TEST_CASE("a bigger lump is drawn bigger, and the tiniest still shows", "[client][grains]") {
    GrainField small;
    GrainField large;
    const auto start = MonotonicClock::now();
    small.receive(told(1, 0, 1), start);
    large.receive(told(1, 0, 32'768 * 4), start);
    CHECK(small.capsules(start, 1.0, 9.81)[0].radius > 0.0);
    CHECK(large.capsules(start, 1.0, 9.81)[0].radius > small.capsules(start, 1.0, 9.81)[0].radius);
}
