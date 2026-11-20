#include <hog/baked/chunk_store.hpp>
#include <hog/baked/generating.hpp>
#include <hog/baked/wants_table.hpp>
#include <hog/client/held_pieces.hpp>
#include <hog/client/piece_asking.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/grid/view.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/net/events.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/server/chunk_sending.hpp>
#include <hog/testing/temporary_directory.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using hog::grid::Key;

namespace {

// A player as the client keeps one: what it holds and what it's asked for and not been told is
// done.
struct Player {
    hog::net::ConnectionId id{};
    std::size_t room = 0; // bytes of trees one message carries at most
    hog::client::HeldPieces held;
    hog::client::PieceRequests requests;
    bool came = true;
    // Chunks taken in, each time, and each chunk as it was taken.
    std::size_t taken = 0;
    std::set<std::tuple<std::uint8_t, std::uint8_t, Key, std::uint64_t>> distinct;
};

using Root = std::tuple<std::uint8_t, std::uint32_t, std::uint32_t, std::uint32_t>;
using Ground = std::map<Root, std::array<std::vector<std::byte>, 3>>;

// The ground a player holds, by root: its shape's, material's and water's trees' bytes; woods
// lie over it as an overlay of their own.
Ground ground_of(const hog::client::HeldPieces& held) {
    Ground found;
    for (const auto& [root, shared] : held.entries()) {
        const hog::grid::Piece& piece = *shared;
        std::array<std::vector<std::byte>, 3> trees;
        if (piece.shape) {
            trees[0] = piece.shape->encode();
        }
        if (piece.material) {
            trees[1] = piece.material->encode();
        }
        if (piece.water) {
            trees[2] = piece.water->encode();
        }
        found[{root.depth, root.x, root.y, root.z}] = std::move(trees);
    }
    return found;
}

// The server as it runs for its players: their asks, its store and the log it hears, and filling
// what they wait on.
class Streaming {
public:
    explicit Streaming(const hog::globe::Generator& generator)
        : store_{open_store(generator)}, wants_{open_wants(directory_)},
          generating_{generator, store_, wants_, jobs_}, sending_{store_.bands()} {}

    void join(const Player& player) { sending_.join(player.id); }

    // Has the player ask again as the client does when pieces come, where its wishes changed.
    void ask(Player& player, hog::DVec3 eye, hog::DVec3 facing, const hog::grid::View& view) {
        if (!std::exchange(player.came, false)) {
            return;
        }
        const std::vector<hog::client::PieceWish> wishes =
            hog::client::pieces_to_ask(player.held, eye, facing, view, hog::protocol::most_asked);
        const std::optional<hog::protocol::AskPieces> ask =
            player.requests.ask(player.held, wishes);
        if (!ask) {
            return;
        }
        std::vector<Key> dropped;
        for (const hog::protocol::DroppedPiece& piece : ask->dropped) {
            dropped.push_back({.depth = piece.depth, .x = piece.x, .y = piece.y, .z = piece.z});
        }
        std::vector<hog::server::ChunkSending::Want> asking;
        asking.reserve(ask->pieces.size());
        for (const hog::protocol::AskedPiece& piece : ask->pieces) {
            asking.push_back(
                {.node = {.depth = piece.depth, .x = piece.x, .y = piece.y, .z = piece.z},
                 .depth = piece.to,
                 .held = piece.held,
                 .priority = static_cast<double>(piece.priority)});
        }
        sending_.drop(player.id, dropped);
        sending_.ask(player.id, asking);
    }

    // One of the server's ticks: what the log says changed, what's due to each player, and a
    // step of filling. Whether anything happened.
    bool tick(std::span<Player> players) {
        auto snapshot = store_.read();
        REQUIRE(snapshot.has_value());
        const bool heard = hear(*snapshot);
        bool sent = false;
        for (Player& player : players) {
            const hog::server::ChunkSending::Next next =
                sending_.next(player.id, *snapshot, player.room, hog::protocol::most_chunks);
            for (const hog::protocol::SentChunk& chunk : next.chunks) {
                REQUIRE(player.held.take(chunk).has_value());
                ++player.taken;
                player.distinct.insert(
                    {chunk.kind, chunk.band,
                     Key{.depth = chunk.depth, .x = chunk.x, .y = chunk.y, .z = chunk.z},
                     chunk.version});
            }
            player.requests.done(next.done);
            const bool any = !next.chunks.empty() || !next.done.empty();
            player.came = player.came || any;
            sent = sent || any;
        }
        if (std::vector<hog::baked::WantsTable::Want> wanted = sending_.wanted(); wanted != told_) {
            REQUIRE(wants_.replace(wanted).has_value());
            told_ = std::move(wanted);
        }
        const auto busy = generating_.step(!sent);
        REQUIRE(busy.has_value());
        return heard || sent || *busy;
    }

private:
    static hog::baked::ChunkStore open_store(const hog::globe::Generator& generator) {
        auto store = hog::baked::ChunkStore::scratch(
            hog::baked::baking_of(generator),
            {.map_bytes = std::uint64_t{1} << 30U, .readers = 64, .read_only = false});
        REQUIRE(store.has_value());
        return std::move(*store);
    }
    static hog::baked::WantsTable open_wants(const hog::testing::TemporaryDirectory& directory) {
        auto wants = hog::baked::WantsTable::open(directory.path());
        REQUIRE(wants.has_value());
        return std::move(*wants);
    }

