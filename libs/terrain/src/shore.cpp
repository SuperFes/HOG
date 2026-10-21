#include <hog/planet/projection.hpp>
#include <hog/terrain/shore.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hog::terrain {

ShoreFrame shore_frame(DVec3 eye, double radius) {
    const int level = planet::level_for(shore_anchoring, radius);
    const DVec3 anchor = planet::cell_center(planet::cell_at(eye, level));
    // Any way across will do at the poles, where east has none.
    DVec3 across = glm::cross(DVec3{0.0, 0.0, 1.0}, anchor);
    if (glm::length(across) < 1e-6) {
        across = glm::cross(DVec3{1.0, 0.0, 0.0}, anchor);
    }
    const DVec3 east = glm::normalize(across);
    return {.anchor = anchor, .east = east, .north = glm::cross(anchor, east)};
}

DVec3 shore_direction(const ShoreFrame& frame, double radius, DVec2 along) {
    return glm::normalize((frame.anchor * radius) + (frame.east * along.x) +
                          (frame.north * along.y));
}

DVec2 shore_along(const ShoreFrame& frame, double radius, DVec3 direction) {
    const double scale = radius / glm::dot(direction, frame.anchor);
    return DVec2{glm::dot(direction, frame.east), glm::dot(direction, frame.north)} * scale;
}

std::vector<float> shore_depths(const planet::Elevation& terrain, const ShoreFrame& frame,
                                double radius, double spacing) {
    std::vector<float> depths;
    depths.reserve(static_cast<std::size_t>(shore_size) * shore_size);
    const double first = (0.5 - (shore_size / 2.0)) * spacing;
    for (int row = 0; row < shore_size; ++row) {
        for (int column = 0; column < shore_size; ++column) {
            const DVec3 at = shore_direction(frame, radius,
                                             {first + (column * spacing), first + (row * spacing)});
            const double elevation =
                planet::fixed_to_double(terrain.elevation(planet::cell_at(at)));
            depths.push_back(std::max(static_cast<float>(-elevation), shore_highest));
        }
    }
    return depths;
}

namespace {

constexpr auto shore_texels = static_cast<std::size_t>(shore_size) * shore_size;

// Water at least this deep is taken as the open sea.
constexpr float open_sea = 20.0F; // metres

// Spreads what the sea reaches from the texels marked to every texel lower than its reach.
void spread(std::span<const float> layer, std::vector<std::uint8_t>& reached) {
    std::vector<std::size_t> reaching;
    for (std::size_t at = 0; at < reached.size(); ++at) {
        if (reached[at] != 0) {
            reaching.push_back(at);
        }
    }
    const auto size = static_cast<std::size_t>(shore_size);
    while (!reaching.empty()) {
        const std::size_t at = reaching.back();
        reaching.pop_back();
        const std::size_t column = at % size;
        const std::size_t row = at / size;
        const auto reach = [&](bool inside, std::size_t next) {
            if (inside && reached[next] == 0 && layer[next] > -shore_reach) {
                reached[next] = 1;
                reaching.push_back(next);
            }
        };
        reach(column > 0, at - 1);
        reach(column + 1 < size, at + 1);
        reach(row > 0, at - size);
        reach(row + 1 < size, at + size);
    }
}

} // namespace

namespace {

// Whether the texel at `column` and `row` of layer `layer` is open sea: on the coarsest, deep or
// water at its edge; on any finer, water where the coarser layer's sea, `coarser`, reaches.
bool open_sea_at(std::span<const float> depths, std::size_t layer, std::size_t column,
                 std::size_t row, const std::vector<std::uint8_t>& coarser) {
    const auto size = static_cast<std::size_t>(shore_size);
    const std::size_t layers = depths.size() / shore_texels;
    const float depth = depths[(layer * shore_texels) + (row * size) + column];
    if (layer + 1 == layers) {
        const bool edge = row == 0 || column == 0 || row + 1 == size || column + 1 == size;
        return depth >= open_sea || (edge && depth > 0.0F);
    }
    if (depth <= 0.0F) {
        return false;
    }
    // The coarser layer's texel this one's centre lies in.
    const double ratio = shore_spacing.at(layer) / shore_spacing.at(layer + 1);
    const auto coarse = [&](std::size_t at) {
        return std::floor(((static_cast<double>(at) + 0.5 - (shore_size / 2.0)) * ratio) +
                          (shore_size / 2.0));
    };
    const double coarse_column = coarse(column);
    const double coarse_row = coarse(row);
    if (coarse_column < 0.0 || coarse_row < 0.0 || coarse_column >= shore_size ||
        coarse_row >= shore_size) {
        return false;
    }
    const std::size_t under =
        (static_cast<std::size_t>(coarse_row) * size) + static_cast<std::size_t>(coarse_column);
    return coarser[under] != 0 && depths[((layer + 1) * shore_texels) + under] > 0.0F;
}

} // namespace

void keep_to_sea_reach(std::span<float> depths) {
    const std::size_t layers = depths.size() / shore_texels;
    const auto size = static_cast<std::size_t>(shore_size);
    std::vector<std::uint8_t> coarser;
    for (std::size_t layer = layers; layer-- > 0;) {
        std::vector<std::uint8_t> reached(shore_texels, 0);
        for (std::size_t row = 0; row < size; ++row) {
            for (std::size_t column = 0; column < size; ++column) {
                reached[(row * size) + column] =
                    open_sea_at(depths, layer, column, row, coarser) ? 1 : 0;
            }
        }
        const std::span<float> here = depths.subspan(layer * shore_texels, shore_texels);
        spread(here, reached);
        for (std::size_t at = 0; at < shore_texels; ++at) {
            if (reached[at] == 0) {
                here[at] = shore_highest;
            }
        }
        coarser = std::move(reached);
    }
}

void ShoreStream::follow(DVec3 eye) {
    wanted_ = shore_frame(eye, radius_);
    if (!working_ && wanted_ != started_) {
        start(*wanted_);
    }
}

void ShoreStream::start(const ShoreFrame& frame) {
    started_ = frame;
    working_ = std::make_unique<jobs::Async<ShoreDepths>>(
        *jobs_, [terrain = terrain_, frame, radius = radius_] {
            ShoreDepths found{.frame = frame, .depths = {}};
            for (const double spacing : shore_spacing) {
                const std::vector<float> layer = shore_depths(*terrain, frame, radius, spacing);
                found.depths.insert(found.depths.end(), layer.begin(), layer.end());
            }
            keep_to_sea_reach(found.depths);
            return found;
        });
}

std::optional<ShoreDepths> ShoreStream::collect() {
    if (!working_ || !working_->done()) {
        return std::nullopt;
    }
    ShoreDepths found = working_->take();
    working_.reset();
    if (wanted_ && wanted_ != started_) {
        start(*wanted_);
    }
    return found;
}

std::optional<ShoreDepths> ShoreStream::finish() {
    std::optional<ShoreDepths> last;
    while (working_) {
        last = working_->take();
        working_.reset();
        if (wanted_ && wanted_ != started_) {
            start(*wanted_);
        }
    }
    return last;
}

} // namespace hog::terrain
