#include <hog/baked/chunk_filling.hpp>
#include <hog/globe/grid_fill.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <thread>
#include <utility>

namespace hog::baked {

globe::Filled fill_region(const globe::Generator& generator, const grid::Grid& grid,
                          const Bands& bands, Region region) {
    return globe::fill(generator, grid, region.root,
                       static_cast<std::uint8_t>(bands.foot(region.band) - region.root.depth));
}

Result<void> write_regions(ChunkStore& store, std::span<const Region> regions,
                           std::span<const std::optional<globe::Filled>> filled) {
    return store.write([&](ChunkStore::Batch& batch) -> Result<void> {
        for (std::size_t n = 0; n < regions.size(); ++n) {
            const Region region = regions[n];
            const globe::Filled& trees = filled[n].value();
            for (const Result<std::vector<Chunk>>& put :
                 {batch.put(region.band, region.root, trees.shape),
                  batch.put(region.band, region.root, trees.material),
                  batch.put(region.band, region.root, trees.water),
                  batch.put(region.band, region.root, trees.woods)}) {
                if (!put) {
                    return std::unexpected{put.error()};
                }
            }
        }
        return {};
    });
}

bool holds(const ChunkStore::Snapshot& snapshot, Region region) {
    return snapshot.holds<grid::Shape>(region.band, region.root) &&
           snapshot.holds<grid::Material>(region.band, region.root) &&
           snapshot.holds<grid::Water>(region.band, region.root) &&
           snapshot.holds<grid::Woods>(region.band, region.root);
}

struct ChunkFiller::Impl {
    Impl(const globe::Generator& from, ChunkStore& into, jobs::JobSystem& on, Options given)
        : generator(&from), store(&into), jobs(&on), grid(globe::grid_of(from)),
          regions(into.bands(), std::max<std::uint8_t>(given.fill_levels, 1)),
          writer([this](const std::stop_token& stop) { write_on(stop); }) {}

    const globe::Generator* generator;
    ChunkStore* store;
    jobs::JobSystem* jobs;
    grid::Grid grid;
    Regions regions;

    mutable std::mutex lock;
    std::vector<std::pair<Asked, Rank>> asked; // guarded by lock

    // Fills under way, a few per thread, so the pool's queue never overflows onto the thread
    // pumping; and those waiting their turn, highest rank first.
    std::map<Region, std::unique_ptr<jobs::Async<globe::Filled>>> filling;
    std::set<std::pair<Rank, Region>, std::greater<>> queued;
    std::map<Region, Rank> queued_regions;
    // Asks waiting on a region being filled, by the region, with the highest rank each was asked
    // at, gone over again once it's written.
    std::map<Region, std::map<Asked, Rank>> blocked;
    std::set<Asked> answered;

    // Fills are written on a thread of their own, so the thread pumping never waits on the disk
    // or on another writer: the batches handed over, in order, and those written, with how it
    // went. `writes_out` counts those handed over and not yet taken back, on the thread pumping.
    struct Batch {
        std::vector<Region> regions;
        std::vector<std::optional<globe::Filled>> filled;
    };
    std::mutex writes_lock;
    std::condition_variable_any writes_changed;
    std::deque<Batch> to_write;                                      // guarded by writes_lock
    std::vector<std::pair<std::vector<Region>, Result<void>>> wrote; // guarded by writes_lock
    std::size_t writes_out = 0;
    // Last, so it stops before what it writes from goes.
    std::jthread writer;

    void write_on(const std::stop_token& stop) {
        std::unique_lock guard{writes_lock};
        while (writes_changed.wait(guard, stop, [&] { return !to_write.empty(); })) {
            Batch batch = std::move(to_write.front());
            to_write.pop_front();
            guard.unlock();
            Result<void> done = write_regions(*store, batch.regions, batch.filled);
            guard.lock();
            wrote.emplace_back(std::move(batch.regions), std::move(done));
            writes_changed.notify_all();
        }
    }

    // The regions an ask still needs filled, top down: those of the first band not held whole;
    // none where it's answered.
    [[nodiscard]] std::vector<Region> missing(const ChunkStore::Snapshot& snapshot,
                                              Asked ask) const {
        const Bands& bands = regions.bands();
        const std::uint8_t last = bands.band_of(ask.depth);
        std::uint8_t band = bands.band_of(ask.node.depth);
        std::vector<Region> at{regions.over(band, ask.node)};
        for (;; ++band) {
            std::vector<Region> needed;
            std::ranges::copy_if(at, std::back_inserter(needed),
                                 [&](Region region) { return !holds(snapshot, region); });
            if (!needed.empty() || band == last) {
                return needed;
            }
            const auto piece = snapshot.piece<grid::Shape>(ask.node, bands.foot(band));
            if (!piece) {
                return {};
            }
            at = regions.under(static_cast<std::uint8_t>(band + 1), ask.node, piece->tree);
            if (at.empty()) {
                return {};
            }
        }
    }

