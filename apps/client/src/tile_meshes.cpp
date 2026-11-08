#include <hog/client/tile_meshes.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/voxel_mesh.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <numbers>
#include <utility>

namespace hog::client {
namespace {

constexpr int side = globe::Volume::tile_side;
// A tile is a cell this many levels coarser than its voxels'.
constexpr int coarser = std::countr_zero(static_cast<unsigned>(side));

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

// The bits of `value`, 21 of them, spread to every third bit.
std::uint64_t spread(std::uint32_t value) noexcept {
    std::uint64_t bits = value & 0x1F'FFFFU;
    bits = (bits | (bits << 32U)) & 0x1F'0000'0000'FFFFULL;
    bits = (bits | (bits << 16U)) & 0x1F'0000'FF00'00FFULL;
    bits = (bits | (bits << 8U)) & 0x100F'00F0'0F00'F00FULL;
    bits = (bits | (bits << 4U)) & 0x10C3'0C30'C30C'30C3ULL;
    bits = (bits | (bits << 2U)) & 0x1249'2492'4924'9249ULL;
    return bits;
}

} // namespace

std::uint64_t tile_key(globe::TileKey key) noexcept {
    // Bits enough for every tile of a face 2^18 tiles across; never all ones, which is the
    // cutaway's.
    return (std::uint64_t{key.level} << 59U) | (std::uint64_t{key.face} << 56U) |
           ((std::uint64_t{key.i} & 0x3'FFFFU) << 38U) |
           ((std::uint64_t{key.j} & 0x3'FFFFU) << 20U) | (std::uint64_t{key.k} & 0xF'FFFFU);
}

std::uint64_t TileMeshes::code(globe::TileKey key) noexcept {
    return (std::uint64_t{key.face} << 61U) | spread(key.i) | (spread(key.j) << 1U) |
           (spread(key.k) << 2U);
}

TileMeshes::TileMeshes(const globe::Planet& planet, const globe::Layers& layers,
                       jobs::JobSystem& jobs)
    : layers_{layers},
      sea_{planet.sea_fraction > 0.0 ? std::optional{planet.radius} : std::nullopt}, jobs_{&jobs},
      by_level_(static_cast<std::size_t>(std::max(layers.level - coarser, 0) + 1)) {
    const auto top = static_cast<std::uint8_t>(by_level_.size() - 1);
    for (std::uint8_t face = 0; face < 6; ++face) {
        for (globe::TileKey root{.level = top, .face = face}; in_shell(root); ++root.k) {
            roots_.push_back(root);
        }
    }
}

TileMeshes::~TileMeshes() = default;

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
    if (key.face >= 6 || key.level >= by_level_.size() || !in_shell(key)) {
        return make_error(ErrorCode::OutOfRange, "a tile sent off the planet");
    }
    const auto [found, added] = held_.try_emplace(key);
    Held& held = found->second;
    if (added) {
        by_level_[key.level].emplace(code(key), key);
    }
    // Sent again, it's drawn as it was till meshed anew.
    held.tree = std::make_shared<const globe::VoxelTree>(std::move(*tree));
    held.arrived = ++taken_;
    held.stale = false;
    for (const globe::TileKey finer : under(key)) {
        Held& replaced = held_.at(finer);
        if (!replaced.stale) {
            replaced.stale = true;
            unfill(finer);
        }
        touched_.insert(finer);
    }
    touched_.insert(key);
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
    for (Work& work : working_) {
        if (work.key == key) {
            abandoned_.push_back(std::move(work.job));
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
                     for (const auto& [next, tile] : near) {
                         if (next == wanted) {
                             return tile.get();
                         }
                     }
                     return nullptr;
                 };
                 return drawn_of(globe::mesh_tile(layers, key, *tree, at), sea, thickness);
             })});
}

void TileMeshes::gather(bool wait) {
    std::erase_if(abandoned_, [&](const auto& job) { return wait || job->done(); });
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
        if (held.shown) {
            changes_[tile_key(work.key)] = GroundChanges::Shown{
                .key = tile_key(work.key), .ground = held.ground, .water = held.water};
        }
        if (std::exchange(held.meshed, true)) {
            continue;
        }
        // Meshed for the first time since it came: what it's drawn for, and in place of, changes.
        if (!held.stale) {
            fill(work.key);
        }
        touched_.insert(work.key);
        for (const globe::TileKey finer : under(work.key)) {
            touched_.insert(finer);
        }
        for (globe::TileKey over = work.key.parent(); over.level < by_level_.size();
             over = over.parent()) {
            if (held_.contains(over)) {
                touched_.insert(over);
            }
        }
    }
    working_ = std::move(still);
}

void TileMeshes::finish() {
    gather(true);
}

std::vector<globe::TileKey> TileMeshes::under(globe::TileKey key) const {
    std::vector<globe::TileKey> found;
    const std::uint64_t place = code(key) & ((std::uint64_t{1} << 61U) - 1U);
    const std::uint64_t face = std::uint64_t{key.face} << 61U;
    for (std::uint8_t level = 0; level < key.level; ++level) {
        const auto finer = static_cast<unsigned>(3 * (key.level - level));
        const std::uint64_t from = face | (place << finer);
        const std::uint64_t to = from + (std::uint64_t{1} << finer);
        const auto& held = by_level_[level];
        for (auto at = held.lower_bound(from); at != held.end() && at->first < to; ++at) {
            found.push_back(at->second);
        }
    }
    return found;
}

