#include <hog/render/camera.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using Catch::Matchers::WithinAbs;
using hog::DVec3;
using hog::Vec3;
using hog::Vec4;
using hog::WorldPosition;
using hog::render::Camera;

namespace {

// Normalised device coordinates of an offset from the eye.
Vec3 device(const Camera& camera, double aspect, Vec3 offset) {
    const Vec4 clip = hog::render::view_projection(camera, aspect) * Vec4{offset, 1.0F};
    return Vec3{clip} / clip.w;
}

} // namespace

TEST_CASE("depth runs from 1 at the near plane towards 0 at infinity", "[render][camera]") {
    Camera camera;
    camera.near_plane = 0.5;
    CHECK_THAT(device(camera, 1.0, {0.0F, 0.0F, -0.5F}).z, WithinAbs(1.0, 1e-6));
    CHECK_THAT(device(camera, 1.0, {0.0F, 0.0F, -1.0F}).z, WithinAbs(0.5, 1e-6));
    const float far = device(camera, 1.0, {0.0F, 0.0F, -1e7F}).z;
    CHECK(far > 0.0F);
    CHECK(far < 1e-6F);
    CHECK_THAT(device(camera, 1.0, {0.0F, 0.0F, -3.0F}).x, WithinAbs(0.0, 1e-6));
}

TEST_CASE("the field of view spans the screen, with y down", "[render][camera]") {
    Camera camera;
    camera.vertical_fov = 1.2;
    const double aspect = 16.0 / 9.0;
    const auto rise = static_cast<float>(std::tan(0.6));
    const Vec3 top = device(camera, aspect, {0.0F, 10.0F * rise, -10.0F});
    CHECK_THAT(top.y, WithinAbs(-1.0, 1e-5));
    const Vec3 right = device(camera, aspect, {10.0F * rise * 16.0F / 9.0F, 0.0F, -10.0F});
    CHECK_THAT(right.x, WithinAbs(1.0, 1e-5));
}

TEST_CASE("the view turns forward to -z and up to +y", "[render][camera]") {
    const Camera camera{
        .eye = WorldPosition{1e7, 0.0, 0.0}, .forward = {-1.0, 0.0, 0.0}, .up = {0.0, 0.0, 1.0}};
    const auto view = hog::render::view_rotation(camera);
    const Vec4 ahead = view * Vec4{-1.0F, 0.0F, 0.0F, 0.0F};
    const Vec4 above = view * Vec4{0.0F, 0.0F, 1.0F, 0.0F};
    const Vec4 side = view * Vec4{glm::cross(Vec3{camera.forward}, Vec3{camera.up}), 0.0F};
    CHECK_THAT(ahead.z, WithinAbs(-1.0, 1e-6));
    CHECK_THAT(above.y, WithinAbs(1.0, 1e-6));
    CHECK_THAT(side.x, WithinAbs(1.0, 1e-6));

    // Offsets are taken from the eye in doubles first, so far from the origin stays exact.
    const WorldPosition ground = camera.eye + DVec3{-1.5, 0.0, 0.25};
    const Vec3 seen = device(camera, 1.0, hog::relative_to(ground, camera.eye));
    CHECK_THAT(seen.y, WithinAbs(-0.25 / 1.5 / std::tan(0.5), 1e-5));
}
