#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace hog::planet {

// A world's landform as evolved, kept on disk so the evolving, which takes a while, runs once
// rather than whenever the world is built.
struct LandformBake {
    int level = 0; // of the landform's grid
    // Per grid cell, fixed point: the ground as evolved, how far it moved, the runoff-weighted
    // area draining through it, and the ground at its centre the rivers start from.
    std::vector<std::int64_t> heights;
    std::vector<std::int64_t> change;
    std::vector<std::int64_t> drained;
    std::vector<std::int64_t> ground;

    friend bool operator==(const LandformBake&, const LandformBake&) = default;
};

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

} // namespace hog::planet
