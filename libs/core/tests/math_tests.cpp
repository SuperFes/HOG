#include <hog/core/math.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <numbers>
#include <type_traits>

using Catch::Matchers::WithinAbs;

static_assert(!std::is_convertible_v<hog::DVec3, hog::Vec3>, "narrowing must be explicit");
static_assert(!std::is_convertible_v<hog::DVec3, hog::WorldPosition>, "a vector is not a point");
static_assert(!std::is_convertible_v<hog::WorldPosition, hog::DVec3>);

namespace {

constexpr double earth_radius = 6'371'000.0;

} // namespace

TEST_CASE("points and offsets follow affine arithmetic", "[core][math]") {
    const hog::WorldPosition a{1.0, 2.0, 3.0};
    const hog::DVec3 offset{10.0, 0.0, -1.0};

    CHECK((a + offset).metres() == hog::DVec3{11.0, 2.0, 2.0});
    CHECK((a + offset) - a == offset);
    CHECK((a + offset) - offset == a);
}

TEST_CASE("camera-relative floats keep millimetres on an Earth-sized planet", "[core][math]") {
    const hog::WorldPosition camera{earth_radius * 0.6, earth_radius * 0.48, earth_radius * 0.64};
    const hog::DVec3 offset{1'000.123, 0.0005, -3.25};
    const hog::WorldPosition point = camera + offset;

    const hog::Vec3 relative = hog::relative_to(point, camera);
    CHECK_THAT(relative.x, WithinAbs(1'000.123, 1e-3));
    CHECK_THAT(relative.y, WithinAbs(0.0005, 1e-4));
    CHECK_THAT(relative.z, WithinAbs(-3.25, 1e-4));

    // The same position as raw floats is off by decimetres, which is why relative_to exists.
    const hog::Vec3 absolute{point.metres()};
    CHECK(static_cast<double>(absolute.x) - point.metres().x != 0.0);
}

TEST_CASE("projection maps depth to Vulkan's 0..1 range", "[core][math]") {
    const float near_plane = 0.1F;
    const float far_plane = 1000.0F;
    const hog::Mat4 projection =
        glm::perspective(glm::radians(60.0F), 16.0F / 9.0F, near_plane, far_plane);

    const hog::Vec4 at_near = projection * hog::Vec4{0.0F, 0.0F, -near_plane, 1.0F};
    const hog::Vec4 at_far = projection * hog::Vec4{0.0F, 0.0F, -far_plane, 1.0F};
    CHECK_THAT(at_near.z / at_near.w, WithinAbs(0.0, 1e-6));
    CHECK_THAT(at_far.z / at_far.w, WithinAbs(1.0, 1e-6));
}

TEST_CASE("quaternions rotate vectors", "[core][math]") {
    const hog::Quat quarter_turn =
        glm::angleAxis(std::numbers::pi_v<float> / 2.0F, hog::Vec3{0.0F, 0.0F, 1.0F});
    const hog::Vec3 rotated = quarter_turn * hog::Vec3{1.0F, 0.0F, 0.0F};

    CHECK_THAT(rotated.x, WithinAbs(0.0, 1e-6));
    CHECK_THAT(rotated.y, WithinAbs(1.0, 1e-6));
    CHECK_THAT(rotated.z, WithinAbs(0.0, 1e-6));
}
