#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace hog::planet {

// A world's landform as evolved, kept on disk so the evolving, which takes a while, runs once
// rather than whenever the world is built.
struct LandformBake {
    int level = 0; // of the landform's grid
    // Per grid cell, fixed point: the ground as evolved, how far it moved, the runoff-weighted
    // area draining through it, the ground at its centre the rivers start from, the soil on it,
    // how far it was lifted and how deep glaciers cut it.
    std::vector<std::int64_t> heights;
    std::vector<std::int64_t> change;
    std::vector<std::int64_t> drained;
    std::vector<std::int64_t> ground;
    std::vector<std::int64_t> soil;
    std::vector<std::int64_t> risen;
    std::vector<std::int64_t> carved;

    friend bool operator==(const LandformBake&, const LandformBake&) = default;
};

// Where HOG_BAKES says landforms are kept, or nowhere.
[[nodiscard]] std::filesystem::path named_bakes();

// Which world a bake is of, and which code baked it.
struct BakeName {
    std::uint64_t world = 0;
    std::uint64_t code = 0;

    friend bool operator==(const BakeName&, const BakeName&) = default;
};

// Where the bake is kept in `directory`.
[[nodiscard]] std::filesystem::path bake_path(const std::filesystem::path& directory,
                                              BakeName name);

// The bake kept in `directory`, or nothing where there's none or it's damaged.
[[nodiscard]] std::optional<LandformBake> read_bake(const std::filesystem::path& directory,
                                                    BakeName name);

// Keeps a bake in `directory`, whole or not at all, making the directory, and removes the world's
// bakes by other code; false where it can't, which leaves the world to evolve again next time.
[[nodiscard]] bool write_bake(const std::filesystem::path& directory, BakeName name,
                              const LandformBake& bake);

// How a claim keeps itself alive and how others wait on it.
struct ClaimTiming {
    // How often the holder marks its claim as alive.
    std::chrono::milliseconds beat{5'000};
    // How long a claim goes unmarked before it's taken as abandoned by a process that died.
    std::chrono::milliseconds abandoned{30'000};
    // How often a waiter looks again.
    std::chrono::milliseconds poll{200};
};

// The right to bake one world in a directory, so processes that build it at once bake it once:
// the first claims it and bakes, and the rest wait for the bake or for the claim to be let go.
class BakeClaim {
public:
    // Waits while another holds the claim and its bake isn't kept yet, calling `waiting` once if
    // it does.
    BakeClaim(const std::filesystem::path& directory, BakeName name,
              const std::function<void()>& waiting = {}, ClaimTiming timing = {});
    BakeClaim(const BakeClaim&) = delete;
    BakeClaim(BakeClaim&&) = delete;
    BakeClaim& operator=(const BakeClaim&) = delete;
    BakeClaim& operator=(BakeClaim&&) = delete;
    ~BakeClaim();

    // Whether this holds the claim, rather than finding the bake kept by another.
    [[nodiscard]] bool held() const noexcept { return held_; }

private:
    std::filesystem::path claim_;
    bool held_ = false;
    std::mutex stopping_;
    std::condition_variable stop_;
    bool stopped_ = false;
    std::thread beating_;
};

} // namespace hog::planet
