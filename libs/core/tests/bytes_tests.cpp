#include <hog/core/bytes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
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
