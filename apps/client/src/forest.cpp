#include <hog/client/forest.hpp>
#include <hog/client/ground_blocks.hpp>

#include <algorithm>
#include <utility>

namespace hog::client {

std::vector<grid::Key> woods_nodes_near(const PiecesSnapshot& pieces, DVec3 eye, double reach) {
    const grid::Grid& grid = pieces.grid();
    const std::uint8_t depth = woods_depth(grid);
    std::vector<std::pair<double, grid::Key>> found;
    std::vector<grid::Key> pending{grid::Key{}};
    while (!pending.empty()) {
        const grid::Key node = pending.back();
        pending.pop_back();
        const double away = grid.distance(node, eye);
        if (away > reach || uniform_ground(pieces, node)) {
            continue;
        }
        if (node.depth == depth) {
            found.emplace_back(away, node);
            continue;
        }
        for (std::size_t octant = 0; octant < 8; ++octant) {
            pending.push_back(node.child(octant));
        }
    }
    std::ranges::sort(found);
    std::vector<grid::Key> out;
    out.reserve(found.size());
    for (const auto& [away, node] : found) {
        out.push_back(node);
    }
    return out;
}

Grove plant_around(const PiecesSnapshot& pieces, DVec3 eye, double reach, std::uint64_t seed,
                   const flora::Planting& planting) {
    Grove grove{.eye = eye, .trees = {}};
    for (const grid::Key node : woods_nodes_near(pieces, eye, reach)) {
        const auto trees = trees_in(pieces, node, seed, planting);
        grove.trees.insert(grove.trees.end(), trees.begin(), trees.end());
    }
    return grove;
}

Forest::Forest(const grid::Grid& grid, jobs::JobSystem& jobs, flora::Planting planting,
               std::uint64_t seed, double reach)
    : grid_{grid}, jobs_{&jobs},
      planting_{std::make_shared<const flora::Planting>(std::move(planting))}, seed_{seed},
      reach_{reach} {}

Forest::~Forest() = default;

void Forest::want(const HeldPieces& held, DVec3 eye, bool pieces_came) {
    stale_ = stale_ || pieces_came;
    if (planting_job_ || finished_) {
        return;
    }
    // Trees within a sixth of the reach of where it was planted are still there to draw.
    const bool moved = !planted_at_ || glm::length(eye - *planted_at_) > reach_ / 6.0;
    if (!stale_ && !moved) {
        return;
    }
    stale_ = false;
    planted_at_ = eye;
    planting_job_ = std::make_unique<jobs::Async<Grove>>(
        *jobs_, [grid = grid_, entries = held.entries(), eye, reach = reach_, seed = seed_,
                 planting = planting_] mutable {
            const PiecesSnapshot pieces{grid, std::move(entries)};
            return plant_around(pieces, eye, reach, seed, *planting);
        });
}

std::optional<Grove> Forest::collect() {
    if (planting_job_ && planting_job_->done()) {
        finish();
    }
    return std::exchange(finished_, std::nullopt);
}

void Forest::finish() {
    if (planting_job_) {
        finished_ = planting_job_->take();
        planting_job_.reset();
    }
}

} // namespace hog::client
