#include <hog/grid/leaves.hpp>
#include <hog/server/piece_source.hpp>

#include <algorithm>
#include <optional>
#include <span>

namespace hog::server {
namespace {

// About what a decoded tree's node takes in memory, with its place in the tree's index.
constexpr std::size_t node_bytes = 128;

constexpr std::uint64_t mixed(std::uint64_t bits) noexcept {
    bits ^= bits >> 30U;
    bits *= 0xBF58'476D'1CE4'E5B9U;
    bits ^= bits >> 27U;
    bits *= 0x94D0'49BB'1331'11EBU;
    return bits ^ (bits >> 31U);
}

std::uint64_t stamp(std::span<const baked::Chunk> chunks) {
    std::uint64_t hash = mixed(chunks.size() + 1);
    for (const baked::Chunk& chunk : chunks) {
        hash = mixed(hash ^ chunk.version);
        hash = mixed(hash ^ (grid::code(chunk.root) << 8U) ^ chunk.band);
    }
    return hash;
}

template <typename Kind>
bool unchanged(const baked::ChunkStore::Snapshot& snapshot, std::span<const baked::Chunk> chunks) {
    return std::ranges::all_of(chunks, [&](const baked::Chunk& chunk) {
        return snapshot.version<Kind>(chunk.band, chunk.root) == chunk.version;
    });
}

// Whether the store holds all there is of the shape under `node` down to `depth`: an open leaf
// short of it is one the generator made, alike all through, rather than the foot of a band whose
// next isn't held under it.
bool reaches(const baked::ChunkStore::Snapshot& snapshot, const baked::Bands& bands,
             const grid::Tree<grid::Shape>& tree, grid::Key node, std::uint8_t depth) {
    bool held = true;
    grid::for_open_leaves(tree, node, [&](grid::Key leaf, const auto&) {
        if (held && leaf.depth < depth && leaf.depth == bands.foot(bands.band_of(leaf.depth))) {
            held = snapshot.holds<grid::Shape>(
                bands.band_of(static_cast<std::uint8_t>(leaf.depth + 1)), leaf);
        }
    });
    return held;
}

} // namespace

bool PieceSource::overlaps(Touched touched, grid::Key node, std::uint8_t depth) noexcept {
    if (!touched.node.contains(node) && !node.contains(touched.node)) {
        return false;
    }
    return std::max({touched.from, touched.node.depth, node.depth}) <= std::min(touched.to, depth);
}

Result<PieceSource> PieceSource::open(baked::ChunkStore& store, baked::ChunkFiller* filler,
                                      std::size_t budget) {
    Result<baked::ChunkStore::Snapshot> snapshot = store.read();
    if (!snapshot) {
        return std::unexpected{snapshot.error()};
    }
    return PieceSource{store, std::move(*snapshot), filler, budget};
}

bool PieceSource::current(const Read& read) const {
    return unchanged<grid::Shape>(snapshot_, read.shape) &&
           unchanged<grid::Material>(snapshot_, read.material) &&
           unchanged<grid::Water>(snapshot_, read.water) &&
           unchanged<grid::Woods>(snapshot_, read.woods);
}

std::shared_ptr<const PieceAnswer> PieceSource::answer(grid::Key node, std::uint8_t depth,
                                                       baked::Rank rank) {
    const std::uint8_t finest = store_->baking().grid.depth();
    if (!node.is_valid() || node.depth > finest) {
        return nullptr;
    }
    depth = std::clamp(depth, node.depth, finest);
    std::shared_ptr<const PieceAnswer> found;
    if (const auto at = kept_.find(node); at != kept_.end()) {
        if (const auto kept = at->second.find(depth); kept != at->second.end()) {
            if (current(kept->second.read)) {
                recent_.splice(recent_.begin(), recent_, kept->second.recent);
                found = kept->second.answer;
                ++counts_.found;
            } else {
                let_go(node, depth);
            }
        }
    }
    if (!found) {
        found = made(node, depth);
    }
    if ((!found || !found->whole) && filler_ != nullptr) {
        filler_->want(node, depth, rank);
    }
    return found;
}

std::shared_ptr<const PieceAnswer> PieceSource::made(grid::Key node, std::uint8_t depth) {
    auto shape = snapshot_.piece<grid::Shape>(node, depth);
    auto material = snapshot_.piece<grid::Material>(node, depth);
    auto water = snapshot_.piece<grid::Water>(node, depth);
    auto woods = snapshot_.piece<grid::Woods>(node, depth);
    if (!shape || !material || !water || !woods) {
        return nullptr;
    }
    auto made = std::make_shared<PieceAnswer>();
    made->whole = reaches(snapshot_, store_->bands(), shape->tree, node, depth);
    made->stamps = {.shape = stamp(shape->chunks),
                    .material = stamp(material->chunks),
                    .water = stamp(water->chunks),
                    .woods = stamp(woods->chunks)};
    for (const grid::Water::Value body : water->tree.leaf_values()) {
        if (body != 0) {
            made->bodies.push_back(body);
        }
    }
    std::ranges::sort(made->bodies);
    const auto [from, to] = std::ranges::unique(made->bodies);
    made->bodies.erase(from, to);
    const std::size_t nodes =
        shape->tree.nodes() + material->tree.nodes() + water->tree.nodes() + woods->tree.nodes();
    made->piece = {.root = node,
                   .generator = store_->baking().generator,
                   .shape = std::move(shape->tree),
                   .material = std::move(material->tree),
                   .water = std::move(water->tree),
                   .woods = std::move(woods->tree)};
    made->bytes = made->piece.encode();

    recent_.emplace_front(node, depth);
    Entry& entry = kept_[node][depth];
    entry = {.answer = made,
             .read = {.shape = std::move(shape->chunks),
                      .material = std::move(material->chunks),
                      .water = std::move(water->chunks),
                      .woods = std::move(woods->chunks)},
             .bytes = made->bytes.size() + (nodes * node_bytes),
             .recent = recent_.begin()};
    counts_.bytes += entry.bytes;
    ++counts_.made;
    while (counts_.bytes > budget_ && recent_.size() > 1) {
        const auto [oldest, deep] = recent_.back();
        let_go(oldest, deep);
    }
    return made;
}

void PieceSource::let_go(grid::Key node, std::uint8_t depth) {
    const auto at = kept_.find(node);
    if (at == kept_.end()) {
        return;
    }
    const auto kept = at->second.find(depth);
    if (kept == at->second.end()) {
        return;
    }
    counts_.bytes -= kept->second.bytes;
    recent_.erase(kept->second.recent);
    at->second.erase(kept);
    if (at->second.empty()) {
        kept_.erase(at);
    }
    ++counts_.let_go;
}

Result<std::vector<PieceSource::Touched>> PieceSource::pump(bool wait) {
    std::vector<Touched> touched;
    if (filler_ != nullptr) {
        auto written = filler_->pump(wait);
        if (!written) {
            return std::unexpected{written.error()};
        }
        const baked::Bands& bands = store_->bands();
        for (const baked::Region region : *written) {
            touched.push_back({.node = region.root,
                               .from = static_cast<std::uint8_t>(
                                   region.band == 0 ? 0 : bands.roots(region.band) + 1),
                               .to = bands.foot(region.band)});
        }
        for (const baked::ChunkFiller::Asked ask : filler_->take_answered()) {
            touched.push_back({.node = ask.node, .from = ask.node.depth, .to = ask.depth});
        }
    }
    if (Result<void> refreshed = snapshot_.refresh(); !refreshed) {
        return std::unexpected{refreshed.error()};
    }
    std::vector<Asked> gone;
    for (const Touched& each : touched) {
        grid::for_nested(kept_, each.node, [&](grid::Key node, const auto& depths) {
            for (const auto& [depth, entry] : depths) {
                if (overlaps(each, node, depth)) {
                    gone.emplace_back(node, depth);
                }
            }
        });
    }
    for (const auto& [node, depth] : gone) {
        let_go(node, depth);
    }
    return touched;
}

} // namespace hog::server
