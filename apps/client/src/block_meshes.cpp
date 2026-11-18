#include <hog/client/block_meshes.hpp>
#include <hog/client/ground_blocks.hpp>
#include <hog/client/ground_field.hpp>
#include <hog/client/page_meshes.hpp>
#include <hog/core/assert.hpp>

#include <algorithm>
#include <iterator>
#include <utility>

namespace hog::client {
namespace {

const std::shared_ptr<const terrain::BlockMesh>& no_water() {
    static const auto none = std::make_shared<const terrain::BlockMesh>();
    return none;
}

} // namespace

std::uint64_t block_key(grid::Key node) noexcept {
    return page_key(node, false);
}

bool BlockMeshes::Look::same(const Look& other) const noexcept {
    return block.cells == other.block.cells && block.finer == other.block.finer &&
           reads.size() == other.reads.size() &&
           std::ranges::equal(reads, other.reads, [](const auto& one, const auto& two) {
               return one.first == two.first && one.second == two.second;
           });
}

BlockMeshes::BlockMeshes(const grid::Grid& grid, jobs::JobSystem& jobs, std::uint8_t cells)
    : grid_{grid}, jobs_{&jobs}, cells_{cells},
      most_working_{std::max<std::size_t>(2, jobs.thread_count())} {}

BlockMeshes::~BlockMeshes() {
    gather(true);
    planning_.reset();
    showing_.reset();
}

void BlockMeshes::want(const HeldPages& held, DVec3 eye) {
    HOG_ASSERT(settled(), "a drawing wanted while the last is still meshing");
    wanted_.clear();
    planning_ = std::make_unique<jobs::Async<Plan>>(
        *jobs_,
        [grid = grid_, cells = cells_, entries = held.entries(), eye, was = &drawn_] mutable {
            Plan plan{.wanted = {},
                      .queued = {},
                      .pages = std::make_shared<const PagesSnapshot>(grid, std::move(entries))};
            std::vector<std::pair<double, grid::Key>> to_mesh;
            for (const Block& block : ground_blocks(*plan.pages, eye, cells)) {
                Drawing& drawing = plan.wanted[block.node];
                drawing.look = {.block = block, .reads = plan.pages->around(block.node)};
                if (const auto found = was->find(block.node);
                    found != was->end() && found->second.look.same(drawing.look)) {
                    drawing.mesh = found->second.mesh;
                } else {
                    to_mesh.emplace_back(grid.distance(block.node, eye), block.node);
                }
            }
            std::ranges::sort(to_mesh);
            plan.queued.reserve(to_mesh.size());
            for (const auto& [metres, node] : to_mesh) {
                plan.queued.push_back(node);
            }
            return plan;
        });
    stage_ = Stage::Planning;
}

void BlockMeshes::start() {
    while (!queued_.empty() && working_.size() < most_working_) {
        const grid::Key node = queued_.front();
        queued_.pop_front();
        working_.push_back(
            {.node = node,
             .job = std::make_unique<jobs::Async<std::shared_ptr<const terrain::BlockMesh>>>(
                 *jobs_, [grid = grid_, pages = pages_, block = wanted_.at(node).look.block] {
                     GroundField field{*pages};
                     return std::make_shared<const terrain::BlockMesh>(
                         block_of(mesh_block(grid, block, field.field())));
                 })});
    }
}

void BlockMeshes::gather(bool wait) {
    std::vector<Work> still;
    for (Work& work : working_) {
        if (!wait && !work.job->done()) {
            still.push_back(std::move(work));
            continue;
        }
        auto mesh = work.job->take();
        if (const auto found = wanted_.find(work.node); found != wanted_.end()) {
            found->second.mesh = std::move(mesh);
        }
    }
    working_ = std::move(still);
}

void BlockMeshes::advance(bool wait) {
    if (planning_ && (wait || planning_->done())) {
        Plan plan = planning_->take();
        planning_.reset();
        wanted_ = std::move(plan.wanted);
        pages_ = std::move(plan.pages);
        queued_.insert(queued_.end(), plan.queued.begin(), plan.queued.end());
        stage_ = Stage::Meshing;
    }
    if (showing_ && (wait || showing_->done())) {
        Shown shown = showing_->take();
        showing_.reset();
        drawn_ = std::move(shown.drawn);
        changes_.shown.insert(changes_.shown.end(),
                              std::make_move_iterator(shown.changes.shown.begin()),
                              std::make_move_iterator(shown.changes.shown.end()));
        changes_.gone.insert(changes_.gone.end(), shown.changes.gone.begin(),
                             shown.changes.gone.end());
        stage_ = Stage::Settled;
    }
    if (stage_ == Stage::Meshing && queued_.empty() && working_.empty()) {
        pages_.reset();
        showing_ = show();
        stage_ = Stage::Showing;
    }
    start();
}

std::unique_ptr<jobs::Async<BlockMeshes::Shown>> BlockMeshes::show() {
    return std::make_unique<jobs::Async<Shown>>(*jobs_, [wanted = std::move(wanted_),
                                                         was = std::move(drawn_)] mutable {
        GroundChanges changes;
        const auto place = [&](grid::Key node, const std::shared_ptr<const terrain::BlockMesh>& now,
                               const std::shared_ptr<const terrain::BlockMesh>& before) {
            if (now == before) {
                return;
            }
            if (now && !now->indices.empty()) {
                changes.shown.push_back(
                    {.key = block_key(node), .ground = now, .water = no_water()});
            } else if (before && !before->indices.empty()) {
                changes.gone.push_back(block_key(node));
            }
        };
        for (const auto& [node, drawing] : wanted) {
            const auto found = was.find(node);
            place(node, drawing.mesh, found == was.end() ? nullptr : found->second.mesh);
        }
        for (const auto& [node, drawing] : was) {
            if (!wanted.contains(node)) {
                place(node, nullptr, drawing.mesh);
            }
        }
        return Shown{.drawn = std::move(wanted), .changes = std::move(changes)};
    });
}

GroundChanges BlockMeshes::collect() {
    gather(false);
    advance(false);
    return std::exchange(changes_, {});
}

void BlockMeshes::finish() {
    while (!settled()) {
        gather(true);
        advance(true);
    }
}

} // namespace hog::client
