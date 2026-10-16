#include <hog/client/others.hpp>
#include <hog/physics/character.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace hog;
using namespace hog::client;

namespace {

// Small, so the bend that flattens it for drawing shows within tag_reach.
constexpr double radius = 5'000.0;

// Standing on the ground `east` metres east of the point under the eye.
replication::Shown at(std::uint32_t id, double east, std::string_view name = "Matt") {
    const DVec3 up{std::sin(east / radius), 0.0, std::cos(east / radius)};
    return {.id = protocol::NetId{id}, .at = WorldPosition{up * radius}, .name = name};
}

// Two metres up, looking east.
render::Camera camera() {
    return {.eye = WorldPosition{0.0, 0.0, radius + 2.0},
            .forward = {1.0, 0.0, 0.0},
            .up = {0.0, 0.0, 1.0}};
}

constexpr DVec2 size{1'600.0, 900.0};

} // namespace

TEST_CASE("each player keeps a colour of their own", "[client][others]") {
    CHECK(colour_of(protocol::NetId{7}) == colour_of(protocol::NetId{7}));
    for (std::uint32_t id = 1; id < 20; ++id) {
        const Vec3 colour = colour_of(protocol::NetId{id});
        const Vec3 next = colour_of(protocol::NetId{id + 1});
        CHECK(glm::length(colour - next) > 0.1F);
        for (glm::length_t channel = 0; channel < 3; ++channel) {
            CHECK(colour[channel] > 0.0F);
            CHECK(colour[channel] < 1.0F);
        }
    }
}

TEST_CASE("others stand upright where they're shown, as tall as their characters",
          "[client][others]") {
    const std::vector<replication::Shown> shown{at(3, 10.0), at(4, 2'000.0)};
    const std::vector<render::Capsule> capsules = avatars(shown);
    REQUIRE(capsules.size() == 2);
    for (std::size_t each = 0; each < shown.size(); ++each) {
        CHECK(glm::length(capsules[each].feet - shown[each].at) < 1e-9);
        CHECK(glm::length(capsules[each].up - glm::normalize(shown[each].at.metres())) < 1e-9);
        CHECK(capsules[each].height == physics::CharacterSettings{}.height);
        CHECK(capsules[each].radius == physics::CharacterSettings{}.radius);
        CHECK(capsules[each].colour == colour_of(shown[each].id));
    }
}

TEST_CASE("a name is shown over the head of whoever it's theirs", "[client][others]") {
    const std::vector<replication::Shown> shown{at(1, 10.0)};
    const std::vector<Tag> found = tags(shown, camera(), {}, size);
    REQUIRE(found.size() == 1);
    CHECK(found.front().name == "Matt");
    CHECK(found.front().at.x == Catch::Approx(size.x / 2.0));
    // Above the middle: over the head is higher than the eye.
    CHECK(found.front().at.y < size.y / 2.0);
    CHECK(found.front().distance == Catch::Approx(10.0).margin(0.1));
}

TEST_CASE("names are shown nearest over furthest, and not for those unseen or far off",
          "[client][others]") {
    const std::vector<replication::Shown> shown{at(1, 10.0, "near"), at(2, -10.0, "behind"),
                                                at(3, 50.0, "further"), at(4, 500.0, "far")};
    const std::vector<Tag> found = tags(shown, camera(), {}, size);
    REQUIRE(found.size() == 2);
    CHECK(found[0].name == "further");
    CHECK(found[1].name == "near");
    // Off to one side, out of the picture.
    render::Camera aside = camera();
    aside.forward = {0.0, 1.0, 0.0};
    CHECK(tags(shown, aside, {}, size).empty());
}

TEST_CASE("bent, a name is shown over the head as it's drawn", "[client][others]") {
    // Drawn flatter, they're drawn higher.
    const std::vector<replication::Shown> shown{at(1, 90.0)};
    const terrain::Bend bend = terrain::bend_from(camera().eye, radius, 0.0);
    const std::vector<Tag> flat = tags(shown, camera(), {}, size);
    const std::vector<Tag> bent = tags(shown, camera(), bend, size);
    REQUIRE(flat.size() == 1);
    REQUIRE(bent.size() == 1);
    CHECK(bent.front().at.y < flat.front().at.y - 1.0);
}
