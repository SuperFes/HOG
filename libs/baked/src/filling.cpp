#include <hog/baked/filling.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/globe/planet.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

namespace hog::baked {
namespace {

// What a node asked for needs next.
struct Next {
    enum class Is : std::uint8_t { Answered, Filling, Fill } is = Is::Answered;
    grid::Key page;
    std::uint8_t levels = 0; // to fill it to
};

} // namespace

Baking baking_of(const globe::Generator& generator) {
    return {.world = globe::world_id(generator.planet()),
            .grid = globe::grid_of(generator),
            .generator = globe::generator_version};
}

Result<void> write_filled(PageStore& store, std::span<const grid::Key> pages,
                          std::span<const std::optional<globe::Filled>> filled) {
    return store.write([&](baked::PageStore::Batch& batch) -> Result<void> {
        for (std::size_t n = 0; n < pages.size(); ++n) {
            const globe::Filled& page = filled[n].value();
            for (const Result<std::uint32_t>& put :
                 {batch.put(pages[n], page.shape), batch.put(pages[n], page.material),
                  batch.put(pages[n], page.water)}) {
                if (!put) {
                    return std::unexpected{put.error()};
                }
            }
        }
        return {};
    });
}

struct Filler::Impl {
    Impl(const globe::Generator& from, PageStore& into, jobs::JobSystem& on)
        : generator(&from), store(&into), jobs(&on), grid(globe::grid_of(from)) {}

    const globe::Generator* generator;
    PageStore* store;
    jobs::JobSystem* jobs;
    grid::Grid grid;

    mutable std::mutex lock;
    std::vector<grid::Key> asked; // guarded by lock

    // Fills under way, a few per thread, so the pool's queue never overflows onto the thread
    // pumping; and those waiting their turn, first asked first, as deep as each is to go.
    std::map<grid::Key, std::unique_ptr<jobs::Async<globe::Filled>>> filling;
    std::deque<std::pair<grid::Key, std::uint8_t>> queued;
    std::set<grid::Key> queued_pages;
    // Nodes asked for waiting on a page being filled, by the page, gone over again once it's
    // written; and all of them, so one asked again waits once.
    std::map<grid::Key, std::vector<grid::Key>> blocked;
    std::set<grid::Key> waiting;

    // Pages read from the store in one pump, by root.
    using Seen = std::map<grid::Key, std::optional<PageStore::Read>>;

    // Down the pages over `node` to the first that isn't held whole as this generator makes it,
    // as deep as the node needs, or that answers it.
    [[nodiscard]] Next next(const PageStore::Snapshot& snapshot, Seen& seen, grid::Key node) const {
        grid::Key page{};
        while (true) {
            const auto foot = static_cast<std::uint8_t>(page.depth + grid.levels_under(page.depth));
            const auto needs = static_cast<std::uint8_t>(
                std::clamp<int>(node.depth - page.depth, 1, foot - page.depth));
            if (filling.contains(page) || queued_pages.contains(page)) {
                return {.is = Next::Is::Filling, .page = page, .levels = needs};
            }
            auto [at, fresh] = seen.try_emplace(page);
            if (fresh) {
                at->second = snapshot.page(page);
            }
            const std::optional<PageStore::Read>& read = at->second;
            if (!read || !read->page.shape || !read->page.material || !read->page.water ||
                read->page.shape->tree.levels() < needs) {
                return {.is = Next::Is::Fill, .page = page, .levels = needs};
            }
            if (node.depth <= foot) {
                return {.is = Next::Is::Answered, .page = page, .levels = needs};
            }
            const grid::Key over = node.ancestor(foot);
            const auto below = static_cast<std::uint8_t>(foot - page.depth);
            const auto found =
                read->page.shape->tree.at(below, over.x - (page.x << below),
                                          over.y - (page.y << below), over.z - (page.z << below));
            if (!found.open) {
                return {.is = Next::Is::Answered, .page = page, .levels = needs};
            }
            page = over;
        }
    }

    void start(grid::Key page, std::uint8_t levels) {
        if (queued_pages.insert(page).second) {
            queued.emplace_back(page, levels);
        }
    }

    // Starts fills waiting their turn while there's room.
    void launch() {
        const std::size_t most = std::size_t{2} * jobs->thread_count();
        while (filling.size() < most && !queued.empty()) {
            const auto [page, levels] = queued.front();
            queued.pop_front();
            queued_pages.erase(page);
            filling.emplace(
                page, std::make_unique<jobs::Async<globe::Filled>>(*jobs, [this, page, levels] {
                    return globe::fill(*generator, grid, page, levels);
                }));
        }
    }

    // Takes the fills finished, or with `wait` the first under way where none has.
    Result<std::vector<grid::Key>> write_finished(bool wait) {
        std::vector<grid::Key> pages;
        std::vector<std::optional<globe::Filled>> filled;
        const bool any =
            std::ranges::any_of(filling, [](const auto& at) { return at.second->done(); });
        for (auto at = filling.begin(); at != filling.end();) {
            if (at->second->done() || (wait && !any && at == filling.begin())) {
                pages.push_back(at->first);
                filled.emplace_back(at->second->take());
                at = filling.erase(at);
            } else {
                ++at;
            }
        }
        if (pages.empty()) {
            return pages;
        }
        if (Result<void> written = write_filled(*store, pages, filled); !written) {
            // Forgotten, so they're gone over afresh when next asked for.
            for (const grid::Key page : pages) {
                if (const auto found = blocked.find(page); found != blocked.end()) {
                    for (const grid::Key node : found->second) {
                        waiting.erase(node);
                    }
                    blocked.erase(found);
                }
            }
            return std::unexpected{written.error()};
        }
        return pages;
    }
};

Filler::Filler(const globe::Generator& generator, PageStore& store, jobs::JobSystem& jobs)
    : impl_(std::make_unique<Impl>(generator, store, jobs)) {}

Filler::~Filler() = default;

void Filler::want(grid::Key node) {
    const std::scoped_lock guard{impl_->lock};
    impl_->asked.push_back(node);
}

Result<std::vector<grid::Key>> Filler::pump(bool wait) {
    Impl& impl = *impl_;
    auto written = impl.write_finished(wait);
    if (!written) {
        return written;
    }
    // Those asked for anew, and those waiting on the pages just written.
    std::vector<grid::Key> nodes;
    {
        const std::scoped_lock guard{impl.lock};
        for (const grid::Key node : impl.asked) {
            if (impl.waiting.insert(node).second) {
                nodes.push_back(node);
            }
        }
        impl.asked.clear();
    }
    for (const grid::Key page : *written) {
        if (const auto found = impl.blocked.find(page); found != impl.blocked.end()) {
            nodes.insert(nodes.end(), found->second.begin(), found->second.end());
            impl.blocked.erase(found);
        }
    }
    if (nodes.empty()) {
        impl.launch();
        return written;
    }
    Result<PageStore::Snapshot> snapshot = impl.store->read();
    if (!snapshot) {
        return std::unexpected{snapshot.error()};
    }
    Impl::Seen seen;
    for (const grid::Key node : nodes) {
        const Next next = impl.next(*snapshot, seen, node);
        if (next.is == Next::Is::Fill) {
            impl.start(next.page, next.levels);
        }
        if (next.is == Next::Is::Answered) {
            impl.waiting.erase(node);
        } else {
            impl.blocked[next.page].push_back(node);
        }
    }
    impl.launch();
    return written;
}

bool Filler::idle() const {
    const std::scoped_lock guard{impl_->lock};
    return impl_->asked.empty() && impl_->filling.empty() && impl_->queued.empty();
}

} // namespace hog::baked
