#include <hog/server/page_source.hpp>

namespace hog::server {

Result<PageSource> PageSource::open(baked::PageStore& store, baked::Filler* filler) {
    Result<baked::PageStore::Snapshot> snapshot = store.read();
    if (!snapshot) {
        return std::unexpected{snapshot.error()};
    }
    return PageSource{std::move(*snapshot), filler};
}

std::optional<grid::Page> PageSource::page(grid::Key root, std::uint8_t levels, baked::Rank rank) {
    std::optional<baked::PageStore::Read> read = snapshot_.page(root);
    if (!read || !read->page.shape || !read->page.material || !read->page.water) {
        ask(root, levels, rank);
        return std::nullopt;
    }
    grid::Page& page = read->page;
    if (page.shape->tree.levels() < levels) {
        ask(root, levels, rank);
    }
    page.shape->tree = page.shape->tree.trimmed(levels);
    page.material->tree = page.material->tree.trimmed(levels);
    page.water->tree = page.water->tree.trimmed(levels);
    // Pages filled before woods were carry none; they come with the next fill.
    if (page.woods) {
        page.woods->tree = page.woods->tree.trimmed(levels);
    }
    return std::move(page);
}

std::optional<grid::Tree<grid::Shape>> PageSource::shape(grid::Key root, std::uint8_t levels,
                                                         baked::Rank rank) {
    std::optional<grid::Held<grid::Shape>> held = snapshot_.tree<grid::Shape>(root);
    if (!held || held->tree.levels() < levels) {
        ask(root, levels, rank);
    }
    if (!held) {
        return std::nullopt;
    }
    return held->tree.trimmed(levels);
}

void PageSource::ask(grid::Key root, std::uint8_t levels, baked::Rank rank) {
    if (filler_ != nullptr) {
        // The corner node `levels` under the root, so the filler goes that deep into the page.
        filler_->want({.depth = static_cast<std::uint8_t>(root.depth + levels),
                       .x = root.x << levels,
                       .y = root.y << levels,
                       .z = root.z << levels},
                      rank);
    }
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
