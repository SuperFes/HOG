#include <hog/globe/pockets.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <optional>
#include <span>
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

// The open stretches under matter in the column through `cell`, between the layers `lowest` and
// `highest`, as its voxels now are.
void open_runs(const World& world, Cell cell, int lowest, int highest,
               std::vector<std::pair<double, double>>& found) {
    const Layers& layers = world.layers();
    const std::optional<int> held = highest_layer(world, cell);
    if (!held) {
        return;
    }
    highest = std::min(highest, *held);
    if (highest < lowest) {
        return;
    }
    const std::vector<Voxel> voxels = world.fill(std::span{&cell, 1}, lowest, highest - lowest + 1);
    const auto in_ground = [](const Voxel& voxel) {
        return voxel.solid != Material::Air || voxel.grain_amount >= Voxel::full / 2;
    };
    bool covered = false;
    std::optional<int> top;
    for (int layer = highest; layer >= lowest; --layer) {
        const bool open = !in_ground(voxels[static_cast<std::size_t>(layer - lowest)]);
        if (!open) {
            if (top) {
                found.emplace_back(layers.bottom(layer + 1), layers.bottom(*top + 1));
                top.reset();
            }
            covered = true;
        } else if (covered && !top) {
            top = layer;
        }
    }
    if (top) {
        found.emplace_back(layers.bottom(lowest), layers.bottom(*top + 1));
    }
}

std::size_t root_of(std::vector<std::size_t>& parent, std::size_t at) {
    while (parent[at] != at) {
        parent[at] = parent[parent[at]];
        at = parent[at];
    }
    return at;
}

// The grid of columns searched about the eye: `side` across, `spacing` metres apart, laid on the
// ground under it, and the heights from the planet's centre searched between.
struct Grid {
    DVec3 eye{0.0};
    double range = 0.0;
    double spacing = 0.0;
    double radius = 0.0;
    DVec3 up{0.0};
    DVec3 east{0.0};
    DVec3 north{0.0};
    int half = 0;
    int side = 0;
    double floor = 0.0;
    double ceiling = 0.0;
};

// The open runs within range in row `i` of the grid, into `row`; `found` is room to work in.
void runs_in_row(const World& world, const Grid& grid, int i,
                 std::vector<std::pair<double, double>>& found, std::vector<Run>& row) {
    const Layers& layers = world.generator().layers();
    for (int j = 0; j < grid.side; ++j) {
        const DVec3 across = (grid.east * (static_cast<double>(i - grid.half) * grid.spacing)) +
                             (grid.north * (static_cast<double>(j - grid.half) * grid.spacing));
        if (glm::length(across) > grid.range) {
            continue;
        }
        const DVec3 direction = glm::normalize((grid.up * grid.radius) + across);
        const Cell cell = cell_under(direction, layers.level);
        found.clear();
        open_runs(world, cell, std::max(0, layers.layer_at(grid.floor)),
                  std::min(layers.count - 1, layers.layer_at(grid.ceiling)), found);
        for (const auto& [bottom, top] : found) {
            // Within range of the eye, not only along the ground and up or down.
            if (glm::length((direction * ((bottom + top) / 2.0)) - grid.eye) > grid.range) {
                continue;
            }
            row.push_back({.i = i, .j = j, .bottom = bottom, .top = top});
        }
    }
}

// Joins runs in neighbouring columns whose stretches overlap into one pocket, `runs` in order of
// row.
void join_overlapping(const std::vector<Run>& runs, std::vector<std::size_t>& parent) {
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
}

} // namespace

std::vector<Pocket> find_pockets(const World& world, DVec3 eye, double range, double spacing) {
    const Generator& generator = world.generator();
    const Planet& planet = generator.planet();
    const double lod = voxel_lod(generator);
    const DVec3 up = glm::normalize(eye);
    const DVec3 pole = std::abs(up.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 east = glm::normalize(glm::cross(pole, up));
    const DVec3 north = glm::cross(up, east);
    const int half = static_cast<int>(std::ceil(range / spacing));
    const int side = (2 * half) + 1;
    const Grid grid{.eye = eye,
                    .range = range,
                    .spacing = spacing,
                    .radius = planet.body.radius,
                    .up = up,
                    .east = east,
                    .north = north,
                    .half = half,
                    .side = side,
                    .floor = glm::length(eye) - range,
                    .ceiling = glm::length(eye) + range};

    // Each row of the grid by its own thread's turn.
    std::vector<std::vector<Run>> rows(static_cast<std::size_t>(side));
    {
        const unsigned workers = std::max(1U, std::thread::hardware_concurrency());
        std::vector<std::thread> threads;
        threads.reserve(workers);
        for (unsigned worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&, worker] {
                std::vector<std::pair<double, double>> found;
                for (int i = static_cast<int>(worker); i < side; i += static_cast<int>(workers)) {
                    runs_in_row(world, grid, i, found, rows[static_cast<std::size_t>(i)]);
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
    }

    // Runs in neighbouring columns whose stretches overlap are one pocket.
    std::vector<Run> runs;
    for (const std::vector<Run>& row : rows) {
        runs.insert(runs.end(), row.begin(), row.end());
    }
    std::vector<std::size_t> parent(runs.size());
    for (std::size_t k = 0; k < parent.size(); ++k) {
        parent[k] = k;
    }
    join_overlapping(runs, parent);
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
        const DVec3 at = (up * planet.body.radius) +
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
    std::ranges::stable_sort(pockets, {},
                             [&](const Pocket& pocket) { return glm::length(pocket.at - eye); });
    return pockets;
}

} // namespace hog::globe
