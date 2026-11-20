#include <hog/bake/bake.hpp>
#include <hog/globe/grid_fill.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace hog::bake {
namespace {

// A region found, with its share of the whole bake: its parent's split evenly among those wanted
// under it.
struct Found {
    baked::Region region;
    double share = 0.0;
};

// A region being filled on the workers.
struct Filling {
    Found found;
    std::unique_ptr<jobs::Async<globe::Filled>> trees;
};

class Baking {
public:
    Baking(const globe::Generator& generator, baked::ChunkStore& store, jobs::JobSystem& jobs,
           const Reach& reach, const Pace& pace)
        : generator_{&generator}, grid_{globe::grid_of(generator)}, store_{&store}, jobs_{&jobs},
          reach_{reach}, regions_{store.bands(), std::max<std::uint8_t>(pace.fill_levels, 1)},
          batch_{std::max<std::size_t>(pace.batch, 1)} {}

    // Depth first off a stack, regions under one on top in Z order, so only those along the way
    // down wait and neighbours are filled together; as many filled at once as the batch, those done
    // written a batch at a time.
    Result<Baked> run(const Progress& progress) {
        if (const baked::Region top{}; wanted(grid_, reach_, top, true)) {
            pending_.push_back({.region = top, .share = 1.0});
        }
        Result<baked::ChunkStore::Snapshot> snapshot = store_->read();
        while (!pending_.empty() || !filling_.empty() || !done_.empty()) {
            if (!snapshot) {
                return std::unexpected{snapshot.error()};
            }
            start(*snapshot);
            if (!collect() && !filling_.empty()) {
                finish(0);
            }
            if (done_.size() >= batch_ || (filling_.empty() && !done_.empty())) {
                if (Result<void> written = write(); !written) {
                    return std::unexpected{written.error()};
                }
                snapshot = store_->read();
                if (progress) {
                    progress({.done = baked_.filled + baked_.kept,
                              .waiting = pending_.size() + filling_.size(),
                              .share = shared_});
                }
            }
        }
        if (progress) {
            progress({.done = baked_.filled + baked_.kept, .waiting = 0, .share = shared_});
        }
        return baked_;
    }

private:
    // Takes regions off the stack while there's room: those held go down at once, the rest start
    // filling.
    void start(const baked::ChunkStore::Snapshot& snapshot) {
        const baked::Bands& bands = store_->bands();
        while (!pending_.empty() && filling_.size() < batch_) {
            const Found found = pending_.back();
            const baked::Region region = found.region;
            pending_.pop_back();
            if (baked::holds(snapshot, region)) {
                if (auto piece =
                        snapshot.piece<grid::Shape>(region.root, bands.foot(region.band))) {
                    ++baked_.kept;
                    under(found, piece->tree);
                    continue;
                }
            }
            filling_.push_back(
                {.found = found,
                 .trees = std::make_unique<jobs::Async<globe::Filled>>(*jobs_, [this, region] {
                     return baked::fill_region(*generator_, grid_, store_->bands(), region);
                 })});
        }
    }

    // Takes in the fills done; whether there were any.
    bool collect() {
        bool any = false;
        for (std::size_t n = 0; n < filling_.size();) {
            if (filling_[n].trees->done()) {
                finish(n);
                any = true;
            } else {
                ++n;
            }
        }
        return any;
    }

    // Waits for fill `n` if it's still going, then goes on under it.
    void finish(std::size_t n) {
        Filling filling = std::move(filling_[n]);
        filling_.erase(filling_.begin() + static_cast<std::ptrdiff_t>(n));
        globe::Filled trees = filling.trees->take();
        under(filling.found, trees.shape);
        done_.emplace_back(filling.found.region, std::move(trees));
    }

    // Puts the regions wanted under `found` on the stack, or counts its share done where there
    // are none.
    void under(const Found& found, const grid::Tree<grid::Shape>& shape) {
        const auto next = static_cast<std::uint8_t>(found.region.band + 1);
        const std::size_t from = pending_.size();
        if (next < store_->bands().count()) {
            regions_.each_under(next, found.region.root, shape,
                                [&](baked::Region region, const auto& leaf) {
                                    if (wanted(grid_, reach_, region, !leaf.value.is_solid())) {
                                        pending_.push_back({.region = region, .share = 0.0});
                                    }
                                });
        }
        const std::size_t count = pending_.size() - from;
        if (count == 0) {
            shared_ += found.share;
            return;
        }
        const auto first = pending_.begin() + static_cast<std::ptrdiff_t>(from);
        for (auto each = first; each != pending_.end(); ++each) {
            each->share = found.share / static_cast<double>(count);
        }
        std::sort(first, pending_.end(),
                  [](const Found& a, const Found& b) { return a.region.root > b.region.root; });
    }

    Result<void> write() {
        std::vector<baked::Region> regions;
        std::vector<std::optional<globe::Filled>> filled;
        regions.reserve(done_.size());
        filled.reserve(done_.size());
        for (auto& [region, trees] : done_) {
            baked_.shape_bytes += trees.shape.encode().size();
            baked_.material_bytes += trees.material.encode().size();
            baked_.water_bytes += trees.water.encode().size();
            baked_.woods_bytes += trees.woods.encode().size();
            regions.push_back(region);
            filled.emplace_back(std::move(trees));
        }
        baked_.filled += done_.size();
        done_.clear();
        return baked::write_regions(*store_, regions, filled);
    }

    const globe::Generator* generator_;
    grid::Grid grid_;
    baked::ChunkStore* store_;
    jobs::JobSystem* jobs_;
    Reach reach_;
    baked::Regions regions_;
    std::size_t batch_;
    std::vector<Found> pending_;
    std::vector<Filling> filling_;
    std::vector<std::pair<baked::Region, globe::Filled>> done_;
    Baked baked_;
    double shared_ = 0.0; // of the whole, in regions with nothing wanted under them
};

} // namespace

std::uint8_t depth_of(const grid::Grid& grid, double metres) noexcept {
    // Leeway for a size typed short of the level it means, as a third of a metre is.
    const double most = metres * (1.0 + 1e-6);
    for (std::uint8_t depth = 0; depth < grid.depth(); ++depth) {
        if (grid.size(depth) <= most) {
            return depth;
        }
    }
    return grid.depth();
}

bool wanted(const grid::Grid& grid, const Reach& reach, baked::Region region,
            bool surface) noexcept {
    if (reach.around && grid.distance(region.root, *reach.around) <= reach.across / 2.0) {
        return true;
    }
    return surface && reach.band && region.band <= *reach.band;
}

Result<Baked> bake(const globe::Generator& generator, baked::ChunkStore& store,
                   jobs::JobSystem& jobs, const Reach& reach, const Pace& pace,
                   const Progress& progress) {
    return Baking{generator, store, jobs, reach, pace}.run(progress);
}

} // namespace hog::bake
