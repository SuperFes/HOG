#include <hog/baked/store.hpp>

#include <lmdb++.h>

#include <string>
#include <string_view>
#include <utility>

namespace hog::baked {
namespace {

constexpr std::string_view header_key = "header";

struct Shared {
    lmdb::env env{nullptr};
    lmdb::dbi pages;
    lmdb::dbi meta;
    Options options;
};

std::string_view as_view(std::span<const std::byte> bytes) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::span<const std::byte> as_bytes(std::string_view view) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const std::byte*>(view.data()), view.size()};
}

Error error_of(const lmdb::error& error) {
    return Error{.code = ErrorCode::Io, .message = error.what()};
}

// Runs `body`, turning what LMDB throws into an error.
template <typename Body>
auto guarded(const Body& body) -> decltype(body()) {
    try {
        return body();
    } catch (const lmdb::error& error) {
        return std::unexpected{error_of(error)};
    }
}

// The record holding `p`, the greatest key no greater than any key it could have.
std::optional<Record> holding(lmdb::cursor& cursor, Point p) {
    const Key search = key_at(p);
    std::string_view key = as_view(search);
    std::string_view value;
    const bool after = cursor.get(key, value, MDB_SET_RANGE);
    key = {};
    if (!(after ? cursor.get(key, value, MDB_PREV) : cursor.get(key, value, MDB_LAST))) {
        return std::nullopt;
    }
    const std::optional<Node> node = node_of(as_bytes(key));
    if (!node || !node->contains(p)) {
        return std::nullopt;
    }
    const Result<Page> page = Page::view(as_bytes(value));
    if (!page) {
        return std::nullopt;
    }
    return Record{.node = *node, .page = *page};
}

// Each record from the first key in `node` to its last, in order.
template <typename Visit>
void within(lmdb::cursor& cursor, const Node& node, const Visit& visit) {
    const Key first =
        key_of(Node{.face = node.face, .size = 0, .x = node.x, .y = node.y, .z = node.z});
    const Key last = last_key_in(node);
    std::string_view key = as_view(first);
    std::string_view value;
    for (bool found = cursor.get(key, value, MDB_SET_RANGE); found && key <= as_view(last);
         found = cursor.get(key, value, MDB_NEXT)) {
        if (!visit(key, value)) {
            return;
        }
    }
}

} // namespace

struct Store::Impl {
    Shared shared;
};

struct Snapshot::Impl {
    const Shared* shared = nullptr;
    lmdb::txn txn{nullptr};
    lmdb::cursor cursor{nullptr};
};

struct Batch::Impl {
    Shared* shared = nullptr;
    lmdb::txn txn{nullptr};

    [[nodiscard]] std::optional<Record> record(Point p) const {
        lmdb::cursor cursor = lmdb::cursor::open(txn, shared->pages);
        return holding(cursor, p);
    }

    // Writes a page as is, as its halves where it's too large.
    void store(const Node& node, std::span<const std::byte> bytes) const {
        struct Part {
            Node node;
            std::vector<std::byte> bytes;
        };
        std::vector<Part> parts{{.node = node, .bytes = {bytes.begin(), bytes.end()}}};
        while (!parts.empty()) {
            const Part part = std::move(parts.back());
            parts.pop_back();
            const Page page = *Page::view(part.bytes);
            if (part.bytes.size() <= shared->options.page_bytes || page.size() == page.voxel()) {
                const Key key = key_of(part.node);
                shared->pages.put(txn, as_view(key), as_view(part.bytes));
                continue;
            }
            for (std::size_t k = 0; k < 8; ++k) {
                const Node half = part.node.child(k);
                parts.push_back(
                    {.node = half,
                     .bytes = encode_part(page, half.x - part.node.x, half.y - part.node.y,
                                          half.z - part.node.z, half.size)});
            }
        }
    }

    // Cuts any record holding more than `node` down until one is just `node`, or none holds it.
    void cut_to(const Node& node) const {
        for (;;) {
            const std::optional<Record> holder =
                record(Point{.face = node.face, .x = node.x, .y = node.y, .z = node.z});
            if (!holder || holder->node.size <= node.size) {
                return;
            }
            const Node whole = holder->node;
            // The page lies in the map, which writing may move.
            const std::vector<std::byte> bytes{holder->page.bytes().begin(),
                                               holder->page.bytes().end()};
            const Page page = *Page::view(bytes);
            const Key key = key_of(whole);
            shared->pages.del(txn, as_view(key));
            for (std::size_t k = 0; k < 8; ++k) {
                const Node half = whole.child(k);
                const std::vector<std::byte> part = encode_part(
                    page, half.x - whole.x, half.y - whole.y, half.z - whole.z, half.size);
                const Key half_key = key_of(half);
                shared->pages.put(txn, as_view(half_key), as_view(part));
            }
        }
    }

    void erase(const Node& node) const {
        cut_to(node);
        lmdb::cursor cursor = lmdb::cursor::open(txn, shared->pages);
        const Key first =
            key_of(Node{.face = node.face, .size = 0, .x = node.x, .y = node.y, .z = node.z});
        const Key last = last_key_in(node);
        // Sought again after each delete: reading on from a deleted last key fails.
        for (;;) {
            std::string_view key = as_view(first);
            std::string_view value;
            if (!cursor.get(key, value, MDB_SET_RANGE) || key > as_view(last)) {
                return;
            }
            cursor.del();
        }
    }
};

