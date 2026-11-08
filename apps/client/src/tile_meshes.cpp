#include <hog/client/tile_meshes.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/voxel_mesh.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace hog::client {
namespace {

constexpr int side = globe::Volume::tile_side;
// A tile is a cell this many levels coarser than its voxels'.
constexpr int coarser = std::countr_zero(static_cast<unsigned>(side));

// The tile `levels` over `key`.
globe::TileKey over(globe::TileKey key, int levels) noexcept {
    for (int n = 0; n < levels; ++n) {
        key = key.parent();
    }
    return key;
}

// The six tiles beside `key` on its face at its level, as many as there are.
std::vector<globe::TileKey> beside(globe::TileKey key) {
    std::vector<globe::TileKey> next;
    for (const auto [di, dj, dk] :
         {std::array{-1, 0, 0}, std::array{1, 0, 0}, std::array{0, -1, 0}, std::array{0, 1, 0},
          std::array{0, 0, -1}, std::array{0, 0, 1}}) {
        if ((di < 0 && key.i == 0) || (dj < 0 && key.j == 0) || (dk < 0 && key.k == 0)) {
            continue;
        }
        next.push_back({.level = key.level,
                        .face = key.face,
                        .i = key.i + static_cast<std::uint32_t>(di),
                        .j = key.j + static_cast<std::uint32_t>(dj),
                        .k = key.k + static_cast<std::uint32_t>(dk)});
    }
    return next;
}

} // namespace

