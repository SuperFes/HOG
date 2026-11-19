#include "lmdb_shared.hpp"

#include <hog/baked/chunk_store.hpp>
#include <hog/core/bytes.hpp>
#include <hog/grid/z_order.hpp>

#include <lmdb++.h>

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <map>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace hog::baked {
namespace {

using detail::as_bytes;
using detail::as_view;
using detail::get;
using detail::guarded;
using detail::Removed;
using detail::tag_of;
using grid::aligned;
using grid::aligned_end;

constexpr const char* chunks_name = "chunks";
constexpr const char* log_name = "log";
// Not the page store's, so neither is taken for the other.
constexpr const char* about_name = "chunk store";
constexpr std::string_view format_key = "format";
constexpr std::string_view world_key = "world";
constexpr std::string_view next_version_key = "next version";

struct Shared {
    lmdb::env env{nullptr};
    lmdb::dbi chunks;
    lmdb::dbi about;
    // Absent in a store opened read only from before the log was kept.
    std::optional<lmdb::dbi> log;
    ChunkStore::Options options;
    Baking baking;
    Bands bands;
};

// A chunk's key: which kind and band, then its root's aligned code, big-endian, then its depth,
// so a band's chunks lie in Z order, each before those under it.
using ChunkKey = std::array<std::byte, 11>;

ChunkKey key_of(std::uint8_t tag, std::uint8_t band, grid::Key root) noexcept {
    ChunkKey key{std::byte{tag}, std::byte{band}};
    const std::uint64_t code = aligned(root);
    for (std::size_t k = 0; k < 8; ++k) {
        key.at(2 + k) = static_cast<std::byte>(code >> (8U * (7U - k)));
    }
    key.back() = std::byte{root.depth};
    return key;
}

struct Parsed {
    std::uint8_t tag = 0;
    std::uint8_t band = 0;
    grid::Key root;
};

std::optional<Parsed> parse(std::string_view bytes) noexcept {
    if (bytes.size() != std::tuple_size_v<ChunkKey>) {
        return std::nullopt;
    }
    const std::span<const std::byte> key = as_bytes(bytes);
    std::uint64_t code = 0;
    for (std::size_t k = 0; k < 8; ++k) {
        code = (code << 8U) | std::to_integer<std::uint64_t>(key[2 + k]);
    }
    const auto depth = std::to_integer<std::uint8_t>(key[10]);
    if (depth > grid::max_depth) {
        return std::nullopt;
    }
    const std::uint64_t bits = code >> (3U * (grid::max_depth - depth));
    return Parsed{.tag = std::to_integer<std::uint8_t>(key[0]),
                  .band = std::to_integer<std::uint8_t>(key[1]),
                  .root = {.depth = depth,
                           .x = grid::gather(bits),
                           .y = grid::gather(bits >> 1U),
                           .z = grid::gather(bits >> 2U)}};
}

// A chunk as held: the generator version that made it, its version, then its tree's bytes.
struct Held {
    grid::Key root;
    std::uint32_t generator = 0;
    std::uint64_t version = 0;
    std::span<const std::byte> tree;
};

std::optional<Held> held_from(grid::Key root, std::string_view value) {
    ByteReader in{as_bytes(value)};
    const auto generator = in.read<std::uint32_t>();
    const auto version = in.read<std::uint64_t>();
    if (!generator || !version) {
        return std::nullopt;
    }
    return Held{.root = root,
                .generator = *generator,
                .version = *version,
                .tree = as_bytes(value).subspan(sizeof(std::uint32_t) + sizeof(std::uint64_t))};
}

std::optional<Held> held_at(const lmdb::txn& txn, const lmdb::dbi& chunks, std::uint8_t tag,
                            std::uint8_t band, grid::Key root) {
    std::string_view value;
    if (!get(txn, chunks, as_view(key_of(tag, band, root)), value)) {
        return std::nullopt;
    }
    return held_from(root, value);
}

// The band's chunk whose root is `node` or over it, made by any generator.
std::optional<Held> over(const lmdb::txn& txn, const lmdb::dbi& chunks, std::uint8_t tag,
                         std::uint8_t band, grid::Key node) {
    lmdb::cursor cursor = lmdb::cursor::open(txn.handle(), chunks.handle());
    const ChunkKey wanted = key_of(tag, band, node);
    std::string_view key = as_view(wanted);
    std::string_view value;
    // Chunks never overlap, so the one holding the node is the last at or before it.
    if (cursor.get(key, value, MDB_SET_RANGE)) {
        if (key != as_view(wanted) && !cursor.get(key, value, MDB_PREV)) {
            return std::nullopt;
        }
    } else if (!cursor.get(key, value, MDB_LAST)) {
        return std::nullopt;
    }
    const std::optional<Parsed> parsed = parse(key);
    if (!parsed || parsed->tag != tag || parsed->band != band || !parsed->root.contains(node)) {
        return std::nullopt;
    }
    return held_from(parsed->root, value);
}

// The band's chunks rooted under `node`, not at it, made by any generator, in key order.
std::vector<Held> under(const lmdb::txn& txn, const lmdb::dbi& chunks, std::uint8_t tag,
                        std::uint8_t band, grid::Key node) {
    std::vector<Held> found;
    if (node.depth >= grid::max_depth) {
        return found;
    }
    lmdb::cursor cursor = lmdb::cursor::open(txn.handle(), chunks.handle());
    ChunkKey first = key_of(tag, band, node);
    first.back() = std::byte{static_cast<std::uint8_t>(node.depth + 1U)};
    const std::uint64_t end = aligned_end(node);
    std::string_view key = as_view(first);
    std::string_view value;
    for (bool more = cursor.get(key, value, MDB_SET_RANGE); more;
         more = cursor.get(key, value, MDB_NEXT)) {
        const std::optional<Parsed> parsed = parse(key);
        if (!parsed || parsed->tag != tag || parsed->band != band || aligned(parsed->root) > end) {
            break;
        }
        if (std::optional<Held> held = held_from(parsed->root, value)) {
            found.push_back(*held);
        }
    }
    return found;
}

using LogKey = std::array<std::byte, 8>;

LogKey log_key(std::uint64_t at) noexcept {
    LogKey key{};
    for (std::size_t k = 0; k < key.size(); ++k) {
        key.at(k) = static_cast<std::byte>(at >> (8U * (7U - k)));
    }
    return key;
}

std::uint64_t log_at(std::string_view key) noexcept {
    std::uint64_t at = 0;
    for (const std::byte part : as_bytes(key)) {
        at = (at << 8U) | std::to_integer<std::uint64_t>(part);
    }
    return at;
}

std::vector<std::byte> log_value(const Logged& logged) {
    ByteWriter out;
    out.write(static_cast<std::uint8_t>(logged.what));
    out.write(logged.kind);
    out.write(logged.band);
    out.write(logged.node.depth);
    out.write(logged.node.x);
    out.write(logged.node.y);
    out.write(logged.node.z);
    out.write(logged.depth);
    return std::move(out).take();
}

std::optional<Logged> logged_from(std::string_view key, std::string_view value) {
    ByteReader in{as_bytes(value)};
    const auto what = in.read<std::uint8_t>();
    const auto kind = in.read<std::uint8_t>();
    const auto band = in.read<std::uint8_t>();
    const auto depth = in.read<std::uint8_t>();
    const auto x = in.read<std::uint32_t>();
    const auto y = in.read<std::uint32_t>();
    const auto z = in.read<std::uint32_t>();
    const auto to = in.read<std::uint8_t>();
    if (key.size() != sizeof(LogKey) || !what || *what > 1 || !kind || !band || !depth || !x ||
        !y || !z || !to) {
        return std::nullopt;
    }
    return Logged{.at = log_at(key),
                  .what = static_cast<Logged::What>(*what),
                  .kind = *kind,
                  .band = *band,
                  .node = {.depth = *depth, .x = *x, .y = *y, .z = *z},
                  .depth = *to};
}

// Where `node` lies under `root`: its depth below it and place among that depth's.
struct Below {
    std::uint8_t depth = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t z = 0;
};

Below below(grid::Key root, grid::Key node) noexcept {
    const auto depth = static_cast<std::uint8_t>(node.depth - root.depth);
    return {.depth = depth,
            .x = node.x - (root.x << depth),
            .y = node.y - (root.y << depth),
            .z = node.z - (root.z << depth)};
}

std::vector<std::byte> world_bytes(const Baking& baking, std::uint8_t band_height) {
    ByteWriter out;
    out.write(baking.world);
    out.write(baking.grid.depth());
    out.write(std::bit_cast<std::uint64_t>(baking.grid.finest()));
    out.write(band_height);
    return std::move(out).take();
}

std::vector<std::byte> format_bytes() {
    ByteWriter out;
    out.write(ChunkStore::format);
    return std::move(out).take();
}

// Opens the store's tables, made where it's new: whether it was.
Result<bool> open_tables(const lmdb::txn& txn, Shared& shared) {
    const bool read_only = shared.options.read_only;
    // Named databases are keys of the unnamed one: anything else there is another store's.
    const lmdb::dbi names = lmdb::dbi::open(txn, nullptr);
    std::string_view held;
    const bool fresh = !get(txn, names, about_name, held);
    if (fresh) {
        MDB_stat stat{};
        lmdb::dbi_stat(txn, names, &stat);
        if (stat.ms_entries != 0) {
            return make_error(ErrorCode::Unsupported, "not a chunk store");
        }
        if (read_only) {
            return make_error(ErrorCode::NotFound, "no chunk store");
        }
    }
    const unsigned create = fresh ? MDB_CREATE : 0U;
    shared.about = lmdb::dbi::open(txn, about_name, create);
    shared.chunks = lmdb::dbi::open(txn, chunks_name, create);
    // Stores from before the log get one, unless only read.
    if (get(txn, names, log_name, held) || !read_only) {
        shared.log = lmdb::dbi::open(txn, log_name, read_only ? 0U : MDB_CREATE);
    }
    return fresh;
}

Result<void> mark(const lmdb::txn& txn, Shared& shared, bool fresh) {
    const std::vector<std::byte> format = format_bytes();
    const std::vector<std::byte> world = world_bytes(shared.baking, shared.bands.height());
    if (fresh) {
        shared.about.put(txn, format_key, as_view(format));
        shared.about.put(txn, world_key, as_view(world));
        return {};
    }
    std::string_view held;
    if (!get(txn, shared.about, format_key, held) || held != as_view(format)) {
        ByteReader in{as_bytes(held)};
        const auto version = in.read<std::uint32_t>();
        return make_error(ErrorCode::Unsupported,
                          std::format("chunk store format {}", version ? *version : 0U));
    }
    if (!get(txn, shared.about, world_key, held) || held != as_view(world)) {
        return make_error(ErrorCode::InvalidArgument,
                          "a chunk store of another world, grid or band height");
    }
    return {};
}

// The chunks a band's tree under `root` is held as: the whole, or where it's over `budget`, its
// root's children's, in turn, down to the band's roots.
template <typename Kind>
std::vector<std::pair<grid::Key, std::vector<std::byte>>>
split(grid::Key root, grid::Tree<Kind> tree, std::size_t budget, std::uint8_t roots) {
    std::vector<std::pair<grid::Key, std::vector<std::byte>>> parts;
    std::vector<std::pair<grid::Key, grid::Tree<Kind>>> splitting;
    splitting.emplace_back(root, std::move(tree));
    while (!splitting.empty()) {
        auto [at, part] = std::move(splitting.back());
        splitting.pop_back();
        std::vector<std::byte> bytes = part.encode();
        if (bytes.size() <= budget || at.depth >= roots || part.nodes() == 1) {
            parts.emplace_back(at, std::move(bytes));
            continue;
        }
        for (std::size_t k = 8; k-- > 0;) {
            const grid::Key child = at.child(k);
            splitting.emplace_back(child, part.cut(1, child.x - (2 * at.x), child.y - (2 * at.y),
                                                   child.z - (2 * at.z),
                                                   static_cast<std::uint8_t>(part.levels() - 1)));
        }
    }
    return parts;
}

} // namespace

Bands::Bands(std::uint8_t height, std::uint8_t finest) noexcept
    : height_{height}, finest_{finest},
      count_{static_cast<std::uint8_t>(finest == 0 ? 1 : ((finest - 1) / height) + 1)} {}

std::uint8_t Bands::band_of(std::uint8_t depth) const noexcept {
    return depth == 0 ? 0 : static_cast<std::uint8_t>((depth - 1) / height_);
}

std::uint8_t Bands::roots(std::uint8_t band) const noexcept {
    return static_cast<std::uint8_t>(band * height_);
}

std::uint8_t Bands::foot(std::uint8_t band) const noexcept {
    return static_cast<std::uint8_t>(std::min<int>((band + 1) * height_, finest_));
}

struct ChunkStore::Impl {
    // Ahead of the store, so it's removed only once the store is closed.
    Removed scratch;
    std::filesystem::path directory;
    Shared shared;
};

struct ChunkStore::Snapshot::Impl {
    const Shared* shared = nullptr;
    lmdb::txn txn{nullptr};

    // The band's chunk over `node`, where this generator made it.
    [[nodiscard]] std::optional<Held> current_over(std::uint8_t tag, std::uint8_t band,
                                                   grid::Key node) const {
        std::optional<Held> held = over(txn, shared->chunks, tag, band, node);
        if (!held || held->generator != shared->baking.generator) {
            return std::nullopt;
        }
        return held;
    }

    [[nodiscard]] std::vector<Held> current_under(std::uint8_t tag, std::uint8_t band,
                                                  grid::Key node) const {
        std::vector<Held> found = under(txn, shared->chunks, tag, band, node);
        std::erase_if(found,
                      [&](const Held& held) { return held.generator != shared->baking.generator; });
        return found;
    }

    // The node as a closed leaf over it in a band coarser than `band` has it, `levels` deep;
    // none where the first such band holding it has it open.
    template <typename Kind>
    [[nodiscard]] std::optional<Piece<Kind>> closed_over(grid::Key node, std::uint8_t band,
                                                         std::uint8_t levels) const {
        using Tree = grid::Tree<Kind>;
        for (std::uint8_t coarser = band; coarser-- > 0;) {
            const std::optional<Held> held = current_over(tag_of<Kind>(), coarser, node);
            if (!held) {
                continue;
            }
            const Result<Tree> tree = Tree::decode(held->tree);
            if (!tree) {
                return std::nullopt;
            }
            const Below at = below(held->root, node.ancestor(shared->bands.foot(coarser)));
            const typename Tree::Found leaf = tree->at(at.depth, at.x, at.y, at.z);
            if (leaf.open) {
                return std::nullopt;
            }
            return Piece<Kind>{
                .tree = Tree{levels, leaf.value, false},
                .chunks = {{.band = coarser, .root = held->root, .version = held->version}}};
        }
        return std::nullopt;
    }

    // Puts each chunk of `band` in at its root under `node`, down to `depth`.
    template <typename Kind>
    [[nodiscard]] bool graft(Piece<Kind>& piece, grid::Key node, std::uint8_t band,
                             std::uint8_t depth, std::span<const Held> chunks) const {
        for (const Held& held : chunks) {
            const Result<grid::Tree<Kind>> tree = grid::Tree<Kind>::decode(held.tree);
            if (!tree) {
                return false;
            }
            const Below at = below(node, held.root);
            piece.tree.graft(
                at.depth, at.x, at.y, at.z,
                tree->cut(0, 0, 0, 0, static_cast<std::uint8_t>(depth - held.root.depth)));
            piece.chunks.push_back({.band = band, .root = held.root, .version = held.version});
        }
        return true;
    }
};

struct ChunkStore::Batch::Impl {
    Shared* shared = nullptr;
    lmdb::txn txn{nullptr};

    [[nodiscard]] std::uint64_t next_version() const {
        std::uint64_t next = 1;
        std::string_view held;
        if (get(txn, shared->about, next_version_key, held)) {
            ByteReader in{as_bytes(held)};
            next = in.read<std::uint64_t>().value_or(1);
        }
        ByteWriter out;
        out.write(next + 1);
        shared->about.put(txn, next_version_key, as_view(out.bytes()));
        return next;
    }

    // Logs `logged` at the number `at`, where the store keeps a log.
    void log(std::uint64_t at, Logged logged) const {
        if (!shared->log) {
            return;
        }
        logged.at = at;
        shared->log->put(txn, as_view(log_key(at)), as_view(log_value(logged)));
    }

    // Lets go of the oldest entries past what the log keeps.
    void trim() const {
        if (!shared->log) {
            return;
        }
        MDB_stat stat{};
        lmdb::dbi_stat(txn, *shared->log, &stat);
        std::size_t over =
            stat.ms_entries > shared->options.logged ? stat.ms_entries - shared->options.logged : 0;
        lmdb::cursor cursor = lmdb::cursor::open(txn.handle(), shared->log->handle());
        std::string_view key;
        std::string_view value;
        while (over > 0 && cursor.get(key, value, MDB_FIRST)) {
            lmdb::cursor_del(cursor.handle(), 0);
            --over;
        }
    }
};

ChunkStore::Snapshot::Snapshot(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
ChunkStore::Snapshot::Snapshot(Snapshot&&) noexcept = default;
ChunkStore::Snapshot& ChunkStore::Snapshot::operator=(Snapshot&&) noexcept = default;
ChunkStore::Snapshot::~Snapshot() = default;

template <typename Kind>
std::optional<Piece<Kind>> ChunkStore::Snapshot::piece(grid::Key node, std::uint8_t depth) const {
    const Bands& bands = impl_->shared->bands;
    const grid::Grid& grid = impl_->shared->baking.grid;
    if (!node.is_valid() || node.depth > depth || depth > grid.depth()) {
        return std::nullopt;
    }
    constexpr std::uint8_t tag = tag_of<Kind>();
    using Tree = grid::Tree<Kind>;
    try {
        Piece<Kind> piece{.tree = Tree{0, {}, false}, .chunks = {}};
        // Bands whose chunks under the node go in over what the coarser bands give, finest first,
        // with the depth each is read down to.
        struct Finer {
            std::uint8_t depth = 0;
            std::vector<Held> chunks;
        };
        std::vector<Finer> finer;
        std::uint8_t reach = depth;
        for (;;) {
            const std::uint8_t band = bands.band_of(reach);
            if (const std::optional<Held> held = impl_->current_over(tag, band, node)) {
                const Result<Tree> tree = Tree::decode(held->tree);
                if (!tree) {
                    return std::nullopt;
                }
                const Below at = below(held->root, node);
                piece.tree = tree->cut(at.depth, at.x, at.y, at.z,
                                       static_cast<std::uint8_t>(reach - node.depth));
                piece.chunks.push_back(
                    {.band = band, .root = held->root, .version = held->version});
                break;
            }
            if (band == 0) {
                return std::nullopt;
            }
            const std::uint8_t roots = bands.roots(band);
            if (node.depth > roots) {
                // No chunk holds the node in its own band; a closed leaf over it still holds all
                // of it.
                return impl_->closed_over<Kind>(node, band,
                                                static_cast<std::uint8_t>(depth - node.depth));
            }
            finer.push_back({.depth = reach, .chunks = impl_->current_under(tag, band, node)});
            reach = roots;
        }
        for (auto band = finer.rbegin(); band != finer.rend(); ++band) {
            if (!impl_->graft(piece, node, bands.band_of(band->depth), band->depth,
                              std::span<const Held>{band->chunks})) {
                return std::nullopt;
            }
        }
        if (!finer.empty()) {
            piece.tree.compact();
        }
        return piece;
    } catch (const lmdb::error&) {
        return std::nullopt;
    }
}

template <typename Kind>
std::uint64_t ChunkStore::Snapshot::version(std::uint8_t band, grid::Key root) const {
    try {
        const std::optional<Held> held =
            held_at(impl_->txn, impl_->shared->chunks, tag_of<Kind>(), band, root);
        return held && held->generator == impl_->shared->baking.generator ? held->version : 0;
    } catch (const lmdb::error&) {
        return 0;
    }
}

template <typename Kind>
std::vector<Chunk> ChunkStore::Snapshot::chunks(std::uint8_t band, grid::Key node) const {
    std::vector<Chunk> found;
    if (!node.is_valid()) {
        return found;
    }
    try {
        if (const std::optional<Held> held = impl_->current_over(tag_of<Kind>(), band, node)) {
            found.push_back({.band = band, .root = held->root, .version = held->version});
        }
        for (const Held& held : impl_->current_under(tag_of<Kind>(), band, node)) {
            found.push_back({.band = band, .root = held.root, .version = held.version});
        }
    } catch (const lmdb::error&) {
        return {};
    }
    return found;
}

template <typename Kind>
std::vector<StoredChunk> ChunkStore::Snapshot::stored(std::uint8_t band, grid::Key node) const {
    std::vector<StoredChunk> found;
    if (!node.is_valid()) {
        return found;
    }
    const auto add = [&](const Held& held) {
        found.push_back({.chunk = {.band = band, .root = held.root, .version = held.version},
                         .tree = held.tree});
    };
    try {
        if (const std::optional<Held> held = impl_->current_over(tag_of<Kind>(), band, node)) {
            add(*held);
        }
        for (const Held& held : impl_->current_under(tag_of<Kind>(), band, node)) {
            add(held);
        }
    } catch (const lmdb::error&) {
        return {};
    }
    return found;
}

template <typename Kind>
bool ChunkStore::Snapshot::holds(std::uint8_t band, grid::Key node) const {
    if (!node.is_valid()) {
        return false;
    }
    try {
        if (impl_->current_over(tag_of<Kind>(), band, node)) {
            return true;
        }
        // Chunks never overlap, so they hold all of it where their volumes add up to its.
        const auto volume = [](grid::Key key) {
            return std::uint64_t{1} << (3U * (grid::max_depth - key.depth));
        };
        std::uint64_t held = 0;
        for (const Held& chunk : impl_->current_under(tag_of<Kind>(), band, node)) {
            held += volume(chunk.root);
        }
        return held == volume(node);
    } catch (const lmdb::error&) {
        return false;
    }
}

Result<void> ChunkStore::Snapshot::refresh() {
    return guarded([&] -> Result<void> {
        impl_->txn.reset();
        impl_->txn.renew();
        return {};
    });
}

template <typename Kind>
Result<std::vector<Chunk>> ChunkStore::Batch::put(std::uint8_t band, grid::Key root,
                                                  const grid::Tree<Kind>& tree) {
    Shared& shared = *impl_->shared;
    const Bands& bands = shared.bands;
    if (!root.is_valid() || band >= bands.count() || root.depth > bands.roots(band)) {
        return make_error(ErrorCode::InvalidArgument, "a chunk rooted under its band's roots");
    }
    if (tree.levels() != bands.foot(band) - root.depth) {
        return make_error(ErrorCode::InvalidArgument, "a chunk's tree not down to its band's foot");
    }
    constexpr std::uint8_t tag = tag_of<Kind>();
    using Tree = grid::Tree<Kind>;
    return guarded([&] -> Result<std::vector<Chunk>> {
        std::vector<Chunk> touched;
        const lmdb::txn& txn = impl_->txn;
        const auto logged = [&](grid::Key at) {
            return Logged{.at = 0,
                          .what = Logged::What::Chunk,
                          .kind = tag,
                          .band = band,
                          .node = at,
                          .depth = 0};
        };
        const auto erase = [&](grid::Key at) {
            const ChunkKey key = key_of(tag, band, at);
            shared.chunks.del(txn, as_view(key));
            touched.push_back({.band = band, .root = at, .version = 0});
            impl_->log(impl_->next_version(), logged(at));
        };
        // What's to be held, rooted where.
        grid::Key whole = root;
        Tree replacing = tree;
        if (const std::optional<Held> held = over(txn, shared.chunks, tag, band, root);
            held && held->root != root) {
            Result<Tree> wider = held->generator == shared.baking.generator
                                     ? Tree::decode(held->tree)
                                     : Result<Tree>{make_error(ErrorCode::Unsupported, "stale")};
            if (wider) {
                const Below at = below(held->root, root);
                wider->graft(at.depth, at.x, at.y, at.z, tree);
                whole = held->root;
                replacing = std::move(*wider);
            } else {
                erase(held->root);
            }
        }
        // What's held now in its place, to keep where unchanged and let go of otherwise.
        std::map<grid::Key, std::uint64_t> before;
        if (const std::optional<Held> held = held_at(txn, shared.chunks, tag, band, whole)) {
            before.emplace(whole, held->version);
        }
        for (const Held& held : under(txn, shared.chunks, tag, band, whole)) {
            before.emplace(held.root, held.version);
        }
        for (auto& [at, bytes] :
             split(whole, std::move(replacing), shared.options.budget, bands.roots(band))) {
            before.erase(at);
            const std::optional<Held> held = held_at(txn, shared.chunks, tag, band, at);
            if (held && held->generator == shared.baking.generator &&
                std::ranges::equal(held->tree, bytes)) {
                continue;
            }
            const std::uint64_t version = impl_->next_version();
            ByteWriter out;
            out.write(shared.baking.generator);
            out.write(version);
            out.write_bytes(bytes);
            shared.chunks.put(txn, as_view(key_of(tag, band, at)), as_view(out.bytes()));
            touched.push_back({.band = band, .root = at, .version = version});
            impl_->log(version, logged(at));
        }
        for (const auto& [at, version] : before) {
            erase(at);
        }
        return touched;
    });
}

Result<void> ChunkStore::Batch::answered(grid::Key node, std::uint8_t depth) {
    if (!node.is_valid()) {
        return make_error(ErrorCode::InvalidArgument, "a node off the grid answered");
    }
    return guarded([&] -> Result<void> {
        impl_->log(impl_->next_version(), {.at = 0,
                                           .what = Logged::What::Answered,
                                           .kind = 0,
                                           .band = 0,
                                           .node = node,
                                           .depth = depth});
        return {};
    });
}

std::vector<Logged> ChunkStore::Snapshot::logged_since(std::uint64_t after,
                                                       std::size_t most) const {
    std::vector<Logged> found;
    const std::optional<lmdb::dbi>& log = impl_->shared->log;
    if (!log || most == 0) {
        return found;
    }
    try {
        lmdb::cursor cursor = lmdb::cursor::open(impl_->txn.handle(), log->handle());
        const LogKey first = log_key(after + 1);
        std::string_view key = as_view(first);
        std::string_view value;
        for (bool more = cursor.get(key, value, MDB_SET_RANGE); more && found.size() < most;
             more = cursor.get(key, value, MDB_NEXT)) {
            if (std::optional<Logged> logged = logged_from(key, value)) {
                found.push_back(*logged);
            }
        }
    } catch (const lmdb::error&) {
        return {};
    }
    return found;
}

std::uint64_t ChunkStore::Snapshot::first_logged() const {
    const std::optional<lmdb::dbi>& log = impl_->shared->log;
    if (!log) {
        return 0;
    }
    try {
        lmdb::cursor cursor = lmdb::cursor::open(impl_->txn.handle(), log->handle());
        std::string_view key;
        std::string_view value;
        return cursor.get(key, value, MDB_FIRST) ? log_at(key) : 0;
    } catch (const lmdb::error&) {
        return 0;
    }
}

std::uint64_t ChunkStore::Snapshot::last_logged() const {
    const std::optional<lmdb::dbi>& log = impl_->shared->log;
    if (!log) {
        return 0;
    }
    try {
        lmdb::cursor cursor = lmdb::cursor::open(impl_->txn.handle(), log->handle());
        std::string_view key;
        std::string_view value;
        return cursor.get(key, value, MDB_LAST) ? log_at(key) : 0;
    } catch (const lmdb::error&) {
        return 0;
    }
}

ChunkStore::ChunkStore(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
ChunkStore::ChunkStore(ChunkStore&&) noexcept = default;
ChunkStore& ChunkStore::operator=(ChunkStore&&) noexcept = default;
ChunkStore::~ChunkStore() = default;

Result<ChunkStore> ChunkStore::open(const std::filesystem::path& directory, const Baking& baking,
                                    const Options& options) {
    if (options.band_height == 0) {
        return make_error(ErrorCode::InvalidArgument, "bands of no levels");
    }
    return guarded([&] -> Result<ChunkStore> {
        std::error_code failed;
        if (!options.read_only) {
            std::filesystem::create_directories(directory, failed);
            if (failed) {
                return make_error(ErrorCode::Io, failed.message());
            }
        }
        auto impl = std::make_unique<Impl>(
            Impl{.scratch = {},
                 .directory = directory,
                 .shared = {.env = lmdb::env{nullptr},
                            .chunks = {},
                            .about = {},
                            .log = {},
                            .options = options,
                            .baking = baking,
                            .bands = Bands{options.band_height, baking.grid.depth()}}});
        Shared& shared = impl->shared;
        shared.env = lmdb::env::create();
        shared.env.set_mapsize(options.map_bytes);
        shared.env.set_max_readers(options.readers);
        shared.env.set_max_dbs(3);
        // Snapshots aren't tied to the thread that took them; reads are scattered, so the OS
        // reading ahead only wastes the page cache.
        unsigned flags = MDB_NOTLS | MDB_NORDAHEAD;
        if (options.read_only) {
            flags |= MDB_RDONLY;
        }
        shared.env.open(directory.string().c_str(), flags, 0644);
        lmdb::txn txn = lmdb::txn::begin(shared.env, nullptr, options.read_only ? MDB_RDONLY : 0U);
        const Result<bool> fresh = open_tables(txn, shared);
        if (!fresh) {
            return std::unexpected{fresh.error()};
        }
        if (Result<void> marked = mark(txn, shared, *fresh); !marked) {
            return std::unexpected{marked.error()};
        }
        txn.commit();
        return ChunkStore{std::move(impl)};
    });
}

Result<ChunkStore> ChunkStore::scratch(const Baking& baking, const Options& options) {
    Result<std::filesystem::path> directory = detail::new_scratch_directory();
    if (!directory) {
        return std::unexpected{directory.error()};
    }
    Options writable = options;
    writable.read_only = false;
    Result<ChunkStore> store = open(*directory, baking, writable);
    if (!store) {
        std::error_code failed;
        std::filesystem::remove_all(*directory, failed);
        return store;
    }
    store->impl_->scratch.directory = *directory;
    return store;
}

const std::filesystem::path& ChunkStore::directory() const noexcept {
    return impl_->directory;
}

const Bands& ChunkStore::bands() const noexcept {
    return impl_->shared.bands;
}

const Baking& ChunkStore::baking() const noexcept {
    return impl_->shared.baking;
}

Result<ChunkStore::Snapshot> ChunkStore::read() const {
    return guarded([&] -> Result<Snapshot> {
        auto impl = std::make_unique<Snapshot::Impl>();
        impl->shared = &impl_->shared;
        impl->txn = lmdb::txn::begin(impl_->shared.env, nullptr, MDB_RDONLY);
        return Snapshot{std::move(impl)};
    });
}

Result<void> ChunkStore::write(const std::function<Result<void>(Batch&)>& body) {
    return guarded([&] -> Result<void> {
        Batch::Impl impl{.shared = &impl_->shared, .txn = lmdb::txn::begin(impl_->shared.env)};
        Batch batch{impl};
        if (Result<void> done = body(batch); !done) {
            impl.txn.abort();
            return done;
        }
        impl.trim();
        impl.txn.commit();
        return {};
    });
}

Result<ChunkStore::Stats> ChunkStore::stats() const {
    return guarded([&] -> Result<Stats> {
        const lmdb::txn txn = lmdb::txn::begin(impl_->shared.env, nullptr, MDB_RDONLY);
        MDB_stat stat{};
        lmdb::dbi_stat(txn, impl_->shared.chunks, &stat);
        MDB_envinfo info{};
        mdb_env_info(impl_->shared.env, &info);
        MDB_stat env_stat{};
        mdb_env_stat(impl_->shared.env, &env_stat);
        return Stats{.chunks = stat.ms_entries,
                     .bytes = (info.me_last_pgno + 1) * std::size_t{env_stat.ms_psize}};
    });
}

template std::optional<Piece<grid::Shape>>
ChunkStore::Snapshot::piece<grid::Shape>(grid::Key, std::uint8_t) const;
template std::optional<Piece<grid::Material>>
ChunkStore::Snapshot::piece<grid::Material>(grid::Key, std::uint8_t) const;
template std::optional<Piece<grid::Water>>
ChunkStore::Snapshot::piece<grid::Water>(grid::Key, std::uint8_t) const;
template std::optional<Piece<grid::Woods>>
ChunkStore::Snapshot::piece<grid::Woods>(grid::Key, std::uint8_t) const;
template std::uint64_t ChunkStore::Snapshot::version<grid::Shape>(std::uint8_t, grid::Key) const;
template std::uint64_t ChunkStore::Snapshot::version<grid::Material>(std::uint8_t, grid::Key) const;
template std::uint64_t ChunkStore::Snapshot::version<grid::Water>(std::uint8_t, grid::Key) const;
template std::uint64_t ChunkStore::Snapshot::version<grid::Woods>(std::uint8_t, grid::Key) const;
template std::vector<Chunk> ChunkStore::Snapshot::chunks<grid::Shape>(std::uint8_t,
                                                                      grid::Key) const;
template bool ChunkStore::Snapshot::holds<grid::Shape>(std::uint8_t, grid::Key) const;
template std::vector<StoredChunk> ChunkStore::Snapshot::stored<grid::Shape>(std::uint8_t,
                                                                            grid::Key) const;
template std::vector<Chunk> ChunkStore::Snapshot::chunks<grid::Material>(std::uint8_t,
                                                                         grid::Key) const;
template bool ChunkStore::Snapshot::holds<grid::Material>(std::uint8_t, grid::Key) const;
template std::vector<StoredChunk> ChunkStore::Snapshot::stored<grid::Material>(std::uint8_t,
                                                                               grid::Key) const;
template std::vector<Chunk> ChunkStore::Snapshot::chunks<grid::Water>(std::uint8_t,
                                                                      grid::Key) const;
template bool ChunkStore::Snapshot::holds<grid::Water>(std::uint8_t, grid::Key) const;
template std::vector<StoredChunk> ChunkStore::Snapshot::stored<grid::Water>(std::uint8_t,
                                                                            grid::Key) const;
template std::vector<Chunk> ChunkStore::Snapshot::chunks<grid::Woods>(std::uint8_t,
                                                                      grid::Key) const;
template bool ChunkStore::Snapshot::holds<grid::Woods>(std::uint8_t, grid::Key) const;
template std::vector<StoredChunk> ChunkStore::Snapshot::stored<grid::Woods>(std::uint8_t,
                                                                            grid::Key) const;
template Result<std::vector<Chunk>>
ChunkStore::Batch::put<grid::Shape>(std::uint8_t, grid::Key, const grid::Tree<grid::Shape>&);
template Result<std::vector<Chunk>>
ChunkStore::Batch::put<grid::Material>(std::uint8_t, grid::Key, const grid::Tree<grid::Material>&);
template Result<std::vector<Chunk>>
ChunkStore::Batch::put<grid::Water>(std::uint8_t, grid::Key, const grid::Tree<grid::Water>&);
template Result<std::vector<Chunk>>
ChunkStore::Batch::put<grid::Woods>(std::uint8_t, grid::Key, const grid::Tree<grid::Woods>&);

} // namespace hog::baked
