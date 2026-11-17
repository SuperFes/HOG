#pragma once

#include <hog/client/held_pages.hpp>
#include <hog/core/math.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/view.hpp>

#include <cstdint>
#include <vector>

namespace hog::client {

// A held page as drawn: how deep, and the pages under it drawn in its place, which it leaves out.
struct DrawnPage {
    grid::Key page;
    std::uint8_t levels = 0;
    std::vector<grid::Key> under; // in order

    friend bool operator==(const DrawnPage&, const DrawnPage&) = default;
};

// The held pages drawn as seen from `eye`, metres from the planet's centre, so each place is
// drawn once: from the top page down, each as deep as `view` sees it from there and as deep as
// it's held; under a page drawn whole, the held pages its open leaves lead to where the view
// wants them, in its place. In order of the pages' roots.
[[nodiscard]] std::vector<DrawnPage> drawn_pages(const HeldPages& held, DVec3 eye,
                                                 const grid::View& view = grid::View{});

} // namespace hog::client
