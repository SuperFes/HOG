#pragma once

#include <hog/client/held_pieces.hpp>
#include <hog/client/tree_stands.hpp>
#include <hog/core/math.hpp>
#include <hog/flora/placement.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/jobs/job_system.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace hog::client {

// The nodes of the woods' finest depth within `reach` of `eye` the held ground may cross,
// nearest first.
[[nodiscard]] std::vector<grid::Key> woods_nodes_near(const PiecesSnapshot& pieces, DVec3 eye,
                                                      double reach);

// The trees standing within `reach` of an eye, as planted from what was held.
struct Grove {
    DVec3 eye{0.0};
    std::vector<StandingTree> trees;
};
[[nodiscard]] Grove plant_around(const PiecesSnapshot& pieces, DVec3 eye, double reach,
                                 std::uint64_t seed, const flora::Planting& planting);

// The trees round the eye, planted on the jobs so a frame never waits on it.
class Forest {
public:
    // The jobs must outlive it.
    Forest(const grid::Grid& grid, jobs::JobSystem& jobs, flora::Planting planting,
           std::uint64_t seed, double reach);
    ~Forest();
    Forest(const Forest&) = delete;
    Forest& operator=(const Forest&) = delete;
    Forest(Forest&&) = delete;
    Forest& operator=(Forest&&) = delete;

    // Plants anew round `eye` from the held pieces once the last planting's done, if pieces
    // came since it began or the eye has moved a good way.
    void want(const HeldPieces& held, DVec3 eye, bool pieces_came);
    // The grove a planting finished since last asked, once.
    [[nodiscard]] std::optional<Grove> collect();
    // Waits for a planting in hand, if any, for collect to give.
    void finish();

private:
    grid::Grid grid_;
    jobs::JobSystem* jobs_;
    std::shared_ptr<const flora::Planting> planting_;
    std::uint64_t seed_;
    double reach_;
    std::optional<DVec3> planted_at_;
    bool stale_ = true;
    std::unique_ptr<jobs::Async<Grove>> planting_job_;
    std::optional<Grove> finished_;
};

} // namespace hog::client
