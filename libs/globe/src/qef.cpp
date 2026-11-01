#include <hog/core/assert.hpp>
#include <hog/globe/qef.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace hog::globe {

namespace {

// Planes whose normals part by less than about 37° are taken as one: the weaker direction's share
// of the strongest falls under this, and solving along it would only amplify noise.
constexpr double weakest = 0.1;

// One Jacobi rotation, zeroing the matrix's entry at p, q.
void rotate(glm::dmat3& matrix, glm::dmat3& vectors, glm::length_t p, glm::length_t q) {
    const double apq = matrix[q][p];
    if (apq == 0.0) {
        return;
    }
    const double theta = (matrix[q][q] - matrix[p][p]) / (2.0 * apq);
    const double t =
        std::copysign(1.0, theta) / (std::abs(theta) + std::sqrt((theta * theta) + 1.0));
    const double c = 1.0 / std::sqrt((t * t) + 1.0);
    const double s = t * c;
    glm::dmat3 rotation{1.0};
    rotation[p][p] = c;
    rotation[q][q] = c;
    rotation[q][p] = s;
    rotation[p][q] = -s;
    matrix = glm::transpose(rotation) * matrix * rotation;
    vectors = vectors * rotation;
}

// Eigenvectors (columns) and eigenvalues of a symmetric matrix, by Jacobi rotations.
void eigen(glm::dmat3 matrix, glm::dmat3& vectors, DVec3& values) {
    vectors = glm::dmat3{1.0};
    constexpr int sweeps = 12;
    for (int sweep = 0; sweep < sweeps; ++sweep) {
        const double off = (matrix[1][0] * matrix[1][0]) + (matrix[2][0] * matrix[2][0]) +
                           (matrix[2][1] * matrix[2][1]);
        if (!(off > 1e-24)) {
            break;
        }
        rotate(matrix, vectors, 0, 1);
        rotate(matrix, vectors, 0, 2);
        rotate(matrix, vectors, 1, 2);
    }
    values = {matrix[0][0], matrix[1][1], matrix[2][2]};
}

} // namespace

DVec3 nearest_to_planes(std::span<const DVec3> points, std::span<const DVec3> normals) {
    HOG_ASSERT(!points.empty() && points.size() == normals.size(),
               "a plane is a point and its normal");
    DVec3 mean{0.0};
    for (const DVec3 point : points) {
        mean += point;
    }
    mean /= static_cast<double>(points.size());
    // Solved about the mean, so it's where directions the planes don't pin down are left.
    glm::dmat3 normal_squares{0.0};
    DVec3 pull{0.0};
    for (std::size_t k = 0; k < points.size(); ++k) {
        const DVec3 normal = normals[k];
        normal_squares += glm::outerProduct(normal, normal);
        pull += normal * glm::dot(normal, points[k] - mean);
    }
    glm::dmat3 vectors{1.0};
    DVec3 values{0.0};
    eigen(normal_squares, vectors, values);
    const double strongest = std::max({values.x, values.y, values.z});
    DVec3 offset{0.0};
    for (glm::length_t axis = 0; axis < 3; ++axis) {
        if (strongest > 0.0 && values[axis] > weakest * strongest) {
            offset += vectors[axis] * (glm::dot(vectors[axis], pull) / values[axis]);
        }
    }
    return mean + offset;
}

} // namespace hog::globe
