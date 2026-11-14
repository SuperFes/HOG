#pragma once

#include <hog/formation/bedrock.hpp>
#include <hog/formation/body.hpp>
#include <hog/formation/climate.hpp>
#include <hog/formation/drifts.hpp>
#include <hog/formation/ice.hpp>
#include <hog/formation/lakes.hpp>
#include <hog/formation/landforms.hpp>
#include <hog/formation/ores.hpp>
#include <hog/formation/plates.hpp>
#include <hog/formation/regimes.hpp>
#include <hog/formation/rivers.hpp>
#include <hog/formation/sediment.hpp>
#include <hog/formation/shores.hpp>
#include <hog/formation/soil.hpp>
#include <hog/formation/sphere.hpp>

namespace hog::formation {

// Every stage of a planet's forming, each worked out from those before it: plates laid out on a
// coarse sphere, everything after held on a finer one.
struct World {
    explicit World(const Body& made_with, int layout_splits = 5, int detail_splits = 7);
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;
    ~World() = default;

    Body body;
    Sphere layout;
    Sphere sphere;
    Plates plates;
    Crust crust;
    Climate climate;
    Bedrock bedrock;
    Rivers worn; // before ice shaped the land
    Ice ice;
    Rivers rivers; // over the land as ice left it
    Outcrop outcrop;
    Lakes lakes;
    Regimes regimes;
    Sediment sediment;
    Shores shores;
    Drifts drifts;
    Soil soil;
    Landforms landforms;
    Ores ores;
};

} // namespace hog::formation
