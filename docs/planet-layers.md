# Planet layers: what we have, what an MMO world needs

An audit of what the world presents today against what a planet must provide for an MMO, as of
2026-11-09. It covers the world itself, not combat, UI or social systems (see `roadmap.md`).

Sources agree on what separates a world that is "pretty but empty" from one "worth living in":

1. Resources placed by readable geology, scarce enough to trade.
2. An ecology that reacts to players and can be used up.
3. Landmarks that look different, with variety at planet, region and local scale.
4. Geography that makes chokepoints, risk gradients and natural town sites.
5. History and consequences that stay in the world.

We have the physical base for all five and none of the five yet.

## What the world presents today

| Layer | State | Reaches the client | Where |
|---|---|---|---|
| Plates, crust, ridges, arcs, trenches | working | yes | `formation/src/plates.cpp` |
| Bedrock: strata, folds, soluble beds, lava | working | yes | `formation/src/bedrock.cpp`, `globe/src/strata.cpp` |
| 22 materials by property | working | yes | `globe/include/hog/globe/material.hpp` |
| Joints, crags, rockfall blocks | working | yes | `globe/src/joints.cpp`, `rockfall.cpp` |
| Caves (districts, passages, lava tubes) | partial, noise-driven | yes | `globe/src/caves.cpp` |
| Volcanic vents, lava poured near players | working | yes | `globe/src/vents.cpp` |
| Landform likelihoods (karst, mesas, scree…) | lab only | no | `formation/src/landforms.cpp` |
| Ore likelihoods (11 kinds) | lab only | no | `formation/src/ores.cpp` |
| Climate (energy balance, rain shadow, seasons) | working | yes | `formation/src/climate.cpp` |
| Rivers: incision, discharge, carved valleys | partial; courses WIP | valleys only, **no water** | `formation/src/rivers.cpp`, `globe/src/valleys.cpp` |
| Lakes | partial; shore walls | yes | `formation/src/lakes.cpp`, `globe/src/lake_fill.hpp` |
| Glaciers, fjords, till, outwash | working | yes | `formation/src/ice.cpp` |
| Soil layers (peat → saprolite, loess, alluvium) | working | yes | `formation/src/soil.cpp` |
| Coasts: shoreface, bars, beach kinds | working; cliffs next | yes | `globe/src/shoreface.cpp` |
| Live flow: water, lava, slump, wash, wear | working near players | yes | `flow/src/region.cpp` |
| Live weather: wind, cloud, rain, snow | working; ignores climate map | yes | `weather/src/wind.cpp` |
| Ocean waves, tides, shoaling, foam | working | yes | `render/src/ocean.cpp` |
| Star system: stars, moons, rings, planets | working | tides only; **no sky bodies drawn** | `planet/src/system.cpp` |
| Vegetation | **none** (purged) | no | `flora` keeps only leaf-season maths |
| Fauna | **none** (purged) | no | none |
| Biome / region labels | **none** | no | none |
| POIs, settlements, roads, routes | **none** | no | none |
| Bake to disk (tiles) | working | yes | `store/src/sqlite_volume_archive.cpp` |
| Player edits | partial; no permissions | yes | `Dug`/`Built` on the server |

## What the planet needs

Tiers follow what the world can't do without, then MMO structure, then depth. Each item builds on
something that already exists; "from" names that.

### Tier 1: the world is empty without these

1. **Running water in rivers.** Valleys are carved but dry. Channels hold water at the depth
   their discharge gives. Waterfalls form where courses cross crags. Rivers may sink into
   soluble rock or fail in deserts. *From:* discharge, courses, channel geometry in
   `valleys.cpp`.
2. **Lake fine shore fill.** Water ends only where the ground stands at or above its level, and
   outlets are recorded. *From:* WIP fill (in progress).
3. **Plant habitats.** The generator marks where each kind of plant can live, from
   temperature, rain, soil depth, drainage and the moisture record. Plant life itself is
   already planned and runs after generation, reading these marks. *From:* climate, soil
   layers, the flow's moisture record, `hog-vegetation-architecture`.
4. **Sky bodies.** Moons start as billboards, placed and lit (phase) from the same ephemeris
   that already drives the tides (`apps/client/src/tides.cpp`), so high water follows the
   moon you see. Rings and other planets come later. Moonlight sets how dark nights get. Stars are a seeded field of real points in 3D around
   the system, each with a distance, brightness and colour from its temperature. They're seen
   along the planet's spin axis and orbit, so the sky turns once a sidereal day, the night sky
   changes through the year, and latitude decides which stars rise, set or circle a pole star.
   That's enough to navigate by. *From:* `planet/src/ephemeris.cpp`, `system.cpp`.