Snapshot::Snapshot(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
Snapshot::Snapshot(Snapshot&&) noexcept = default;
Snapshot& Snapshot::operator=(Snapshot&&) noexcept = default;
Snapshot::~Snapshot() = default;

std::optional<Record> Snapshot::record(Point p) const {
    try {
        return holding(impl_->cursor, p);
    } catch (const lmdb::error&) {
        return std::nullopt;
    }
}

std::optional<Leaf> Snapshot::at(Point p) const {
    const std::optional<Record> found = record(p);
    if (!found) {
        return std::nullopt;
    }
    const Node& node = found->node;
    return found->page.at(p.x - node.x, p.y - node.y, p.z - node.z);
}

void Snapshot::each(const Node& node, const std::function<void(const Record&)>& visit) const {
    if (const std::optional<Record> holder =
            record(Point{.face = node.face, .x = node.x, .y = node.y, .z = node.z});
        holder && holder->node.size > node.size) {
        visit(*holder);
        return;
    }
    within(impl_->cursor, node, [&](std::string_view key, std::string_view value) {
        const std::optional<Node> at = node_of(as_bytes(key));
        const Result<Page> page = Page::view(as_bytes(value));
        if (at && page) {
            visit(Record{.node = *at, .page = *page});
        }
        return true;
    });
}

Result<void> Snapshot::refresh() {
    return guarded([&] -> Result<void> {
        impl_->txn.reset();
        impl_->txn.renew();
        impl_->cursor.renew(impl_->txn);
        return {};
    });
}

Result<void> Batch::set_header(std::span<const std::byte> header) {
    return guarded([&] -> Result<void> {
        impl_->shared->meta.put(impl_->txn, header_key, as_view(header));
        return {};
    });
}

Result<void> Batch::put(const Node& node, std::span<const std::byte> page) {
    const Result<Page> view = Page::view(page);
    if (!view) {
        return std::unexpected{view.error()};
    }
    if (view->size() != node.size || node.face >= 6) {
        return make_error(ErrorCode::InvalidArgument, "a page must be its node's size");
    }
    return guarded([&] -> Result<void> {
        impl_->erase(node);
        impl_->store(node, page);
        return {};
    });
}

Result<void> Batch::erase(const Node& node) {
    return guarded([&] -> Result<void> {
        impl_->erase(node);
        return {};
    });
}

std::optional<Record> Batch::record(Point p) const {
    try {
        return impl_->record(p);
    } catch (const lmdb::error&) {
        return std::nullopt;
    }
}

Store::Store(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
Store::Store(Store&&) noexcept = default;
Store& Store::operator=(Store&&) noexcept = default;
Store::~Store() = default;

Result<Store> Store::open(const std::filesystem::path& directory, const Options& options) {
    return guarded([&] -> Result<Store> {
        std::error_code failed;
        if (!options.read_only) {
            std::filesystem::create_directories(directory, failed);
            if (failed) {
                return make_error(ErrorCode::Io, failed.message());
            }
        }
        auto impl = std::make_unique<Impl>();
        Shared& shared = impl->shared;
        shared.options = options;
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
        const unsigned create = options.read_only ? 0U : MDB_CREATE;
        shared.pages = lmdb::dbi::open(txn, "pages", create);
        shared.meta = lmdb::dbi::open(txn, "meta", create);
        txn.commit();
        return Store{std::move(impl)};
    });
}

Result<std::vector<std::byte>> Store::header() const {
    return guarded([&] -> Result<std::vector<std::byte>> {
        const lmdb::txn txn = lmdb::txn::begin(impl_->shared.env, nullptr, MDB_RDONLY);
        std::string_view value;
        if (!impl_->shared.meta.get(txn, header_key, value)) {
            return std::vector<std::byte>{};
        }
        const std::span<const std::byte> bytes = as_bytes(value);
        return std::vector<std::byte>{bytes.begin(), bytes.end()};
    });
}

Result<Snapshot> Store::read() const {
    return guarded([&] -> Result<Snapshot> {
        auto impl = std::make_unique<Snapshot::Impl>();
        impl->shared = &impl_->shared;
        impl->txn = lmdb::txn::begin(impl_->shared.env, nullptr, MDB_RDONLY);
        impl->cursor = lmdb::cursor::open(impl->txn, impl_->shared.pages);
        return Snapshot{std::move(impl)};
    });
}

Result<void> Store::write(const std::function<Result<void>(Batch&)>& body) {
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

Result<Store::Stats> Store::stats() const {
    return guarded([&] -> Result<Stats> {
        const lmdb::txn txn = lmdb::txn::begin(impl_->shared.env, nullptr, MDB_RDONLY);
        MDB_stat stat{};
        lmdb::dbi_stat(txn, impl_->shared.pages, &stat);
        MDB_envinfo info{};
        mdb_env_info(impl_->shared.env, &info);
        MDB_stat env_stat{};
        mdb_env_stat(impl_->shared.env, &env_stat);
        return Stats{.records = stat.ms_entries,
                     .bytes = (info.me_last_pgno + 1) * std::size_t{env_stat.ms_psize}};
    });
}

} // namespace hog::baked
