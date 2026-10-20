#include <hog/core/bytes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <vector>

namespace {

std::vector<std::byte> bytes_of(std::initializer_list<int> values) {
    std::vector<std::byte> bytes;
    for (const int value : values) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

} // namespace

TEST_CASE("integers are written little-endian at their full width", "[core][bytes]") {
    hog::ByteWriter writer;
    writer.write(std::uint8_t{0xAB});
    writer.write(std::uint16_t{0x0102});
    writer.write(std::uint32_t{0x03040506});
    writer.write(std::uint64_t{0x0708090A0B0C0D0E});
    writer.write_bytes(bytes_of({0xFF, 0x00}));
    CHECK(writer.size() == 17);
    const auto written = std::move(writer).take();
    CHECK(written == bytes_of({0xAB, 0x02, 0x01, 0x06, 0x05, 0x04, 0x03, 0x0E, 0x0D, 0x0C, 0x0B,
                               0x0A, 0x09, 0x08, 0x07, 0xFF, 0x00}));
}

TEST_CASE("a reader returns what was written", "[core][bytes]") {
    hog::ByteWriter writer;
    writer.write(std::uint64_t{0xFEDCBA9876543210});
    writer.write(std::uint16_t{0xFFFF});
    writer.write(std::uint8_t{0x80});
    const auto bytes = std::move(writer).take();

    hog::ByteReader reader{bytes};
    CHECK(reader.read<std::uint64_t>() == 0xFEDCBA9876543210);
    CHECK(reader.remaining() == 3);
    CHECK_FALSE(reader.finish());
    CHECK(reader.read<std::uint16_t>() == 0xFFFF);
    CHECK(reader.read<std::uint8_t>() == 0x80);
    CHECK(reader.finish());
}

TEST_CASE("reading past the end fails without consuming", "[core][bytes]") {
    const auto bytes = bytes_of({1, 2, 3});
    hog::ByteReader reader{bytes};
    const auto wide = reader.read<std::uint32_t>();
    REQUIRE_FALSE(wide);
    CHECK(wide.error().code == hog::ErrorCode::Parse);
    CHECK(reader.remaining() == 3);

    const auto two = reader.read_bytes(2);
    REQUIRE(two);
    CHECK(std::vector<std::byte>(two->begin(), two->end()) == bytes_of({1, 2}));
    CHECK_FALSE(reader.read_bytes(2));
    CHECK(reader.read<std::uint8_t>() == 3);
    CHECK(reader.finish());
}

TEST_CASE("a value reader reads in order", "[core][bytes]") {
    hog::ByteWriter writer;
    writer.write(std::uint16_t{7});
    writer.write(std::bit_cast<std::uint32_t>(1.5F));
    writer.write(std::bit_cast<std::uint64_t>(-2.25));
    writer.write_bytes(bytes_of({'h', 'i'}));
    const auto bytes = std::move(writer).take();

    hog::ValueReader reader{bytes};
    CHECK(reader.integer<std::uint16_t>() == 7);
    CHECK(reader.single() == 1.5F);
    CHECK(reader.twice() == -2.25);
    CHECK(reader.text(2) == "hi");
    CHECK(reader.ok());
    CHECK(reader.finish());
}

TEST_CASE("a value reader keeps its first failure", "[core][bytes]") {
    const auto bytes = bytes_of({1, 2, 3});
    hog::ValueReader reader{bytes};
    CHECK(reader.integer<std::uint32_t>() == 0);
    CHECK_FALSE(reader.ok());
    // Nothing more is read once it has failed, though these bytes are there.
    CHECK(reader.integer<std::uint8_t>() == 0);
    CHECK(reader.text(1).empty());
    reader.fail(hog::Error{.code = hog::ErrorCode::InvalidArgument, .message = "later"});
    const auto finished = reader.finish();
    REQUIRE_FALSE(finished);
    CHECK(finished.error().code == hog::ErrorCode::Parse);
}

TEST_CASE("a value reader fails with bytes left over", "[core][bytes]") {
    const auto bytes = bytes_of({1, 2});
    hog::ValueReader reader{bytes};
    CHECK(reader.integer<std::uint8_t>() == 1);
    CHECK(reader.ok());
    CHECK_FALSE(reader.finish());
}

TEST_CASE("varints take a byte for every seven bits", "[core][bytes]") {
    hog::ByteWriter writer;
    writer.write_varint(0);
    writer.write_varint(127);
    writer.write_varint(128);
    writer.write_varint(300);
    const auto written = std::move(writer).take();
    CHECK(written == bytes_of({0x00, 0x7F, 0x80, 0x01, 0xAC, 0x02}));

    hog::ByteWriter widest;
    widest.write_varint(std::numeric_limits<std::uint64_t>::max());
    CHECK(widest.size() == 10);
}

TEST_CASE("signed varints keep small values short either side of zero", "[core][bytes]") {
    hog::ByteWriter writer;
    for (const std::int64_t value : {0, -1, 1, -2, 63, -64}) {
        writer.write_signed(value);
    }
    const auto written = std::move(writer).take();
    CHECK(written == bytes_of({0x00, 0x01, 0x02, 0x03, 0x7E, 0x7F}));
}

TEST_CASE("varints read back as written", "[core][bytes]") {
    constexpr auto most = std::numeric_limits<std::uint64_t>::max();
    constexpr auto lowest = std::numeric_limits<std::int64_t>::min();
    constexpr auto highest = std::numeric_limits<std::int64_t>::max();
    hog::ByteWriter writer;
    for (const std::uint64_t value : {std::uint64_t{0}, std::uint64_t{1} << 35U, most}) {
        writer.write_varint(value);
    }
    for (const std::int64_t value : {lowest, std::int64_t{-300}, std::int64_t{0}, highest}) {
        writer.write_signed(value);
    }
    const auto bytes = std::move(writer).take();
    hog::ByteReader reader{bytes};
    CHECK(reader.read_varint() == 0);
    CHECK(reader.read_varint() == std::uint64_t{1} << 35U);
    CHECK(reader.read_varint() == most);
    CHECK(reader.read_signed() == lowest);
    CHECK(reader.read_signed() == -300);
    CHECK(reader.read_signed() == 0);
    CHECK(reader.read_signed() == highest);
    CHECK(reader.finish());
}

TEST_CASE("varints cut short or too large fail without consuming", "[core][bytes]") {
    for (const auto& bytes :
         {bytes_of({}), bytes_of({0x80}), bytes_of({0xFF, 0xFF}),
          bytes_of({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02}),
          bytes_of({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00})}) {
        hog::ByteReader reader{bytes};
        const auto value = reader.read_varint();
        REQUIRE_FALSE(value);
        CHECK(value.error().code == hog::ErrorCode::Parse);
        CHECK(reader.remaining() == bytes.size());
    }
}
