# World v2: a planet that makes itself

Oct 28, 2026 · @Aaron Nixon

Today's world is stamped: a frozen plate snapshot plus noise sets the continents, sea level, mountain heights, volcanoes, ocean floor, beds, climate and biomes, and only the 5 Myr erosion loop, lake filling and live weather run as processes. The pieces are made apart and then forced to fit, which is where rivers fighting lakes, coasts frozen to a noise field and mountains counted twice come from.

World v2 builds the planet in the order nature does, each stage working from what the one before it left. Nothing is placed: mountains stand where plates collide, rain falls where they lift the air, rivers run where it drains, lakes sit where drainage is blocked, deltas build where sediment comes out. We don't loop over billions of years; each stage runs only enough steps, or solves its equilibrium directly, to reach a believable end state, then the coarse result is softened and given fine detail driven by the data.

## Principles

- **Stage order, one way.** A stage reads only what earlier stages stored. No stage reaches forward or stamps over a later one.
- **One source of truth per quantity.** One climate (erosion, biomes and live weather all start from it), one drainage, one sea level, one test for continental crust.
- **Stored fields, baked.** Large-scale results live on grids in the bake, not as functions recomputed from noise. Fine detail is synthesised on demand from them.
- **Solve, don't loop, where nature is at equilibrium.** Isostasy, climate, lakes and groundwater are solved directly; tectonics and landscape evolution take big implicit steps.
- **Judged by looking.** Every stage has map stills and fly-over stills, and tests for its physical invariants (mass conserved, water runs downhill, crust area kept).
- **Built alongside.** `generator = "v2"` in `config/planet.toml` picks it; the current world stays playable until v2 overtakes it. Today's generation is a test bed, so nothing of its look is preserved.

## Stages

| # | Stage | Kind | Grid | Hands on |
|---|---|---|---|---|
| 0 | Planet | given | – | radius, gravity, day, tilt, water inventory |
| 1 | Tectonics | stepped, ~40 × 10–25 Myr | level 6–7 (2.8–1.4 km) | crust type, thickness, age; orogeny age; arcs; hotspot tracks; sutures, faults; rock provinces |
| 2 | Isostasy and sea | solved | level 7 | large-scale height; ocean depth by crust age; sea level from the water inventory, so coasts emerge |
| 3 | Climate | solved, seasonal | level 5–6 | temperature, winds, ocean currents, rain with orographic lift and rain shadows, snowline, ice-age swing |
| 4 | Landscape | implicit, tens of steps | level 10 (177 m) | heights, soil, sediment, glacial troughs, coasts moved by sea-level cycles and waves |
| 5 | Hydrology | solved | level 10 and finer | runoff, groundwater and water table, losses, lakes and ponds by water balance, rivers and their regimes, one water surface |
| 6 | Soil and surface | solved | level 10 | soil by weathering and transport, sediment where it settles, beaches from waves and supply |
| 7 | Life | solved, then live | level 10+ | biomes as mixes by suitability, plant cover, its feedback on soil and erosion |
| 8 | Detail | on demand | down to 0.25 m | softened coarse forms, channel bends, ledges, talus, rock texture, guided by stages 1–7 |

### 0 · Planet

Radius from population (as now) or `radius_km`; bigger is fine if it makes room for big drainage. The sea comes from a water inventory, not a land fraction: `ocean_fraction` becomes the target the inventory is chosen to meet on the first pass, so the coast falls wherever the water actually sits.

### 1 · Tectonics

Rigid plates on the sphere turning about Euler poles, after Cortial et al. 2019 ("Procedural Tectonic Planets"). Each step:

- **Spreading:** divergent edges make new oceanic crust, age 0.
- **Subduction:** oceanic crust dives under the other plate, thickening it into an arc with volcanoes; the trench is where it dives.
- **Collision:** continents meet, crust piles up into a range, the plates weld along a suture.
- **Rifting:** a plate may split along a weak line, opening a new ocean with passive margins either side.
- **Hotspots:** fixed plumes leave chains of islands as plates pass over.
- Crust carries its age, thickness, type and the age of its last mountain building, so old ranges wear low and young ones stand high.

