#include <hog/core/bytes.hpp>
#include <hog/probe/messages.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

using namespace hog;
using namespace hog::probe;

namespace {

// The next message, which must not fail.
std::optional<Message> next_of(FrameReader& reader) {
    auto next = reader.next();
    REQUIRE(next);
    return *next;
}

Message round_trip(const Message& message) {
    FrameReader reader;
    reader.feed(encode(message));
    const std::optional<Message> next = next_of(reader);
    REQUIRE(next.has_value());
    CHECK(reader.pending() == 0);
    return next.value_or(Message{});
}

std::vector<std::byte> frame(std::uint8_t kind, std::span<const std::byte> payload) {
    ByteWriter out;
    out.write(static_cast<std::uint32_t>(payload.size()));
    out.write(kind);
    out.write_bytes(payload);
    return std::move(out).take();
}

Layers sample_layers() {
    return Layers{.time = GameTime{std::chrono::hours{24 * 365}},
                  .clock = Clock{.paused = true, .scale = 600},
                  .layers = {Layer{.name = "weather/temperature", .values = {-12.5F, 0.0F, 31.25F}},
                             Layer{.name = "forest/fire", .values = {}}}};
}

} // namespace

TEST_CASE("every message survives a round trip", "[probe][messages]") {
    CHECK(round_trip(Hello{}) == Message{Hello{.version = protocol_version}});
    CHECK(round_trip(Hello{.version = 9}) == Message{Hello{.version = 9}});

    const Welcome welcome{.version = protocol_version,
                          .world_id = 0xFEDCBA9876543210,
                          .level = 5,
                          .planet_radius = 115'000.5,
                          .layers = {LayerInfo{.name = "weather/temperature", .unit = "°C"},
                                     LayerInfo{.name = "wildlife/deer", .unit = "per km²"}},
                          .controllable = true,
                          .day_length = std::chrono::hours{4},
                          .axial_tilt = 23.44};
    CHECK(round_trip(welcome) == Message{welcome});

    CHECK(round_trip(Refused{.reason = "speaks version 9"}) ==
          Message{Refused{.reason = "speaks version 9"}});

    const Fetch fetch{.names = {"weather/temperature", "", "forest/biomass"}};
    CHECK(round_trip(fetch) == Message{fetch});
    CHECK(round_trip(Fetch{}) == Message{Fetch{}});

    CHECK(round_trip(sample_layers()) == Message{sample_layers()});

    for (const Action action :
         {Action::Pause, Action::Resume, Action::Step, Action::Speed, Action::RealTime}) {
        const Control control{.action = action, .amount = 0xFFFF'FFFF};
        CHECK(round_trip(control) == Message{control});
    }
    const Controlled controlled{.clock = Clock{.paused = false, .scale = 3'600},
                                .error = "time control is off"};
    CHECK(round_trip(controlled) == Message{controlled});

    const Survey survey{.west = -12.5,
                        .east = 30.0,
                        .north = 45.25,
                        .south = -2.0,
                        .width = 2,
                        .height = 1,
                        .underground = true,
                        .depth = 120.5F};
    CHECK(round_trip(survey) == Message{survey});
    const Ground ground{.survey = survey,
                        .elevation = {-3'500.0F, 812.25F},
                        .biome = {0, 7},
                        .water_table = {0.0F, 12.5F},
                        .river = {0, 40},
                        .rock = {6, 4},
                        .deposit = {0, 3}};
    CHECK(round_trip(ground) == Message{ground});
    Survey above = survey;
    above.underground = false;
    const Ground surface{.survey = above,
                         .elevation = ground.elevation,
                         .biome = ground.biome,
                         .water_table = ground.water_table,
                         .river = ground.river,
                         .rock = {},
                         .deposit = {}};
    CHECK(round_trip(surface) == Message{surface});

    const Inspect inspect{.latitude = -33.75, .longitude = 151.125};
    CHECK(round_trip(inspect) == Message{inspect});
    const Inspection inspection{
        .inspect = inspect,
        .time = GameTime{std::chrono::hours{30}},
        .cell = 4'097,
        .values = {12.5F, -0.25F, 1'013.0F},
        .column = Column{.elevation = 240.5F,
                         .biome = 7,
                         .water_table = 3.25F,
                         .water = 0.5F,
                         .continental = true,
                         .cover = 1,
                         .cover_thickness = 2.5F,
                         .sediment = 3,
                         .sediment_thickness = 410.0F,
                         .bedrock = 5,
                         .seams = {Seam{.resource = 0, .top = 12.0F, .thickness = 1.5F},
                                   Seam{.resource = 4, .top = 300.0F, .thickness = 0.25F}}}};
    CHECK(round_trip(inspection) == Message{inspection});
    CHECK(round_trip(Inspection{}) == Message{Inspection{}});
}

TEST_CASE("a frame is its payload length, kind and payload", "[probe][messages]") {
    const std::vector<std::byte> bytes = encode(Hello{.version = 0x0102});
    const std::vector<std::byte> expected{std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0},
                                          std::byte{1}, std::byte{2}, std::byte{1}};
    CHECK(bytes == expected);
}

TEST_CASE("messages arrive however the stream splits them", "[probe][messages]") {
    std::vector<std::byte> stream = encode(sample_layers());
    const std::vector<std::byte> second = encode(Fetch{.names = {"forest/fire"}});
    stream.insert(stream.end(), second.begin(), second.end());

    FrameReader reader;
    std::vector<Message> received;
    for (const std::byte byte : stream) {
        reader.feed(std::span{&byte, 1});
        auto next = reader.next();
        REQUIRE(next);
        if (next->has_value()) {
            received.push_back(**next);
        }
    }
    REQUIRE(received.size() == 2);
    CHECK(received[0] == Message{sample_layers()});
    CHECK(received[1] == Message{Fetch{.names = {"forest/fire"}}});
    CHECK(reader.pending() == 0);
}

TEST_CASE("several messages in one read come out in order", "[probe][messages]") {
    FrameReader reader;
    reader.feed(encode(Hello{}));
    reader.feed(encode(Refused{.reason = "full"}));
    CHECK(next_of(reader) == std::optional<Message>{Hello{}});
    CHECK(next_of(reader) == std::optional<Message>{Refused{.reason = "full"}});
    CHECK(next_of(reader) == std::optional<Message>{});
}

TEST_CASE("a partial frame waits for the rest", "[probe][messages]") {
    const std::vector<std::byte> bytes = encode(Refused{.reason = "nope"});
    FrameReader reader;
    reader.feed(std::span{bytes}.first(bytes.size() - 1));
    CHECK(next_of(reader) == std::optional<Message>{});
    CHECK(reader.pending() == bytes.size() - 1);
    reader.feed(std::span{bytes}.last(1));
    CHECK(next_of(reader) == std::optional<Message>{Refused{.reason = "nope"}});
}

TEST_CASE("broken streams fail and stay failed", "[probe][messages]") {
    FrameReader reader;
    SECTION("an unknown kind") {
        reader.feed(frame(99, {}));
    }
    SECTION("an oversized frame, before it arrives") {
        ByteWriter out;
        out.write(std::uint32_t{max_payload + 1});
        out.write(std::uint8_t{1});
        reader.feed(out.bytes());
    }
    SECTION("a payload with bytes left over") {
        ByteWriter payload;
        payload.write(std::uint16_t{1});
        payload.write(std::uint8_t{0});
        reader.feed(frame(1, payload.bytes()));
    }
    SECTION("a payload cut short") {
        ByteWriter payload;
        payload.write(std::uint16_t{10});
        payload.write_bytes(std::as_bytes(std::span{"abc", 3}));
        reader.feed(frame(3, payload.bytes()));
    }
    SECTION("a count larger than the payload could hold") {
        ByteWriter payload;
        payload.write(std::uint64_t{0});
        payload.write(std::uint8_t{0});
        payload.write(std::uint32_t{1});
        payload.write(std::uint32_t{0xFFFF'FFFF});
        reader.feed(frame(5, payload.bytes()));
    }
    SECTION("an unknown action") {
        ByteWriter payload;
        payload.write(std::uint8_t{9});
        payload.write(std::uint32_t{0});
        reader.feed(frame(6, payload.bytes()));
    }
    SECTION("a flag that is neither true nor false") {
        ByteWriter payload;
        payload.write(std::uint8_t{2});
        payload.write(std::uint32_t{1});
        payload.write(std::uint16_t{0});
        reader.feed(frame(7, payload.bytes()));
    }
    SECTION("ground without a value for every sample") {
        reader.feed(encode(Ground{.survey = Survey{.width = 2, .height = 2},
                                  .elevation = {1.0F, 2.0F, 3.0F},
                                  .biome = {0, 0, 0},
                                  .water_table = {0.0F, 0.0F, 0.0F},
                                  .river = {0, 0, 0},
                                  .rock = {},
                                  .deposit = {}}));
    }
    SECTION("more seams than could fit") {
        ByteWriter payload;
        payload.write(std::uint64_t{0}); // latitude
        payload.write(std::uint64_t{0}); // longitude
        payload.write(std::uint64_t{0}); // time
        payload.write(std::uint32_t{0}); // cell
        payload.write(std::uint32_t{0}); // no values
        payload.write(std::uint32_t{0}); // elevation
        for (int field = 0; field < 3; ++field) {
            payload.write(std::uint8_t{0}); // biome, continental, cover
        }
        payload.write(std::uint32_t{0});
        payload.write(std::uint8_t{0});
        payload.write(std::uint32_t{0});
        payload.write(std::uint8_t{0});
        payload.write(std::uint32_t{1'000});
        reader.feed(frame(11, payload.bytes()));
    }
    SECTION("ground without a river for every sample") {
        reader.feed(encode(Ground{.survey = Survey{.width = 1, .height = 1},
                                  .elevation = {1.0F},
                                  .biome = {0},
                                  .water_table = {0.0F},
                                  .river = {},
                                  .rock = {},
                                  .deposit = {}}));
    }
    SECTION("ground with rock it wasn't asked for") {
        reader.feed(encode(Ground{.survey = Survey{.width = 1, .height = 1},
                                  .elevation = {1.0F},
                                  .biome = {0},
                                  .water_table = {0.0F},
                                  .river = {0},
                                  .rock = {1},
                                  .deposit = {0}}));
    }
    const auto first = reader.next();
    REQUIRE_FALSE(first);
    CHECK(first.error().code == ErrorCode::Parse);
    reader.feed(encode(Hello{}));
    CHECK_FALSE(reader.next());
}

TEST_CASE("float values keep their exact bits", "[probe][messages]") {
    const std::vector<float> values{std::bit_cast<float>(0x7FC0'0001U), -0.0F,
                                    std::numeric_limits<float>::infinity(),
                                    std::numeric_limits<float>::denorm_min()};
    const Message received = round_trip(
        Layers{.time = {}, .clock = {}, .layers = {Layer{.name = "x", .values = values}}});
    const auto& layer = std::get<Layers>(received).layers.at(0);
    REQUIRE(layer.values.size() == values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        CHECK(std::bit_cast<std::uint32_t>(layer.values[index]) ==
              std::bit_cast<std::uint32_t>(values[index]));
    }
}
