#include <hog/client/block_meshes.hpp>
#include <hog/client/ground_blocks.hpp>
#include <hog/client/ground_field.hpp>
#include <hog/client/water_field.hpp>
#include <hog/core/assert.hpp>
#include <hog/globe/material.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <iterator>
#include <utility>

namespace hog::client {
namespace {

const std::shared_ptr<const terrain::BlockMesh>& empty() {
    static const auto none = std::make_shared<const terrain::BlockMesh>();
    return none;
}

using Pair = std::array<std::shared_ptr<const terrain::BlockMesh>, 2>; // ground, water

bool drawn(const Pair& meshes) {
    return std::ranges::any_of(meshes,
                               [](const auto& mesh) { return mesh && !mesh->indices.empty(); });
}

// Into `changes`, what's drawn under `key` now in place of what was.
void changed(GroundChanges& changes, std::uint64_t key, const Pair& now, const Pair& before) {
    if (now == before) {
        return;
    }
    if (drawn(now)) {
        changes.shown.push_back(
            {.key = key, .ground = now[0] ? now[0] : empty(), .water = now[1] ? now[1] : empty()});
    } else if (drawn(before)) {
        changes.gone.push_back(key);
    }
}

// Past the tiles' keys, whose level takes the top bits but never this high, and the cutaway's.
constexpr std::uint64_t block_keys = std::uint64_t{0b110} << 61U;

} // namespace

std::uint64_t block_key(grid::Key node) noexcept {
    HOG_ASSERT(node.depth < 20, "a block key in the bits kept for it");
    return block_keys | grid::code(node);
}

terrain::BlockMesh block_of(const ShapeMesh& mesh) {
    terrain::BlockMesh block{
        .origin = WorldPosition{mesh.origin}, .vertices = {}, .indices = mesh.indices};
    block.vertices.reserve(mesh.vertices.size());
    for (const ShapeVertex& vertex : mesh.vertices) {
        const std::uint16_t shown =
            vertex.share >= 0.5F ? vertex.materials[0] : vertex.materials[1];
        // The generator's materials come first in the palette; what's unknown shows as soil.
        const auto tile = shown == 0 || shown > 0xFF
                              ? static_cast<std::uint8_t>(globe::Material::Soil)
                              : static_cast<std::uint8_t>(shown);
        const Vec3 normal = glm::dot(vertex.normal, vertex.normal) > 0.0F
                                ? vertex.normal
                                : Vec3{glm::normalize(mesh.origin + DVec3{vertex.position})};
        block.vertices.push_back({.position = vertex.position,
                                  .normal = normal,
                                  .tiles = {tile, tile, tile},
                                  .corner = terrain::packed_corner(0, 0.0)});
    }
    terrain::mix_corners(block);
    return block;
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

std::vector<DrawnBlock> BlockMeshes::drawn() const {
    std::vector<DrawnBlock> found;
    found.reserve(drawn_.size());
    for (const auto& [node, drawing] : drawn_) {
        DrawnBlock block{.block = drawing.look->block, .reads = {}};
        for (const auto& [root, piece] : drawing.look->reads) {
            block.reads.push_back(root);
        }
        if (drawing.mesh && drawing.mesh->ground) {
            block.ground_triangles = drawing.mesh->ground->indices.size() / 3;
        }
        if (drawing.mesh && drawing.mesh->water) {
            block.water_triangles = drawing.mesh->water->indices.size() / 3;
        }
        found.push_back(std::move(block));
    }
    return found;
}

BlockMeshes::~BlockMeshes() {
    gather(true);
    planning_.reset();
}

void BlockMeshes::want(const HeldPieces& held, DVec3 eye) {
    HOG_ASSERT(!planning(), "a drawing wanted while the last is still being planned");
    // Every mesh made that may serve again: those drawn, and those meshed for what was wanted.
    Drawings known = drawn_;
    for (const auto& [node, drawing] : wanted_) {
        if (drawing.mesh) {
            known.insert_or_assign(node, drawing);
        }
    }
    planning_ = std::make_unique<jobs::Async<Plan>>(
        *jobs_, [grid = grid_, cells = cells_, entries = held.entries(),
                 bodies = held.shared_bodies(), eye, known = std::move(known)] mutable {
            const auto started = std::chrono::steady_clock::now();
            const PiecesSnapshot pieces{grid, std::move(entries)};
            Plan plan{.wanted = {}, .queued = {}, .bodies = std::move(bodies)};
            std::vector<std::pair<double, grid::Key>> to_mesh;
            for (const Block& block : ground_blocks(pieces, eye, cells)) {
                Drawing& drawing = plan.wanted[block.node];
                drawing.look = std::make_shared<const Look>(
                    Look{.block = block, .reads = pieces.around(block.node)});
                if (const auto found = known.find(block.node);
                    found != known.end() && found->second.look->same(*drawing.look)) {
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
            plan.milliseconds = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - started)
                                    .count();
            return plan;
        });
}

void BlockMeshes::take_plan(bool wait) {
    if (!planning_ || !(wait || planning_->done())) {
        return;
    }
    Plan plan = planning_->take();
    planning_.reset();
    wanted_ = std::move(plan.wanted);
    bodies_ = std::move(plan.bodies);
    ++stats_.plans;
    stats_.last_plan_ms = plan.milliseconds;
    pending_.clear();
    for (const auto& [node, drawing] : wanted_) {
        const auto found = drawn_.find(node);
        if (drawing.mesh && (found == drawn_.end() || found->second.mesh != drawing.mesh)) {
            pending_.insert(node);
        }
    }
    // What's meshing as it's wanted now lands as it is; the rest is queued afresh.
    queued_.clear();
    for (const grid::Key node : plan.queued) {
        const bool meshing = std::ranges::any_of(working_, [&](const Work& work) {
            return work.node == node && work.look->same(*wanted_.at(node).look);
        });
        if (!meshing) {
            queued_.push_back(node);
        }
    }
}

void BlockMeshes::start() {
    while (!queued_.empty() && working_.size() < most_working_) {
        const grid::Key node = queued_.front();
        queued_.pop_front();
        const std::shared_ptr<const Look>& look = wanted_.at(node).look;
        working_.push_back(
            {.node = node,
             .look = look,
             .job = std::make_unique<jobs::Async<Meshed>>(
                 *jobs_, [grid = grid_, look, bodies = bodies_, block = look->block] {
                     // Only what the block reads, so looking it up stays in the cache.
                     const auto pieces = std::make_shared<const PiecesSnapshot>(grid, look->reads);
                     GroundField ground{*pieces};
                     Meshed meshed{.ground = std::make_shared<const terrain::BlockMesh>(
                                       block_of(mesh_block(grid, block, ground.field()))),
                                   .water = std::make_shared<const terrain::BlockMesh>()};
                     if (may_hold_water(*pieces, block.node)) {
                         WaterField water{*pieces, *bodies};
                         meshed.water = std::make_shared<const terrain::BlockMesh>(
                             water_of(mesh_block(grid, block, water.field()), water));
                     }
                     return meshed;
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
        Meshed mesh = work.job->take();
        ++stats_.meshed;
        // Wanted otherwise since it was started, it's left for the mesh queued as it's wanted.
        if (const auto found = wanted_.find(work.node);
            found != wanted_.end() && found->second.look->same(*work.look)) {
            found->second.mesh = std::move(mesh);
            pending_.insert(work.node);
        } else {
            ++stats_.wasted;
        }
    }
    working_ = std::move(still);
}

void BlockMeshes::hide(grid::Key node) {
    const auto found = drawn_.find(node);
    if (found == drawn_.end()) {
        return;
    }
    if (found->second.mesh) {
        changed(changes_, block_key(node), {},
                {found->second.mesh->ground, found->second.mesh->water});
    }
    drawn_.erase(found);
}

void BlockMeshes::show(grid::Key node) {
    const Drawing& wanted = wanted_.at(node);
    std::vector<grid::Key> under;
    const std::uint64_t end = grid::aligned_end(node);
    for (auto at = drawn_.lower_bound(node); at != drawn_.end() && grid::aligned(at->first) <= end;
         ++at) {
        if (at->first != node) {
            under.push_back(at->first);
        }
    }
    for (const grid::Key finer : under) {
        hide(finer);
    }
    const auto found = drawn_.find(node);
    const Pair before = found != drawn_.end() && found->second.mesh
                            ? Pair{found->second.mesh->ground, found->second.mesh->water}
                            : Pair{};
    const Meshed& mesh = wanted.mesh.value();
    changed(changes_, block_key(node), {mesh.ground, mesh.water}, before);
    drawn_.insert_or_assign(node, wanted);
    pending_.erase(node);
}

void BlockMeshes::publish() {
    const std::vector<grid::Key> ready{pending_.begin(), pending_.end()};
    for (const grid::Key node : ready) {
        if (!pending_.contains(node)) {
            continue;
        }
        // A coarser block drawn over it gives way only to all that's wanted under it at once.
        std::optional<grid::Key> coarser;
        for (std::uint8_t depth = 0; depth < node.depth && !coarser; ++depth) {
            if (drawn_.contains(node.ancestor(depth))) {
                coarser = node.ancestor(depth);
            }
        }
        if (coarser) {
            refine(*coarser);
        } else {
            show(node);
        }
    }
    if (settled()) {
        std::vector<grid::Key> unwanted;
        for (const auto& [node, drawing] : drawn_) {
            if (!wanted_.contains(node)) {
                unwanted.push_back(node);
            }
        }
        for (const grid::Key node : unwanted) {
            hide(node);
        }
    }
}

void BlockMeshes::refine(grid::Key coarser) {
    std::vector<grid::Key> finer;
    const std::uint64_t end = grid::aligned_end(coarser);
    for (auto at = wanted_.upper_bound(coarser);
         at != wanted_.end() && grid::aligned(at->first) <= end; ++at) {
        if (!at->second.mesh) {
            return;
        }
        finer.push_back(at->first);
    }
    hide(coarser);
    for (const grid::Key each : finer) {
        show(each);
    }
}

GroundChanges BlockMeshes::collect() {
    take_plan(false);
    gather(false);
    publish();
    start();
    return std::exchange(changes_, {});
}

void BlockMeshes::finish() {
    while (!settled()) {
        take_plan(true);
        start();
        gather(true);
        publish();
    }
}

} // namespace hog::client
