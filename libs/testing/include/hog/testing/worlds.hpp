#pragma once

namespace hog::testing {

// Metres between the cells worlds built for tests wear their land on: far coarser than a real
// world's, so they evolve in seconds. Tests of the finer wear itself keep a real world's cells.
inline constexpr double coarse_erosion_cell = 1'500.0;

} // namespace hog::testing
