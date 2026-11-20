#include <hog/client/chunk_cache.hpp>
#include <hog/grid/z_order.hpp>

#include <lmdb++.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <utility>

namespace hog::client {
namespace {

// Bumped when what's kept changes shape; a cache of another is emptied.
constexpr std::uint64_t cache_format = 1;
constexpr std::size_t kinds = 4;

// A chunk's key, ordered as the server's store orders them: kind, band, then its root's aligned
// code, big-endian, and depth, so a node's chunks under it lie together after it.
using Key = std::array<std::byte, 11>;
// When a chunk was used, then its key, so the oldest come first.
using UsedKey = std::array<std::byte, 19>;

void put_u64(std::span<std::byte> out, std::uint64_t value) noexcept {
    for (std::size_t k = 0; k < 8; ++k) {
        out[k] = static_cast<std::byte>(value >> (8U * (7U - k)));
    }
}

std::uint64_t get_u64(std::span<const std::byte> in) noexcept {
    std::uint64_t value = 0;
    for (std::size_t k = 0; k < 8; ++k) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(in[k]);
    }
    return value;
}

std::span<const std::byte> bytes_of(std::string_view view) noexcept {
    return std::as_bytes(std::span{view});
}

Key key_of(std::uint8_t kind, std::uint8_t band, grid::Key root) noexcept {
    Key key{};
    key[0] = std::byte{kind};
    key[1] = std::byte{band};
    put_u64(std::span{key}.subspan(2, 8), grid::aligned(root));
    key[10] = std::byte{root.depth};
    return key;
}

struct Parsed {
    std::uint8_t kind = 0;
    std::uint8_t band = 0;
    grid::Key root;
};

std::optional<Parsed> parse(std::string_view bytes) noexcept {
    if (bytes.size() != std::tuple_size_v<Key>) {
        return std::nullopt;
    }
    const std::span<const std::byte> in = bytes_of(bytes);
    const auto depth = std::to_integer<std::uint8_t>(in[10]);
    if (depth > grid::max_depth) {
        return std::nullopt;
    }
    const std::uint64_t bits = get_u64(in.subspan(2, 8)) >> (3U * (grid::max_depth - depth));
    return Parsed{.kind = std::to_integer<std::uint8_t>(in[0]),
                  .band = std::to_integer<std::uint8_t>(in[1]),
                  .root = {.depth = depth,
                           .x = grid::gather(bits),
                           .y = grid::gather(bits >> 1U),
                           .z = grid::gather(bits >> 2U)}};
}

template <std::size_t N>
std::string_view view(const std::array<std::byte, N>& bytes) noexcept {
    return {reinterpret_cast<const char*>(bytes.data()), N}; // NOLINT(*-reinterpret-cast)
}

UsedKey used_key(std::uint64_t used, const Key& key) noexcept {
    UsedKey out{};
    put_u64(out, used);
    std::ranges::copy(key, std::span{out}.subspan(8).begin());
    return out;
}

std::array<std::byte, 8> u64_bytes(std::uint64_t value) noexcept {
    std::array<std::byte, 8> out{};
    put_u64(out, value);
    return out;
}

std::uint64_t u64_of(std::string_view bytes) noexcept {
    return bytes.size() == 8 ? get_u64(bytes_of(bytes)) : 0;
}

Error cache_error(const lmdb::error& error) {
    return Error{.code = ErrorCode::Io,
                 .message = std::format("the chunk cache: {}", error.what())};
}

} // namespace

struct ChunkCache::Impl {
    lmdb::env env{nullptr};
    lmdb::dbi chunks{0}; // key: version then tree
    lmdb::dbi stamps{0}; // key: when it was used
    lmdb::dbi used{0};   // when used and key: nothing
    lmdb::dbi about{0};  // format, bytes, count, clock, highest band
    std::uint64_t budget = 0;
    std::uint64_t bytes = 0;
    std::uint64_t count = 0;
    std::uint64_t clock = 0;
    std::uint8_t bands = 0; // one past the highest band kept

    void save(lmdb::txn& txn) {
        for (const auto& [name, value] : std::array<std::pair<std::string_view, std::uint64_t>, 4>{
                 {{"bytes", bytes}, {"count", count}, {"clock", clock}, {"bands", bands}}}) {
            about.put(txn, name, view(u64_bytes(value)));
        }
    }

    // Has the chunk at `key` used now.
    void stamp(lmdb::txn& txn, const Key& key) {
        std::string_view was;
        if (stamps.get(txn, view(key), was)) {
            used.del(txn, view(used_key(u64_of(was), key)));
        }
        ++clock;
        stamps.put(txn, view(key), view(u64_bytes(clock)));
        used.put(txn, view(used_key(clock, key)), std::string_view{});
    }

