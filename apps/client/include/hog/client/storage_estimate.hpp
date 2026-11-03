#pragma once

#include <string>

namespace hog::client {

// Bytes a column of the world's surface costs to store, per format, from what was measured on
// generated Earth-like ground at 0.5 m cells (zstd, per shard); rougher at other cell sizes.
struct StorageCosts {
    double bricks = 35.3;      // today's bricks: 1 mm distances, normals and derived fields kept
    double narrow_band = 6.9;  // bricks with 2 cm distances in a narrow band, nothing derivable kept
    double surface = 0.64;     // only the ground's height, to 2 cm
};

struct StorageEstimate {
    double bricks = 0.0; // bytes
    double narrow_band = 0.0;
    double surface = 0.0;
};

// What `columns` columns of the surface cost in each format.
[[nodiscard]] StorageEstimate estimate_storage(double columns, const StorageCosts& costs = {});

// Bytes as B, KB, MB, GB or TB.
[[nodiscard]] std::string readable_bytes(double bytes);

// A short account of what storing the ground drawn at full detail, and the whole planet, would
// cost, at cells `cell_metres` wide: `walked_columns` and `planet_columns` columns each.
[[nodiscard]] std::string describe_storage(double walked_columns, double planet_columns,
                                           double cell_metres, double walked_square_metres);

} // namespace hog::client
