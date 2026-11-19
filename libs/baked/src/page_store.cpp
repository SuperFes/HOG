#include "lmdb_shared.hpp"

#include <hog/baked/page_store.hpp>
#include <hog/core/bytes.hpp>

#include <lmdb++.h>

#include <algorithm>
#include <array>
#include <bit>
#include <format>
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

constexpr const char* trees_name = "trees";
constexpr const char* about_name = "about";
constexpr std::string_view format_key = "format";
constexpr std::string_view world_key = "world";

struct Shared {
    lmdb::env env{nullptr};
    lmdb::dbi trees;
    lmdb::dbi about;
    PageStore::Options options;
    Baking baking;
};

// A tree's key: which kind, then its page's root as its depth and Morton code, big-endian, so a
// kind's pages of one depth lie in Z order.
using TreeKey = std::array<std::byte, 10>;

TreeKey key_of(std::uint8_t tag, grid::Key root) noexcept {
    TreeKey key{std::byte{tag}, std::byte{root.depth}};
    const std::uint64_t code = grid::morton(root);
    for (std::size_t k = 0; k < 8; ++k) {
        key.at(2 + k) = static_cast<std::byte>(code >> (8U * (7U - k)));
    }
    return key;
}

// A tree as held: the generator version that made it, its own version, then its bytes.
struct Held {
    std::uint32_t generator = 0;
    std::uint32_t version = 0;
    std::span<const std::byte> tree;
};

std::optional<Held> held_at(const lmdb::txn& txn, const lmdb::dbi& trees, const TreeKey& key) {
    std::string_view value;
    if (!get(txn, trees, as_view(key), value)) {
        return std::nullopt;
    }
    ByteReader in{as_bytes(value)};
    const auto generator = in.read<std::uint32_t>();
    const auto version = in.read<std::uint32_t>();
    if (!generator || !version) {
        return std::nullopt;
    }
    return Held{.generator = *generator,
                .version = *version,
                .tree = as_bytes(value).subspan(2 * sizeof(std::uint32_t))};
}

bool is_page_root(const grid::Grid& grid, grid::Key key) noexcept {
    return key.is_valid() && key.depth < grid.depth() && grid.page_of(key.child(0)) == key;
}

std::vector<std::byte> world_bytes(const Baking& baking) {
    ByteWriter out;
    out.write(baking.world);
    out.write(baking.grid.depth());
    out.write(std::bit_cast<std::uint64_t>(baking.grid.finest()));
    return std::move(out).take();
}

std::vector<std::byte> format_bytes() {
    ByteWriter out;
    out.write(PageStore::format);
    return std::move(out).take();
}

// Takes the store's marks where it's new, or checks them where it isn't.
Result<void> mark(const lmdb::txn& txn, Shared& shared, bool fresh) {
    const std::vector<std::byte> format = format_bytes();
    const std::vector<std::byte> world = world_bytes(shared.baking);
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
                          std::format("page store format {}", version ? *version : 0U));
    }
    if (!get(txn, shared.about, world_key, held) || held != as_view(world)) {
        return make_error(ErrorCode::InvalidArgument, "a page store of another world or grid");
    }
    return {};
}

} // namespace

struct PageStore::Impl {
    // Ahead of the store, so it's removed only once the store is closed.
    Removed scratch;
    std::filesystem::path directory;
    Shared shared;
};

struct PageStore::Snapshot::Impl {
    const Shared* shared = nullptr;
    lmdb::txn txn{nullptr};
};

struct PageStore::Batch::Impl {
    Shared* shared = nullptr;
    lmdb::txn txn{nullptr};
};

