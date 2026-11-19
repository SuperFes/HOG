#pragma once

#include <hog/baked/filling.hpp>
#include <hog/core/result.hpp>
#include <hog/grid/key.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace hog::baked {

// What players wait on a chunk store to hold: nodes down to depths, each ranked. One process
// writes it and any reads it. It's kept in an environment of its own beside the store's, so
// writing it never waits on the store's writer, which may be busy with a fill.
class WantsTable {
public:
    struct Want {
        grid::Key node;
        std::uint8_t depth = 0;
        Rank rank;

        friend constexpr bool operator==(const Want&, const Want&) noexcept = default;
    };

    // Where the table for the chunk store in `store` is kept.
    [[nodiscard]] static std::filesystem::path beside(const std::filesystem::path& store) {
        return store / "wants";
    }

    // Opens the table in `directory`, making it where missing.
    [[nodiscard]] static Result<WantsTable> open(const std::filesystem::path& directory);
    WantsTable(WantsTable&&) noexcept;
    WantsTable& operator=(WantsTable&&) noexcept;
    WantsTable(const WantsTable&) = delete;
    WantsTable& operator=(const WantsTable&) = delete;
    ~WantsTable();

    // Holds `wants` in place of what was held, writing only what changed. Of a node asked for
    // more than once down to the same depth, the last is kept.
    [[nodiscard]] Result<void> replace(std::span<const Want> wants);
    // Everything held, in Z order of the nodes.
    [[nodiscard]] Result<std::vector<Want>> read() const;

private:
    struct Impl;
    explicit WantsTable(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace hog::baked
