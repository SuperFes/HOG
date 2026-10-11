#include <hog/core/bytes.hpp>
#include <hog/core/hash.hpp>
#include <hog/save/snapshot.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

using hog::ByteWriter;
using hog::ErrorCode;
using hog::save::Section;
using hog::save::section_id;
using hog::save::Snapshot;

namespace {

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> bytes;
    for (const char c : text) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    return bytes;
}

Snapshot sample() {
    Snapshot snapshot{
        .world = 0x1122334455667788,
        .time = hog::GameTime{hog::GameDuration{-1'234'567}},
        .tick = hog::Tick{42},
        .sections = {},
    };
    snapshot.add(Section{.id = section_id("ENTS"), .version = 3, .bytes = bytes_of("entities")});
    snapshot.add(Section{.id = section_id("NONE"), .version = 1, .bytes = {}});
    snapshot.add(Section{.id = 0xFFFFFFFF, .version = 0xFFFF, .bytes = bytes_of("?")});
    return snapshot;
}

// A version 1 container with the given section count and raw section bytes, checksummed.
std::vector<std::byte> container(std::uint32_t count, std::span<const std::byte> sections,
                                 std::uint16_t version = 1) {
    ByteWriter writer;
    writer.write_bytes(bytes_of({"HOGSNAP\0", 8}));
    writer.write(version);
    writer.write(std::uint64_t{1});
    writer.write(std::uint64_t{2});
    writer.write(std::uint64_t{3});
    writer.write(count);
    writer.write_bytes(sections);
    writer.write(hog::hash_bytes(writer.bytes()));
    return std::move(writer).take();
}

std::vector<std::byte> raw_section(std::uint32_t id, std::uint64_t size, std::string_view bytes) {
    ByteWriter writer;
    writer.write(id);
    writer.write(std::uint16_t{1});
    writer.write(size);
    writer.write_bytes(bytes_of(bytes));
    return std::move(writer).take();
}

ErrorCode decode_error(std::span<const std::byte> bytes) {
    const auto decoded = hog::save::decode(bytes);
    REQUIRE_FALSE(decoded);
    return decoded.error().code;
}

} // namespace

TEST_CASE("section ids read as little-endian text", "[save][snapshot]") {
    STATIC_CHECK(section_id("ENTS") == 0x53544E45);
    STATIC_CHECK(section_id("ENTS") != section_id("STNE"));
}

TEST_CASE("snapshots survive a round trip", "[save][snapshot]") {
    const Snapshot snapshot = sample();
    const auto bytes = hog::save::encode(snapshot);
    const auto decoded = hog::save::decode(bytes);
    REQUIRE(decoded);
    CHECK(*decoded == snapshot);
    CHECK(hog::save::encode(*decoded) == bytes);

    const Snapshot empty;
    CHECK(hog::save::decode(hog::save::encode(empty)) == empty);
}

TEST_CASE("the container layout is fixed", "[save][snapshot]") {
    Snapshot snapshot{.world = 7, .time = hog::GameTime{}, .tick = hog::Tick{9}, .sections = {}};
    snapshot.add(Section{.id = section_id("ABCD"), .version = 2, .bytes = bytes_of("xy")});

    ByteWriter expected;
    expected.write_bytes(bytes_of({"HOGSNAP\0", 8}));
    expected.write(std::uint16_t{1});
    expected.write(std::uint64_t{7});
    expected.write(std::uint64_t{0});
    expected.write(std::uint64_t{9});
    expected.write(std::uint32_t{1});
    expected.write_bytes(bytes_of("ABCD"));
    expected.write(std::uint16_t{2});
    expected.write(std::uint64_t{2});
    expected.write_bytes(bytes_of("xy"));
    expected.write(hog::hash_bytes(expected.bytes()));
    const auto layout = std::move(expected).take();
    CHECK(hog::save::encode(snapshot) == layout);
}

TEST_CASE("sections are found by id", "[save][snapshot]") {
    const Snapshot snapshot = sample();
    const Section* entities = snapshot.find(section_id("ENTS"));
    REQUIRE(entities != nullptr);
    CHECK(entities->version == 3);
    CHECK(snapshot.find(section_id("WTHR")) == nullptr);

    const hog::testing::AssertionGuard guard;
    Snapshot copy = sample();
    CHECK_THROWS_AS(copy.add(Section{.id = section_id("ENTS"), .version = 1, .bytes = {}}),
                    hog::testing::AssertionFailed);
}

TEST_CASE("damaged snapshots are rejected", "[save][snapshot]") {
    const auto good = hog::save::encode(sample());

    CHECK(decode_error({}) == ErrorCode::Parse);
    CHECK(decode_error(bytes_of("HOGSNAP")) == ErrorCode::Parse);
    CHECK(decode_error(bytes_of({"HOGSNAP\0", 8})) == ErrorCode::Parse);

    auto wrong_magic = good;
    wrong_magic[0] = std::byte{'h'};
    CHECK(decode_error(wrong_magic) == ErrorCode::Parse);

    // Every flipped bit past the magic is caught, whether in a header, a section or the checksum.
    for (std::size_t i = 10; i < good.size(); ++i) {
        auto flipped = good;
        flipped[i] ^= std::byte{0x10};
        INFO("byte " << i);
        CHECK(decode_error(flipped) == ErrorCode::Parse);
    }

    for (const std::size_t size : {good.size() - 1, good.size() / 2, std::size_t{12}}) {
        CHECK(decode_error(std::span{good}.first(size)) == ErrorCode::Parse);
    }
}

TEST_CASE("well-formed but invalid containers are rejected", "[save][snapshot]") {
    CHECK(hog::save::decode(container(0, {})));

    CHECK(decode_error(container(0, {}, hog::save::format_version + 1)) == ErrorCode::Unsupported);

    const auto once = raw_section(section_id("ENTS"), 1, "a");
    auto twice = once;
    twice.insert(twice.end(), once.begin(), once.end());
    CHECK(decode_error(container(2, twice)) == ErrorCode::Parse);

    CHECK(decode_error(container(1, raw_section(1, 5, "abc"))) == ErrorCode::Parse);
    CHECK(decode_error(container(1, raw_section(1, ~0ULL, "abc"))) == ErrorCode::Parse);
    CHECK(decode_error(container(2, raw_section(1, 3, "abc"))) == ErrorCode::Parse);
    CHECK(decode_error(container(1, raw_section(1, 2, "abc"))) == ErrorCode::Parse);
}
