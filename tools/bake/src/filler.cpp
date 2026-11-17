#include "pages.hpp"

#include <hog/bake/filler.hpp>
#include <hog/globe/grid_fill.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace hog::bake {
namespace {

// What a node asked for needs next.
struct Next {
    enum class Is : std::uint8_t { Answered, Filling, Fill } is = Is::Answered;
    grid::Key page;
};

} // namespace

struct Filler::Impl {
    Impl(const globe::Generator& generator, baked::PageStore& store, jobs::JobSystem& jobs)
        : generator(&generator), store(&store), jobs(&jobs), grid(globe::grid_of(generator)) {}

    const globe::Generator* generator;
    baked::PageStore* store;
    jobs::JobSystem* jobs;
    grid::Grid grid;

    mutable std::mutex lock;
    std::vector<grid::Key> asked; // guarded by lock

    std::vector<grid::Key> waiting;
    std::map<grid::Key, std::unique_ptr<jobs::Async<globe::Filled>>> filling;

    // Down the pages over `node` to the first that isn't held whole as this generator makes it,
    // or that answers it.
    [[nodiscard]] Next next(const baked::PageStore::Snapshot& snapshot, grid::Key node) const {
        grid::Key page{};
        while (true) {
            if (filling.contains(page)) {
                return {.is = Next::Is::Filling, .page = page};
            }
            const std::optional<baked::PageStore::Read> read = snapshot.page(page);
            if (!read || !read->page.shape || !read->page.material || !read->page.water) {
                return {.is = Next::Is::Fill, .page = page};
            }
            const std::uint8_t levels = grid.levels_under(page.depth);
            const auto foot = static_cast<std::uint8_t>(page.depth + levels);
            const grid::Key over = node.ancestor(std::min(node.depth, foot));
            const auto below = static_cast<std::uint8_t>(over.depth - page.depth);
            const auto found =
                read->page.shape->tree.at(below, over.x - (page.x << below),
                                          over.y - (page.y << below), over.z - (page.z << below));
            if (node.depth <= foot || !found.open) {
                return {.is = Next::Is::Answered, .page = page};
            }
            page = node.ancestor(foot);
        }
    }

    void start(grid::Key page) {
        filling.emplace(page, std::make_unique<jobs::Async<globe::Filled>>(*jobs, [this, page] {
                            return globe::fill(*generator, grid, page,
                                               grid.levels_under(page.depth));
                        }));
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
        if (Result<void> written = write_pages(*store, pages, filled); !written) {
            return std::unexpected{written.error()};
        }
        return pages;
    }
};

Filler::Filler(const globe::Generator& generator, baked::PageStore& store, jobs::JobSystem& jobs)
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
    {
        const std::scoped_lock guard{impl.lock};
        impl.waiting.insert(impl.waiting.end(), impl.asked.begin(), impl.asked.end());
        impl.asked.clear();
    }
    if (impl.waiting.empty()) {
        return written;
    }
    Result<baked::PageStore::Snapshot> snapshot = impl.store->read();
    if (!snapshot) {
        return std::unexpected{snapshot.error()};
    }
    std::vector<grid::Key> still;
    for (const grid::Key node : impl.waiting) {
        const Next next = impl.next(*snapshot, node);
        if (next.is == Next::Is::Fill) {
            impl.start(next.page);
        }
        if (next.is != Next::Is::Answered) {
            still.push_back(node);
        }
    }
    impl.waiting = std::move(still);
    return written;
}

bool Filler::idle() const {
    const std::scoped_lock guard{impl_->lock};
    return impl_->asked.empty() && impl_->waiting.empty() && impl_->filling.empty();
}

} // namespace hog::bake
