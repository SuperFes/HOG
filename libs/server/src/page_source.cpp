#include <hog/server/page_source.hpp>

#include <algorithm>

namespace hog::server {
namespace {

// About what a decoded tree's node takes in memory, with its place in the tree's index.
constexpr std::size_t node_bytes = 128;

template <typename Kind>
std::size_t held_bytes(const std::optional<grid::Held<Kind>>& tree) {
    return tree ? tree->tree.nodes() * node_bytes : 0;
}

std::size_t held_bytes(const grid::Page& page) {
    return held_bytes(page.shape) + held_bytes(page.material) + held_bytes(page.water) +
           held_bytes(page.woods);
}

template <typename Kind>
void cut(std::optional<grid::Held<Kind>>& tree, std::uint8_t levels) {
    if (tree) {
        tree->tree = tree->tree.trimmed(levels);
    }
}

} // namespace

Result<PageSource> PageSource::open(baked::PageStore& store, baked::Filler* filler,
                                    std::size_t budget) {
    Result<baked::PageStore::Snapshot> snapshot = store.read();
    if (!snapshot) {
        return std::unexpected{snapshot.error()};
    }
    return PageSource{std::move(*snapshot), filler, budget};
}

std::optional<grid::Page> PageSource::page(grid::Key root, std::uint8_t levels, baked::Rank rank) {
    const std::shared_ptr<const Answer> found = answer(root, levels, rank);
    if (!found) {
        return std::nullopt;
    }
    return found->page;
}

std::shared_ptr<const Answer> PageSource::answer(grid::Key root, std::uint8_t levels,
                                                 baked::Rank rank) {
    const baked::PageStore::Versions versions = snapshot_.versions(root);
    if (versions.shape == 0 || versions.material == 0 || versions.water == 0) {
        let_go(root);
        ask(root, levels, rank);
        return nullptr;
    }
    auto found = kept_.find(root);
    if (found != kept_.end() && found->second.versions != versions) {
        let_go(root);
        found = kept_.end();
    }
    if (found == kept_.end()) {
        std::optional<baked::PageStore::Read> read = snapshot_.page(root);
        if (!read || !read->page.shape || !read->page.material || !read->page.water) {
            ask(root, levels, rank);
            return nullptr;
        }
        recent_.push_front(root);
        found = kept_
                    .emplace(root, Page{.versions = versions,
                                        .levels = read->page.shape->tree.levels(),
                                        .answers = {},
                                        .bytes = 0,
                                        .recent = recent_.begin()})
                    .first;
        // Held whole for cutting any answer from, kept as the deepest answer.
        auto whole = std::make_shared<Answer>();
        whole->page = std::move(read->page);
        Page& page = found->second;
        page.bytes = held_bytes(whole->page);
        counts_.bytes += page.bytes;
        page.answers.emplace(page.levels, std::move(whole));
    } else {
        recent_.splice(recent_.begin(), recent_, found->second.recent);
    }
    Page& page = found->second;
    if (page.levels < levels) {
        ask(root, levels, rank);
    }
    const std::uint8_t depth = std::min(levels, page.levels);
    auto& kept = page.answers[depth];
    if (kept && !kept->bytes.empty()) {
        ++counts_.found;
        return kept;
    }
    auto made = std::make_shared<Answer>();
    made->page = page.answers.at(page.levels)->page;
    cut(made->page.shape, depth);
    cut(made->page.material, depth);
    cut(made->page.water, depth);
    // Pages filled before woods were carry none; they come with the next fill.
    cut(made->page.woods, depth);
    made->bytes = made->page.encode();
    if (made->page.water) {
        for (const grid::Water::Value body : made->page.water->tree.leaf_values()) {
            if (body != 0) {
                made->bodies.push_back(body);
            }
        }
    }
    std::ranges::sort(made->bodies);
    const auto [from, to] = std::ranges::unique(made->bodies);
    made->bodies.erase(from, to);
    // As deep as it's held, it takes the place of the page held whole, already counted.
    const std::size_t bytes =
        made->bytes.size() + (depth == page.levels ? 0 : held_bytes(made->page));
    page.bytes += bytes;
    counts_.bytes += bytes;
    ++counts_.made;
    kept = std::move(made);
    std::shared_ptr<const Answer> given = kept;
    while (counts_.bytes > budget_ && recent_.size() > 1) {
        let_go(recent_.back());
    }
    return given;
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

void PageSource::let_go(grid::Key root) {
    const auto found = kept_.find(root);
    if (found == kept_.end()) {
        return;
    }
    counts_.bytes -= found->second.bytes;
    recent_.erase(found->second.recent);
    kept_.erase(found);
    ++counts_.let_go;
}

Result<std::vector<grid::Key>> PageSource::pump(bool wait) {
    std::vector<grid::Key> written;
    if (filler_ != nullptr) {
        auto pumped = filler_->pump(wait);
        if (!pumped) {
            return pumped;
        }
        written = std::move(*pumped);
        for (const grid::Key page : filler_->take_ready()) {
            if (std::ranges::find(written, page) == written.end()) {
                written.push_back(page);
            }
        }
    }
    if (Result<void> refreshed = snapshot_.refresh(); !refreshed) {
        return std::unexpected{refreshed.error()};
    }
    for (const grid::Key page : written) {
        let_go(page);
    }
    return written;
}

} // namespace hog::server