    // Lets go of the chunk at `key`, if it's kept.
    void drop(lmdb::txn& txn, const Key& key) {
        std::string_view value;
        if (!chunks.get(txn, view(key), value)) {
            return;
        }
        bytes -= std::min<std::uint64_t>(bytes, value.size() - 8);
        --count;
        chunks.del(txn, view(key));
        std::string_view was;
        if (stamps.get(txn, view(key), was)) {
            used.del(txn, view(used_key(u64_of(was), key)));
            stamps.del(txn, view(key));
        }
    }

    // Lets go of what was used longest ago while over the budget.
    void trim(lmdb::txn& txn) {
        while (bytes > budget) {
            lmdb::cursor cursor = lmdb::cursor::open(txn, used);
            std::string_view first;
            std::string_view nothing;
            if (!cursor.get(first, nothing, MDB_FIRST) ||
                first.size() != std::tuple_size_v<UsedKey>) {
                return;
            }
            Key key{};
            std::ranges::copy(bytes_of(first).subspan(8), key.begin());
            cursor.close();
            drop(txn, key);
        }
    }

    // Into `found`, the band's chunk of the kind whose root holds `node`, if one's kept.
    void over(const lmdb::txn& txn, std::uint8_t kind, std::uint8_t band, grid::Key node,
              std::set<Key>& found) const {
        lmdb::cursor cursor = lmdb::cursor::open(txn, chunks);
        const Key wanted = key_of(kind, band, node);
        std::string_view key = view(wanted);
        std::string_view value;
        // A band's chunks never overlap, so the one holding the node is the last at or before it.
        if (cursor.get(key, value, MDB_SET_RANGE)) {
            if (key != view(wanted) && !cursor.get(key, value, MDB_PREV)) {
                return;
            }
        } else if (!cursor.get(key, value, MDB_LAST)) {
            return;
        }
        const std::optional<Parsed> parsed = parse(key);
        if (parsed && parsed->kind == kind && parsed->band == band && parsed->root.contains(node)) {
            found.insert(key_of(kind, band, parsed->root));
        }
    }

    // Into `found`, the band's chunks of the kind rooted under `node`, no deeper than `depth`.
    void under(const lmdb::txn& txn, std::uint8_t kind, std::uint8_t band, grid::Key node,
               std::uint8_t depth, std::set<Key>& found) const {
        if (node.depth >= grid::max_depth || node.depth >= depth) {
            return;
        }
        lmdb::cursor cursor = lmdb::cursor::open(txn, chunks);
        Key first = key_of(kind, band, node);
        first.back() = std::byte{static_cast<std::uint8_t>(node.depth + 1U)};
        const std::uint64_t end = grid::aligned_end(node);
        std::string_view key = view(first);
        std::string_view value;
        for (bool more = cursor.get(key, value, MDB_SET_RANGE); more;
             more = cursor.get(key, value, MDB_NEXT)) {
            const std::optional<Parsed> parsed = parse(key);
            if (!parsed || parsed->kind != kind || parsed->band != band ||
                grid::aligned(parsed->root) > end) {
                break;
            }
            if (parsed->root.depth <= depth && node.contains(parsed->root)) {
                found.insert(key_of(kind, band, parsed->root));
            }
        }
    }
};