    bool hear(const hog::baked::ChunkStore::Snapshot& snapshot) {
        std::vector<hog::baked::Region> written;
        std::vector<hog::baked::ChunkFiller::Asked> answered;
        for (const hog::baked::Logged& each : snapshot.logged_since(logged_, 1U << 16U)) {
            if (each.what == hog::baked::Logged::What::Chunk) {
                written.push_back({.band = each.band, .root = each.node});
            } else {
                answered.push_back({.node = each.node, .depth = each.depth});
            }
            logged_ = each.at;
        }
        sending_.changed(written);
        sending_.answered(answered);
        return !written.empty() || !answered.empty();
    }

    hog::baked::ChunkStore store_;
    hog::testing::TemporaryDirectory directory_;
    hog::baked::WantsTable wants_;
    hog::jobs::JobSystem jobs_{2};
    hog::baked::Generating generating_;
    hog::server::ChunkSending sending_;
    std::uint64_t logged_ = 0;
    std::vector<hog::baked::WantsTable::Want> told_;
};

// What's held differently by the two, each root once.
std::vector<std::string> differences(const Ground& first, const Ground& second) {
    std::vector<std::string> found;
    const auto name = [](const Root& root) {
        const auto [depth, x, y, z] = root;
        return std::to_string(depth) + " " + std::to_string(x) + " " + std::to_string(y) + " " +
               std::to_string(z);
    };
    for (const auto& [root, trees] : first) {
        if (const auto other = second.find(root); other == second.end()) {
            found.push_back("only the first holds " + name(root));
        } else if (other->second != trees) {
            found.push_back("they differ at " + name(root));
        }
    }
    for (const auto& [root, trees] : second) {
        if (!first.contains(root)) {
            found.push_back("only the second holds " + name(root));
        }
    }
    return found;
}

} // namespace

TEST_CASE("players asking from one place come to hold the same ground, all they want",
          "[client][streaming]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    Streaming streaming{generator};
    // Coarser than the client's own, so the test world is seen whole in a few hundred fills.
    const hog::grid::View view{64.0};
    const hog::DVec3 up = glm::normalize(hog::DVec3{0.2, 0.7, -0.68});
    const hog::DVec3 eye =
        up * (generator.column(up, hog::globe::voxel_lod(generator)).ground() + 300.0);
    const hog::DVec3 facing =
        glm::normalize(glm::cross(up, hog::DVec3{0.0, 0.0, 1.0}) - (0.3 * up));
    // One sent big messages, the other small, so each is answered in its own pieces and times.
    std::array<Player, 2> players{Player{.id = hog::net::ConnectionId{1},
                                         .room = std::size_t{256} << 10U,
                                         .held = hog::client::HeldPieces{grid},
                                         .requests = {},
                                         .came = true,
                                         .taken = 0,
                                         .distinct = {}},
                                  Player{.id = hog::net::ConnectionId{2},
                                         .room = std::size_t{8} << 10U,
                                         .held = hog::client::HeldPieces{grid},
                                         .requests = {},
                                         .came = true,
                                         .taken = 0,
                                         .distinct = {}}};
    for (const Player& player : players) {
        streaming.join(player);
    }
    int quiet = 0;
    for (int tick = 0; tick < 20'000 && quiet < 50; ++tick) {
        for (Player& player : players) {
            streaming.ask(player, eye, facing, view);
        }
        quiet = streaming.tick(players) ? 0 : quiet + 1;
    }
    for (const Player& player : players) {
        CAPTURE(static_cast<std::uint32_t>(player.id));
        CHECK(hog::client::pieces_to_ask(player.held, eye, facing, view, hog::protocol::most_asked)
                  .empty());
        CHECK(player.held.pieces() > 0);
        // Each goes about once: only what's asked together shares what's sent.
        CHECK(static_cast<double>(player.taken) <=
              1.1 * static_cast<double>(player.distinct.size()));
    }
    const std::vector<std::string> differ =
        differences(ground_of(players[0].held), ground_of(players[1].held));
    for (std::size_t n = 0; n < std::min<std::size_t>(differ.size(), 8); ++n) {
        UNSCOPED_INFO(differ[n]);
    }
    CHECK(differ.empty());
}
