#include "pages.hpp"

#include <hog/bake/bake.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/globe/planet.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace hog::bake {
namespace {

// Metres from `point` to the nearest of the node.
double distance_to(const grid::Grid& grid, grid::Key key, DVec3 point) {
    const DVec3 low = grid.low(key);
    const DVec3 nearest = glm::clamp(point, low, low + DVec3{grid.size(key.depth)});
    return glm::length(point - nearest);
}

// Each open leaf of a page's shape tree, as its key in the grid, and whether it's open for the
// ground passing near rather than for caves in the solid.
template <typename Visit>
void open_leaves(const grid::Tree<grid::Shape>& tree, grid::Key root, const Visit& visit) {
    struct At {
        std::uint8_t depth = 0;
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::uint32_t z = 0;
    };
    std::vector<At> pending{{}};
    while (!pending.empty()) {
        const At at = pending.back();
        pending.pop_back();
        const auto found = tree.at(at.depth, at.x, at.y, at.z);
        const bool leaf = at.depth == tree.levels() ||
                          tree.at(at.depth + 1, 2 * at.x, 2 * at.y, 2 * at.z).depth == at.depth;
        if (!leaf) {
            for (std::uint32_t octant = 0; octant < 8; ++octant) {
                pending.push_back({.depth = static_cast<std::uint8_t>(at.depth + 1),
                                   .x = (2 * at.x) + (octant & 1U),
                                   .y = (2 * at.y) + ((octant >> 1U) & 1U),
                                   .z = (2 * at.z) + ((octant >> 2U) & 1U)});
            }
            continue;
        }
        if (found.open) {
            visit(grid::Key{.depth = static_cast<std::uint8_t>(root.depth + at.depth),
                            .x = (root.x << at.depth) + at.x,
                            .y = (root.y << at.depth) + at.y,
                            .z = (root.z << at.depth) + at.z},
                  !found.value.is_solid());
        }
    }
}

// The pages of `pages` the store holds whole as this generator makes them, by their shape trees.
Result<std::vector<std::optional<grid::Tree<grid::Shape>>>> held(const baked::PageStore& store,
                                                                 std::span<const grid::Key> pages) {
    Result<baked::PageStore::Snapshot> snapshot = store.read();
    if (!snapshot) {
        return std::unexpected{snapshot.error()};
    }
    std::vector<std::optional<grid::Tree<grid::Shape>>> shapes;
    shapes.reserve(pages.size());
    for (const grid::Key page : pages) {
        std::optional<baked::PageStore::Read> read = snapshot->page(page);
        if (read && read->page.shape && read->page.material && read->page.water) {
            shapes.emplace_back(std::move(read->page.shape->tree));
        } else {
            shapes.emplace_back();
        }
    }
    return shapes;
}

// Fills those of `pages` the store doesn't hold yet and writes them together, giving each page's
// shape tree, held or filled.
Result<std::vector<grid::Tree<grid::Shape>>>
bake_pages(const globe::Generator& generator, const grid::Grid& grid, baked::PageStore& store,
           jobs::JobSystem& jobs, std::span<const grid::Key> pages, Baked& baked) {
    auto shapes = held(store, pages);
    if (!shapes) {
        return std::unexpected{shapes.error()};
    }
    std::vector<grid::Key> missing;
    std::vector<std::size_t> slots;
    for (std::size_t n = 0; n < pages.size(); ++n) {
        if (!(*shapes)[n]) {
            missing.push_back(pages[n]);
            slots.push_back(n);
        }
    }
    std::vector<std::optional<globe::Filled>> filled(missing.size());
    jobs.parallel_for(
        static_cast<std::uint32_t>(missing.size()), 1, [&](jobs::Range range, std::uint32_t) {
            for (std::uint32_t n = range.begin; n < range.end; ++n) {
                filled[n] =
                    globe::fill(generator, grid, missing[n], grid.levels_under(missing[n].depth));
            }
        });
    if (Result<void> written = write_pages(store, missing, filled); !written) {
        return std::unexpected{written.error()};
    }
    for (std::size_t m = 0; m < missing.size(); ++m) {
        globe::Filled& page = filled[m].value();
        baked.shape_bytes += page.shape.encode().size();
        baked.material_bytes += page.material.encode().size();
        baked.water_bytes += page.water.encode().size();
        (*shapes)[slots[m]] = std::move(page.shape);
    }
    baked.filled += missing.size();
    baked.kept += pages.size() - missing.size();
    std::vector<grid::Tree<grid::Shape>> made;
    made.reserve(pages.size());
    for (std::optional<grid::Tree<grid::Shape>>& shape : *shapes) {
        made.push_back(std::move(shape).value());
    }
    return made;
}

} // namespace

baked::Baking baking_of(const globe::Generator& generator) {
    return {.world = globe::world_id(generator.planet()),
            .grid = globe::grid_of(generator),
            .generator = globe::generator_version};
}

std::vector<grid::Key> pages_under(const grid::Grid& grid, const Reach& reach, grid::Key page,
                                   grid::Key leaf, bool surface) {
    // Rooted at the page's foot, which is the leaf itself only where the leaf is at that foot.
    const auto roots = static_cast<std::uint8_t>(page.depth + grid.levels_under(page.depth));
    if (roots >= grid.depth()) {
        return {};
    }
    const bool every =
        surface &&
        grid.size(static_cast<std::uint8_t>(roots + grid.levels_under(roots))) >= reach.coarse;
    const auto near = [&](grid::Key key) {
        return reach.around && distance_to(grid, key, *reach.around) <= reach.across / 2.0;
    };
    std::vector<grid::Key> found;
    std::vector<grid::Key> pending{leaf};
    while (!pending.empty()) {
        const grid::Key key = pending.back();
        pending.pop_back();
        if (!every && !near(key)) {
            continue;
        }
        if (key.depth == roots) {
            found.push_back(key);
            continue;
        }
        for (std::size_t octant = 0; octant < 8; ++octant) {
            pending.push_back(key.child(octant));
        }
    }
    std::ranges::sort(found);
    return found;
}

Result<Baked> bake(const globe::Generator& generator, baked::PageStore& store,
                   jobs::JobSystem& jobs, const Reach& reach, std::size_t batch,
                   const Progress& progress) {
    const grid::Grid grid = globe::grid_of(generator);
    batch = std::max<std::size_t>(batch, 1);
    Baked baked;
    // A depth of pages at a time, top down, each leading to those under its open leaves.
    std::vector<grid::Key> wave{grid::Key{}};
    while (!wave.empty()) {
        std::vector<grid::Key> next;
        for (std::size_t from = 0; from < wave.size(); from += batch) {
            const std::span<const grid::Key> pages =
                std::span{wave}.subspan(from, std::min(batch, wave.size() - from));
            auto shapes = bake_pages(generator, grid, store, jobs, pages, baked);
            if (!shapes) {
                return std::unexpected{shapes.error()};
            }
            for (std::size_t n = 0; n < pages.size(); ++n) {
                open_leaves((*shapes)[n], pages[n], [&](grid::Key leaf, bool surface) {
                    const std::vector<grid::Key> under =
                        pages_under(grid, reach, pages[n], leaf, surface);
                    next.insert(next.end(), under.begin(), under.end());
                });
            }
            if (progress) {
                const std::size_t done = baked.filled + baked.kept;
                progress(done, done + (wave.size() - from - pages.size()) + next.size());
            }
        }
        std::ranges::sort(next);
        wave = std::move(next);
    }
    return baked;
}

} // namespace hog::bake
