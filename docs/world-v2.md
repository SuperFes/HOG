# World v2: a planet that makes itself

Oct 28, 2026 · @Aaron Nixon

**Superseded by [The globe](globe.md):** the world is voxels on a cube-sphere, from a fresh generator. What follows is kept for the processes it lists, not for its heightfield or its square cells.

Today's world is stamped: a frozen plate snapshot plus noise sets the continents, sea level, mountain heights, volcanoes, ocean floor, beds, climate and biomes, and only the 5 Myr erosion loop, lake filling and live weather run as processes. The pieces are made apart and then forced to fit, which is where rivers fighting lakes, coasts frozen to a noise field and mountains counted twice come from.

World v2 builds the planet in the order nature does, each stage working from what the one before it left. Nothing is placed: mountains stand where plates collide, rain falls where they lift the air, rivers run where it drains, lakes sit where drainage is blocked, deltas build where sediment comes out. We don't loop over billions of years; each stage runs only enough steps, or solves its equilibrium directly, to reach a believable end state, then the coarse result is softened and given fine detail driven by the data.

Everything lives in one sparse volume: rock, sediment, soil, ice, water, air and magma, in cells that are as big as they can be and split only where something changes. The same volume is generated, simulated, edited and baked, so water really flows, clouds really roll, rock that loses its footing falls, and volcanoes keep building the world while it's played.

## Principles

- **Stage order, one way.** A stage reads only what earlier stages stored. No stage reaches forward or stamps over a later one.
- **One source of truth per quantity.** One climate (erosion, biomes and live weather all start from it), one drainage, one sea level, one test for continental crust.
- **One volume, baked.** Results live in the volume and the bake, not as functions recomputed from noise. Fine detail is synthesised on demand from the coarse cells, the seed and the edits, and nothing that can be synthesised again is stored.
- **At rest costs nothing.** Generation runs until the world is stable. After it, only what moves is simulated, and only near players; once it settles it's baked back.
- **Pay for what happens, not for size.** Cost scales with activity near players, never with the planet.
- **Solve, don't loop, where nature is at equilibrium.** Isostasy, climate, lakes and groundwater are solved directly; tectonics and landscape evolution take big implicit steps.
- **Judged by looking.** Every stage has map stills and fly-over stills, and tests for its physical invariants (mass conserved, water runs downhill, crust area kept).
- **Built alongside.** Today's generation is a test bed, so nothing of its look is preserved.

## The volume

### Cells

The cube-sphere the level grids already use, extended outward in shells. Each cube face is the root of a tree that splits across into four, as the level grids do, and outward into layers, so a cell is a slightly tapered box standing on a radial line. Gravity is toward the centre, straight through the bottom of every cell, and physics takes it from position, never from a grid axis. Cells near the cube's corners are up to about 1.4 times wider than mid-face; hexagons would be more even but don't nest, which the tree needs.

A cell splits only where its contents differ: at the ground, a rock contact, a shore, a water surface, a dug hole. A kilometre of granite or deep ocean is one cell. Neighbouring cells of one material merge back once they settle, so the tree doesn't fragment forever.

### What a cell holds

- **Material:** rock by province (basalt, granite, sandstone, limestone, metamorphic), sediment, soil, ice, water, air, magma.
- **State by material:** temperature; strength and porosity for rock; water content for soil and sediment; flow, temperature and salinity for water; wind, moisture and cloud water for air; temperature and gas for magma.
- **What a material is for:** only the properties that change how the land forms — strength and how fast it wears, density for how high it floats, porosity and how it dissolves for springs and caves, where it melts. Nothing is mixed or tracked by makeup; below the crust, the mantle is only where heat and the plates' forces come from.
- **Where a surface crosses it:** a signed distance and normal, so the smooth display can find sharp cliffs and soft hills from the same data.

### Water

Oceans, lakes and rivers are water cells, coarse in the deep interior and finer toward the surface, shores and bed, down to a finest size below which we stop splitting and pretend:

