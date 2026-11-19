#include <hog/bake/bake.hpp>
#include <hog/globe/grid_fill.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace hog::bake {
namespace {

// The shape trees of `regions`, held or filled now, filling and writing together those the store
// doesn't hold yet.
Result<std::vector<grid::Tree<grid::Shape>>>
bake_regions(const globe::Generator& generator, const grid::Grid& grid, baked::ChunkStore& store,
             jobs::JobSystem& jobs, std::span<const baked::Region> regions, Baked& baked) {
    const baked::Bands& bands = store.bands();
    Result<baked::ChunkStore::Snapshot> snapshot = store.read();
    if (!snapshot) {
        return std::unexpected{snapshot.error()};
    }
    std::vector<std::optional<grid::Tree<grid::Shape>>> shapes(regions.size());
    std::vector<baked::Region> missing;
    std::vector<std::size_t> slots;
    for (std::size_t n = 0; n < regions.size(); ++n) {
        const baked::Region region = regions[n];
        if (baked::holds(*snapshot, region)) {
            if (auto piece = snapshot->piece<grid::Shape>(region.root, bands.foot(region.band))) {
                shapes[n] = std::move(piece->tree);
                continue;
            }
        }
        missing.push_back(region);
        slots.push_back(n);
    }
    std::vector<std::optional<globe::Filled>> filled(missing.size());
    jobs.parallel_for(static_cast<std::uint32_t>(missing.size()), 1,
                      [&](jobs::Range range, std::uint32_t) {
                          for (std::uint32_t n = range.begin; n < range.end; ++n) {
                              filled[n] = baked::fill_region(generator, grid, bands, missing[n]);
                          }
                      });
    if (Result<void> written = baked::write_regions(store, missing, filled); !written) {
        return std::unexpected{written.error()};
    }
    for (std::size_t m = 0; m < missing.size(); ++m) {
        globe::Filled& trees = filled[m].value();
        baked.shape_bytes += trees.shape.encode().size();
        baked.material_bytes += trees.material.encode().size();
        baked.water_bytes += trees.water.encode().size();
        baked.woods_bytes += trees.woods.encode().size();
        shapes[slots[m]] = std::move(trees.shape);
    }
    baked.filled += missing.size();
    baked.kept += regions.size() - missing.size();
    std::vector<grid::Tree<grid::Shape>> made;
    made.reserve(regions.size());
    for (std::optional<grid::Tree<grid::Shape>>& shape : shapes) {
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

bool wanted(const grid::Grid& grid, const baked::Bands& bands, const Reach& reach,
            baked::Region region, bool surface) noexcept {
    if (reach.around && grid.distance(region.root, *reach.around) <= reach.across / 2.0) {
        return true;
    }
    return surface && std::isfinite(reach.coarse) &&
           region.band <= bands.band_of(depth_of(grid, reach.coarse));
}

Result<Baked> bake(const globe::Generator& generator, baked::ChunkStore& store,
                   jobs::JobSystem& jobs, const Reach& reach, const Pace& pace,
                   const Progress& progress) {
    const grid::Grid grid = globe::grid_of(generator);
    const baked::Bands& bands = store.bands();
    const baked::Regions regions{bands, std::max<std::uint8_t>(pace.fill_levels, 1)};
    const std::size_t batch = std::max<std::size_t>(pace.batch, 1);
    Baked baked;
    // Depth first: a batch off the top, then the regions under it on top in Z order, so only those
    // along the way down wait, and neighbours are filled and written together.
    std::vector<baked::Region> pending;
    if (const baked::Region top{}; wanted(grid, bands, reach, top, true)) {
        pending.push_back(top);
    }
    std::vector<baked::Region> taken;
    std::vector<baked::Region> under;
    while (!pending.empty()) {
        const std::size_t take = std::min(batch, pending.size());
        taken.assign(pending.end() - static_cast<std::ptrdiff_t>(take), pending.end());
        pending.resize(pending.size() - take);
        auto shapes = bake_regions(generator, grid, store, jobs, taken, baked);
        if (!shapes) {
            return std::unexpected{shapes.error()};
        }
        under.clear();
        for (std::size_t n = 0; n < taken.size(); ++n) {
            const auto next = static_cast<std::uint8_t>(taken[n].band + 1);
            if (next >= bands.count()) {
                continue;
            }
            regions.each_under(
                next, taken[n].root, (*shapes)[n], [&](baked::Region region, const auto& found) {
                    if (wanted(grid, bands, reach, region, !found.value.is_solid())) {
                        under.push_back(region);
                    }
                });
        }
        std::ranges::sort(under, std::ranges::greater{}, &baked::Region::root);
        pending.insert(pending.end(), under.begin(), under.end());
        if (progress) {
            progress(baked.filled + baked.kept, pending.size());
        }
    }
    return baked;
}

} // namespace hog::bake
