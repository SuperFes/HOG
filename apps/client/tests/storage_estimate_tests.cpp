#include <hog/client/storage_estimate.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using namespace hog::client;

TEST_CASE("storage grows with the columns, and the formats rank as measured", "[client][storage]") {
    const StorageEstimate one = estimate_storage(1'000.0);
    const StorageEstimate two = estimate_storage(2'000.0);
    CHECK(two.bricks == Approx(2.0 * one.bricks));
    CHECK(one.bricks > one.narrow_band);
    CHECK(one.narrow_band > one.surface);
    CHECK(estimate_storage(0.0).bricks == 0.0);
}

TEST_CASE("bytes read in the largest unit that keeps them under a thousand", "[client][storage]") {
    CHECK(readable_bytes(0.0) == "0 B");
    CHECK(readable_bytes(999.0) == "999 B");
    CHECK(readable_bytes(1'500.0) == "1.5 KB");
    CHECK(readable_bytes(42'000'000.0) == "42.0 MB");
    CHECK(readable_bytes(227e9) == "227 GB");
    CHECK(readable_bytes(2.6e12) == "2.6 TB");
}

TEST_CASE("the account names the cell size, the area and both totals", "[client][storage]") {
    const std::string text = describe_storage(1.0e6, 4.1e11, 0.5, 250'000.0);
    CHECK(text.find("0.5 m") != std::string::npos);
    CHECK(text.find("0.250 km2") != std::string::npos);
    CHECK(text.find("the whole planet") != std::string::npos);
    CHECK(text.find("TB") != std::string::npos);
}
