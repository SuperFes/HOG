#include <hog/render/camera.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace hog::render {

Mat4 view_rotation(const Camera& camera) noexcept {
    return Mat4(glm::lookAt(DVec3{0.0}, camera.forward, camera.up));
}

Mat4 projection(const Camera& camera, double aspect) noexcept {
    const double focal = 1.0 / std::tan(camera.vertical_fov / 2.0);
    glm::dmat4 matrix{0.0};
    matrix[0][0] = focal / aspect;
    matrix[1][1] = -focal;
    matrix[3][2] = camera.near_plane;
    matrix[2][3] = -1.0;
    return Mat4(matrix);
}

Mat4 view_projection(const Camera& camera, double aspect) noexcept {
    return projection(camera, aspect) * view_rotation(camera);
}

} // namespace hog::render