PageStore::Snapshot::Snapshot(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
PageStore::Snapshot::Snapshot(Snapshot&&) noexcept = default;
PageStore::Snapshot& PageStore::Snapshot::operator=(Snapshot&&) noexcept = default;
PageStore::Snapshot::~Snapshot() = default;

std::optional<PageStore::Read> PageStore::Snapshot::page(grid::Key root) const {
    const Shared& shared = *impl_->shared;
    Read read{.page = {.root = root,
                       .generator = shared.baking.generator,
                       .shape = {},
                       .material = {},
                       .water = {},
                       .woods = {}},
              .stale = {}};
    bool any = false;
    const auto take = [&]<typename Kind>(std::optional<grid::Held<Kind>>& tree, bool& stale) {
        const std::optional<Held> held =
            held_at(impl_->txn, shared.trees, key_of(tag_of<Kind>(), root));
        if (!held) {
            return;
        }
        any = true;
        if (held->generator != shared.baking.generator) {
            stale = true;
            return;
        }
        if (auto decoded = grid::Tree<Kind>::decode(held->tree)) {
            tree = grid::Held<Kind>{.version = held->version, .tree = std::move(*decoded)};
        }
    };
    try {
        take(read.page.shape, read.stale.shape);
        take(read.page.material, read.stale.material);
        take(read.page.water, read.stale.water);
        take(read.page.woods, read.stale.woods);
    } catch (const lmdb::error&) {
        return std::nullopt;
    }
    if (!any) {
        return std::nullopt;
    }
    return read;
}

PageStore::Versions PageStore::Snapshot::versions(grid::Key root) const {
    const Shared& shared = *impl_->shared;
    Versions versions;
    const auto take = [&]<typename Kind>(std::uint32_t& version) {
        const std::optional<Held> held =
            held_at(impl_->txn, shared.trees, key_of(tag_of<Kind>(), root));
        if (held && held->generator == shared.baking.generator) {
            version = held->version;
        }
    };
    try {
        take.template operator()<grid::Shape>(versions.shape);
        take.template operator()<grid::Material>(versions.material);
        take.template operator()<grid::Water>(versions.water);
        take.template operator()<grid::Woods>(versions.woods);
    } catch (const lmdb::error&) {
        return {};
    }
    return versions;
}

template <typename Kind>
std::optional<grid::Held<Kind>> PageStore::Snapshot::tree(grid::Key root) const {
    const Shared& shared = *impl_->shared;
    try {
        const std::optional<Held> held =
            held_at(impl_->txn, shared.trees, key_of(tag_of<Kind>(), root));
        if (!held || held->generator != shared.baking.generator) {
            return std::nullopt;
        }
        auto decoded = grid::Tree<Kind>::decode(held->tree);
        if (!decoded) {
            return std::nullopt;
        }
        return grid::Held<Kind>{.version = held->version, .tree = std::move(*decoded)};
    } catch (const lmdb::error&) {
        return std::nullopt;
    }
}

template std::optional<grid::Held<grid::Shape>>
    PageStore::Snapshot::tree<grid::Shape>(grid::Key) const;
template std::optional<grid::Held<grid::Material>>
    PageStore::Snapshot::tree<grid::Material>(grid::Key) const;
template std::optional<grid::Held<grid::Water>>
    PageStore::Snapshot::tree<grid::Water>(grid::Key) const;
template std::optional<grid::Held<grid::Woods>>
    PageStore::Snapshot::tree<grid::Woods>(grid::Key) const;

template <typename Kind>
std::optional<grid::Key> PageStore::Snapshot::holder(grid::Key key) const {
    const Shared& shared = *impl_->shared;
    const grid::Grid& grid = shared.baking.grid;
    if (!key.is_valid() || key.depth > grid.depth()) {
        return std::nullopt;
    }
    grid::Key root = key.depth < grid.depth() ? grid.page_of(key.child(0)) : grid.page_of(key);
    try {
        for (;;) {
            if (const std::optional<Held> held =
                    held_at(impl_->txn, shared.trees, key_of(tag_of<Kind>(), root));
                held && held->generator == shared.baking.generator) {
                return root;
            }
            if (root.depth == 0) {
                return std::nullopt;
            }
            root = grid.page_of(root);
        }
    } catch (const lmdb::error&) {
        return std::nullopt;
    }
}

template std::optional<grid::Key> PageStore::Snapshot::holder<grid::Shape>(grid::Key) const;
template std::optional<grid::Key> PageStore::Snapshot::holder<grid::Material>(grid::Key) const;
template std::optional<grid::Key> PageStore::Snapshot::holder<grid::Water>(grid::Key) const;
template std::optional<grid::Key> PageStore::Snapshot::holder<grid::Woods>(grid::Key) const;

Result<void> PageStore::Snapshot::refresh() {
    return guarded([&] -> Result<void> {
        impl_->txn.reset();
        impl_->txn.renew();
        return {};
    });
}

template <typename Kind>
Result<std::uint32_t> PageStore::Batch::put(grid::Key root, const grid::Tree<Kind>& tree) {
    Shared& shared = *impl_->shared;
    const grid::Grid& grid = shared.baking.grid;
    if (!is_page_root(grid, root)) {
        return make_error(ErrorCode::InvalidArgument, "a tree must be held at a page's root");
    }
    if (tree.levels() > grid.levels_under(root.depth)) {
        return make_error(ErrorCode::InvalidArgument, "a tree deeper than its page");
    }
    const std::vector<std::byte> bytes = tree.encode();
    const TreeKey key = key_of(tag_of<Kind>(), root);
    return guarded([&] -> Result<std::uint32_t> {
        std::uint32_t version = 1;
        if (const std::optional<Held> held = held_at(impl_->txn, shared.trees, key)) {
            const bool same = std::ranges::equal(held->tree, bytes);
            if (same && held->generator == shared.baking.generator) {
                return held->version;
            }
            version = same ? held->version : held->version + 1;
        }
        ByteWriter out;
        out.write(shared.baking.generator);
        out.write(version);
        out.write_bytes(bytes);
        shared.trees.put(impl_->txn, as_view(key), as_view(out.bytes()));
        return version;
    });
}

template Result<std::uint32_t> PageStore::Batch::put<grid::Shape>(grid::Key,
                                                                  const grid::Tree<grid::Shape>&);
template Result<std::uint32_t>
PageStore::Batch::put<grid::Material>(grid::Key, const grid::Tree<grid::Material>&);
template Result<std::uint32_t> PageStore::Batch::put<grid::Water>(grid::Key,
                                                                  const grid::Tree<grid::Water>&);
template Result<std::uint32_t> PageStore::Batch::put<grid::Woods>(grid::Key,
                                                                  const grid::Tree<grid::Woods>&);

PageStore::PageStore(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
PageStore::PageStore(PageStore&&) noexcept = default;
PageStore& PageStore::operator=(PageStore&&) noexcept = default;
PageStore::~PageStore() = default;

Result<PageStore> PageStore::open(const std::filesystem::path& directory, const Baking& baking,
                                  const Options& options) {
    return guarded([&] -> Result<PageStore> {
        std::error_code failed;
        if (!options.read_only) {
            std::filesystem::create_directories(directory, failed);
            if (failed) {
                return make_error(ErrorCode::Io, failed.message());
            }
        }
        auto impl = std::make_unique<Impl>(Impl{.scratch = {},
                                                .directory = directory,
                                                .shared = {.env = lmdb::env{nullptr},
                                                           .trees = {},
                                                           .about = {},
                                                           .options = options,
                                                           .baking = baking}});
        Shared& shared = impl->shared;
        shared.env = lmdb::env::create();
        shared.env.set_mapsize(options.map_bytes);
        shared.env.set_max_readers(options.readers);
        shared.env.set_max_dbs(2);
        // Snapshots aren't tied to the thread that took them; reads are scattered, so the OS
        // reading ahead only wastes the page cache.
        unsigned flags = MDB_NOTLS | MDB_NORDAHEAD;
        if (options.read_only) {
            flags |= MDB_RDONLY;
        }
        shared.env.open(directory.string().c_str(), flags, 0644);
        lmdb::txn txn = lmdb::txn::begin(shared.env, nullptr, options.read_only ? MDB_RDONLY : 0U);
        // Named databases are keys of the unnamed one: anything else there is another store's.
        const lmdb::dbi names = lmdb::dbi::open(txn, nullptr);
        std::string_view held;
        const bool fresh = !get(txn, names, about_name, held);
        if (fresh) {
            MDB_stat stat{};
            lmdb::dbi_stat(txn, names, &stat);
            if (stat.ms_entries != 0) {
                return make_error(ErrorCode::Unsupported, "not a page store");
            }
            if (options.read_only) {
                return make_error(ErrorCode::NotFound, "no page store");
            }
        }
        const unsigned create = fresh ? MDB_CREATE : 0U;
        shared.about = lmdb::dbi::open(txn, about_name, create);
        shared.trees = lmdb::dbi::open(txn, trees_name, create);
        if (Result<void> marked = mark(txn, shared, fresh); !marked) {
            return std::unexpected{marked.error()};
        }
        txn.commit();
        return PageStore{std::move(impl)};
    });
}

Result<PageStore> PageStore::scratch(const Baking& baking, const Options& options) {
    Result<std::filesystem::path> directory = detail::new_scratch_directory();
    if (!directory) {
        return std::unexpected{directory.error()};
    }
    Options writable = options;
    writable.read_only = false;
    Result<PageStore> store = open(*directory, baking, writable);
    if (!store) {
        std::error_code failed;
        std::filesystem::remove_all(*directory, failed);
        return store;
    }
    store->impl_->scratch.directory = *directory;
    return store;
}

const std::filesystem::path& PageStore::directory() const noexcept {
    return impl_->directory;
}

Result<PageStore::Snapshot> PageStore::read() const {
    return guarded([&] -> Result<Snapshot> {
        auto impl = std::make_unique<Snapshot::Impl>();
        impl->shared = &impl_->shared;
        impl->txn = lmdb::txn::begin(impl_->shared.env, nullptr, MDB_RDONLY);
        return Snapshot{std::move(impl)};
    });
}

Result<void> PageStore::write(const std::function<Result<void>(Batch&)>& body) {
    return guarded([&] -> Result<void> {
        Batch::Impl impl{.shared = &impl_->shared, .txn = lmdb::txn::begin(impl_->shared.env)};
        Batch batch{impl};
        if (Result<void> done = body(batch); !done) {
            impl.txn.abort();
            return done;
        }
        impl.txn.commit();
        return {};
    });
}

Result<PageStore::Stats> PageStore::stats() const {
    return guarded([&] -> Result<Stats> {
        const lmdb::txn txn = lmdb::txn::begin(impl_->shared.env, nullptr, MDB_RDONLY);
        MDB_stat stat{};
        lmdb::dbi_stat(txn, impl_->shared.trees, &stat);
        MDB_envinfo info{};
        mdb_env_info(impl_->shared.env, &info);
        MDB_stat env_stat{};
        mdb_env_stat(impl_->shared.env, &env_stat);
        return Stats{.trees = stat.ms_entries,
                     .bytes = (info.me_last_pgno + 1) * std::size_t{env_stat.ms_psize}};
    });
}

} // namespace hog::baked