std::uint64_t tile_key(globe::TileKey key) noexcept {
    // Bits enough for every tile of a face 2^18 tiles across; never all ones, which is the
    // cutaway's.
    return (std::uint64_t{key.level} << 59U) | (std::uint64_t{key.face} << 56U) |
           ((std::uint64_t{key.i} & 0x3'FFFFU) << 38U) |
           ((std::uint64_t{key.j} & 0x3'FFFFU) << 20U) | (std::uint64_t{key.k} & 0xF'FFFFU);
}

TileMeshes::TileMeshes(const globe::Planet& planet, const globe::Layers& layers,
                       jobs::JobSystem& jobs)
    : layers_{layers},
      sea_{planet.sea_fraction > 0.0 ? std::optional{planet.radius} : std::nullopt}, jobs_{&jobs} {}

bool TileMeshes::in_shell(globe::TileKey key) const noexcept {
    return static_cast<std::int64_t>(key.k) * (std::int64_t{side} << key.level) < layers_.count;
}

Result<void> TileMeshes::take(const protocol::SentTile& sent) {
    auto tree = globe::VoxelTree::decode(sent.bytes);
    if (!tree) {
        return std::unexpected(std::move(tree).error());
    }
    if (tree->side() != side) {
        return make_error(ErrorCode::Parse, "a tile sent not a tile across");
    }
    const globe::TileKey key{
        .level = sent.level, .face = sent.face, .i = sent.i, .j = sent.j, .k = sent.k};
    if (key.face >= 6 || key.level + coarser > layers_.level || !in_shell(key)) {
        return make_error(ErrorCode::OutOfRange, "a tile sent off the planet");
    }
    Held& held = held_[key];
    held.tree = std::make_shared<const globe::VoxelTree>(std::move(*tree));
    held.arrived = ++taken_;
    held.meshed = false;
    waiting_.insert(key);
    // Those beside it read it at their edges.
    for (const globe::TileKey next : beside(key)) {
        if (held_.contains(next)) {
            waiting_.insert(next);
        }
    }
    return {};
}

void TileMeshes::mesh(globe::TileKey key) {
    const Held& held = held_.at(key);
    std::vector<std::pair<globe::TileKey, std::shared_ptr<const globe::VoxelTree>>> near;
    for (const globe::TileKey next : beside(key)) {
        if (const auto found = held_.find(next); found != held_.end()) {
            near.emplace_back(next, found->second.tree);
        }
    }
    std::erase_if(working_, [&](const Work& work) { return work.key == key; });
    const double thickness = std::ldexp(layers_.thickness, key.level);
    working_.push_back(
        {.key = key,
         .arrived = held.arrived,
         .job = std::make_unique<jobs::Async<ColumnDrawn>>(
             *jobs_, [layers = layers_, sea = sea_, key, tree = held.tree, near = std::move(near),
                      thickness] {
                 const globe::TileAt at = [&](globe::TileKey wanted) -> const globe::VoxelTree* {
                     for (const auto& [next, beside] : near) {
                         if (next == wanted) {
                             return beside.get();
                         }
                     }
                     return nullptr;
                 };
                 return drawn_of(globe::mesh_tile(layers, key, *tree, at), sea, thickness);
             })});
}

void TileMeshes::gather(bool wait) {
    std::vector<Work> still;
    for (Work& work : working_) {
        if (!wait && !work.job->done()) {
            still.push_back(std::move(work));
            continue;
        }
        ColumnDrawn drawn = work.job->take();
        const auto found = held_.find(work.key);
        if (found == held_.end() || found->second.arrived != work.arrived) {
            continue;
        }
        Held& held = found->second;
        held.ground = std::make_shared<const terrain::BlockMesh>(std::move(drawn.ground));
        held.water = std::make_shared<const terrain::BlockMesh>(std::move(drawn.water));
        held.meshed = true;
        if (held.shown) {
            changes_.shown.push_back(
                {.key = tile_key(work.key), .ground = held.ground, .water = held.water});
        }
    }
    working_ = std::move(still);
}

void TileMeshes::finish() {
    gather(true);
}

std::uint64_t TileMeshes::finer_cover(globe::TileKey key,
                                      const std::map<globe::TileKey, std::uint64_t>& cover) const {
    if (key.level == 0) {
        return 0;
    }
    std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t octant = 0; octant < 8; ++octant) {
        if (const globe::TileKey child = key.child(octant); in_shell(child)) {
            const auto found = cover.find(child);
            oldest = std::min(oldest, found == cover.end() ? 0 : found->second);
        }
    }
    return oldest;
}

std::map<globe::TileKey, std::uint64_t> TileMeshes::covers() const {
    // Each cube's cover, bottom up: its own arrival where it's held meshed, or its children's
    // oldest where all are covered, whichever came after.
    std::map<globe::TileKey, std::uint64_t> cover;
    std::uint8_t top = 0;
    for (const auto& [key, held] : held_) {
        top = std::max(top, key.level);
    }
    std::set<globe::TileKey> level;
    for (std::uint8_t at = 0; at <= top; ++at) {
        for (const auto& [key, held] : held_) {
            if (key.level == at && held.meshed) {
                level.insert(key);
            }
        }
        std::set<globe::TileKey> parents;
        for (const globe::TileKey key : level) {
            const auto found = held_.find(key);
            const std::uint64_t own =
                found != held_.end() && found->second.meshed ? found->second.arrived : 0;
            cover[key] = std::max(own, finer_cover(key, cover));
            parents.insert(key.parent());
        }
        level = std::move(parents);
    }
    // What each held tile is covered by finer.
    std::map<globe::TileKey, std::uint64_t> finer;
    for (const auto& [key, held] : held_) {
        finer[key] = finer_cover(key, cover);
    }
    return finer;
}

bool TileMeshes::replaced(globe::TileKey key, const Held& held) const {
    for (int up = 1; key.level + up + coarser <= layers_.level; ++up) {
        const auto found = held_.find(over(key, up));
        if (found != held_.end() && found->second.meshed && found->second.arrived > held.arrived) {
            return true;
        }
    }
    return false;
}

bool TileMeshes::under_meshed(globe::TileKey key) const {
    for (int up = 1; key.level + up + coarser <= layers_.level; ++up) {
        const auto found = held_.find(over(key, up));
        if (found != held_.end() && found->second.meshed) {
            return true;
        }
    }
    return false;
}

void TileMeshes::drop(globe::TileKey key) {
    const auto found = held_.find(key);
    if (found == held_.end()) {
        return;
    }
    if (found->second.shown) {
        changes_.gone.push_back(tile_key(key));
    }
    waiting_.erase(key);
    held_.erase(found);
}

GroundChanges TileMeshes::collect() {
    gather(false);
    for (const globe::TileKey key : waiting_) {
        mesh(key);
    }
    waiting_.clear();

    const std::map<globe::TileKey, std::uint64_t> finer = covers();
    std::vector<globe::TileKey> gone;
    for (const auto& [key, held] : held_) {
        if (replaced(key, held) || finer.at(key) > held.arrived) {
            gone.push_back(key);
        }
    }
    for (const globe::TileKey key : gone) {
        drop(key);
    }
    for (auto& [key, held] : held_) {
        const bool draw = held.meshed && !under_meshed(key);
        if (draw && !held.shown) {
            changes_.shown.push_back(
                {.key = tile_key(key), .ground = held.ground, .water = held.water});
        } else if (!draw && held.shown) {
            changes_.gone.push_back(tile_key(key));
        }
        held.shown = draw;
    }
    return std::exchange(changes_, {});
}

bool TileMeshes::drawn(globe::TileKey key) const {
    const auto found = held_.find(key);
    return found != held_.end() && found->second.shown;
}

double TileMeshes::covered(DVec3 eye, double metres) const {
    double nearest = metres;
    for (const auto& [key, held] : held_) {
        if (held.shown) {
            continue;
        }
        const globe::Cell cell{.level =
                                   static_cast<std::uint8_t>(layers_.level - coarser - key.level),
                               .face = key.face,
                               .i = key.i,
                               .j = key.j};
        const int span = side << key.level;
        const double middle =
            layers_.bottom(std::min((static_cast<int>(key.k) * span) + (span / 2), layers_.count));
        const double half = 0.87 * span * layers_.thickness;
        nearest = std::min(nearest,
                           std::max(0.0, glm::length((globe::center(cell) * middle) - eye) - half));
    }
    return nearest;
}

} // namespace hog::client
