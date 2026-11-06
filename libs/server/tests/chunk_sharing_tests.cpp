#include <hog/chunks/globe_stream.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/globe/world.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/server/chunk_sharing.hpp>

#include <glm/geometric.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <span>
#include <chrono>
#include <cstddef>
#include <format>
#include <map>
#include <print>
#include <set>
#include <thread>
#include <vector>

namespace {

using hog::DVec3;
using hog::net::ConnectionId;
using hog::server::ChunkSharing;

struct Earth {
    hog::globe::Generator generator{hog::globe::Planet{}};
    hog::globe::VoxelStore store{generator.layers()};
    hog::globe::World world{generator, store};
};

const Earth& earth() {
    static const Earth planet;
    return planet;
}

DVec3 eye_over(DVec3 up) {
    const auto& generator = earth().generator;
    return up * (generator.planet().radius + hog::chunks::globe_ground(generator, up) + 2.0);
}

const DVec3 here = hog::globe::center(hog::globe::cell_at_index(7, 4'321));

// Runs `turn` once a tick's worth of time, until it says it's done or the time's up.
template <typename Turn>
bool until(Turn&& turn, int seconds = 240) {
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds{seconds};
    while (std::chrono::steady_clock::now() < give_up) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        if (turn()) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("a player is sent the plan, and chunks only once they ask, nearest first",
          "[server][chunks]") {
    hog::jobs::JobSystem jobs{3};
    ChunkSharing sharing{earth().world, jobs, {.ratio = 1.5, .least = 120.0}};
    const ConnectionId anna{1};
    sharing.join(anna);
    CHECK_FALSE(sharing.next(anna, 10).plan);
    sharing.follow(anna, eye_over(here));

    std::vector<hog::protocol::PlannedChunk> plan;
    REQUIRE(until([&] {
        sharing.step();
        const ChunkSharing::Next next = sharing.next(anna, 10);
        // Nothing before it's asked for.
        CHECK(next.chunks.empty());
        if (next.plan) {
            plan = next.plan->wanted;
        }
        return !plan.empty();
    }));
    // The plan is what's wanted, nearest first.
    const auto wanted = sharing.wanted(anna);
    REQUIRE(plan.size() == wanted.size());
    for (std::size_t at = 0; at < plan.size(); ++at) {
        CHECK(plan[at].key == hog::chunks::globe_key(wanted[at]));
    }

    // Asking for the nearest, they come a few at a time, nearest first, versioned.
    std::vector<std::uint64_t> asked;
    for (std::size_t at = 0; at < 12; ++at) {
        asked.push_back(plan[at].key);
    }
    sharing.ask(anna, asked);
    std::vector<std::uint64_t> order;
    REQUIRE(until([&] {
        sharing.step();
        const ChunkSharing::Next next = sharing.next(anna, 2);
        for (const hog::protocol::Chunk& chunk : next.chunks) {
            CHECK(chunk.version >= 1);
            CHECK_FALSE(chunk.ground.indices.empty());
            if (chunk.part == 0) {
                order.push_back(chunk.key);
            }
        }
        return order.size() >= asked.size();
    }));
    CHECK(std::set<std::uint64_t>(order.begin(), order.end()) ==
          std::set<std::uint64_t>(asked.begin(), asked.end()));
    // Not asked for again, not sent again.
    CHECK(sharing.next(anna, 10).chunks.empty());
}

TEST_CASE("a player who leaves is no longer sent anything", "[server][chunks]") {
    hog::jobs::JobSystem jobs{3};
    ChunkSharing sharing{earth().world, jobs, {.ratio = 1.5, .least = 120.0}};
    const ConnectionId anna{1};
    sharing.join(anna);
    sharing.follow(anna, eye_over(here));
    sharing.leave(anna);
    const ChunkSharing::Next next = sharing.next(anna, 10);
    CHECK_FALSE(next.plan);
    CHECK(next.chunks.empty());
    CHECK(sharing.waiting(anna) == 0);
}

TEST_CASE("a chunk asked for before it's meshed is sent once it is", "[server][chunks]") {
    hog::jobs::JobSystem jobs{3};
    ChunkSharing sharing{earth().world, jobs, {.ratio = 1.5, .least = 120.0}};
    const ConnectionId anna{1};
    sharing.join(anna);
    sharing.follow(anna, eye_over(here));
    const auto wanted = sharing.wanted(anna);
    REQUIRE_FALSE(wanted.empty());
    const std::uint64_t key = hog::chunks::globe_key(wanted.front());
    sharing.ask(anna, std::vector<std::uint64_t>{key});
    bool sent = false;
    REQUIRE(until([&] {
        sharing.step();
        for (const hog::protocol::Chunk& chunk : sharing.next(anna, 4).chunks) {
            sent = sent || chunk.key == key;
        }
        return sent;
    }));
}

TEST_CASE("changes to the world make a newer version of the chunks they touch, asked for again",
          "[server][chunks]") {
    hog::globe::Generator generator{hog::globe::Planet{}};
    hog::globe::VoxelStore store{generator.layers()};
    hog::globe::World world{generator, store};
    hog::jobs::JobSystem jobs{3};
    ChunkSharing sharing{world, jobs, {.ratio = 1.5, .least = 120.0}};
    const ConnectionId anna{1};
    sharing.join(anna);
    const DVec3 eye = eye_over(here);
    sharing.follow(anna, eye);

    std::map<std::uint64_t, std::uint32_t> held;
    bool dug = false;
    bool newer = false;
    REQUIRE(until([&] {
        sharing.step();
        const ChunkSharing::Next next = sharing.next(anna, 8);
        if (next.plan) {
            std::vector<std::uint64_t> lacking;
            for (const hog::protocol::PlannedChunk& planned : next.plan->wanted) {
                const auto have = held.find(planned.key);
                if (planned.version > 0 && (have == held.end() || have->second < planned.version)) {
                    lacking.push_back(planned.key);
                    if (have != held.end()) {
                        newer = true;
                    }
                }
            }
            sharing.ask(anna, lacking);
        }
        for (const hog::protocol::Chunk& chunk : next.chunks) {
            held[chunk.key] = chunk.version;
        }
        if (!dug && held.size() >= 10) {
            dug = true;
            hog::globe::dig(world, eye - (hog::DVec3{glm::normalize(eye)} * 2.0), 3.0);
        }
        return newer;
    }));
    CHECK(dug);
}

TEST_CASE("the plan lists what's in view first, each half nearest first", "[server][chunks]") {
    hog::jobs::JobSystem jobs{3};
    ChunkSharing sharing{earth().world, jobs, {.ratio = 1.5, .least = 120.0}};
    const ConnectionId anna{1};
    sharing.join(anna);
    const DVec3 eye = eye_over(here);
    const DVec3 forward = glm::normalize(glm::cross(here, DVec3{0.0, 0.0, 1.0}));
    sharing.follow(anna, eye, forward);

    const double radius = earth().generator.planet().radius;
    const auto in_view = [&](hog::globe::Cell chunk) {
        return glm::dot(glm::normalize((hog::globe::center(chunk) * radius) - eye), forward) >
               hog::chunks::globe_view_cos;
    };
    const std::span<const hog::globe::Cell> wanted = sharing.wanted(anna);
    REQUIRE_FALSE(wanted.empty());
    const auto behind = std::ranges::find_if_not(wanted, in_view);
    REQUIRE(behind != wanted.end());
    CHECK(std::ranges::none_of(behind, wanted.end(), in_view));
    // Turning right round has the other half first.
    sharing.follow(anna, eye, -forward);
    CHECK_FALSE(in_view(sharing.wanted(anna).front()));
}

TEST_CASE("a plan that couldn't be sent is sent again at once", "[server][chunks]") {
    hog::jobs::JobSystem jobs{3};
    ChunkSharing sharing{earth().world, jobs, {.ratio = 1.5, .least = 120.0}};
    const ConnectionId anna{1};
    sharing.join(anna);
    sharing.follow(anna, eye_over(here));
    REQUIRE(until([&] { return sharing.next(anna, 1).plan.has_value(); }));
    CHECK_FALSE(sharing.next(anna, 1).plan);
    sharing.retry_plan(anna);
    CHECK(sharing.next(anna, 1).plan);
}

// Prints how many chunks are wanted at each falloff, and how they're spread by detail, to look at.
TEST_CASE("how many chunks each detail falloff wants, to look at", "[.look][server][chunks]") {
    hog::jobs::JobSystem jobs{2};
    for (const double ratio : {6.0, 3.0, 2.0, 1.5, 1.25}) {
        ChunkSharing sharing{earth().world, jobs, {.ratio = ratio, .least = 1'000.0}};
        const ConnectionId anna{1};
        sharing.join(anna);
        sharing.follow(anna, eye_over(here));
        std::map<int, int> by_detail;
        for (const hog::globe::Cell chunk : sharing.wanted(anna)) {
            ++by_detail[hog::globe::detail_of(chunk, earth().generator.layers())];
        }
        std::string spread;
        for (const auto& [detail, count] : by_detail) {
            spread += std::format(" d{}:{}", detail, count);
        }
        std::println("ratio {:4.1f}: {:4} chunks;{}", ratio, sharing.wanted(anna).size(), spread);
        sharing.leave(anna);
    }
}

TEST_CASE("a player who asks for detail to fall off sooner is planned fewer chunks",
          "[server][chunks]") {
    hog::jobs::JobSystem jobs{2};
    ChunkSharing sharing{earth().world, jobs};
    const ConnectionId anna{1};
    sharing.join(anna);
    sharing.detail(anna, {.ratio = 3.0F, .near = 1'000.0F, .slope = 1.0F});
    sharing.follow(anna, eye_over(here));
    const std::size_t fine = sharing.wanted(anna).size();
    sharing.detail(anna, {.ratio = 1.5F, .near = 1'000.0F, .slope = 1.0F});
    sharing.follow(anna, eye_over(here));
    const std::size_t coarse = sharing.wanted(anna).size();
    CHECK(fine > 0);
    CHECK(coarse < fine);
    sharing.leave(anna);
}

TEST_CASE("a player who asks for a steeper falloff is planned fewer chunks still",
          "[server][chunks]") {
    hog::jobs::JobSystem jobs{2};
    ChunkSharing sharing{earth().world, jobs};
    const ConnectionId anna{1};
    sharing.join(anna);
    sharing.detail(anna, {.ratio = 3.0F, .near = 1'000.0F, .slope = 1.0F});
    sharing.follow(anna, eye_over(here));
    const std::size_t gentle = sharing.wanted(anna).size();
    sharing.detail(anna, {.ratio = 3.0F, .near = 1'000.0F, .slope = 2.0F});
    sharing.follow(anna, eye_over(here));
    CHECK(sharing.wanted(anna).size() < gentle);
    sharing.leave(anna);
}

// Prints how fast chunks come through plan, asking and sending the way a client does, to look at.
TEST_CASE("how fast chunks come through asking, to look at", "[.look][server][chunks]") {
    hog::jobs::JobSystem jobs{8};
    ChunkSharing sharing{earth().world, jobs};
    const ConnectionId anna{1};
    sharing.join(anna);
    constexpr double degree = 3.14159265358979 / 180.0;
    const DVec3 up{glm::normalize(DVec3{std::cos(27.927653 * degree) * std::cos(-2.869545 * degree),
                                        std::cos(27.927653 * degree) * std::sin(-2.869545 * degree),
                                        std::sin(27.927653 * degree)})};
    const DVec3 eye = eye_over(up);
    sharing.follow(anna, eye);
    std::map<std::uint64_t, std::uint32_t> held;
    std::set<std::uint64_t> asked;
    std::vector<hog::protocol::PlannedChunk> plan;
    const auto began = std::chrono::steady_clock::now();
    auto last_report = began;
    std::size_t last_count = 0;
    int tick = 0;
    while (std::chrono::steady_clock::now() - began < std::chrono::seconds{40}) {
        // A tick at 20 Hz, as the server's: following the player where they are.
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        if (++tick == 5) {
            // The client's say in the falloff, a moment after it's welcomed.
            sharing.detail(anna, {.ratio = 3.0F, .near = 1'000.0F, .slope = 1.0F});
        }
        sharing.step();
        sharing.follow(anna, eye);
        const ChunkSharing::Next next = sharing.next(anna, 4);
        if (next.plan) {
            plan = next.plan->wanted;
        }
        for (const hog::protocol::Chunk& chunk : next.chunks) {
            held[chunk.key] = chunk.version;
            asked.erase(chunk.key);
        }
        std::vector<std::uint64_t> keys;
        for (const hog::protocol::PlannedChunk& planned : plan) {
            if (asked.size() + keys.size() >= 24) {
                break;
            }
            if (!held.contains(planned.key) && !asked.contains(planned.key)) {
                keys.push_back(planned.key);
            }
        }
        asked.insert(keys.begin(), keys.end());
        sharing.ask(anna, keys);
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds{5}) {
            std::println("{:3.0f} s: {} held, plan {}, +{} in 5 s, {} waiting, {} asked",
                         std::chrono::duration<double>(now - began).count(), held.size(),
                         plan.size(), held.size() - last_count, sharing.waiting(anna),
                         asked.size());
            last_report = now;
            last_count = held.size();
        }
    }
    sharing.leave(anna);
}
