#include <hog/viewer/frame.hpp>

#include <algorithm>
#include <utility>

namespace hog::viewer {

Frame::Frame(probe::Welcome welcome, probe::Layers layers, std::uint32_t cells)
    : welcome_(std::move(welcome)), time_(layers.time), clock_(layers.clock),
      layers_(std::move(layers.layers)) {
    std::erase_if(layers_,
                  [cells](const probe::Layer& layer) { return layer.values.size() != cells; });
}

std::span<const float> Frame::layer(std::string_view name) const noexcept {
    const auto found = std::ranges::find(layers_, name, &probe::Layer::name);
    if (found == layers_.end()) {
        return {};
    }
    return found->values;
}

std::vector<std::string> Frame::species() const {
    constexpr std::string_view prefix = "wildlife/";
    std::vector<std::string> species;
    for (const probe::LayerInfo& layer : welcome_.layers) {
        const std::string_view name = layer.name;
        if (!name.starts_with(prefix)) {
            continue;
        }
        const std::string_view rest = name.substr(prefix.size());
        // Forage and carrion are what animals eat, measured in kg rather than animals.
        if (!rest.contains('/') && layer.unit == "animals") {
            species.emplace_back(rest);
        }
    }
    return species;
}

} // namespace hog::viewer
