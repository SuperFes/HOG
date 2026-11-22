#pragma once

#include <hog/baked/chunk_store.hpp>
#include <hog/baked/filling.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/leaves.hpp>
#include <hog/grid/tree.hpp>
#include <hog/jobs/job_system.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace hog::baked {

// A fill: one band's nodes under a root, as the generator makes them.
struct Region {
    std::uint8_t band = 0;
    grid::Key root;

    friend constexpr auto operator<=>(const Region&, const Region&) noexcept = default;
};

// Where a band's fills are rooted: `levels` over its foot, so a fill takes no more than that many
// levels however coarse the band, but no deeper than the band's roots, so its chunks may be as wide
// as their budget allows.
class Regions {
public:
    // Precondition: levels > 0.
    Regions(Bands bands, std::uint8_t levels) noexcept : bands_{bands}, levels_{levels} {}

    [[nodiscard]] const Bands& bands() const noexcept { return bands_; }
    [[nodiscard]] std::uint8_t depth(std::uint8_t band) const noexcept {
        return static_cast<std::uint8_t>(
            std::min<int>(bands_.roots(band), std::max(0, bands_.foot(band) - levels_)));
    }
    // The band's region holding `node`, or the node itself where it's as wide as one.
    [[nodiscard]] Region over(std::uint8_t band, grid::Key node) const noexcept {
        return {.band = band, .root = node.ancestor(std::min(node.depth, depth(band)))};
    }

    // Calls visit(region, found) for each region of `band` under `node` that `coarser`, the node's
    // tree from the bands above, leaves open, with what it says of the region: one at each open
    // leaf down to the band's fill depth, rooted at the leaf where that lies over the depth. Where
    // the node is narrower than a region, the one region over it, if anything under it is open.
    template <typename Kind, typename Visit>
    void each_under(std::uint8_t band, grid::Key node, const grid::Tree<Kind>& coarser,
                    const Visit& visit) const {
        const std::uint8_t at = depth(band);
        if (at <= node.depth) {
            bool open = false;
            grid::for_open_leaves(coarser, node, [&](grid::Key, const auto&) { open = true; });
            if (open) {
                visit(over(band, node), coarser.root());
            }
            return;
        }
        grid::for_open_leaves(coarser.cut(0, 0, 0, 0, static_cast<std::uint8_t>(at - node.depth)),
                              node, [&](grid::Key leaf, const auto& found) {
                                  visit(Region{.band = band, .root = leaf}, found);
                              });
    }

    template <typename Kind>
    [[nodiscard]] std::vector<Region> under(std::uint8_t band, grid::Key node,
                                            const grid::Tree<Kind>& coarser) const {
        std::vector<Region> found;
        each_under(band, node, coarser,
                   [&](Region region, const auto&) { found.push_back(region); });
        return found;
    }

private:
    Bands bands_;
    std::uint8_t levels_;
};

// The generator's trees over the region, down to its band's foot.
[[nodiscard]] globe::Filled fill_region(const globe::Generator& generator, const grid::Grid& grid,
                                        const Bands& bands, Region region,
                                        globe::Layered layered = {});

// Holds each of `regions` as filled, in one batch.
[[nodiscard]] Result<void> write_regions(ChunkStore& store, std::span<const Region> regions,
                                         std::span<const std::optional<globe::Filled>> filled);

// Whether the store holds all of the region, every kind, as this generator makes it.
[[nodiscard]] bool holds(const ChunkStore::Snapshot& snapshot, Region region);

// Fills the regions the store needs to answer what's asked of it, on the job system's workers,
// and holds them on a thread of its own, so `pump` never waits on the disk. A node asked for down
// to a depth is answered by the bands from the one holding the node down to the one holding the
// depth, top down: each band's regions under the node are those the band above leaves open, so
// nothing is filled under ground the generator has closed. A band is filled once, whatever's
// asked of it later; asking deeper fills only the bands under it. Fills start in rank order, a
// region taking the highest rank of the asks waiting on it.
class ChunkFiller {
public:
    struct Options {
        // How many levels one fill takes at most.
        std::uint8_t fill_levels = 6;
        // Where finer nodes are filled from coarser ones with seeded layers.
        globe::Layered layered;
    };

    // A node asked for down to a depth.
    struct Asked {
        grid::Key node;
        std::uint8_t depth = 0;

        friend constexpr auto operator<=>(const Asked&, const Asked&) noexcept = default;
    };

    // Made on a thread of `jobs`' pool, as `pump` is called.
    ChunkFiller(const globe::Generator& generator, ChunkStore& store, jobs::JobSystem& jobs,
                Options options);
    ChunkFiller(const globe::Generator& generator, ChunkStore& store, jobs::JobSystem& jobs)
        : ChunkFiller(generator, store, jobs, Options{}) {}
    ChunkFiller(const ChunkFiller&) = delete;
    ChunkFiller& operator=(const ChunkFiller&) = delete;
    ChunkFiller(ChunkFiller&&) = delete;
    ChunkFiller& operator=(ChunkFiller&&) = delete;
    // Waits for fills under way and any write under way, writing no more.
    ~ChunkFiller();

    // From any thread. A depth past the finest is taken as the finest, one over the node as the
    // node's.
    void want(grid::Key node, std::uint8_t depth, Rank rank = {});

    // Hands the fills finished to be written, and starts those the asks need next, giving the
    // regions written since. With `wait`, first waits for a fill or a write under way. From one
    // thread at a time.
    [[nodiscard]] Result<std::vector<Region>> pump(bool wait = false);

    // Lets go of `asks`, as no one wants them any more: fills that only they wait on and haven't
    // started yet aren't started. Fills under way finish and are written. From the thread
    // calling `pump`.
    void forget(std::span<const Asked> asks);

    // What's been asked for that the store holds all of, since last taken. From the thread
    // calling `pump`.
    [[nodiscard]] std::vector<Asked> take_answered();

    // Whether nothing is asked for or being filled. From the thread calling `pump`.
    [[nodiscard]] bool idle() const;

    // The regions waiting their turn to be filled, the first to start first. From the thread
    // calling `pump`.
    [[nodiscard]] std::vector<Region> queued() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hog::baked
