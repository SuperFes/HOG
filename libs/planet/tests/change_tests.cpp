#include <hog/planet/change.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string_view>
#include <vector>

using hog::ErrorCode;
using hog::planet::Biome;
using hog::planet::Change;
using hog::planet::ChangeKind;
using hog::planet::ChangePayload;
using hog::planet::decode_payload;
using hog::planet::encode_payload;
using hog::planet::Excavate;
using hog::planet::Hollow;
using hog::planet::SetBiome;
using hog::planet::SetElevation;

namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    for (const unsigned value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

} // namespace

TEST_CASE("a change's kind follows its payload", "[planet][change]") {
    const Change change{.cell = hog::planet::CellId::from_face(3),
                        .time = {},
                        .author = {},
                        .layer = hog::planet::Layer::Player,
                        .payload = Excavate{.depth = hog::planet::fixed_one}};
    CHECK(change.kind() == ChangeKind::Excavate);
}

TEST_CASE("every layer and change kind has a distinct name", "[planet][change]") {
    std::set<std::string_view> layers;
    for (int layer = 0; layer < hog::planet::layer_count; ++layer) {
        layers.insert(to_string(static_cast<hog::planet::Layer>(layer)));
    }
    CHECK(layers.size() == hog::planet::layer_count);
    CHECK_FALSE(layers.contains("unknown"));
    std::set<std::string_view> kinds;
    for (int kind = 0; kind < hog::planet::change_kind_count; ++kind) {
        kinds.insert(to_string(static_cast<ChangeKind>(kind)));
    }
    CHECK(kinds.size() == hog::planet::change_kind_count);
    CHECK_FALSE(kinds.contains("unknown"));
}

TEST_CASE("payloads survive encoding", "[planet][change]") {
    constexpr auto lowest = std::numeric_limits<std::int64_t>::min();
    constexpr auto highest = std::numeric_limits<std::int64_t>::max();
    const ChangePayload payload = GENERATE_COPY(
        ChangePayload{SetElevation{.elevation = 0}}, ChangePayload{SetElevation{.elevation = -1}},
        ChangePayload{SetElevation{.elevation = lowest}},
        ChangePayload{SetElevation{.elevation = highest}},
        ChangePayload{Excavate{.depth = 3 * hog::planet::fixed_one / 2}},
        ChangePayload{SetBiome{.biome = Biome::Ocean}},
        ChangePayload{SetBiome{.biome = Biome::Alpine}},
        ChangePayload{Hollow{.top = 2 * hog::planet::fixed_one, .bottom = highest}});
    const auto encoded = encode_payload(payload);
    const auto decoded = decode_payload(static_cast<std::uint8_t>(payload.index()), encoded);
    REQUIRE(decoded);
    CHECK(*decoded == payload);
}

TEST_CASE("payload bytes are fixed little-endian", "[planet][change]") {
    // Stored changes must decode on every platform and every later version.
    CHECK(encode_payload(SetElevation{.elevation = 0x0102'0304'0506'0708}) ==
          bytes({0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01}));
    CHECK(encode_payload(SetElevation{.elevation = -2}) ==
          bytes({0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    CHECK(encode_payload(Excavate{.depth = 0x1'0000}) ==
          bytes({0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}));
    CHECK(encode_payload(SetBiome{.biome = Biome::Desert}) == bytes({8}));
    CHECK(encode_payload(Hollow{.top = 0x0201, .bottom = 0x0403}) ==
          bytes({0x01, 0x02, 0, 0, 0, 0, 0, 0, 0x03, 0x04, 0, 0, 0, 0, 0, 0}));
}

TEST_CASE("malformed payloads are rejected", "[planet][change]") {
    const auto rejects = [](std::uint8_t kind, const std::vector<std::byte>& payload) {
        const auto decoded = decode_payload(kind, payload);
        return !decoded && decoded.error().code == ErrorCode::Parse;
    };
    const auto elevation = static_cast<std::uint8_t>(ChangeKind::SetElevation);
    const auto biome = static_cast<std::uint8_t>(ChangeKind::SetBiome);
    CHECK(rejects(elevation, bytes({})));
    CHECK(rejects(elevation, bytes({1, 2, 3, 4, 5, 6, 7})));
    CHECK(rejects(elevation, bytes({1, 2, 3, 4, 5, 6, 7, 8, 9})));
    CHECK(rejects(biome, bytes({static_cast<unsigned>(hog::planet::biome_count)})));
    const auto hollow = static_cast<std::uint8_t>(ChangeKind::Hollow);
    CHECK(rejects(hollow, bytes({1, 2, 3, 4, 5, 6, 7, 8})));
    CHECK(rejects(hollow, bytes({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15})));
    CHECK(rejects(hog::planet::change_kind_count, bytes({})));
    CHECK(rejects(255, bytes({0})));
}