bool TileMeshes::children_filled(globe::TileKey key) const {
    if (key.level == 0) {
        return false;
    }
    for (std::size_t octant = 0; octant < 8; ++octant) {
        if (const globe::TileKey child = key.child(octant);
            in_shell(child) && !filled_.contains(child)) {
            return false;
        }
    }
    return true;
}

void TileMeshes::fill(globe::TileKey key) {
    filled_.insert(key);
    for (globe::TileKey over = key.parent(); over.level < by_level_.size(); over = over.parent()) {
        if (filled_.contains(over) || !children_filled(over)) {
            return;
        }
        filled_.insert(over);
        if (held_.contains(over)) {
            touched_.insert(over);
        }
    }
}

void TileMeshes::unfill(globe::TileKey key) {
    if (filled_.erase(key) == 0) {
        return;
    }
    for (globe::TileKey over = key.parent(); over.level < by_level_.size(); over = over.parent()) {
        if (!filled_.contains(over)) {
            return;
        }
        // Filled by its own meshes, it's still filled.
        if (const auto found = held_.find(over);
            found != held_.end() && found->second.meshed && !found->second.stale) {
            return;
        }
        filled_.erase(over);
    }
}

bool TileMeshes::under_drawn(globe::TileKey key) const {
    for (globe::TileKey over = key.parent(); over.level < by_level_.size(); over = over.parent()) {
        if (const auto found = held_.find(over); found != held_.end()) {
            const Held& held = found->second;
            if (held.shown || (held.meshed && !held.stale)) {
                return true;
            }
        }
    }
    return false;
}

bool TileMeshes::replaced(globe::TileKey key, const Held& held) const {
    for (globe::TileKey over = key.parent(); over.level < by_level_.size(); over = over.parent()) {
        if (const auto found = held_.find(over); found != held_.end()) {
            const Held& by = found->second;
            if (by.meshed && !by.stale && by.arrived > held.arrived) {
                return true;
            }
        }
    }
    return false;
}

void TileMeshes::decide(globe::TileKey key) {
    const auto found = held_.find(key);
    if (found == held_.end()) {
        return;
    }
    Held& held = found->second;
    if (replaced(key, held) || children_filled(key)) {
        drop(key);
        return;
    }
    const bool draw = held.meshed && !under_drawn(key);
    if (draw != held.shown) {
        held.shown = draw;
        changes_[tile_key(key)] =
            draw ? std::optional{GroundChanges::Shown{
                       .key = tile_key(key), .ground = held.ground, .water = held.water}}
                 : std::nullopt;
        // Those under it are drawn for it now, or no longer.
        for (const globe::TileKey finer : under(key)) {
            touched_.insert(finer);
        }
    }
    const auto drawn_under = [&] {
        return std::ranges::any_of(under(key),
                                   [&](globe::TileKey finer) { return held_.at(finer).shown; });
    };
    if (!held.meshed && !under_drawn(key) && !drawn_under()) {
        bare_.insert(key);
    } else {
        bare_.erase(key);
    }
}

void TileMeshes::drop(globe::TileKey key) {
    const auto found = held_.find(key);
    if (found == held_.end()) {
        return;
    }
    if (found->second.shown) {
        changes_[tile_key(key)] = std::nullopt;
    }
    for (Work& work : working_) {
        if (work.key == key) {
            abandoned_.push_back(std::move(work.job));
        }
    }
    std::erase_if(working_, [&](const Work& work) { return work.key == key; });
    waiting_.erase(key);
    bare_.erase(key);
    held_.erase(found);
    by_level_[key.level].erase(code(key));
    unfill(key);
    // Those under it may be drawn now in its place.
    for (const globe::TileKey finer : under(key)) {
        touched_.insert(finer);
    }
}

GroundChanges TileMeshes::collect() {
    gather(false);
    for (const globe::TileKey key : waiting_) {
        mesh(key);
    }
    waiting_.clear();
    while (!touched_.empty()) {
        const globe::TileKey key = *touched_.begin();
        touched_.erase(touched_.begin());
        decide(key);
    }
    GroundChanges out;
    for (auto& [key, shown] : changes_) {
        if (shown) {
            out.shown.push_back(std::move(*shown));
        } else {
            out.gone.push_back(key);
        }
    }
    changes_.clear();
    return out;
}

bool TileMeshes::drawn(globe::TileKey key) const {
    const auto found = held_.find(key);
    return found != held_.end() && found->second.shown;
}

double TileMeshes::covered(DVec3 eye, double metres) const {
    const bool whole = std::ranges::all_of(
        roots_, [&](globe::TileKey root) { return filled_.contains(root) || drawn(root); });
    if (bare_.empty() && whole) {
        // Round the whole planet.
        return std::numbers::pi * layers_.ceiling();
    }
    double nearest = metres;
    for (const globe::TileKey key : bare_) {
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