5. **Weather that agrees with the climate.** Live rain and temperature follow the formation's
   climate, and seasons show on the ground: snow cover, frozen lakes, leaf colour, river levels.
   *From:* climate model, `WeatherKeeper`, `hog-rain-climate-mismatch`.
6. **Ores in the voxels.** Ore bodies in their host rock and strata, placers in river gravels,
   coal in its beds. Surface tells (stained soil, float rocks in streams) let prospecting be
   read from the ground. *From:* `ores.cpp` maps, strata, rockfall blocks as carriers of float.

### Tier 2: MMO structure

7. **Fauna as populations.** Species fed by plant life, with food webs, seasonal migration
   along terrain, and herds that thin when overhunted. Spawning reads population, not spawn
   tables. *From:* vegetation, travel-cost graph.
8. **Ecoregions and names.** Labels derived from continuous fields (climate, soil, cover,
   relief), never authored. Named ranges, rivers, coasts and regions hold a map together.
   Gameplay tables and the map read these. *From:* all formation fields.
9. **Travel-cost graph.** Cost from slope, water, snow, soil and cover. It shows passes, fords,
   straits and chokepoints, and gives the natural routes later roads follow. Snow closes passes
   by season. *From:* relief, rivers, climate.
10. **Settlement suitability.** Water, flat ground, soil fertility, defensibility, route
    junctions and resources combined into one field. NPC towns grow at its peaks, and players
    can read it. *From:* travel graph, soil, rivers, ores.
11. **Landmarks.** Score prominence and visibility, and make sure rare, distinctive features
    exist at km scale and stand apart: crater lakes, volcanoes, gorges, arches, sea stacks,
    waterfalls, hoodoos, and features from faults and geothermal activity (springs, geysers).
    *From:* landform likelihoods (lab), `hog-world-events` backlog, coasts.
12. **Gameplay field queries and meta areas.** One server API answers "what's here":
    temperature, moisture, fertility, ore, danger, region, owner, rules. Meta areas carry what
    is authored or derived. *From:* `hog-meta-areas` (queued).
13. **Bake the formation.** Save `Formed` to a file so generation runs once. Quality can then
    beat speed. *From:* SQLite tile archive; the formation cache is missing.

### Tier 3: depth

14. **Groundwater.** A water table and aquifers give wells, springs, losing streams, irrigation
    and flooding mines. *From:* soluble beds, porosity by material, flow pore water.
15. **Caves formed by water.** Km-scale systems in kept beds, with their own ecology, ore access
    and mine stability. *From:* `hog-caves-and-visibility` (queued).
16. **Hazards from real processes.** Eruptions, landslides, avalanches, seasonal floods, quakes
    on faults, storms, wildfire. Each runs near players and leaves its mark. *From:* vents,
    slump, discharge, live weather.
17. **History and ruins.** A simulated past of people, settlements, roads, mines and wars leaves
    ruins and old routes where suitability and resources put them. It is the strongest fix for
    samey places. *From:* settlement suitability, travel graph.
18. **Sources and sinks.** Biomass regrows through the simulation. Ore is finite with deep
    reserves. Edits and builds weather and decay (`hog-things-fall-apart`). This avoids
    Ultima Online's closed-pool collapse.
19. **Scale and density.** At real radius, walking around the planet takes about 8,000 hours.
    Interest has to concentrate (starting regions, natural cores) and travel comes in tiers:
    boats on navigable rivers and seas under live wind, mounts, later flight. *From:* rivers,
    live wind, travel graph.
20. **Persistent edits at scale.** An edits store, permissions and claims, with edits visible to
    all and surviving rebakes. *From:* `Dug`/`Built`, the parked edits store.
21. **Other bodies.** Moons and planets as playable worlds with their own gravity, atmosphere
    and materials (anorthosite and regolith already exist). One planet per server for now.
    *From:* `planet/src/system.cpp`.

## Suggested order

Finish what's open: courses, lake shore fill, coast cliffs. Then:

1. River water (1)
2. Bake the formation (13)
3. Plant habitats (3); plant life follows on its own track
4. Ores in voxels (6)
5. Sky bodies (4)
6. Weather and seasons (5)
7. Ecoregions (8)
8. Travel graph (9)
9. Settlements (10)
10. Landmarks (11)
11. Fauna (7)

