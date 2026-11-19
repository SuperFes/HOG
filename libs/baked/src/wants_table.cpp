#include "lmdb_shared.hpp"

#include <hog/baked/wants_table.hpp>
#include <hog/core/bytes.hpp>
#include <hog/grid/z_order.hpp>

#include <lmdb++.h>

#include <array>
#include <bit>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

namespace hog::baked {
namespace {

using detail::as_bytes;
using detail::as_view;
using detail::guarded;

// A want's key: its node's aligned code, big-endian, the node's depth and the depth wanted, so
// the table lies in Z order.
using WantKey = std::array<std::byte, 10>;

WantKey key_of(grid::Key node, std::uint8_t depth) noexcept {
    WantKey key{};
    const std::uint64_t code = grid::aligned(node);
    for (std::size_t k = 0; k < 8; ++k) {
        key.at(k) = static_cast<std::byte>(code >> (8U * (7U - k)));
    }
    key.at(8) = std::byte{node.depth};
    key.at(9) = std::byte{depth};
    return key;
}

std::optional<WantsTable::Want> want_from(std::string_view key, std::string_view value) {
    if (key.size() != std::tuple_size_v<WantKey>) {
        return std::nullopt;
    }
    const std::span<const std::byte> bytes = as_bytes(key);
    std::uint64_t code = 0;
    for (std::size_t k = 0; k < 8; ++k) {
        code = (code << 8U) | std::to_integer<std::uint64_t>(bytes[k]);
    }
    const auto depth = std::to_integer<std::uint8_t>(bytes[8]);
    if (depth > grid::max_depth) {
        return std::nullopt;
    }
    const std::uint64_t bits = code >> (3U * (grid::max_depth - depth));
    ByteReader in{as_bytes(value)};
    const auto round = in.read<std::uint64_t>();
    const auto priority = in.read<std::uint64_t>();
    if (!round || !priority) {
        return std::nullopt;
    }
    return WantsTable::Want{
        .node = {.depth = depth,
                 .x = grid::gather(bits),
                 .y = grid::gather(bits >> 1U),
                 .z = grid::gather(bits >> 2U)},
        .depth = std::to_integer<std::uint8_t>(bytes[9]),
        .rank = {.round = *round, .priority = std::bit_cast<double>(*priority)}};
}

std::vector<std::byte> value_of(const Rank& rank) {
    ByteWriter out;
    out.write(rank.round);
    out.write(std::bit_cast<std::uint64_t>(rank.priority));
    return std::move(out).take();
}

} // namespace

struct WantsTable::Impl {
    lmdb::env env{nullptr};
    lmdb::dbi wants;
};

WantsTable::WantsTable(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
WantsTable::WantsTable(WantsTable&&) noexcept = default;
WantsTable& WantsTable::operator=(WantsTable&&) noexcept = default;
WantsTable::~WantsTable() = default;

Result<WantsTable> WantsTable::open(const std::filesystem::path& directory) {
    std::error_code failed;
    std::filesystem::create_directories(directory, failed);
    if (failed) {
        return make_error(ErrorCode::Io,
                          std::format("can't make {}: {}", directory.string(), failed.message()));
    }
    return guarded([&] -> Result<WantsTable> {
        auto impl = std::make_unique<Impl>();
        impl->env = lmdb::env::create();
        impl->env.set_mapsize(std::size_t{256} << 20U);
        impl->env.open(directory.string().c_str(), MDB_NOTLS, 0644);
        lmdb::txn txn = lmdb::txn::begin(impl->env);
        impl->wants = lmdb::dbi::open(txn, nullptr);
        txn.commit();
        return WantsTable{std::move(impl)};
    });
}

Result<void> WantsTable::replace(std::span<const Want> wants) {
    std::map<WantKey, std::vector<std::byte>> wanted;
    for (const Want& want : wants) {
        wanted.insert_or_assign(key_of(want.node, want.depth), value_of(want.rank));
    }
    return guarded([&] -> Result<void> {
        lmdb::txn txn = lmdb::txn::begin(impl_->env);
        std::vector<WantKey> gone;
        {
            lmdb::cursor cursor = lmdb::cursor::open(txn.handle(), impl_->wants.handle());
            std::string_view key;
            std::string_view value;
            for (bool more = cursor.get(key, value, MDB_FIRST); more;
                 more = cursor.get(key, value, MDB_NEXT)) {
                if (key.size() != std::tuple_size_v<WantKey>) {
                    continue;
                }
                WantKey held{};
                std::ranges::copy(as_bytes(key), held.begin());
                const auto found = wanted.find(held);
                if (found == wanted.end()) {
                    gone.push_back(held);
                } else if (as_view(found->second) == value) {
                    wanted.erase(found);
                }
            }
        }
        for (const WantKey& key : gone) {
            impl_->wants.del(txn, as_view(key));
        }
        for (const auto& [key, value] : wanted) {
            impl_->wants.put(txn, as_view(key), as_view(value));
        }
        txn.commit();
        return {};
    });
}

Result<std::vector<WantsTable::Want>> WantsTable::read() const {
    return guarded([&] -> Result<std::vector<Want>> {
        const lmdb::txn txn = lmdb::txn::begin(impl_->env, nullptr, MDB_RDONLY);
        lmdb::cursor cursor = lmdb::cursor::open(txn.handle(), impl_->wants.handle());
        std::vector<Want> found;
        std::string_view key;
        std::string_view value;
        for (bool more = cursor.get(key, value, MDB_FIRST); more;
             more = cursor.get(key, value, MDB_NEXT)) {
            if (std::optional<Want> want = want_from(key, value)) {
                found.push_back(*want);
            }
        }
        return found;
    });
}

} // namespace hog::baked