- **Cells carry the bulk:** level, currents from wind, density and the planet's spin, tides, temperature and salinity. Ocean currents feed the climate, warm and cold.
- **Below the finest cell, waves are a spectrum** on the water's surface, driven by the air cells' wind over its fetch, as the current wave trains are but from the sim rather than set by hand. Breaking and foam come from the spectrum and the depth.
- **Rivers narrower than a cell** are carried as flow through it and drawn as a channel, until the cells near the eye are fine enough to hold them.
- **At rest, water is solved, not stepped:** the sea's level, the lakes' levels and the rivers' flows are hydrology's equilibrium, baked. Near players, a local fine sim runs from that equilibrium for anything that disturbs it: a breach, a dam, a wave on a beach.
- **Columns, not cubes, under the surface:** a column of water is one tall cell under a few cubic ones at the surface (tall cell grids, Chentanez and Müller 2011; made adaptive by Narita et al. 2025), so depth costs almost nothing. Most rivers and lakes need only shallow water equations; full 3D runs only where water falls, fills a cave or breaks a dam.
- **Water carries what's in it:** sediment, which settles where it slows (deltas grow live), and heat and salt, which freeze it, stratify it and drive currents. Sea ice forms where it's cold enough, drifts and breaks; icebergs calve off glaciers and float off.

### Air

The atmosphere is coarse air cells (kilometres) over the whole planet, stepped on the server all the time: winds from heating and spin, moisture carried and wrung out over ground, cloud water that forms, moves with the wind and rains out. Clouds are drawn from it, so they roll and drift because the air does.

- **Heat from the sun:** temperature comes from a radiation balance (sunlight by orbit and season, what ground, snow, sea and cloud reflect, what the air holds in), not a formula by latitude, so snow cooling the land and cloud shading it feed back by themselves.
- **Below the cell:** thunderstorms are too small for kilometre cells, so convection is reckoned within a cell from how unstable its air is: showers, hail, lightning (which starts fires) and squalls. Cyclones and monsoons are big enough to emerge on their own over warm seas and heating continents.
- **What the wind carries:** water, dust and sand (dunes, loess), ash from eruptions and smoke from fires; each settles where the air drops it.
- **Near players, finer air:** sea breezes, valley and mountain winds, föhn and cold air pooling into fog, nested in the coarse cells the way fine water is.

### Snow and ice

