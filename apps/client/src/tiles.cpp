#include <hog/client/tiles.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <numbers>

namespace hog::client::tiles {

Ground ground_at(const planet::World& world, std::uint32_t face, double s, double t) {
    const DVec3 direction = glm::normalize(
        planet::face_uv_to_xyz(static_cast<int>(face), planet::st_to_uv(s), planet::st_to_uv(t)));
    const planet::CellState state = world.state(planet::cell_at(direction));
    return {.direction = direction,
            .elevation =
                static_cast<double>(state.elevation) / static_cast<double>(planet::fixed_one),
            .state = state};
}

DVec3 hillside(const planet::World& world, std::uint32_t face, double s, double t, double half,
               const Ground& ground) {
    const double radius = world.geology().planet_radius();
    const auto point = [&](const Ground& at) { return at.direction * (radius + at.elevation); };
    const DVec3 to_east =
        point(ground_at(world, face, s + half, t)) - point(ground_at(world, face, s - half, t));
    const DVec3 to_north =
        point(ground_at(world, face, s, t + half)) - point(ground_at(world, face, s, t - half));
    const DVec3 normal = glm::normalize(glm::cross(to_east, to_north));
    return glm::dot(normal, ground.direction) < 0.0 ? -normal : normal;
}

double span_of(double radius, int level) {
    return (std::numbers::pi / 2.0) * radius / static_cast<double>(std::uint32_t{1} << level);
}

double distance_to(WorldPosition eye, WorldPosition origin, double span) {
    const DVec3 up = glm::normalize(eye.metres());
    const DVec3 away = origin - eye;
    return std::max(0.0, glm::length(away - (up * glm::dot(away, up))) - span);
}

std::set<planet::CellId> wanted(WorldPosition eye, double radius, int level, double reach) {
    const double span = span_of(radius, level);
    std::set<planet::CellId> found;
    std::vector<planet::CellId> frontier{planet::cell_at(eye, level)};
    while (!frontier.empty()) {
        const planet::CellId cell = frontier.back();
        frontier.pop_back();
        if (!found.insert(cell).second) {
            continue;
        }
        for (const planet::CellId beside : planet::edge_neighbors(cell)) {
            if (found.contains(beside)) {
                continue;
            }
            const WorldPosition origin{planet::cell_center(beside) * radius};
            if (distance_to(eye, origin, span) < reach) {
                frontier.push_back(beside);
            }
        }
    }
    return found;
}

bool over(planet::CellId tile, planet::CellId cell) {
    return tile.level() <= cell.level() ? cell.parent(tile.level()) == tile
                                        : tile.parent(cell.level()) == cell;
}

} // namespace hog::client::tiles