Rock provinces fall out: basalt ocean floor, granite cratons, sediment in basins at margins and forelands, metamorphic cores in eroded ranges, lava in arcs. These set erodibility in stage 4, aquifers in stage 5 and soil in stage 6.

### 2 · Isostasy and sea

Height from crust thickness and density (Airy), and ocean depth from crust age (cooling plate, depth ~ √age), plus swells over plumes. The water inventory fills the lowest ground: that's the sea, and the coastline is wherever it lands. Recomputed after stage 4, so erosion and sediment move the shore.

### 3 · Climate

One seasonal equilibrium from insolation, tilt and day length:

- **Temperature** by latitude and season, eased by the sea near coasts and warm or cold currents.
- **Winds** from the circulation cells (trade winds, westerlies, polar easterlies), turned by continents and ranges.
- **Rain** carried on the winds and wrung out where ground lifts the air (linear upslope model, Smith and Barstad 2004), leaving rain shadows; drier far inland.
- **Snowline and ice**, and an ice-age swing for stage 4.

Erosion, biomes, rivers and the live weather all start from it, so they agree.

### 4 · Landscape

Today's implicit stream-power and hillslope loop, kept and fed properly:

- **Uplift** from stage 1's active ranges, not stamped ridges; start from stage 2's height plus fine roughness.
- **Erodibility** from stage 1's rock provinces and the beds laid in its basins.
- **Deposition** (Yuan et al. 2019 sediment flux): alluvial plains, fans, deltas, filled basins.
- **Glaciers** from stage 3's snowline and ice-age swing, with troughs, cirques and fjords.
- **Sea-level cycles**, so coasts move, valleys drown into rias and fjords, and terraces are left behind; waves cut soft coasts back and leave hard headlands.

### 5 · Hydrology

Water from stage 3's rain and snowmelt and stage 4's glaciers:

- **Groundwater:** the water table and aquifers through permeable rock; springs and seeps where it meets the surface.
- **Losses:** evaporation by climate, seepage by bed rock and water table, karst sinks rising again as springs.
- **Lakes and ponds** wherever drainage is blocked, filled until inflow balances evaporation and loss: overflowing lakes, salt lakes, playas, dry basins.
- **Rivers** carry what's left downstream and end where it runs out. Width and depth from discharge; pattern (straight, meandering, braided) from slope, flow, sediment and banks; regimes (glacial, snowmelt, rain, monsoon, spring-fed, ephemeral) from their sources.
- **One water surface:** level and flow at any point, sea and lakes and rivers alike, drawn wherever it stands above the ground. A local GPU shallow-water sim near the eye builds on it later.

### 6 · Soil and surface

Soil weathers out of rock by climate and time and creeps downhill (as now, fed by stages 1 and 3); sediment lies where stage 4 left it; beaches form where waves meet sediment supply, cliffs where they meet hard rock.

### 7 · Life

Biomes as blends of what suits each spot (temperature, rain, soil, water table, rock), not thresholds with overrides. Plant cover then holds soil and slows erosion. The live forest and weather sims start from it.

### 8 · Detail

Below the stored grids, detail is synthesised from them: softened coarse forms, channel bends sized by discharge, strata ledges where beds lie flat, talus under cliffs, rock texture by province. Fine features stay faked, but always where the data says they belong.

## Build order

1. **Stages 0–2:** plates that move, crust, isostasy, sea from water. The first visible result is the "real mountains" request: ranges along collisions and margins with wide plains behind.
2. **Stage 3:** one climate with winds and rain shadows.
3. **Stage 4:** the erosion loop on v2's uplift, rock and climate, with deposition and sea-level cycles.
4. **Stage 5:** hydrology and the one water surface. This replaces the river strips and the lake fill around shores.
5. **Stages 6–8.**

Each step lands with unit tests, map and fly-over stills and a profile of its generation time; golden tiles are rewritten as stages change.

## What carries over

- **Kept:** the level grids, the bake, drainage and priority flood, the implicit stream-power solver, hillslope creep and collapse, glacier troughs, lake filling, river courses and carving, plumes, noise.
- **Replaced:** the continent field and its rule bumps, stamped ridges, stamped volcano placement (cone shapes stay, placed by arcs and plumes), the climate formulas, moisture, crust thickness from height, biome overrides.
