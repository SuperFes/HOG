#include <hog/physics/character.hpp>
#include <hog/physics/space.hpp>
#include <hog/planet/tangent_frame.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

using namespace hog;
using namespace hog::physics;

namespace {

constexpr double radius = 115'000.0;

// Ground built on the plane touching the planet at a point, in metres east, north and up of it.
class Scenery {
public:
    explicit Scenery(DVec3 direction = {1.0, 0.0, 0.0})
        : origin_(glm::normalize(direction) * radius), frame_(planet::tangent_frame(direction)) {}

    // Corners counter-clockwise seen from the side that collides.
    Scenery& quad(DVec3 a, DVec3 b, DVec3 c, DVec3 d) {
        const auto first = static_cast<std::uint32_t>(mesh_.vertices.size());
        for (const DVec3 corner : {a, b, c, d}) {
            mesh_.vertices.emplace_back(frame_.to_world(corner));
        }
        mesh_.triangles.push_back({first, first + 1, first + 2});
        mesh_.triangles.push_back({first, first + 2, first + 3});
        return *this;
    }

    // Level ground `up` metres high, from `west` to `east`, 40 m wide.
    Scenery& floor(double west, double east, double up = 0.0) {
        return quad({west, -20.0, up}, {east, -20.0, up}, {east, 20.0, up}, {west, 20.0, up});
    }

    // A wall facing west at `east`, from `low` to `high`.
    Scenery& wall(double east, double low, double high) {
        return quad({east, 20.0, low}, {east, -20.0, low}, {east, -20.0, high}, {east, 20.0, high});
    }

    [[nodiscard]] Mesh mesh() const {
        Mesh mesh = mesh_;
        mesh.origin = origin_;
        return mesh;
    }

    [[nodiscard]] WorldPosition at(DVec3 local) const { return origin_ + frame_.to_world(local); }
    [[nodiscard]] DVec3 local(WorldPosition position) const {
        return frame_.to_local(position - origin_);
    }
    [[nodiscard]] DVec3 east() const { return frame_.east; }

private:
    WorldPosition origin_;
    planet::TangentFrame frame_;
    Mesh mesh_;
};

struct Stage {
    explicit Stage(Scenery built) : scenery(std::move(built)) {
        REQUIRE(space.add(scenery.mesh()));
    }

    Scenery scenery;
    Space space;
};

constexpr Stride still{};

} // namespace

TEST_CASE("a character comes to stand on the ground, wherever it is on the planet",
          "[physics][character]") {
    for (const DVec3 direction : {DVec3{1.0, 0.0, 0.0}, DVec3{0.0, -1.0, 0.0}, DVec3{0.0, 0.0, 1.0},
                                  DVec3{-1.0, 2.0, 3.0}}) {
        Stage stage{Scenery{direction}.floor(-20.0, 20.0)};
        Character character{stage.space, stage.scenery.at({0.0, 0.0, 1.0})};
        CHECK_FALSE(character.standing());
        character.move(still, 2.0);
        const DVec3 at = stage.scenery.local(character.feet());
        CHECK(at.z == Catch::Approx(0.0).margin(0.05));
        CHECK(glm::length(DVec3{at.x, at.y, 0.0}) < 0.01);
        CHECK(character.standing());
        CHECK(glm::length(character.velocity()) < 0.1);
    }
}

TEST_CASE("a character walks and runs at its speeds", "[physics][character]") {
    const CharacterSettings settings;
    for (const bool running : {false, true}) {
        Stage stage{Scenery{}.floor(-20.0, 30.0)};
        Character character{stage.space, stage.scenery.at({0.0, 0.0, 0.0}), settings};
        character.move(still, 0.5);
        character.move({.along = stage.scenery.east(), .running = running, .jumping = false}, 2.0);
        const double speed = running ? settings.running : settings.walking;
        CHECK(stage.scenery.local(character.feet()).x == Catch::Approx(speed * 2.0).margin(0.1));
        CHECK(character.standing());
    }
}

