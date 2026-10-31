#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/rainfall.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>
#include <vector>

using hog::DVec3;
using hog::planet::fixed_one;
using hog::planet::LevelGrid;
using hog::planet::Rainfall;
using hog::planet::wetness;

namespace {

constexpr double radius = 115'000.0;
constexpr double degree = std::numbers::pi / 180.0;

struct Place {
    double latitude = 0.0; // degrees
    double longitude = 0.0;
};

Place place_of(DVec3 at) {
    return {.latitude = std::asin(at.z) / degree, .longitude = std::atan2(at.y, at.x) / degree};
}

DVec3 direction(double latitude, double longitude) {
    return {std::cos(latitude * degree) * std::cos(longitude * degree),
            std::cos(latitude * degree) * std::sin(longitude * degree),
            std::sin(latitude * degree)};
}

std::int64_t fixed(double value) {
    return std::llround(value * static_cast<double>(fixed_one));
}

// Rain over ground `metres` high at each place, negative under the sea, warm as Earth's at sea
// level unless `warmth` says otherwise.
Rainfall rain_over(const std::function<double(Place)>& metres,
                   const std::function<double(Place)>& warmth = {}) {
    LevelGrid grid{5, radius};
    std::vector<std::int64_t> ground(grid.size());
    std::vector<std::int64_t> warm(grid.size());
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const Place place = place_of(hog::planet::cell_center(grid.cell(index)));
        const double height = metres(place);
        const double sine = std::abs(std::sin(place.latitude * degree));
        const double sea_level = warmth ? warmth(place) : 28.0 - (53.0 * sine * sine * sine);
        ground[index] = fixed(height);
        warm[index] = fixed(sea_level - (0.0065 * std::max(height, 0.0)));
    }
    return Rainfall{std::move(grid), ground, warm, radius, {}};
}

// Mean millimetres a year over the cells inside a patch.
double mean_rain(const Rainfall& rainfall, const std::function<bool(Place)>& inside) {
    double total = 0.0;
    int count = 0;
    for (std::uint32_t index = 0; index < rainfall.grid().size(); ++index) {
        if (inside(place_of(hog::planet::cell_center(rainfall.grid().cell(index))))) {
            total += hog::planet::fixed_to_double(rainfall.rain()[index]);
            ++count;
        }
    }
    REQUIRE(count > 0);
    return total / count;
}

double sea(Place /*place*/) {
    return -4'000.0;
}

bool between(double value, double low, double high) {
    return value >= low && value <= high;
}

} // namespace

TEST_CASE("The winds blow as Earth's circulation cells have them", "[rainfall]") {
    const auto east_and_north = [](double latitude) {
        const DVec3 at = direction(latitude, 30.0);
        const DVec3 wind = hog::planet::prevailing_wind(at);
        const DVec3 east = glm::normalize(DVec3{-at.y, at.x, 0.0});
        const DVec3 north = glm::cross(at, east);
        CHECK(std::abs(glm::dot(wind, at)) < 1e-9);
        return std::pair{glm::dot(wind, east), glm::dot(wind, north)};
    };
    // Trades blow from the east towards the equator, westerlies towards the poles.
    const auto [trade_east, trade_north] = east_and_north(15.0);
    CHECK(trade_east < 0.0);
    CHECK(trade_north < 0.0);
    const auto [southern_east, southern_north] = east_and_north(-15.0);
    CHECK(southern_east < 0.0);
    CHECK(southern_north > 0.0);
    const auto [westerly_east, westerly_north] = east_and_north(45.0);
    CHECK(westerly_east > 0.0);
    CHECK(westerly_north > 0.0);
    const auto [polar_east, polar_north] = east_and_north(75.0);
    CHECK(polar_east < 0.0);
    CHECK(polar_north < 0.0);
    // The trades blow across the equator; calm where the other cells meet.
    const auto [equator_east, equator_north] = east_and_north(0.0);
    CHECK(equator_east < 0.0);
    CHECK(std::abs(equator_north) < 1e-9);
    CHECK(glm::length(hog::planet::prevailing_wind(direction(30.0, 30.0))) < 1e-6);
    CHECK(glm::length(hog::planet::prevailing_wind(direction(60.0, 30.0))) < 1e-6);
    CHECK(glm::length(hog::planet::prevailing_wind(direction(90.0, 0.0))) == 0.0);
}

