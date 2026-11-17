#pragma once

#include <hog/baked/page_store.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/grid/key.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace hog::bake {

// Writes each of `pages` as filled, in one batch.
inline Result<void> write_pages(baked::PageStore& store, std::span<const grid::Key> pages,
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

} // namespace hog::bake