Snow is a material: it falls out of the air cells, lies, melts or packs down year on year into firn and then ice. Ice is a very slow, very viscous fluid (Glen's flow law): it flows downhill under its own weight, slides and grinds its bed into troughs and cirques, carries rock to drop as moraines, and calves where it reaches the sea. Through generation it runs at geological pace, so ice ages carve fjords; live, snow comes and goes with the seasons while glaciers creep too slowly to watch, but they're the same ice.

### Magma and living volcanoes

Magma is a material: chambers under arcs and over plumes, fed from below. An eruption is an event: magma rises, comes out as lava, flows as a very viscous fluid, cools and becomes basalt. Over generation that builds arcs and hotspot chains; afterwards the young end of a chain stays live, so a Hawaii keeps erupting, flowing into the sea and growing new ground. Each eruption is simulated while it flows and baked when it sets.

- **Style from the magma:** runny basalt over plumes and ridges builds shields and fissures; sticky, gassy magma over arcs builds stratovolcanoes that blow. Explosions throw ash into the air cells (cooling the climate for a while, fertile soil later), pyroclastic flows run down the flanks, and ash meeting snow or rain makes lahars.
- **What it leaves:** lava tubes as the crust of a flow sets over the stream still running in it, pillow lava under water, calderas where an emptied chamber's roof falls in, and the old island sinking and wearing as the plate carries it off the plume, until a reef ringing it is all that's left (an atoll).
- **Warnings before it goes:** a filling chamber swells the ground, cracks rock into swarms of small quakes and lets gas out, so an eruption can be seen coming.

### Rock, heat and the deep

- **The rock cycle:** sediment laid in a basin is buried by more and pressed into rock; buried deep or caught in a collision it's folded, faulted and turned metamorphic; deeper still it melts. Strata are real layers in the volume, so a cliff or a mine goes through them.
- **Heat:** the interior is hot and heat flows out through every cell, solved once and kept. Where groundwater meets it: hot springs, geysers, fumaroles.
- **What's in the rock:** ores and resources come from the processes that make them, never scattered: veins where hot water rose round arcs and intrusions, gold and tin in the gravels of rivers wearing them out, coal from buried swamps, oil and gas trapped under tight rock over buried sea mud, salt where seas dried, clay and sand where rivers left them.

### Ground that moves

- **Earthquakes:** stress builds on faults as the plates move, and goes all at once. The ground is offset into scarps, shaking loosens slopes, and a sea floor that moves makes a tsunami.
- **Slopes fail on their own,** not only when they're dug: rain soaks soil and weakens it into landslides, debris flows and mudflows; freezing and thawing pry rock off cliffs into talus; soil creeps; thawing frozen ground slides. Snow fails in layers as avalanches.
- **Weathering** by climate and rock: frost wedging, dissolving, salt, sheeting off. It sets how fast everything else wears and how much soil there is.

### Caves

Caves are air in the volume, made by what makes them: groundwater dissolving limestone along beds and fractures toward its springs (karst, with sinkholes and disappearing rivers; Paris et al. 2021), lava tubes, waves cutting into cliffs (sea caves, arches, stacks), meltwater under glaciers. Dripstone is detail. A cave can flood, collapse or be dug into like any other part of the volume.

### At rest and active

The baked world is at rest: generation keeps fracturing, calving, crumbling and settling until nothing would move. After that only an edit or an event can wake anything, and only near it:

1. **An edit** (a cut, a dig, a blast) is applied as an operation and the cells it touches are remeshed.
2. **Support:** a flood fill from it through bonded rock, weighted by strength, until it reaches an anchor (any large settled cell). It costs as much as the piece that might fall, not the world.
3. **A piece that lost its footing** becomes a rigid body, simulated on the server and streamed to players nearby.
4. **When it stops,** it's either written back into the volume at rest or left as a resting object that later decays into it, as dead wood does.
5. **Rubble** stays a few large pieces; the rest becomes sediment heaped at its angle of repose.

What runs all the time is the air, coarse; everything else only while it moves, and regions with no one near are frozen.

### Edits and baking

The world is the generator, the bake and a log of operations ("remove this shape here", "this piece fell and came to rest there"). The server orders them; players near an edit get the operation and apply it the same way, so changes are small on the wire and every client agrees. Each region's log is baked into a new snapshot from time to time, which gives every change an atomic place in time, bounds storage, and makes rolling a region back easy. A region is its snapshot's version plus the operations since.

- **Built things are materials too:** timber, stone, brick and the like, with their own strength, held up by the same support test as rock, and timber burns.
- **Players are part of the processes:** a felled forest sheds more water and soil, a dam makes a lake behind it, a cut channel can steal a river, a mine below the water table floods and an unpropped one caves in, farmed soil wears out unless it's tended.
- **Limits:** claims, rates and how much one player can change, since every edit costs storage until it's baked; the log makes undoing abuse easy.

### Display

**First, blocky:** each cell's exposed faces drawn as tapered boxes coloured by material, merged per chunk. It's quick to build and good enough to judge every stage while we build them.

**Later, smooth:** surfaces pulled out of the volume (dual contouring, from the stored distances and normals), seams stitched between cells of different sizes, textured by material. Water, vegetation and texturing move onto it then.

## Processes

Every process, where it runs and how. **Resolved** means the cells hold it; **below the cell** means it's reckoned within a cell from what the cell holds. Generation runs geological processes at geological pace; live, the same process runs at the pace it really has, so most of them are too slow to watch and advance between visits (see Running it).

| Sphere | Process | Generation | Live | How |
|---|---|---|---|---|
| Space | orbit, seasons, day | sets climate | yes | given; tilt, eccentricity and precession cycle |
| Space | moon(s) | tides | tides, phases, eclipses, night light | given |
| Space | impacts | early bombardment craters | rare falls | events |
| Space | magnetic field | – | aurora by the poles | given |
| Interior | heat flow | solved | kept | resolved, coarse |
| Interior | plates | stepped by forces | too slow | plate frames |
| Interior | rifting, supercontinent cycle | yes | – | from the plates' forces and heat under continents |
| Interior | hotspots | chains | live end erupts | fixed plumes |
| Interior | rock cycle, strata, folding | yes | – | resolved |
| Interior | magma chambers | yes | fill, swell, empty | resolved |
| Interior | faults, earthquakes | scarps | quakes | stress per fault; rupture as event |
| Interior | ores and resources | yes | – | from the process that makes each |
| Volcanic | eruptions, lava, tubes | yes | yes | events; lava resolved near players |
| Volcanic | ash, pyroclastic flows, lahars | yes | yes | air cells carry ash; flows as events |
| Volcanic | hot springs, geysers | yes | yes | below the cell, from heat and groundwater |
| Surface | isostasy, flexure | solved | rebound, too slow | solved |
| Surface | weathering | yes | too slow | below the cell, by climate and rock |
| Surface | stream power, hillslope, deposition | implicit steps | too slow | resolved on the surface |
| Surface | landslides, debris flows, rockfall, creep | yes | yes | support test; flows as events |
| Surface | caves | karst, tubes, sea caves | collapse, flood | resolved |
| Surface | dunes, loess, dust | yes | dunes creep | resolved coarse, cellular rules |
| Surface | coasts: cliffs, arches, spits, barriers, tidal flats | yes | slowly | resolved, waves and drift |
| Surface | reefs, atolls | yes | too slow | grow on warm shallow shelves |
| Water | sea level | from ice volume | tides | solved |
| Water | ocean currents, upwelling | solved | yes | resolved, coarse |
| Water | waves, breaking, tsunamis | shapes coasts | yes | spectrum below the cell; tsunamis resolved |
| Water | groundwater, springs | solved | wells, mines flooding | resolved, porous rock |
| Water | lakes, salt lakes, playas | solved | levels by season | solved |
| Water | rivers, regimes, floods | solved | yes | shallow water, flow below the cell |
| Water | meanders, oxbows, avulsion | yes | slowly | resolved on the surface |
| Water | sediment in water, deltas | yes | yes | carried in water cells |
| Water | wetlands, peat | yes | slowly | below the cell |
| Ice | snow, firn, glaciers, ice ages | yes | snow live, glaciers slow | resolved |
| Ice | sea, lake and river ice, icebergs | yes | yes | resolved |
| Ice | permafrost, frost heave | yes | seasonal thaw | below the cell |
| Ice | avalanches | – | yes | snowpack layers; events |
| Air | radiation and heat | solved | yes | resolved, coarse |
| Air | winds, moisture, clouds, rain, snow | solved | yes | resolved, coarse |
| Air | thunderstorms, lightning, hail | – | yes | below the cell |
| Air | cyclones, monsoons | – | yes | emerge, resolved |
| Air | local winds, fog, frost, dew | – | near players | finer air cells |
| Life | soil and fertility | yes | worn by farming | below the cell |
| Life | biomes, plant cover, succession | yes | yes | forest sim |
| Life | fire | yes | yes | lightning or players; spread by fuel, dryness, wind |
| Life | wildlife, grazing, beaver dams | – | yes | populations moving with the seasons |
| People | digging, building, logging, dams, farming, mining | – | yes | operations in the log |

## Running it

- **Every process at its own size and pace.** Each gets the coarsest cells and longest step that keep it right. A step shorter than one frame of the slowest process isn't paid for until it's needed.
- **Waking a region catches it up.** Regions with no one near are frozen, but the season isn't: when a player arrives, the region is advanced in closed form over the time it slept, from the air sim's record of what fell and how warm it was (snow lying or melted, lakes up or down, plants grown, fires burnt out).
- **World ticks for slow processes.** Dunes, meanders, coasts, glaciers and volcanoes advance world-wide in batches on their own servers, so a place nobody visits for a year has still changed, and its snapshot is rebaked.
- **Budgets:** water (sea, ice, lakes, groundwater, air), sediment and heat are each counted world-wide and per region. Tests hold them, and a live drift in one means a leak, so it's watched.
- **Determinism where clients replay:** operations are integer cell coordinates and fixed point, so every client applies them the same way. Fluids, falling pieces and fire are the server's and streamed, never replayed. A bake is the same bytes from the same inputs.
- **The generator is versioned:** fine detail is synthesised from the generator's code, so each snapshot names the version it was made with. A generator change migrates regions (rebake from the log) rather than moving the ground under what players built.
- **Servers by job:**
  - a generation farm (global stages once, then regions in parallel);
  - the air and ocean server, coarse and always running, broadcasting weather;
  - region servers near players (fine water and air, falling pieces, fire, eruptions);
  - world tick servers for slow processes;
  - a bake service turning logs into snapshots, and storage.
  Neighbouring regions on different servers exchange what crosses their border (water, sediment, air, falling pieces), so a river is one river across them.

## Stages

| # | Stage | Kind | Grid | Hands on |
|---|---|---|---|---|
| 0 | Planet | given | – | radius, gravity, day, tilt, orbit and its cycles, moon(s), air, magnetic field, water inventory |
| 1 | Tectonics | stepped, ~40 × 10–25 Myr | level 6–7 (2.8–1.4 km) | crust type, thickness, age; orogeny age; arcs; hotspot tracks; sutures, faults and their stress; strata; rock provinces; impact craters |
| 2 | Isostasy and sea | solved | level 7 | large-scale height; ocean depth by crust age; flexure under ice and sediment; heat flow; sea level from the water inventory, so coasts emerge |
| 3 | Climate | solved, seasonal | level 5–6 | radiation balance, winds, ocean currents, rain with orographic lift and rain shadows, snowline, ice ages from the orbit's cycles |
| 4 | Landscape | implicit, tens of steps | level 10 (177 m) | heights, weathering, sediment, landslides, glaciers, dunes and loess, permafrost, coasts moved by sea level and waves, reefs |
| 5 | Hydrology | solved | level 10 and finer | runoff, groundwater and water table, karst caves, losses, lakes and ponds by water balance, rivers and their regimes, wetlands, one water surface |
| 6 | Soil and surface | solved | level 10 | soil and fertility, sediment where it settles, beaches from waves and supply, ores and resources |
| 7 | Life | solved, then live | level 10+ | biomes as mixes by suitability, plant cover, fire, its feedback on soil, erosion and climate; wildlife |
| 8 | Detail | on demand | down to 0.25 m | softened coarse forms, channel bends, ledges, talus, rock texture, guided by stages 1–7 |

### 0 · Planet

Every knob of a planet lives once, by name, in the one `[planet]` table of `config/planet.toml` (and its tables): the formation's (`radius` in metres, `gravity`, `day` in hours, `tilt`, rock, climate, wear and the rest), the voxel shell's, and the system's below. What's left out keeps the working world's value. Gravity is set, not worked out from the size.

The planet belongs to a small system, Earth's by default: one or more stars (a binary is two, either both orbited or one far off), the planet's own orbit, any number of moons, optional rings in the equator's plane, and other planets seen as lights. Sunlight is the sum over the stars, and tides the sum over moons and stars, so another moon or a second sun is a matter of settings. The orbit's tilt, eccentricity and precession wander in slow cycles, which is what brings ice ages; a moon (or more) raises tides and lights the night; the air's makeup sets how much heat it holds. The sea comes from a water inventory, not a land fraction. The world starts with enough water to stand in an even layer about as deep as Earth's would, and the coast falls wherever that water actually sits. Once the world has settled, if the sea covers less than `ocean_fraction`, it's topped up with as much more as it takes, as if it came from space. Nothing is tested against the target before generation is done.

### 1 · Tectonics

Rigid plates on the sphere turning about Euler poles, after Cortial et al. 2019 ("Procedural Tectonic Planets"). Each plate keeps its crust in its own frame, and only its turn changes; crust is never copied from cell to cell, so plates stay whole, and where two overlap they've met and where none lies they've parted. The volume's lithosphere is read from whichever plate lies over each cell. Each step:

- **Spreading:** divergent edges make new oceanic crust, age 0.
- **Subduction:** oceanic crust dives under the other plate, thickening it into an arc with volcanoes; the trench is where it dives.
- **Collision:** continents meet, crust piles up into a range, the plates weld along a suture.
- **Rifting:** a plate may split along a weak line, opening a new ocean with passive margins either side.
- **Hotspots:** fixed plumes leave chains of islands as plates pass over; the youngest stays live.
- **What moves them:** each plate's turn comes from the forces on it: old, cold, heavy floor diving at its trenches pulls hardest, ridges push a little, and drag holds it back, more under thick continents. Plates speed up, slow down and swing round as trenches open and close, and continents gather into supercontinents that split again where heat builds up under them, without a script.
- **Strata and faults:** basins fill with beds that later stages bury, fold and wear; every boundary leaves faults that keep their stress for earthquakes.
- **Impacts** in the early steps leave craters that the rest wears down or buries.
- Crust carries its age, thickness, type and the age of its last mountain building, so old ranges wear low and young ones stand high.

Rock provinces fall out: basalt ocean floor, granite cratons, sediment in basins at margins and forelands, metamorphic cores in eroded ranges, lava in arcs. These set erodibility in stage 4, aquifers in stage 5 and soil in stage 6.

### 2 · Isostasy and sea

Height from crust thickness and density (Airy), and ocean depth from crust age (cooling plate, depth ~ √age), plus swells over plumes. The crust bends under loads (flexure): ice sheets press it down and it rebounds when they melt, leaving raised beaches; deltas sink under their own sediment. The water inventory fills the lowest ground: that's the sea, and the coastline is wherever it lands. Recomputed after stage 4, so erosion and sediment move the shore.

### 3 · Climate

The air cells and the ocean's currents, brought to one seasonal equilibrium from insolation, tilt and day length, and then kept stepping live:

- **Temperature** from the radiation balance by latitude and season, eased by the sea near coasts and warm or cold currents.
- **Winds** from the circulation cells (trade winds, westerlies, polar easterlies), turned by continents and ranges.
- **Rain** carried on the winds and wrung out where ground lifts the air (linear upslope model, Smith and Barstad 2004), leaving rain shadows; drier far inland.
- **Snowline and ice.** Ice ages come from the orbit's cycles, amplified by snow reflecting sunlight; nothing sets their size by hand.

Erosion, biomes, rivers and the live weather all start from it, so they agree.

### 4 · Landscape

Today's implicit stream-power and hillslope loop, kept and fed properly:

- **Uplift** from stage 1's active ranges, not stamped ridges; start from stage 2's height plus fine roughness.
- **Erodibility** from stage 1's rock provinces and the beds laid in its basins.
- **Deposition** (Yuan et al. 2019 sediment flux): alluvial plains, fans, deltas, filled basins.
- **Weathering** by climate and rock sets what's loose to move.
- **Landslides, rockfall and creep** where slopes are steeper than their rock and soil can hold.
- **Glaciers** from stage 3's ice ages over several cycles (Cordonnier et al. 2023), with troughs, hanging valleys, cirques, fjords and moraines. The last stretch spans at least 400,000 years in steps of no more than about 10,000, so it passes through the cycles' cold lows rather than averaging them away. On Earth's settings that brings at least one ice age, and the run ends in the warm spell after it, as Earth's present does; a warmer world may get none, and that's left to its climate.
- **Sea level** falls as ice locks water up and rises as it melts, so coasts move, valleys drown into rias and fjords, and terraces are left behind; waves cut soft coasts back into cliffs, caves, arches and stacks and leave hard headlands, and drift along the shore builds spits, barriers and lagoons.
- **Wind** builds dunes where sand is loose and plants are few, and drops dust as loess downwind.
- **Cold ground:** permafrost, frost heave and patterned ground near the ice.
- **Reefs** grow on warm shallow shelves and keep up with sinking islands.

### 5 · Hydrology

Water from stage 3's rain and snowmelt and stage 4's glaciers:

- **Groundwater:** the water table and aquifers through permeable rock; springs and seeps where it meets the surface.
- **Karst:** where groundwater runs through limestone it dissolves caves along beds and fractures toward its springs, opens sinkholes and swallows rivers.
- **Losses:** evaporation by climate, seepage by bed rock and water table, karst sinks rising again as springs.
- **Lakes and ponds** wherever drainage is blocked, filled until inflow balances evaporation and loss: overflowing lakes, salt lakes, playas, dry basins.
- **Rivers** carry what's left downstream and end where it runs out. Width and depth from discharge; pattern (straight, meandering, braided) from slope, flow, sediment and banks; regimes (glacial, snowmelt, rain, monsoon, spring-fed, ephemeral) from their sources. Meanders migrate and cut off into oxbow lakes, floods build levees and floodplains, and a river may jump its bed.
- **Wetlands** where the water table meets the ground and drainage is slow; peat builds up in them.
- **One water surface:** the top of the water cells, sea and lakes and rivers alike, with level and flow at any point. The local fine sim near players builds on it.

### 6 · Soil and surface

Soil weathers out of rock by climate and time and creeps downhill (as now, fed by stages 1 and 3); sediment lies where stage 4 left it; beaches form where waves meet sediment supply, cliffs where they meet hard rock. Fertility comes from the parent rock, ash, flood silt and what grows and rots on it. Ores and resources are placed by the processes that made them (see Rock, heat and the deep).

### 7 · Life

Biomes as blends of what suits each spot (temperature, rain, soil, water table, rock), not thresholds with overrides. Plant cover then holds soil, slows erosion, pins dunes, and gives water back to the air and darkens the ground for the climate. Fire, started by lightning and spread by fuel, dryness and wind, resets patches that grow back in succession. Wildlife lives where its food does and moves with the seasons; grazers keep grass short and beavers dam streams. The live forest and weather sims start from it.

### 8 · Detail

Below the stored grids, detail is synthesised from them: softened coarse forms, channel bends sized by discharge, strata ledges where beds lie flat, talus under cliffs, rock texture by province. Fine features stay faked, but always where the data says they belong.

## Build order

Small steps, each one a commit or a few, each landing with unit tests, stills where there's something to see, and a profile of what it costs. A stage that takes more than seconds where a few should do is profiled before anything is built on it. Golden tiles are rewritten as stages change.

**A · The volume**
1. Cell addresses: face, path across, shell; from a position to its cell and back.
2. Neighbours, across cube faces and between cells of different sizes.
3. The sparse store: material and state per cell; split and merge, with material conserved.
4. Lookup and rays through the volume.
5. Surface crossings: distance and normal per cell.
6. Regions: bake and load, the same bytes back; stamped with the generator's version.
7. Budgets: water, sediment and heat counted per region and world-wide.
8. A timing harness: every stage reports what it took in mapgen's output.

**B · Seeing it**
9. Blocky meshes: a chunk's exposed faces by material.
10. A region view in the client, and a planet overview.
11. Map and fly-over stills from the volume, as `[.look]` tests.
12. Debug views coloured by state: temperature, age, strength, water.

**C · Planet and plates**
13. The planet and its system:
    - 13a. The record and its settings, Earth's by default: radius, gravity, day, tilt; stars, orbit, moons, rings, other planets; air; water inventory.
    - 13b. Where each body is at a time, by its orbit, and the light each star sends.
    - 13c. The orbit's slow cycles over deep time: tilt, eccentricity, precession.
    - 13d. Tides from every moon and star.
14. Plates as frames carrying their own crust; the volume's crust read from them.
15. Boundaries from overlaps and gaps: spreading, subduction, collision.
16. Plates moved by their forces: slab pull, ridge push, drag.
17. Rifting and the supercontinent cycle.
18. Hotspots and their chains.
19. Strata laid in basins, buried, folded and faulted; the rock cycle.
20. Faults kept with their stress.
21. Early impacts.
22. Retire v1's welding and gap rules.
23. Profile stages 0–1.

**D · Height, heat and sea**
24. Isostasy and ocean depth by age.
25. Flexure under loads.
26. Heat flow through the volume.
27. Magma chambers from heat under arcs and plumes.
28. The sea fills water cells from the inventory: the first coasts. The "real mountains" check: ranges along collisions and margins, wide plains behind.
29. Profile stages 0–2.

**E · Air and ocean**
30. The radiation balance: sunlight by orbit and season, reflection, held heat.
31. Air cells: winds from heating and spin.
32. Moisture, clouds, rain and snow; lift over ground and rain shadows.
33. Convection below the cell: showers, thunderstorms, lightning, hail.
34. Cyclones and monsoons, tested to emerge, not placed.
35. Ocean currents: by wind, by density; upwelling.
36. Sea ice.
37. Ice ages from the orbit's cycles and snow's reflection.
38. Clouds drawn from the air cells, drifting with the sim.
39. Finer air near players: sea breezes, valley winds, föhn, fog, frost, dew.
40. Profile stage 3.

**F · Shaping the land**
41. Weathering by climate and rock.
42. Stream power, hillslope and deposition on the volume's surface.
43. Landslides, debris flows, rockfall, talus and creep.
44. Snow, firn and glacier ice: flow and erosion over glacial cycles.
45. Sea level from ice volume: moving coasts, terraces, rias, fjords.
46. Flexure from ice, erosion and sediment; rebound and raised beaches.
47. Coasts: cliffs, sea caves, arches, stacks; drift, spits, barriers, lagoons, tidal flats.
48. Wind: dunes, loess, dust.
49. Permafrost and frost heave.
50. Reefs and atolls.
51. Profile stage 4.

**G · Water**
52. Groundwater: aquifers, the water table, springs.
53. Karst caves, sinkholes and lost rivers.
54. Lakes and ponds by water balance; salt lakes and playas.
55. Rivers: discharge, width, depth, pattern, regimes.
56. Living rivers: meanders, oxbows, avulsions, levees, floodplains.
57. Water cells as tall cells; the one water surface.
58. Waves from the wind's spectrum; breaking by depth.
59. Tides from the moons and sun.
60. The fine water sim near players: breaches, dams, waterfalls, floods.
61. Sediment carried in water; deltas growing live.
62. Lake and river ice; spring breakup.
63. Wetlands and peat.
64. Profile stage 5.

**H · Fire from below**
65. Eruptions by magma: shields, fissures, stratovolcanoes, explosions.
66. Lava as a viscous fluid that cools: flows, tubes, pillows, new land.
67. Ash in the air cells, pyroclastic flows, lahars.
68. Calderas; hot springs, geysers, fumaroles.
69. Warnings: swelling ground, quake swarms, gas.
70. Earthquakes from fault stress; scarps; shaking that brings slopes down.
71. Tsunamis from quakes, slides and collapses.

**I · Hands on the world**
72. The operation log: server order, fixed-point operations every client applies alike.
73. Support: the flood fill to an anchor.
74. Falling pieces as rigid bodies; rubble into scree.
75. Built materials and buildings under the same support test.
76. Snapshots, rollback, claims and edit limits.
77. Mines and tunnels: flooding from groundwater, caving in.

**J · Soil and life**
78. Soil and fertility.
79. Ores and resources from the processes that make them.
80. Biomes as blends; plant cover and its feedbacks.
81. Fire: lightning, spread, regrowth.
82. The forest sim on the volume; succession.
83. Wildlife moving with the seasons; grazers and beavers.
84. Players feeding the processes: logging, farming, dams, channels.
85. Profile stages 6–7.

**K · Running it**
86. Servers split by job: generation farm, air and ocean, regions, world ticks, bake.
87. Regions on different servers exchange what crosses their borders.
88. Catching a woken region up over the time it slept.
89. World ticks for slow processes, and rebaking what they change.
90. Migrating regions when the generator changes.
91. Budgets watched live.

**L · Sky**
92. Moon phases, eclipses, night light; aurora; meteor showers and rare falls.

**M · Finish**
93. Detail synthesised below the cells (stage 8).
94. The smooth display: dual contouring, seams between sizes, distance levels.
95. Texturing by material and state; water and vegetation on the smooth surface; dark caves and light through water.
96. The system in the sky: each sun, moons in their phases, rings and the shadows they cast and catch, other planets.

## What carries over

- **Kept:** the level grids (the volume's faces split as they do), the bake, drainage and priority flood, the implicit stream-power solver, hillslope creep and collapse, glacier troughs, plumes, noise, the renderer's sky and lighting.
- **Replaced:** the continent field and its rule bumps, stamped ridges, stamped volcano placement, the climate formulas, moisture, crust thickness from height, biome overrides; crust resampled cell to cell each step and the welding and gap rules that patched it; heightfield tiles as the world's store; lake filling and river strips as separate water; clouds that don't move with the air; an ice-age swing set by hand.

## Sources

- Cortial et al. 2019, "Procedural Tectonic Planets".
- Smith and Barstad 2004, linear upslope rain.
- Yuan et al. 2019, sediment flux in implicit landscape evolution.
- Chentanez and Müller 2011, "Real-time Eulerian water simulation using a restricted tall cell grid"; Narita et al. 2025, quadtree tall cells.
- Cordonnier et al. 2023, "Forming Terrains by Glacial Erosion".
- Paris et al. 2021, "Synthesizing Geologically Coherent Cave Networks".
- Werner 1995, cellular dune model.
- Slab pull, ridge push and mantle drag from crust age at trenches (force-balance plate reconstructions).