ChunkCache::ChunkCache(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
ChunkCache::ChunkCache(ChunkCache&&) noexcept = default;
ChunkCache& ChunkCache::operator=(ChunkCache&&) noexcept = default;
ChunkCache::~ChunkCache() = default;

Result<ChunkCache> ChunkCache::open(const std::filesystem::path& file, std::uint64_t budget) {
    try {
        std::error_code made;
        std::filesystem::create_directories(file.parent_path(), made);
        auto impl = std::make_unique<Impl>();
        impl->budget = budget;
        impl->env = lmdb::env::create();
        // Room for the budget's trees and their keys several times over; only address space
        // until it's written.
        impl->env.set_mapsize(std::max<std::uint64_t>(budget * 4, std::uint64_t{256} << 20U));
        impl->env.set_max_dbs(4);
        // A cache: losing the last writes to a crash costs only asking again.
        impl->env.open(file.string().c_str(),
                       MDB_NOSUBDIR | MDB_NOSYNC | MDB_NOMETASYNC | MDB_NOTLS | MDB_NORDAHEAD,
                       0644);
        lmdb::txn txn = lmdb::txn::begin(impl->env);
        impl->chunks = lmdb::dbi::open(txn, "chunks", MDB_CREATE);
        impl->stamps = lmdb::dbi::open(txn, "stamps", MDB_CREATE);
        impl->used = lmdb::dbi::open(txn, "used", MDB_CREATE);
        impl->about = lmdb::dbi::open(txn, "about", MDB_CREATE);
        std::string_view value;
        const std::uint64_t format = impl->about.get(txn, "format", value) ? u64_of(value) : 0;
        if (format != cache_format) {
            for (const lmdb::dbi* table :
                 {&impl->chunks, &impl->stamps, &impl->used, &impl->about}) {
                lmdb::dbi_drop(txn, table->handle(), false);
            }
            impl->about.put(txn, "format", view(u64_bytes(cache_format)));
            impl->save(txn);
        } else {
            const auto read = [&](std::string_view name) {
                std::string_view found;
                return impl->about.get(txn, name, found) ? u64_of(found) : 0;
            };
            impl->bytes = read("bytes");
            impl->count = read("count");
            impl->clock = read("clock");
            impl->bands = static_cast<std::uint8_t>(read("bands"));
        }
        txn.commit();
        return ChunkCache{std::move(impl)};
    } catch (const lmdb::error& error) {
        return std::unexpected{cache_error(error)};
    }
}

Result<void> ChunkCache::keep(std::span<const protocol::SentChunk> chunks) {
    Impl& impl = *impl_;
    try {
        lmdb::txn txn = lmdb::txn::begin(impl.env);
        for (const protocol::SentChunk& chunk : chunks) {
            const grid::Key root{.depth = chunk.depth, .x = chunk.x, .y = chunk.y, .z = chunk.z};
            const Key key = key_of(chunk.kind, chunk.band, root);
            impl.drop(txn, key);
            if (chunk.version == 0 || chunk.kind >= kinds) {
                continue;
            }
            std::vector<std::byte> value(8 + chunk.tree.size());
            put_u64(value, chunk.version);
            std::ranges::copy(chunk.tree, value.begin() + 8);
            impl.chunks.put(
                txn, view(key),
                std::string_view{reinterpret_cast<const char*>( // NOLINT(*-reinterpret-cast)
                                     value.data()),
                                 value.size()});
            impl.stamp(txn, key);
            impl.bytes += chunk.tree.size();
            ++impl.count;
            impl.bands = std::max(impl.bands, static_cast<std::uint8_t>(chunk.band + 1U));
        }
        impl.trim(txn);
        impl.save(txn);
        txn.commit();
        return {};
    } catch (const lmdb::error& error) {
        return std::unexpected{cache_error(error)};
    }
}

Result<std::vector<protocol::SentChunk>> ChunkCache::read(std::span<const Wanted> wanted) {
    Impl& impl = *impl_;
    try {
        lmdb::txn txn = lmdb::txn::begin(impl.env);
        std::set<Key> found;
        for (const Wanted& each : wanted) {
            for (std::uint8_t kind = 0; kind < kinds; ++kind) {
                for (std::uint8_t band = 0; band < impl.bands; ++band) {
                    impl.over(txn, kind, band, each.node, found);
                    impl.under(txn, kind, band, each.node, each.depth, found);
                }
            }
        }
        std::vector<protocol::SentChunk> read;
        read.reserve(found.size());
        for (const Key& key : found) {
            std::string_view value;
            if (!impl.chunks.get(txn, view(key), value) || value.size() < 8) {
                continue;
            }
            const std::optional<Parsed> parsed = parse(view(key));
            if (!parsed) {
                continue;
            }
            const std::span<const std::byte> bytes = bytes_of(value);
            read.push_back({.kind = parsed->kind,
                            .band = parsed->band,
                            .depth = parsed->root.depth,
                            .x = parsed->root.x,
                            .y = parsed->root.y,
                            .z = parsed->root.z,
                            .version = get_u64(bytes),
                            .tree = {bytes.begin() + 8, bytes.end()}});
            impl.stamp(txn, key);
        }
        impl.save(txn);
        txn.commit();
        return read;
    } catch (const lmdb::error& error) {
        return std::unexpected{cache_error(error)};
    }
}

Result<std::vector<protocol::CheckedChunk>> take_kept(ChunkCache& cache, HeldPieces& held,
                                                      std::span<const PieceWish> wishes,
                                                      Looked& looked, std::size_t most) {
    std::vector<ChunkCache::Wanted> wanted;
    for (const PieceWish& wish : wishes) {
        if (wanted.size() == most) {
            break;
        }
        if (looked.emplace(wish.node, wish.depth).second) {
            wanted.push_back({.node = wish.node, .depth = wish.depth});
        }
    }
    std::vector<protocol::CheckedChunk> taken;
    if (wanted.empty()) {
        return taken;
    }
    auto read = cache.read(wanted);
    if (!read) {
        return std::unexpected{read.error()};
    }
    taken.reserve(read->size());
    for (const protocol::SentChunk& chunk : *read) {
        // One that doesn't decode is asked for afresh.
        if (!held.take(chunk)) {
            continue;
        }
        taken.push_back({.kind = chunk.kind,
                         .band = chunk.band,
                         .depth = chunk.depth,
                         .x = chunk.x,
                         .y = chunk.y,
                         .z = chunk.z,
                         .version = chunk.version});
    }
    return taken;
}

std::uint64_t ChunkCache::bytes() const noexcept {
    return impl_->bytes;
}

std::uint64_t ChunkCache::chunks() const noexcept {
    return impl_->count;
}

} // namespace hog::client