TEST_CASE("Air the circulation lifts rains more than air it lets sink", "[rainfall]") {
    const Rainfall rainfall = rain_over(sea);
    const double equator =
        mean_rain(rainfall, [](Place at) { return std::abs(at.latitude) < 5.0; });
    const double subtropics =
        mean_rain(rainfall, [](Place at) { return between(std::abs(at.latitude), 25.0, 35.0); });
    const double subpolar =
        mean_rain(rainfall, [](Place at) { return between(std::abs(at.latitude), 55.0, 65.0); });
    CHECK(equator > 2.0 * subtropics);
    CHECK(subpolar > subtropics);
    // Earth's oceans see a metre or two a year at the equator.
    CHECK(between(equator, 800.0, 4'000.0));
}

TEST_CASE("Cold air carries less water and rains less", "[rainfall]") {
    const double warm =
        mean_rain(rain_over(sea, [](Place) { return 25.0; }), [](Place) { return true; });
    const double cold =
        mean_rain(rain_over(sea, [](Place) { return -10.0; }), [](Place) { return true; });
    CHECK(cold < warm / 3.0);
}

TEST_CASE("A range is wet where the wind climbs it and dry in its shadow", "[rainfall]") {
    // A continent across the westerlies with a range down its middle.
    const auto ground = [](Place at) {
        if (!between(at.latitude, 25.0, 65.0) || std::abs(at.longitude) > 70.0) {
            return -4'000.0;
        }
        return 100.0 + (3'000.0 * std::max(0.0, 1.0 - (std::abs(at.longitude) / 15.0)));
    };
    const Rainfall rainfall = rain_over(ground);
    const auto strip = [&](double west, double east) {
        return mean_rain(rainfall, [=](Place at) {
            return between(at.latitude, 40.0, 50.0) && between(at.longitude, west, east);
        });
    };
    const double windward = strip(-12.0, -3.0);
    const double lee = strip(5.0, 20.0);
    const double plains = strip(-40.0, -25.0);
    CHECK(windward > 1.5 * plains);
    CHECK(lee < 0.6 * plains);
}

TEST_CASE("Far inland, away from the sea the wind came off, the land dries out", "[rainfall]") {
    const auto ground = [](Place at) {
        return between(at.latitude, 25.0, 65.0) && std::abs(at.longitude) < 90.0 ? 100.0 : -4'000.0;
    };
    const Rainfall rainfall = rain_over(ground);
    const auto strip = [&](double west, double east) {
        return mean_rain(rainfall, [=](Place at) {
            return between(at.latitude, 40.0, 50.0) && between(at.longitude, west, east);
        });
    };
    CHECK(strip(-85.0, -70.0) > 1.5 * strip(65.0, 85.0));
}

TEST_CASE("Rain is never negative", "[rainfall]") {
    const Rainfall rainfall = rain_over([](Place at) {
        return 2'000.0 * std::sin(at.longitude * degree * 7.0) *
               std::cos(at.latitude * degree * 5.0);
    });
    for (const std::int64_t rain : rainfall.rain()) {
        CHECK(rain >= 0);
    }
}

TEST_CASE("Ground is as wet as its rain is against what the warmth evaporates", "[rainfall]") {
    // At 0 °C the warmth could evaporate 350 mm a year.
    CHECK(wetness(350 * fixed_one, 0) == fixed_one / 2);
    CHECK(wetness(0, 20 * fixed_one) == 0);
    CHECK(wetness(1'000 * fixed_one, 10 * fixed_one) > wetness(500 * fixed_one, 10 * fixed_one));
    CHECK(wetness(1'000 * fixed_one, 25 * fixed_one) < wetness(1'000 * fixed_one, 5 * fixed_one));
    CHECK(wetness(100'000 * fixed_one, 30 * fixed_one) < fixed_one);
    // Even the coldest ground evaporates something.
    CHECK(wetness(100 * fixed_one, -40 * fixed_one) == fixed_one / 2);
}