Tier 3 follows; history waits on settlements.

## Finishing generation

Agreed 2026-11-10, ahead of any client/server platform change. Every feature here is generator
maths that reads at any tile level and lands in the octree's voxels, water included, with coarse
tiles holding the right average of the finer ones. No side strips, meshes, maps or per-feature
passes beside the tree. The formation bake is an input to that maths, not a second world.

Found missing on 2026-11-10: the default planet forms only 14 lakes, most a point or two across,
and three of them are 1–3 km deep basins standing 26–64 m above the sea. Salt flats, dunes and sea
ice form in `libs/formation` and never reach the voxels. Nothing builds deltas, estuaries, fans,
terraces, floodplains, wetlands, springs, calderas or reefs.

1. **Bake the formation** (13), and cache coarse tiles as the disposable tile cache keyed by world
   and generator revision. `Formed` costs ~172 s every start, but the formation stages are ~5 s:
   166 s is one lake's fill, a single-point lake at 352 m spreading over 1878×2224 cells. Fix that
   runaway first. Tiles of levels 7–13 cost ~66 s of a first view.
2. **World census.** One tool printing, over several seeds: lakes by size and level, wet river
   length, wetland share, salt flats, dunes, beach kinds, caves. Regressions show as numbers.
3. **Finish what's open:** courses, coast cliffs and shore platforms, lake shore fill.
4. **River water** (1): channels at the depth their discharge gives, waterfalls over crags, rivers
   sinking into soluble rock or failing in deserts, mouths, deltas and estuaries.
5. **Lakes from their processes:** glacial scour basins and kettles, oxbows and floodplain ponds,
   dolines and cenotes, crater lakes, fault sag ponds, landslide dams, thaw lakes. Closed basins
   stand where evaporation balances inflow, below the sea if that's where it falls.
6. **Formed results into the voxels:** salt flats, dunes, sea ice, landform likelihoods (karst,
   mesas, scree), ores (6).
7. **Wet and flat ground:** marshes, bogs and fens where flat ground drains poorly; floodplains,
   terraces and alluvial fans; tidal flats and salt marsh on low coasts; snowfields that outlast
   the summer apart from glaciers; permafrost.
8. **Groundwater** (14): a water table giving springs and losing streams, and what caves formed by
   water (15) grow from.
9. **Volcanic and geothermal:** cones, calderas, crater lakes, hot springs and geysers from vents.
   Reefs and atolls on warm shallow shelves.
10. **Plant habitats** (3), where generation hands over to life.
11. **Fidelity at every level:** each term gives the right average when coarse, so LOD changes
    don't show, and coarse tiles cost less than fine ones rather than more.

## References

- Ores by host rock, climate bands, seasons: Vintage Story wiki,
  [Ore Deposits](https://wiki.vintagestory.at/index.php/Ore_Deposits),
  [Temperature](https://wiki.vintagestory.at/index.php/Temperature).
- Asymmetric, geology-driven resources: [Pax Dei FAQ](https://playpaxdei.com/en-us/faq/world).
- Closed-pool ecology collapse: Raph Koster on Ultima Online, [raphkoster.com](https://www.raphkoster.com/?p=46439).
- Population ecology, extinction: [Eco](https://en.wikipedia.org/wiki/Eco_(2018_video_game)).
- Biomes from geology and soil: Star Citizen [Genesis](https://starcitizen.tools/Genesis_%28Star_Engine%29).
- "10,000 bowls of oatmeal": Kate Compton, [Practical procedural generation](https://gamedeveloper.com/design/practical-procedural-generation-for-everyone-).
- Landmark hierarchy, the triangle rule: [Breath of the Wild GDC](https://gdcvault.com/play/1024562/).
- Players' mental maps of open worlds: [Wellesley study](https://cs.wellesley.edu/~pmwh/mvmap/papers/open_world_elements/open_world_elements.html).
- Node sites that grow with players: [Ashes of Creation](https://www.pcgamer.com/ashes-of-creation-explains-how-nodes-help-expand-its-reactive-mmo-world).
- Procedural history: Caves of Qud, [GDC](https://gdcvault.com/play/1025379/Procedurally-Generating-History-in-Caves);
  Dwarf Fortress [aquifers](https://dwarffortresswiki.org/aquifer).
