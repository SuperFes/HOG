#include <hog/formation/world.hpp>

namespace hog::formation {

World::World(const Body& made_with, int layout_splits, int detail_splits)
    : body{made_with}, layout{layout_splits, body.seed}, sphere{detail_splits, body.seed + 1},
      plates{body, layout}, crust{crust_on(sphere, plates, body)},
      climate{climate_on(sphere, crust.base, crust.sea, body)},
      bedrock{bedrock_on(sphere, crust, body)},
      worn{
          rivers_on(sphere, crust, climate, standing_strength(sphere, crust, bedrock, body), body)},
      ice{ice_on(sphere, crust, climate, worn, body)}, rivers{ice_shaped(sphere, worn, ice)},
      outcrop{outcrop_on(sphere, crust, bedrock, rivers, body)},
      lakes{lakes_on(sphere, rivers, climate, body)},
      sediment{sediment_on(sphere, rivers, lakes, ice.outwash, body)},
      soil{soil_on(sphere, rivers, climate, ice, lakes, sediment, body)},
      landforms{landforms_on(sphere, crust, bedrock, outcrop, climate, rivers, ice, body)},
      ores{ores_on(sphere, crust, bedrock, outcrop, climate, rivers, lakes, sediment, soil, body)} {
}

} // namespace hog::formation
