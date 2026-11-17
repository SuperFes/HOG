#include <hog/server/page_source.hpp>

namespace hog::server {

Result<PageSource> PageSource::open(baked::PageStore& store, baked::Filler* filler) {
    Result<baked::PageStore::Snapshot> snapshot = store.read();
    if (!snapshot) {
        return std::unexpected{snapshot.error()};
    }
    return PageSource{std::move(*snapshot), filler};
}

std::optional<grid::Page> PageSource::page(grid::Key root) {
    std::optional<baked::PageStore::Read> read = snapshot_.page(root);
    if (read && read->page.shape && read->page.material && read->page.water) {
        return std::move(read->page);
    }
    if (filler_ != nullptr) {
        // A node under the root, so the filler goes down into the page and not just to its root
        // as the foot of the page over it.
        filler_->want(root.child(0));
    }
    return std::nullopt;
}

Result<std::vector<grid::Key>> PageSource::pump(bool wait) {
    std::vector<grid::Key> written;
    if (filler_ != nullptr) {
        auto pumped = filler_->pump(wait);
        if (!pumped) {
            return pumped;
        }
        written = std::move(*pumped);
    }
    if (Result<void> refreshed = snapshot_.refresh(); !refreshed) {
        return std::unexpected{refreshed.error()};
    }
    return written;
}

} // namespace hog::server
