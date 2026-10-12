#pragma once

#include <hog/core/time.hpp>
#include <hog/planet/world.hpp>
#include <hog/probe/messages.hpp>
#include <hog/sim/forest.hpp>
#include <hog/sim/weather.hpp>
#include <hog/sim/wildlife.hpp>

#include <vector>

namespace hog::app {

// What debug tools can see of the world, all on the weather's grid.
struct LayerSources {
    const planet::World& planet;
    const sim::Weather& weather;
    const sim::Wildlife* wildlife = nullptr;
    const sim::Forest* forest = nullptr;
};

// Every layer, named as "group/field", such as "weather/temperature" or "wildlife/deer".
[[nodiscard]] std::vector<probe::LayerInfo> layer_catalogue(const LayerSources& sources);

// The layers asked for that exist, in catalogue order, as of `time`.
[[nodiscard]] probe::Layers fetch_layers(const LayerSources& sources, const probe::Fetch& fetch,
                                         GameTime time);

// Every layer's value at the cell under a checked point, in catalogue order, with the generated
// ground beneath it, as of `time`.
[[nodiscard]] probe::Inspection inspect_layers(const LayerSources& sources,
                                               const probe::Inspect& inspect, GameTime time);

} // namespace hog::app
