#include <hog/globe/cell.hpp>
#include <hog/weather/stencil.hpp>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>

namespace hog::weather {

namespace {

// Tells each link which of its far cell's leads back.
void pair_back(std::vector<std::array<Link, links_per_cell>>& all) {
    for (std::size_t cell = 0; cell < all.size(); ++cell) {
        for (Link& link : all[cell]) {
            if (link.cell == Link::none) {
                continue;
            }
            const auto& there = all[link.cell];
            for (std::size_t i = 0; i < there.size(); ++i) {
                if (there[i].cell == cell) {
                    link.back = static_cast<std::uint8_t>(i);
                }
            }
        }
    }
}

} // namespace

Mat3 gradient_matrix(const std::array<Link, links_per_cell>& links, std::uint8_t closed) {
    Mat3 sum{0.0F};
    for (std::size_t i = 0; i < links.size(); ++i) {
        if (links[i].cell != Link::none && ((closed >> i) & 1U) == 0U) {
            sum += glm::outerProduct(links[i].direction, links[i].direction);
        }
    }
    // Left to nothing, a direction with no neighbour stays at rest rather than blowing up.
    constexpr float least = 1e-3F;
    sum[0][0] += least;
    sum[1][1] += least;
    sum[2][2] += least;
    return glm::inverse(sum);
}

Vec3 gradient_weights(const std::array<Link, links_per_cell>& links, std::uint8_t closed,
                      Vec3 offset) {
    return gradient_matrix(links, closed) * offset;
}

Stencil::Stencil(const Grid& grid) : grid_{grid} {
    const std::size_t columns = grid.columns();
    std::vector<DVec3> ups(columns);
    up_.resize(columns);
    for (std::size_t column = 0; column < columns; ++column) {
        ups[column] = globe::center(globe::cell_at_index(grid.level, column));
        up_[column] = Vec3{ups[column]};
    }
    links_.resize(grid.size());
    for (std::size_t column = 0; column < columns; ++column) {
        const DVec3 up = ups[column];
        const auto beside = globe::sides(globe::cell_at_index(grid.level, column));
        for (int layer = 0; layer < grid.layers; ++layer) {
            auto& links = links_[grid.index(column, layer)];
            const double radius = grid.radius_of(layer);
            for (std::size_t side = 0; side < beside.all().size(); ++side) {
                const std::size_t other = globe::index_of(beside.all()[side]);
                const DVec3 across = ups[other] - (up * glm::dot(up, ups[other]));
                const double angle = std::acos(std::clamp(glm::dot(up, ups[other]), -1.0, 1.0));
                links[side] = {.cell = static_cast<std::uint32_t>(grid.index(other, layer)),
                               .distance = static_cast<float>(angle * radius),
                               .direction = Vec3{glm::normalize(across)}};
            }
            if (layer + 1 < grid.layers) {
                links[4] = {.cell = static_cast<std::uint32_t>(grid.index(column, layer + 1)),
                            .distance = static_cast<float>(grid.between(layer)),
                            .direction = Vec3{up}};
            }
            if (layer > 0) {
                links[5] = {.cell = static_cast<std::uint32_t>(grid.index(column, layer - 1)),
                            .distance = static_cast<float>(grid.between(layer - 1)),
                            .direction = Vec3{-up}};
            }
        }
    }
    pair_back(links_);
}

} // namespace hog::weather
