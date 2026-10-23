#include <hog/protocol/messages.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using hog::ErrorCode;
using namespace hog::protocol;

namespace {

std::vector<std::byte> bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (const int value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

// Somewhere on an Earth-sized planet's surface.
constexpr Millimetres far_out{3'000'000'000, -4'500'000'123, 2'000'000'000};

Snapshot whole() {
    return {.tick = 7,
            .baseline = {},
            .entered = {{.id = NetId{1}, .at = far_out, .name = "Matt"},
                        {.id = NetId{900}, .at = -far_out, .name = ""}},
            .changed = {},
            .left = {}};
}

Snapshot delta() {
    return {.tick = 120,
            .baseline = 117,
            .entered = {{.id = NetId{4}, .at = far_out, .name = std::string(most_name, 'n')}},
            .changed = {{.id = NetId{1}, .moved_by = Millimetres{300, -120, 0}},
                        {.id = NetId{2}, .moved_by = {}},
                        {.id = NetId{70'000}, .moved_by = Millimetres{0, 0, -5}}},
            .left = {NetId{3}, NetId{0xFFFF'FFFF}}};
}

// Each cell different, with values a byte holds exactly.
Weather weather(std::uint8_t level) {
    Weather out{.level = level, .fade = std::chrono::minutes{10}, .cells = {}};
    for (std::size_t index = 0; index < weather_cells(level); ++index) {
        const auto step = static_cast<float>(index % 4);
        out.cells.push_back({.cloud = step * step / 16.0F,
                             .falling = step * step,
                             .snow = step == 0.0F ? 1.0F : 0.0F,
                             .drift = {-step, static_cast<float>(index) - 12.0F},
                             .humidity = step / 255.0F,
                             .fog_top = step * 32.0F,
                             .base = step * 640.0F,
                             .top = (step + 1.0F) * 1'280.0F});
    }
    return out;
}

// Each cell different, with values a byte holds exactly.
Woodlands woodlands(std::uint8_t level) {
    Woodlands out{.level = level, .cells = {}};
    for (std::size_t index = 0; index < weather_cells(level); ++index) {
        const auto step = static_cast<float>(index % 5);
        out.cells.push_back({.living = (255.0F - step) / 255.0F,
                             .established = (255.0F - (2.0F * step)) / 255.0F,
                             .failing = step / 255.0F,
                             .dead = (3.0F * step) / 255.0F,
                             .charred = index % 2 == 0 ? 1.0F : 0.0F,
                             .scar = (4.0F * step) / 255.0F});
    }
    return out;
}

std::vector<Message> examples() {
    return {Hello{},
            Hello{.version = version, .token = bytes({1, 2, 3, 250})},
            Welcome{},
            Refused{.reason = Refusal::Version, .version = 7, .detail = "too new"},
            Refused{.reason = Refusal::Silent, .version = version, .detail = ""},
            Moved{.sequence = 0xFFFF'FFFF, .corrections = 3, .at = far_out},
            Moved{.sequence = 2,
                  .corrections = 0,
                  .at = far_out,
                  .stance = Stance::Flying,
                  .facing = {.heading = 0xFFFF, .pitch = -16'384}},
            Placed{.at = far_out, .stance = Stance::Walking, .facing = {.heading = 1, .pitch = 7}},
            Placed{.at = far_out, .stance = Stance::Flying, .facing = {}},
            Corrected{.count = 1, .at = far_out},
            Ack{.tick = 0xFFFF'FFF0},
            whole(),
            delta(),
            Snapshot{.tick = 1, .baseline = 0, .entered = {}, .changed = {}, .left = {}},
            weather(0),
            weather(1),
            Said{.from = "", .text = "hello?"},
            Said{.from = std::string(most_name, 'n'), .text = std::string(most_said, 's')},
            Hurry{.scale = 1},
            Hurry{.scale = 16'384},
            Clock{.scale = 1},
            Clock{.scale = 86'400},
            woodlands(0),
            woodlands(2),
            Woodlands{.level = 0, .cells = std::vector<CellWoodland>(6)}};
}

// A snapshot at tick 10 with no baseline and nothing entering, then `rest`.
std::vector<std::byte> snapshot_with(std::initializer_list<int> rest) {
    auto out = bytes({6, 0, 10, 0, 0, 0, 0, 0});
    for (const int value : rest) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

} // namespace

TEST_CASE("protocol messages read back as written", "[protocol]") {
    for (const Message& message : examples()) {
        const auto read = decode(encode(message));
        REQUIRE(read);
        CHECK(*read == message);
    }
}

TEST_CASE("a hello starts with its kind and version, as in every version", "[protocol]") {
    const auto written = encode(Hello{.version = version, .token = {}});
    REQUIRE(written.size() >= 6);
    CHECK(written[0] == std::byte{1});
    CHECK(written[1] == std::byte{0});
    CHECK(written[2] == static_cast<std::byte>(version & 0xFFU));
}

TEST_CASE("a hello of another version is read for its version alone", "[protocol]") {
    // Whatever a later version puts after the version, it isn't read.
    const auto read = decode(bytes({1, 0, 99, 0, 0, 0, 7, 7, 7}));
    REQUIRE(read);
    const auto* hello = std::get_if<Hello>(&*read);
    REQUIRE(hello != nullptr);
    CHECK(hello->version == 99);
    CHECK(hello->token.empty());
}

TEST_CASE("messages cut short or with bytes left over are refused", "[protocol]") {
    for (const Message& message : examples()) {
        const auto whole = encode(message);
        for (std::size_t length = 0; length < whole.size(); ++length) {
            CAPTURE(length);
            const auto read = decode(std::span{whole}.first(length));
            REQUIRE_FALSE(read);
            CHECK(read.error().code == ErrorCode::Parse);
        }
        auto longer = whole;
        longer.push_back(std::byte{0});
        CHECK_FALSE(decode(longer));
    }
}

TEST_CASE("a hurry or a clock to no time at all is refused", "[protocol]") {
    CHECK(decode(bytes({10, 0, 0, 0, 0, 0})).error().code == ErrorCode::OutOfRange);
    CHECK(decode(bytes({11, 0, 0, 0, 0, 0})).error().code == ErrorCode::OutOfRange);
}

TEST_CASE("unknown kinds and refusals, and tokens too long, are refused", "[protocol]") {
    CHECK(decode(bytes({14, 0})).error().code == ErrorCode::Unsupported);
    // A placing at the centre in a stance after flying.
    CHECK(decode(bytes({13, 0, 0, 0, 0, 2, 0, 0, 0, 0})).error().code == ErrorCode::Unsupported);
    CHECK(decode(bytes({3, 0, 1, 0, 0, 0, 200, 0, 0})).error().code == ErrorCode::Parse);
    // A hello of this version with no token, but saying it has one too long.
    std::vector<std::byte> long_token = encode(Hello{});
    long_token.resize(6);
    const auto length = static_cast<std::uint16_t>(most_token + 1);
    long_token.push_back(static_cast<std::byte>(length & 0xFFU));
    long_token.push_back(static_cast<std::byte>(length >> 8U));
    long_token.resize(long_token.size() + length);
    CHECK(decode(long_token).error().code == ErrorCode::OutOfRange);
}

TEST_CASE("a long refusal is cut to what can be sent", "[protocol]") {
    const Refused refused{
        .reason = Refusal::Token, .version = version, .detail = std::string(most_detail * 2, 'x')};
    const auto read = decode(encode(refused));
    REQUIRE(read);
    CHECK(std::get<Refused>(*read).detail == std::string(most_detail, 'x'));
}

TEST_CASE("a refusal says which side to update", "[protocol]") {
    CHECK(explain({.reason = Refusal::Version, .version = 4, .detail = ""}, 3) ==
          "the server speaks protocol 4 and this client 3: update the client");
    CHECK(explain({.reason = Refusal::Version, .version = 2, .detail = ""}, 3) ==
          "the server speaks protocol 2 and this client 3: the server is older than this client");
    CHECK(explain({.reason = Refusal::Token, .version = 3, .detail = "expired"}, 3) ==
          "the server didn't accept the login: expired");
    CHECK(explain({.reason = Refusal::Silent, .version = 3, .detail = "no hello"}, 3) ==
          "the server closed the connection: no hello");
}

TEST_CASE("positions are sent to the millimetre", "[protocol]") {
    const hog::WorldPosition at{hog::DVec3{6'371'000.0004, -0.0006, 1.2345}};
    CHECK(to_millimetres(at) == Millimetres{6'371'000'000, -1, 1'235});
    CHECK(to_millimetres(to_world(far_out)) == far_out);
}

TEST_CASE("a small move takes few bytes", "[protocol]") {
    const Snapshot moved{.tick = 1'000,
                         .baseline = 997,
                         .entered = {},
                         .changed = {{.id = NetId{12}, .moved_by = Millimetres{900, -900, 600}}},
                         .left = {}};
    // Kind, tick, baseline, three counts, then the id, what changed and three axes of two bytes.
    CHECK(encode(moved).size() == 2 + 4 + 1 + 3 + 1 + 1 + 6);
}

TEST_CASE("a snapshot's ids must go up", "[protocol]") {
    // Two changed entities, the second 0 past the first.
    const auto twice = decode(snapshot_with({2, 3, 0, 0, 0, 0}));
    REQUIRE_FALSE(twice);
    CHECK(twice.error().code == ErrorCode::Parse);
    // One leaving, past the largest id.
    CHECK(decode(snapshot_with({0, 1, 0xFF, 0xFF, 0xFF, 0xFF, 0x10})).error().code ==
          ErrorCode::Parse);
}

TEST_CASE("snapshots that can't be right are refused", "[protocol]") {
    // A baseline 11 ticks before tick 10.
    CHECK(decode(bytes({6, 0, 10, 0, 0, 0, 11, 0, 0, 0})).error().code == ErrorCode::Parse);
    // A change of a kind this version doesn't know.
    CHECK(decode(snapshot_with({1, 1, 2, 0})).error().code == ErrorCode::Parse);
    // More entities changed than there are bytes for.
    CHECK(decode(snapshot_with({100, 1, 0})).error().code == ErrorCode::Parse);
    // A name too long.
    auto named = bytes({6, 0, 10, 0, 0, 0, 0, 1, 1, 0, 0, 0, static_cast<int>(most_name + 1)});
    named.resize(named.size() + most_name + 1 + 2, std::byte{'n'});
    CHECK(decode(named).error().code == ErrorCode::OutOfRange);
}

TEST_CASE("weather is sent near enough to draw", "[protocol]") {
    const auto root_within = [](float sent, float given, float steps) {
        return std::abs(std::sqrt(sent) - std::sqrt(given)) <= 0.5F / steps;
    };
    for (const float amount : {0.003F, 0.05F, 0.7F, 9.0F, 120.0F}) {
        CAPTURE(amount);
        const CellWeather sent = as_sent({.cloud = amount / 10.0F, .falling = amount});
        CHECK(root_within(sent.cloud, amount / 10.0F, 64.0F));
        CHECK(root_within(sent.falling, amount, 16.0F));
    }
    CHECK_THAT(as_sent({.snow = 0.31F}).snow, Catch::Matchers::WithinAbs(0.31, 0.5 / 255.0));
    CHECK(as_sent({.drift = {12.4F, -3.6F}}).drift == hog::Vec2{12.0F, -4.0F});
    CHECK(as_sent({.wind = {12.4F, -3.6F}}).wind == hog::Vec2{12.5F, -3.5F});
    CHECK_THAT(as_sent({.humidity = 0.83F}).humidity,
               Catch::Matchers::WithinAbs(0.83, 0.5 / 255.0));
    CHECK_THAT(as_sent({.fog_top = 250.0F}).fog_top, Catch::Matchers::WithinAbs(250.0, 16.0));
    const CellWeather heights = as_sent({.base = 1'210.0F, .top = 9'000.0F});
    CHECK_THAT(heights.base, Catch::Matchers::WithinAbs(1'210.0, 32.0));
    CHECK_THAT(heights.top, Catch::Matchers::WithinAbs(9'000.0, 32.0));
}

TEST_CASE("the sea's waves and current are sent near enough to draw", "[protocol]") {
    const CellWeather sent =
        as_sent({.wind_sea = {.height = 2.3F, .peak = 0.9F, .heading = {0.6F, 0.8F}},
                 .swells = {CellWaves{.height = 0.4F, .peak = 0.45F, .heading = {-1.0F, 0.0F}},
                            CellWaves{.height = 12.0F, .peak = 3.0F, .heading = {0.0F, -1.0F}}},
                 .current = {0.71F, -1.2F}});
    const auto period = [](float peak) {
        return 2.0 * std::numbers::pi / static_cast<double>(peak);
    };
    const std::array<CellWaves, 3> given{
        CellWaves{.height = 2.3F, .peak = 0.9F, .heading = {0.6F, 0.8F}},
        CellWaves{.height = 0.4F, .peak = 0.45F, .heading = {-1.0F, 0.0F}},
        CellWaves{.height = 12.0F, .peak = 3.0F, .heading = {0.0F, -1.0F}}};
    const std::array<CellWaves, 3> got{sent.wind_sea, sent.swells[0], sent.swells[1]};
    for (std::size_t i = 0; i < given.size(); ++i) {
        CAPTURE(i);
        CHECK(std::abs(std::sqrt(got.at(i).height) - std::sqrt(given.at(i).height)) <=
              0.5F / 56.0F);
        CHECK_THAT(period(got.at(i).peak),
                   Catch::Matchers::WithinAbs(period(given.at(i).peak), 0.05));
        CHECK(glm::dot(got.at(i).heading, given.at(i).heading) >
              std::cos(std::numbers::pi_v<float> / 256.0F) - 1e-6F);
        CHECK_THAT(glm::length(got.at(i).heading), Catch::Matchers::WithinAbs(1.0, 1e-6));
    }
    CHECK_THAT(sent.current.x, Catch::Matchers::WithinAbs(0.71, 0.5 / 32.0));
    CHECK_THAT(sent.current.y, Catch::Matchers::WithinAbs(-1.2, 0.5 / 32.0));
    // Waves too short to tell a period for still have one.
    CHECK(as_sent({.wind_sea = {.height = 0.01F, .peak = 200.0F}}).wind_sea.peak > 0.0F);
}

TEST_CASE("weather past what a byte holds is clamped", "[protocol]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const CellWeather over = as_sent({.cloud = 100.0F,
                                      .falling = 1'000.0F,
                                      .snow = 2.0F,
                                      .drift = {500.0F, -500.0F},
                                      .wind = {500.0F, -500.0F},
                                      .humidity = 3.0F,
                                      .fog_top = 20'000.0F,
                                      .base = 20'000.0F,
                                      .top = 20'000.0F,
                                      .wind_sea = {.height = 40.0F, .peak = 0.01F},
                                      .swells = {},
                                      .current = {9.0F, -9.0F}});
    CHECK(over.cloud < 16.0F);
    CHECK(over.cloud > 15.8F);
    CHECK(over.falling < 255.0F);
    CHECK(over.falling > 253.0F);
    CHECK(over.snow == 1.0F);
    CHECK(over.drift == hog::Vec2{127.0F, -127.0F});
    CHECK(over.wind == hog::Vec2{63.5F, -63.5F});
    CHECK(over.humidity == 1.0F);
    CHECK(over.fog_top == 8'160.0F);
    CHECK(over.base == 16'320.0F);
    CHECK(over.top == 16'320.0F);
    CHECK(over.wind_sea.height < 20.8F);
    CHECK(over.wind_sea.height > 20.6F);
    CHECK_THAT(over.wind_sea.peak, Catch::Matchers::WithinAbs(2.0 * std::numbers::pi / 25.5, 1e-6));
    CHECK(over.current == hog::Vec2{127.0F / 32.0F, -127.0F / 32.0F});
    CHECK(as_sent({.cloud = -1.0F,
                   .falling = nan,
                   .snow = -0.5F,
                   .drift = {nan, nan},
                   .wind = {nan, nan},
                   .humidity = nan,
                   .fog_top = -5.0F,
                   .base = nan,
                   .top = -1.0F,
                   .wind_sea = {.height = nan, .peak = nan, .heading = {nan, nan}},
                   .swells = {CellWaves{.height = -1.0F, .peak = -1.0F}, CellWaves{}},
                   .current = {nan, nan}}) == CellWeather{});
}

TEST_CASE("the planet's weather takes twenty-two bytes a cell", "[protocol]") {
    CHECK(weather_cells(most_weather_level) == 6'144);
    CHECK(encode(weather(most_weather_level)).size() == 2 + 1 + 4 + (22 * 6'144));
}

TEST_CASE("weather too fine is refused", "[protocol]") {
    auto finer = encode(weather(1));
    finer[2] = std::byte{most_weather_level + 1};
    CHECK(decode(finer).error().code == hog::ErrorCode::OutOfRange);
    // Level 1's cells, said to be level 0's.
    auto coarser = encode(weather(1));
    coarser[2] = std::byte{0};
    CHECK(decode(coarser).error().code == hog::ErrorCode::Parse);
}

TEST_CASE("woodlands are sent to a 255th of a share, clamped to a byte", "[protocol]") {
    const CellWoodland sent = as_sent({.living = 0.6F,
                                       .established = 0.123F,
                                       .failing = 0.05F,
                                       .dead = 0.33F,
                                       .charred = 0.9F,
                                       .scar = 0.71F});
    for (const auto& [given, back] : {std::pair{0.6F, sent.living},
                                      {0.123F, sent.established},
                                      {0.05F, sent.failing},
                                      {0.33F, sent.dead},
                                      {0.9F, sent.charred},
                                      {0.71F, sent.scar}}) {
        CHECK_THAT(back, Catch::Matchers::WithinAbs(static_cast<double>(given), 0.51 / 255.0));
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(as_sent({.living = 2.0F,
                   .established = nan,
                   .failing = -1.0F,
                   .dead = 0.0F,
                   .charred = 0.0F}) ==
          CellWoodland{.living = 1.0F, .established = 0.0F, .failing = 0.0F});
    // Untouched woods read back untouched.
    CHECK(as_sent(CellWoodland{}) == CellWoodland{});
    // Six bytes a cell over the planet.
    CHECK(encode(woodlands(most_weather_level)).size() == 2 + 1 + (6 * 6'144));
    auto finer = encode(woodlands(1));
    finer[2] = std::byte{most_weather_level + 1};
    CHECK(decode(finer).error().code == hog::ErrorCode::OutOfRange);
    auto coarser = encode(woodlands(1));
    coarser[2] = std::byte{0};
    CHECK(decode(coarser).error().code == hog::ErrorCode::Parse);
}

TEST_CASE("chat too long is cut on sending, refused coming in", "[protocol]") {
    const auto read = decode(encode(
        Said{.from = std::string(most_name * 2, 'n'), .text = std::string(most_said * 2, 's')}));
    REQUIRE(read);
    CHECK(std::get<Said>(*read).from == std::string(most_name, 'n'));
    CHECK(std::get<Said>(*read).text == std::string(most_said, 's'));

    // A line said to be a byte longer than allowed.
    auto bigger = encode(Said{.from = "", .text = std::string(most_said, 's')});
    const auto length = static_cast<std::uint16_t>(most_said + 1);
    bigger[4] = static_cast<std::byte>(length & 0xFFU);
    bigger[5] = static_cast<std::byte>(length >> 8U);
    bigger.push_back(std::byte{'s'});
    CHECK(decode(bigger).error().code == ErrorCode::OutOfRange);
}

TEST_CASE("chat with control characters is refused", "[protocol]") {
    for (const char character : {'\n', '\t', '\r', '\x1b', '\x7f'}) {
        CAPTURE(static_cast<int>(character));
        const std::string text = std::string{"so"} + character + "on";
        CHECK(decode(encode(Said{.from = "Ada", .text = text})).error().code == ErrorCode::Parse);
        CHECK(decode(encode(Said{.from = text, .text = "fine"})).error().code == ErrorCode::Parse);
    }
}