    // Queues `region` at `rank`, or raises it there if queued lower.
    void start(Region region, Rank rank) {
        const auto [at, fresh] = queued_regions.try_emplace(region, rank);
        if (!fresh) {
            if (at->second >= rank) {
                return;
            }
            queued.erase({at->second, region});
            at->second = rank;
        }
        queued.emplace(rank, region);
    }

    void launch() {
        const std::size_t most = std::size_t{2} * jobs->thread_count();
        while (filling.size() < most && !queued.empty()) {
            const Region region = queued.begin()->second;
            queued.erase(queued.begin());
            queued_regions.erase(region);
            filling.emplace(region,
                            std::make_unique<jobs::Async<globe::Filled>>(*jobs, [this, region] {
                                return fill_region(*generator, grid, regions.bands(), region);
                            }));
        }
    }

    // Hands the fills finished to the writer, and gives the regions it has written since. With
    // `wait`, where nothing is being written, first waits for a fill under way where none has
    // finished; then waits for a write.
    Result<std::vector<Region>> write_finished(bool wait) {
        Batch batch;
        const bool any =
            std::ranges::any_of(filling, [](const auto& at) { return at.second->done(); });
        for (auto at = filling.begin(); at != filling.end();) {
            if (at->second->done() || (wait && !any && writes_out == 0 && at == filling.begin())) {
                batch.regions.push_back(at->first);
                batch.filled.emplace_back(at->second->take());
                at = filling.erase(at);
            } else {
                ++at;
            }
        }
        std::vector<std::pair<std::vector<Region>, Result<void>>> done;
        {
            std::unique_lock guard{writes_lock};
            if (!batch.regions.empty()) {
                to_write.push_back(std::move(batch));
                ++writes_out;
                writes_changed.notify_all();
            }
            if (wait && writes_out > 0) {
                writes_changed.wait(guard, [&] { return !wrote.empty(); });
            }
            done.swap(wrote);
        }
        writes_out -= done.size();
        std::vector<Region> written;
        std::optional<Error> failed;
        for (auto& [held, result] : done) {
            if (!result) {
                // Forgotten, so they're gone over afresh when next asked for.
                for (const Region region : held) {
                    blocked.erase(region);
                }
                failed = result.error();
                continue;
            }
            written.insert(written.end(), held.begin(), held.end());
        }
        if (failed) {
            for (const Region region : written) {
                blocked.erase(region);
            }
            return std::unexpected{*failed};
        }
        return written;
    }
};

ChunkFiller::ChunkFiller(const globe::Generator& generator, ChunkStore& store,
                         jobs::JobSystem& jobs, Options options)
    : impl_(std::make_unique<Impl>(generator, store, jobs, options)) {}

ChunkFiller::~ChunkFiller() = default;

void ChunkFiller::want(grid::Key node, std::uint8_t depth, Rank rank) {
    if (!node.is_valid() || node.depth > impl_->grid.depth()) {
        return;
    }
    depth = std::clamp(depth, node.depth, impl_->grid.depth());
    const std::scoped_lock guard{impl_->lock};
    impl_->asked.emplace_back(Asked{.node = node, .depth = depth}, rank);
}

Result<std::vector<Region>> ChunkFiller::pump(bool wait) {
    Impl& impl = *impl_;
    auto written = impl.write_finished(wait);
    if (!written) {
        return written;
    }
    // Those asked for anew, and those waiting on the regions just written.
    std::vector<std::pair<Asked, Rank>> asks;
    {
        const std::scoped_lock guard{impl.lock};
        asks.swap(impl.asked);
    }
    for (const Region region : *written) {
        if (const auto found = impl.blocked.find(region); found != impl.blocked.end()) {
            asks.insert(asks.end(), found->second.begin(), found->second.end());
            impl.blocked.erase(found);
        }
    }
    if (asks.empty()) {
        impl.launch();
        return written;
    }
    Result<ChunkStore::Snapshot> snapshot = impl.store->read();
    if (!snapshot) {
        return std::unexpected{snapshot.error()};
    }
    for (const auto& [ask, rank] : asks) {
        const std::vector<Region> needed = impl.missing(*snapshot, ask);
        if (needed.empty()) {
            impl.answered.insert(ask);
            continue;
        }
        for (const Region region : needed) {
            if (!impl.filling.contains(region)) {
                impl.start(region, rank);
            }
            Rank& waits = impl.blocked[region][ask];
            waits = std::max(waits, rank);
        }
    }
    impl.launch();
    return written;
}

std::vector<ChunkFiller::Asked> ChunkFiller::take_answered() {
    std::vector<Asked> asks{impl_->answered.begin(), impl_->answered.end()};
    impl_->answered.clear();
    return asks;
}

std::vector<Region> ChunkFiller::queued() const {
    std::vector<Region> regions;
    regions.reserve(impl_->queued.size());
    for (const auto& [rank, region] : impl_->queued) {
        regions.push_back(region);
    }
    return regions;
}

bool ChunkFiller::idle() const {
    const std::scoped_lock guard{impl_->lock};
    return impl_->asked.empty() && impl_->filling.empty() && impl_->queued.empty() &&
           impl_->writes_out == 0;
}

} // namespace hog::baked
