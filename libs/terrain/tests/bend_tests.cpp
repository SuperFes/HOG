#include <hog/terrain/bend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <numbers>

using hog::DVec3;
using hog::WorldPosition;
using hog::terrain::Bend;
using hog::terrain::bend_from;
using hog::terrain::BendSettings;
using hog::terrain::bent;
using hog::terrain::faked_radius;
using hog::terrain::unbent;

namespace {

constexpr double radius = 115'000.0;
constexpr double earth = BendSettings{}.faked_radius;
constexpr double blended_out = BendSettings{}.blended_out;
constexpr DVec3 up{0.0, 0.0, 1.0};
constexpr DVec3 east{1.0, 0.0, 0.0};

Bend bend_at(double eye_height, double ground = 0.0) {
    return bend_from(WorldPosition{up * (radius + eye_height)}, radius, ground);
}

// Relative to the eye, `along` metres of ground east of under it and `height` above sea level.
DVec3 point_at(const Bend& bend, double along, double height, DVec3 towards = east) {
    const double angle = along / radius;
    return (((up * std::cos(angle)) + (towards * std::sin(angle))) * (radius + height)) -
           (up * (radius + bend.eye_height));
}

// Metres along the ground to where sea level drawn bent meets the horizon.
double horizon(const Bend& bend, double furthest) {
    double highest = -std::numeric_limits<double>::infinity();
    double at = 0.0;
    for (int metres = 1; metres <= static_cast<int>(furthest); ++metres) {
        const auto along = static_cast<double>(metres);
        const DVec3 drawn = bent(bend, point_at(bend, along, 0.0));
        const double rise = std::atan2(glm::dot(drawn, up), glm::dot(drawn, east));
        if (rise > highest) {
            highest = rise;
            at = along;
        }
    }
    return at;
}

double horizon_on(double sphere, double eye_height) {
    return sphere * std::acos(sphere / (sphere + eye_height));
}

} // namespace

TEST_CASE("the faked radius is Earth's underfoot and the true one high up", "[terrain][bend]") {
    CHECK(faked_radius(radius, 0.0) == earth);
    CHECK(std::abs(faked_radius(radius, 3.0) - earth) < earth * 1e-3);
    CHECK(faked_radius(radius, blended_out) == radius);
    CHECK(faked_radius(radius, 50'000.0) == radius);
    // A planet as large as the faked one already looks it.
    CHECK(faked_radius(earth * 2.0, 3.0) == earth * 2.0);
}

TEST_CASE("the faked radius blends without jumps", "[terrain][bend]") {
    double last = faked_radius(radius, 0.0);
    for (int over = 1; over <= static_cast<int>(blended_out); ++over) {
        const double next = faked_radius(radius, static_cast<double>(over));
        CHECK(next <= last);
        CHECK(last - next < last * 0.01);
        last = next;
    }
}

TEST_CASE("walking 3 m up, the horizon is nearly Earth's", "[terrain][bend]") {
    const Bend walking = bend_at(3.0);
    const double earths = horizon_on(earth, 3.0);
    CHECK(std::abs(horizon(walking, 20'000.0) - earths) < earths * 0.03);

    Bend unfaked = walking;
    unfaked.faked = radius;
    const double trues = horizon_on(radius, 3.0);
    CHECK(std::abs(horizon(unfaked, 5'000.0) - trues) < trues * 0.03);
    CHECK(trues < earths / 7.0);
}

TEST_CASE("bending keeps the ground's distances along and heights above", "[terrain][bend]") {
    const Bend walking = bend_at(3.0);
    for (const double along : {0.0, 50.0, 2'000.0, 40'000.0, 300'000.0}) {
        for (const double height : {-500.0, 0.0, 3.0, 1'500.0, 8'000.0}) {
            const DVec3 drawn = bent(walking, point_at(walking, along, height));
            const DVec3 from_centre = drawn + (up * (walking.faked + walking.eye_height));
            const double turned =
                std::atan2(glm::length(glm::cross(from_centre, up)), glm::dot(from_centre, up));
            CHECK(std::abs(glm::length(from_centre) - walking.faked - height) < 1e-6);
            CHECK(std::abs((turned * walking.faked) - along) < 1e-6);
        }
    }
}

TEST_CASE("near the eye, bending barely moves the ground", "[terrain][bend]") {
    const Bend walking = bend_at(3.0);
    CHECK(bent(walking, DVec3{0.0}) == DVec3{0.0});
    for (int x = -10; x <= 10; ++x) {
        for (int y = -10; y <= 10; ++y) {
            if ((x * x) + (y * y) > 100.0) {
                continue;
            }
            for (const double z : {-3.0, -1.0, 0.0, 5.0}) {
                const DVec3 near{static_cast<double>(x), static_cast<double>(y), z};
                CHECK(glm::length(bent(walking, near) - near) < 1e-3);
            }
        }
    }
}

TEST_CASE("unbending undoes bending", "[terrain][bend]") {
    const DVec3 north{0.0, 1.0, 0.0};
    for (const double eye_height : {0.0, 3.0, 300.0, 4'000.0}) {
        const Bend bend = bend_at(eye_height);
        for (const double along : {0.0, 10.0, 6'000.0, 100'000.0, 330'000.0}) {
            for (const double height : {-200.0, 0.0, 2'000.0}) {
                for (const DVec3 towards : {east, north, glm::normalize(east - north)}) {
                    const DVec3 point = point_at(bend, along, height, towards);
                    const double close = 1e-9 * std::max(glm::length(point), 1.0);
                    CHECK(glm::length(unbent(bend, bent(bend, point)) - point) < close);
                }
            }
        }
    }
}

TEST_CASE("blended out, bending changes nothing", "[terrain][bend]") {
    const Bend high = bend_at(blended_out);
    CHECK(high.faked == radius);
    const DVec3 point = point_at(high, 30'000.0, 100.0);
    CHECK(bent(high, point) == point);
    CHECK(unbent(high, point) == point);

    CHECK(bend_at(blended_out + 2'000.0, 2'000.0).faked == radius);
    CHECK(bend_at(blended_out, 2'000.0).faked > radius);
}

TEST_CASE("climbing through the blend, the ground drawn moves smoothly", "[terrain][bend]") {
    // Fixed in the world, under where the eye climbs from.
    const DVec3 ground = (up * std::cos(0.2) + east * std::sin(0.2)) * radius;
    const auto drawn = [&](double eye_height) {
        const Bend bend = bend_at(eye_height);
        const DVec3 eye = up * (radius + eye_height);
        return eye + bent(bend, ground - eye);
    };
    DVec3 last = drawn(0.0);
    for (int eye_height = 1; eye_height <= static_cast<int>(blended_out); ++eye_height) {
        const DVec3 next = drawn(static_cast<double>(eye_height));
        CHECK(glm::length(next - last) < 10.0);
        last = next;
    }
    CHECK(glm::length(drawn(blended_out - 1.0) - ground) < 0.01);
}

TEST_CASE("while bending, the far side stays well below the horizon", "[terrain][bend]") {
    const double far_side = std::numbers::pi * radius;
    for (int over = 0; over <= static_cast<int>(blended_out); over += 10) {
        const auto height = static_cast<double>(over);
        CHECK(horizon_on(faked_radius(radius, height), height) < far_side / 2.0);
    }
}