TEST_CASE("a character jumps about as high as its speed upwards takes it, and lands",
          "[physics][character]") {
    const CharacterSettings settings;
    Stage stage{Scenery{}.floor(-20.0, 20.0)};
    Character character{stage.space, stage.scenery.at({0.0, 0.0, 0.0}), settings};
    character.move(still, 0.5);
    character.move({.along = {}, .running = false, .jumping = true}, Character::step_seconds);
    double highest = 0.0;
    for (int frame = 0; frame < 120; ++frame) {
        character.move(still, Character::step_seconds);
        highest = std::max(highest, stage.scenery.local(character.feet()).z);
    }
    const double expected = settings.jumping * settings.jumping / (2.0 * settings.gravity);
    CHECK(highest == Catch::Approx(expected).margin(0.1));
    CHECK(character.standing());
    CHECK(stage.scenery.local(character.feet()).z == Catch::Approx(0.0).margin(0.05));
}

TEST_CASE("a character walks up gentle slopes, but not steep ones", "[physics][character]") {
    for (const double degrees : {25.0, 65.0}) {
        const double rise = 20.0 * std::tan(glm::radians(degrees));
        Stage stage{Scenery{}
                        .floor(-20.0, 2.0)
                        .quad({2.0, -20.0, 0.0}, {22.0, -20.0, rise}, {22.0, 20.0, rise},
                              {2.0, 20.0, 0.0})};
        Character character{stage.space, stage.scenery.at({0.0, 0.0, 0.0})};
        character.move(still, 0.5);
        character.move({.along = stage.scenery.east(), .running = true, .jumping = false}, 3.0);
        const double height = stage.scenery.local(character.feet()).z;
        if (degrees < 50.0) {
            CHECK(height > 3.0);
        } else {
            CHECK(height < 0.5);
        }
    }
}

TEST_CASE("a character steps up onto a low ledge, but not a high one", "[physics][character]") {
    for (const double ledge : {0.3, 0.8}) {
        Stage stage{Scenery{}.floor(-20.0, 2.0).wall(2.0, 0.0, ledge).floor(2.0, 20.0, ledge)};
        Character character{stage.space, stage.scenery.at({0.0, 0.0, 0.0})};
        character.move(still, 0.5);
        character.move({.along = stage.scenery.east(), .running = false, .jumping = false}, 3.0);
        const DVec3 at = stage.scenery.local(character.feet());
        if (ledge < 0.4) {
            CHECK(at.x > 2.5);
            CHECK(at.z == Catch::Approx(ledge).margin(0.05));
        } else {
            CHECK(at.x < 2.0);
            CHECK(at.z == Catch::Approx(0.0).margin(0.05));
        }
    }
}

TEST_CASE("a character falls off an edge to the ground below", "[physics][character]") {
    Stage stage{Scenery{}.floor(-20.0, 1.0).floor(1.0, 30.0, -10.0)};
    Character character{stage.space, stage.scenery.at({0.0, 0.0, 0.0})};
    character.move(still, 0.5);
    character.move({.along = stage.scenery.east(), .running = false, .jumping = false}, 1.0);
    character.move(still, 2.0);
    CHECK(stage.scenery.local(character.feet()).z == Catch::Approx(-10.0).margin(0.05));
    CHECK(character.standing());
}

TEST_CASE("a character moves alike at any frame rate", "[physics][character]") {
    Stage stage{Scenery{}.floor(-20.0, 20.0)};
    Character smooth{stage.space, stage.scenery.at({0.0, 0.0, 0.5})};
    Character choppy{stage.space, stage.scenery.at({0.0, 0.0, 0.5})};
    const Stride walking{.along = stage.scenery.east(), .running = false, .jumping = false};
    for (int frame = 0; frame < 120; ++frame) {
        smooth.move(walking, 1.0 / 120.0);
    }
    for (int frame = 0; frame < 30; ++frame) {
        choppy.move(walking, 1.0 / 30.0);
    }
    CHECK(smooth.feet() == choppy.feet());
}

TEST_CASE("a character put somewhere is there at rest", "[physics][character]") {
    Stage stage{Scenery{}.floor(-20.0, 20.0)};
    Character character{stage.space, stage.scenery.at({0.0, 0.0, 0.0})};
    character.move({.along = stage.scenery.east(), .running = true, .jumping = false}, 1.0);
    const WorldPosition there = stage.scenery.at({-5.0, 3.0, 0.0});
    character.place(there);
    CHECK(character.feet() == there);
    CHECK(glm::length(character.velocity()) == 0.0);
}
