#include <hog/core/hash.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

// Expected values come from outside this code base: splitmix64 and hash_combine from a Python
// reference, XXH3 from the xxhsum 0.8.3 command-line tool. Changing any of them changes every
// world, so a failure here is never fixed by updating the numbers.

TEST_CASE("mix64 matches the splitmix64 reference", "[core][hash]") {
    STATIC_CHECK(hog::mix64(0) == 0);
    STATIC_CHECK(hog::mix64(1) == 0x5692161D100B05E5ULL);
    STATIC_CHECK(hog::mix64(0xDEADBEEF) == 0x4E062702EC929EEAULL);
    STATIC_CHECK(hog::mix64(~0ULL) == 0xB4D055FCF2CBBD7BULL);
}

TEST_CASE("hash_combine and hash_values match the reference", "[core][hash]") {
    STATIC_CHECK(hog::hash_combine(0, 0) == 0xE220A8397B1DCDAFULL);
    STATIC_CHECK(hog::hash_combine(42, 7) == 0x2C0E0FEDBE2218A8ULL);
    STATIC_CHECK(hog::hash_values(0x1234, 1, 2, 3) == 0xAF7505AF5805BADEULL);
    STATIC_CHECK(hog::hash_values(0, std::int8_t{-1}, 5U) == 0xA7C262EC69D53D83ULL);
}

TEST_CASE("hash_values depends on order", "[core][hash]") {
    STATIC_CHECK(hog::hash_values(9, 1, 2) != hog::hash_values(9, 2, 1));
}

TEST_CASE("hash_string matches xxhsum", "[core][hash]") {
    CHECK(hog::hash_string("") == 0x2D06800538D394C2ULL);
    CHECK(hog::hash_string("HOG") == 0xF66C0511541148ADULL);
    CHECK(hog::hash_string("The quick brown fox jumps over the lazy dog") == 0xCE7D19A5418FB365ULL);

    // Long inputs take XXH3's vectorized path.
    std::string alphabet(1000, ' ');
    for (std::size_t i = 0; i < alphabet.size(); ++i) {
        alphabet[i] = static_cast<char>('a' + (i % 26));
    }
    CHECK(hog::hash_string(alphabet) == 0xE153425558D7DA5DULL);
}

TEST_CASE("hash_bytes agrees with hash_string and honours the seed", "[core][hash]") {
    const std::array bytes{std::byte{'H'}, std::byte{'O'}, std::byte{'G'}};
    CHECK(hog::hash_bytes(bytes) == hog::hash_string("HOG"));
    CHECK(hog::hash_bytes(bytes, 1) != hog::hash_bytes(bytes, 2));
    CHECK(hog::hash_bytes(bytes, 7) == hog::hash_string("HOG", 7));
}
