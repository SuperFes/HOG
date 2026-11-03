#include <hog/flow/flight.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <cmath>
#include <optional>

namespace {

using hog::DVec3;
using hog::flow::Flight;
using hog::flow::Ground;
using hog::flow::Parcel;
using hog::globe::Material;

constexpr double radius = 1.0e5;
constexpr double gravity = 9.81;
constexpr double reach = 0.5;
constexpr double tick = 0.05;
const double degree = std::acos(-1.0) / 180.0;

// Level ground, or a slope leaning toward +x at `lean` degrees, held at `grip` degrees.
struct Field {
    double lean = 0.0;
    double grip = 30.0;

    std::optional<Ground> operator()(DVec3 at) const {
        const DVec3 up = glm::normalize(at);
        const DVec3 east{1.0, 0.0, 0.0};
        const DVec3 across = glm::normalize(east - (up * glm::dot(east, up)));
        const DVec3 normal =
            glm::normalize((up * std::cos(lean * degree)) + (across * std::sin(lean * degree)));
        return Ground{.radius = radius, .normal = normal, .holds = std::tan(grip * degree)};
    }
};

// Flies the parcel on until it lands; the seconds it took.
double fly(Parcel& parcel, const Field& field, double most = 60.0) {
    double time = 0.0;
    while (time < most) {
        time += tick;
        if (Flight::advance(parcel, tick, gravity, reach, field) == Flight::Fate::Landed) {
            break;
        }
    }
    return time;
}

Parcel dropped(Material material, double height) {
    return Parcel{.at = DVec3{0.0, radius + height, 0.0}, .material = material, .amount = 1};
}

} // namespace

TEST_CASE("a parcel takes the time free fall takes, not a single step", "[flight]") {
    Parcel parcel = dropped(Material::Sand, 20.0);
    const Field field;
    double time = 0.0;
    while (time < 10.0 && parcel.velocity.y <= 0.0) {
        time += tick;
        Flight::advance(parcel, tick, gravity, reach, field);
    }
    // The first look after it met the ground sees it rising.
    CHECK(time == Catch::Approx(std::sqrt(2.0 * 20.0 / gravity)).margin(2.0 * tick));
}

TEST_CASE("a parcel never skips through ground between two looks", "[flight]") {
    Parcel parcel = dropped(Material::Sand, 400.0);
    parcel.velocity = DVec3{0.0, -300.0, 0.0};
    const Field field;
    for (int n = 0; n < 200; ++n) {
        if (Flight::advance(parcel, tick, gravity, reach, field) == Flight::Fate::Landed) {
            break;
        }
        REQUIRE(glm::length(parcel.at) >= radius - 1e-6);
    }
}

TEST_CASE("rubble rebounds higher than sand, which hardly does, and both come to rest",
          "[flight]") {
    const Field field;
    const auto peak = [&](Material material) {
        Parcel parcel = dropped(material, 10.0);
        double top = 0.0;
        bool rising = false;
        for (int n = 0; n < 2000; ++n) {
            if (Flight::advance(parcel, tick, gravity, reach, field) == Flight::Fate::Landed) {
                break;
            }
            if (parcel.velocity.y > 0.0) {
                rising = true;
            }
            if (rising && parcel.velocity.y < 0.0) {
                break;
            }
            if (rising) {
                top = std::max(top, glm::length(parcel.at) - radius);
            }
        }
        return top;
    };
    CHECK(peak(Material::Granite) > 0.5);
    CHECK(peak(Material::Sand) < 0.2);
    Parcel rubble = dropped(Material::Granite, 10.0);
    fly(rubble, field);
    CHECK(glm::length(rubble.velocity) == 0.0);
}

TEST_CASE("a parcel stays on a slope its ground holds and slides down one it doesn't",
              "[flight]") {
    Parcel held = dropped(Material::Sand, 0.0);
    fly(held, Field{.lean = 20.0, .grip = 30.0}, 5.0);
    CHECK(glm::length(held.velocity) == 0.0);

    Parcel loose = dropped(Material::Sand, 0.0);
    const Field slope{.lean = 40.0, .grip = 30.0};
    for (int n = 0; n < 20; ++n) {
        REQUIRE(Flight::advance(loose, tick, gravity, reach, slope) == Flight::Fate::Flying);
    }
    // After a second it moves at about what gravity along the slope less friction leaves.
    const double expected =
        gravity * (std::sin(40.0 * degree) - std::tan(30.0 * degree) * std::cos(40.0 * degree));
    CHECK(glm::length(loose.velocity) == Catch::Approx(expected * 1.0).epsilon(0.15));
}

TEST_CASE("a parcel with no ground to land on stops where it was", "[flight]") {
    Parcel parcel = dropped(Material::Sand, 5.0);
    const auto nothing = [](DVec3) { return std::optional<Ground>{}; };
    const DVec3 before = parcel.at;
    CHECK(Flight::advance(parcel, tick, gravity, reach, nothing) == Flight::Fate::Landed);
    CHECK(parcel.at == before);
}

TEST_CASE("a parcel in motion too long is taken to have stopped", "[flight]") {
    Parcel parcel = dropped(Material::Sand, 0.0);
    parcel.age = Flight::longest;
    CHECK(Flight::advance(parcel, tick, gravity, reach, Field{.lean = 40.0, .grip = 20.0}) ==
          Flight::Fate::Landed);
}
