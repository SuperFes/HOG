#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/weather/state.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstddef>
#include <vector>

using namespace hog::weather;

namespace {

Field small_field(int layers = 6) {
    hog::globe::Planet planet;
    planet.body.radius = 20'000.0;
    const hog::globe::Generator generator{planet};
    return start_field(generator, 2, layers);
}

} // namespace

TEST_CASE("a saved field comes back as it was", "[weather][state]") {
    Field field = small_field();
    field.air[5].cloud = 0.001F;
    field.air[7].wind = {1.0F, -2.0F, 3.0F};
    field.surface[3] = 301.5F;
    const std::vector<std::byte> bytes = encode_state(field);

    Field other = small_field();
    REQUIRE(decode_state(other, bytes));
    CHECK(other.air[5].cloud == 0.001F);
    CHECK(other.air[7].wind == hog::Vec3{1.0F, -2.0F, 3.0F});
    CHECK(other.surface[3] == 301.5F);
}

TEST_CASE("a field of another grid refuses the bytes and is left alone", "[weather][state]") {
    const std::vector<std::byte> bytes = encode_state(small_field(6));
    Field other = small_field(8);
    const Field before = other;
    CHECK_FALSE(decode_state(other, bytes));
    CHECK(other.air.size() == before.air.size());
    CHECK(other.surface == before.surface);
}

TEST_CASE("cut short or spoilt bytes are refused", "[weather][state]") {
    std::vector<std::byte> bytes = encode_state(small_field());
    Field other = small_field();
    const Field before = other;
    CHECK_FALSE(decode_state(other, std::span{bytes}.first(bytes.size() - 1)));
    // A temperature that is not a number.
    const std::size_t at = 24;
    bytes[at] = bytes[at + 1] = std::byte{0xFF};
    bytes[at + 2] = std::byte{0xFF};
    bytes[at + 3] = std::byte{0x7F};
    CHECK_FALSE(decode_state(other, bytes));
    CHECK(other.surface == before.surface);
}
