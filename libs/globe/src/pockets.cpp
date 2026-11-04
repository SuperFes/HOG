#include <hog/globe/pockets.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <optional>
#include <thread>
#include <utility>

namespace hog::globe {
namespace {

// A stretch of open air down a column.
struct Run {
    int i = 0;
    int j = 0;
    double bottom = 0.0; // metres from the planet's centre
    double top = 0.0;
};

// The open stretches of a column's caves, found by sampling them as the voxels would.
void open_runs(const Generator::Column& column, double lowest, double highest, double step,
               std::vector<std::pair<double, double>>& found) {
    for (const auto& [bottom, top] : column.caves(lowest, highest, 0.0)) {
        std::optional<double> from;
        double last = bottom;
        for (double radius = bottom; radius <= top + step; radius += step) {
            const bool open = radius <= top && column.sample(radius).distance > 0.0;
            if (open && !from) {
                from = radius;
            }
            if (open) {
                last = radius;
            } else if (from) {
                // Each open sample stands for a step of the column.
                found.emplace_back(*from, last + step);
                from.reset();
            }
        }
        if (from) {
            found.emplace_back(*from, last + step);
        }
    }
}

std::size_t root_of(std::vector<std::size_t>& parent, std::size_t at) {
    while (parent[at] != at) {
        parent[at] = parent[parent[at]];
        at = parent[at];
    }
    return at;
}

} // namespace

std::vector<Pocket> find_pockets(const Generator& generator, DVec3 eye, double range,
                                 double spacing) {
    const Planet& planet = generator.planet();
    const double lod = voxel_lod(generator);
    const DVec3 up = glm::normalize(eye);
    const DVec3 pole = std::abs(up.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 east = glm::normalize(glm::cross(pole, up));
    const DVec3 north = glm::cross(up, east);
    const int half = static_cast<int>(std::ceil(range / spacing));
    const int side = (2 * half) + 1;
    const double floor = glm::length(eye) - range;
    const double ceiling = glm::length(eye) + range;

    // Each row of the grid by its own thread's turn.
    std::vector<std::vector<Run>> rows(static_cast<std::size_t>(side));
    {
        const unsigned workers = std::max(1U, std::thread::hardware_concurrency());
        std::vector<std::jthread> threads;
        threads.reserve(workers);
        for (unsigned worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&, worker] {
                std::vector<std::pair<double, double>> found;
                for (int i = static_cast<int>(worker); i < side; i += static_cast<int>(workers)) {
                    for (int j = 0; j < side; ++j) {
                        const DVec3 across = (east * (static_cast<double>(i - half) * spacing)) +
                                             (north * (static_cast<double>(j - half) * spacing));
                        if (glm::length(across) > range) {
                            continue;
                        }
                        const DVec3 direction = glm::normalize((up * planet.radius) + across);
                        const Generator::Column column = generator.column(direction, lod);
                        found.clear();
                        open_runs(column, std::max(floor, column.ground() - 1'000.0),
                                  std::min(ceiling, column.ground()), lod / 2.0, found);
                        for (const auto& [bottom, top] : found) {
                            // Within range of the eye, not only along the ground and up or down.
                            if (glm::length((direction * ((bottom + top) / 2.0)) - eye) > range) {
                                continue;
                            }
                            rows[static_cast<std::size_t>(i)].push_back(
                                {.i = i, .j = j, .bottom = bottom, .top = top});
                        }
                    }
                }
            });
        }
    }

    // Runs in neighbouring columns whose stretches overlap are one pocket.
    std::vector<Run> runs;
    for (const std::vector<Run>& row : rows) {
        runs.insert(runs.end(), row.begin(), row.end());
    }
    std::vector<std::size_t> parent(runs.size());
    std::iota(parent.begin(), parent.end(), std::size_t{0});
    for (std::size_t a = 0; a < runs.size(); ++a) {
        for (std::size_t b = a + 1; b < runs.size(); ++b) {
            if (runs[b].i > runs[a].i + 1) {
                break;
            }
            if (std::abs(runs[b].j - runs[a].j) <= 1 && runs[a].bottom <= runs[b].top &&
                runs[b].bottom <= runs[a].top) {
                parent[root_of(parent, a)] = root_of(parent, b);
            }
        }
    }
    struct Gathered {
        DVec3 total{0.0};
        double weight = 0.0;
        double height = 0.0;
        std::size_t columns = 0;
    };
    std::vector<Gathered> gathered(runs.size());
    for (std::size_t k = 0; k < runs.size(); ++k) {
        Gathered& into = gathered[root_of(parent, k)];
        const Run& run = runs[k];
        const double middle = (run.bottom + run.top) / 2.0;
        const DVec3 at = (up * planet.radius) +
                         (east * (static_cast<double>(run.i - half) * spacing)) +
                         (north * (static_cast<double>(run.j - half) * spacing));
        const double weight = std::max(run.top - run.bottom, lod);
        into.total += glm::normalize(at) * middle * weight;
        into.weight += weight;
        into.height = std::max(into.height, run.top - run.bottom);
        ++into.columns;
    }
    std::vector<Pocket> pockets;
    for (const Gathered& one : gathered) {
        if (one.columns == 0) {
            continue;
        }
        pockets.push_back({.at = one.total / one.weight,
                           .across = std::sqrt(static_cast<double>(one.columns)) * spacing,
                           .height = one.height});
    }
    std::ranges::sort(pockets, {},
                      [&](const Pocket& pocket) { return glm::length(pocket.at - eye); });
    return pockets;
}

} // namespace hog::globe
