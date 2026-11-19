#include <hog/bake/bake.hpp>
#include <hog/baked/filling.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/grid/leaves.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <utility>

namespace hog::bake {
namespace {

// A page to bake, and how many levels under its root.
struct Want {
    grid::Key page;
    std::uint8_t levels = 0;
};

// The pages of `wants` the store holds at least as deep as wanted as this generator makes them,
// by their shape trees.
Result<std::vector<std::optional<grid::Tree<grid::Shape>>>> held(const baked::PageStore& store,
                                                                 std::span<const Want> wants) {
    Result<baked::PageStore::Snapshot> snapshot = store.read();
    if (!snapshot) {
        return std::unexpected{snapshot.error()};
    }
    std::vector<std::optional<grid::Tree<grid::Shape>>> shapes;
    shapes.reserve(wants.size());
    for (const Want& want : wants) {
        std::optional<baked::PageStore::Read> read = snapshot->page(want.page);
        if (read && read->page.shape && read->page.material && read->page.water &&
            read->page.shape->tree.levels() >= want.levels) {
            shapes.emplace_back(std::move(read->page.shape->tree));
        } else {
            shapes.emplace_back();
        }
    }
    return shapes;
}

// Fills those of `wants` the store doesn't hold deep enough yet and writes them together, giving
// each page's shape tree, held or filled.
Result<std::vector<grid::Tree<grid::Shape>>>
bake_pages(const globe::Generator& generator, const grid::Grid& grid, baked::PageStore& store,
           jobs::JobSystem& jobs, std::span<const Want> wants, Baked& baked) {
    auto shapes = held(store, wants);
    if (!shapes) {
        return std::unexpected{shapes.error()};
    }
    std::vector<grid::Key> missing;
    std::vector<std::uint8_t> levels;
    std::vector<std::size_t> slots;
    for (std::size_t n = 0; n < wants.size(); ++n) {
        if (!(*shapes)[n]) {
            missing.push_back(wants[n].page);
            levels.push_back(wants[n].levels);
            slots.push_back(n);
        }
    }
    std::vector<std::optional<globe::Filled>> filled(missing.size());
    jobs.parallel_for(static_cast<std::uint32_t>(missing.size()), 1,
                      [&](jobs::Range range, std::uint32_t) {
                          for (std::uint32_t n = range.begin; n < range.end; ++n) {
                              filled[n] = globe::fill(generator, grid, missing[n], levels[n]);
                          }
                      });
    if (Result<void> written = baked::write_filled(store, missing, filled); !written) {
        return std::unexpected{written.error()};
    }
    for (std::size_t m = 0; m < missing.size(); ++m) {
        globe::Filled& page = filled[m].value();
        baked.shape_bytes += page.shape.encode().size();
        baked.material_bytes += page.material.encode().size();
        baked.water_bytes += page.water.encode().size();
        baked.woods_bytes += page.woods.encode().size();
        (*shapes)[slots[m]] = std::move(page.shape);
    }
    baked.filled += missing.size();
    baked.kept += wants.size() - missing.size();
    std::vector<grid::Tree<grid::Shape>> made;
    made.reserve(wants.size());
    for (std::optional<grid::Tree<grid::Shape>>& shape : *shapes) {
        made.push_back(std::move(shape).value());
    }
    return made;
}

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

std::uint8_t levels_for(const grid::Grid& grid, const Reach& reach, grid::Key page,
                        bool surface) noexcept {
    const std::uint8_t whole = grid.levels_under(page.depth);
    if (reach.around && grid.distance(page, *reach.around) <= reach.across / 2.0) {
        return whole;
    }
    const std::uint8_t deepest = depth_of(grid, reach.coarse);
    if (!surface || deepest <= page.depth) {
        return 0;
    }
    return std::min<std::uint8_t>(whole, static_cast<std::uint8_t>(deepest - page.depth));
}

std::vector<grid::Key> pages_under(const grid::Grid& grid, const Reach& reach, grid::Key page,
                                   grid::Key leaf, bool surface) {
    // Rooted at the page's foot, which is the leaf itself only where the leaf is at that foot.
    const auto roots = static_cast<std::uint8_t>(page.depth + grid.levels_under(page.depth));
    if (roots >= grid.depth()) {
        return {};
    }
    const bool every = surface && depth_of(grid, reach.coarse) > roots;
    const auto near = [&](grid::Key key) {
        return reach.around && grid.distance(key, *reach.around) <= reach.across / 2.0;
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
    // Depth first: a batch off the top, then the pages under it on top in Z order, so only the
    // pages along the way down wait, and neighbours are filled and written together.
    std::vector<Want> pending;
    if (const std::uint8_t levels = levels_for(grid, reach, grid::Key{}, true); levels > 0) {
        pending.push_back({.page = grid::Key{}, .levels = levels});
    }
    std::vector<Want> wants;
    std::vector<Want> under;
    while (!pending.empty()) {
        const std::size_t take = std::min(batch, pending.size());
        wants.assign(pending.end() - static_cast<std::ptrdiff_t>(take), pending.end());
        pending.resize(pending.size() - take);
        auto shapes = bake_pages(generator, grid, store, jobs, wants, baked);
        if (!shapes) {
            return std::unexpected{shapes.error()};
        }
        under.clear();
        for (std::size_t n = 0; n < wants.size(); ++n) {
            if (wants[n].levels < grid.levels_under(wants[n].page.depth)) {
                continue;
            }
            grid::for_open_leaves(
                (*shapes)[n], wants[n].page, [&](grid::Key leaf, const auto& found) {
                    const bool surface = !found.value.is_solid();
                    for (const grid::Key page :
                         pages_under(grid, reach, wants[n].page, leaf, surface)) {
                        under.push_back(
                            {.page = page, .levels = levels_for(grid, reach, page, surface)});
                    }
                });
        }
        std::ranges::sort(under, std::ranges::greater{}, &Want::page);
        pending.insert(pending.end(), under.begin(), under.end());
        if (progress) {
            progress(baked.filled + baked.kept, pending.size());
        }
    }
    return baked;
}

} // namespace hog::bake
