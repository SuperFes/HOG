# The globe: a voxel planet of maths and changes

Oct 31, 2026

The world is voxels on a sphere, modelled on Enshrouded's voxel world and stretched into a planet. A generator, written fresh, says what is at any point and at any resolution. A sparse store holds only what has changed: digging, building, water, slides, lava. What players see is a surface drawn from the voxels, so the world looks like a clean modern game, not blocks.

The research behind this is in `reports/Editable voxel planet architecture.md`, with notes in `research_notes/`. It replaces the hexel plan, which was tried first and has since been removed; its sparse tree and windows are in the history for when the store needs them. The old `hog::planet` generator and its heightfield are never used or read.

## Rules

- **Maths plus changes.** The generator is a pure, deterministic function of the planet's description and a point. The store holds only what differs from it. Nothing generated is saved.
- **Bit-identical everywhere.** The server and every client get the same bits from the generator, on every platform, checked in CI.
- **No stored heights.** A planet-scale reference level may exist only as physics computed inside the generator: crust, isostasy, basins. It is never saved, it never defines the surface on its own, and nothing outside the generator reads it.
- **Amounts, counted.** Voxels hold how much of each thing they contain. Water, rock, sand and heat are counted, and every step keeps the counts, in tests.
- **Many worlds.** A planet is a description made of physical knobs. Earth, Mars, Venus and the Moon are the first presets, and alien worlds are other settings.
- **Every landform has a home.** Each feature class gets a term in the generator, the store or the simulation, judged by stills of a fixed suite of spots, one per class.
- **Things fall apart when they ought.** Unlike Enshrouded, builds need support and wear out, so keeping a city standing is real, ongoing work for players, bots and NPCs. Building must not become a chore: sensible builds stand with room to spare, failure shows warning signs first, and repair is easy.
- **Beauty from life.** Enshrouded's beauty is placed by hand; ours grows. Plants spread and die back, animals graze and wear trails, and people build, farm and neglect, all by simulated rules, so the world is shaped by what lives in it rather than by a designer.
- **Libraries only when excellent.** We use one only if it's clearly excellent and has an MIT-compatible licence. Otherwise we build it ourselves.

## Layout

The planet is a cube inflated into a sphere. Each of the six faces is a square grid of columns, warped onto its sixth of the sphere by the tangent mapping, so a cell's area varies by at most 1.414 from face centre to corner. Every column points at the planet's centre, so down is always straight down a column. Across the cube's 12 edges grids meet at an angle, and at its 8 corners three cells meet instead of four. One neighbour function handles both and is tested at every edge and corner.

A cell is addressed by level, face and `(i, j)`. A level has `6 · 4^level` cells, and each splits into exactly four at the next level, so levels nest and sums carry exactly.

**A shell, not a ball.** Voxels run from an impassable floor a few kilometres down up to a sky ceiling, both preset knobs. Below the floor and above the ceiling there are no voxels, only calculation: core, mantle, gravity, heat flow, the air above. Magma enters through the floor as a computed source. Over a few kilometres of depth, cells barely change size.

## The generator

`sample(point, lod)` returns the distance to the nearest solid surface, the material makeup, any loose amounts and the temperature. It is built from the planet's knobs:

- **Planet-scale fields,** computed on demand per column and never saved: crustal provinces, the reference level from isostasy, climate, and dichotomies like Mars's.
- **3D terms:** strata, eroded relief, karst and caves in depth bands, arches and overhangs, craters by size and age, volcanoes and magma bodies, dunes, glacial forms, soils and regolith.
- **One coarse drainage solve per seed:** stream-power erosion to steady state over the whole planet, because a river depends on everything upstream. It sets where rivers, lakes and deltas lie. Detail is added on top.

Terms blend by smooth union and subtraction, weighted by physical fields, not by hand.

What v1 builds, all from the knobs:
- **Reference level:** a province field split at the high crust's share, the provinces floating apart by the isostatic step across margins as wide as that step's slope allows. The radius stands at the sea, filling its share, or at the mean ground on dry worlds. The shell's floor lies `floor_depth` under the lowest ground surveyed.
- **Relief:** mountain belts on long sinuous lines, set by the regime; peaks within them; rolling hills everywhere; ridges down plate worlds' ocean floors. Where water runs, gullies are cut from coarse to fine. Each width runs down the ground coarser than itself, so valleys branch as drainage does, without a simulation.
- **Craters:** counts from Neukum's lunar chronology by age and impact flux, capped at equilibrium. Thick air filters out the small ones. Bowls sit below the gravity-set transition, flat floors and central peaks above it, and creep wears each crater by its age.
- **Strata and soils:** sedimentary beds where a volatile has run, folded where mountains rose and stripped from them; soil, mud, beach sand and snow where water cycles; regolith that thickens with age where it doesn't.
- **Caves:** passages along limestone beds and chambers in them, and lava tubes under basalt flows, carved out of the rock.

A column's terms depend on direction alone, so `Generator::column` computes them once and samples any radius along it. The ground's slope, which the distance needs, is measured by central differences, four extra columns each. Voxels are filled through `Generator::columns`, for any cells of one level, which takes the slope from neighbouring cells instead. Stills come from `hog-stills`: a map, a relief square, a section and a plan slice, all drawn by sampling the 3D field.

## The store

The store is one sparse tree per face over `(i, j, shell)`. Empty means "ask the generator", a uniform block of edits is a single tile, and edited blocks are palette-coded 8³ bricks. Matter comes in two kinds:
- **Shaped matter,** such as rock, earth and buildings, carries a distance, its makeup and its condition: how worn, cracked or rotten it is.
- **Loose matter,** such as water, sand, rubble, snow and lava, carries amounts.

Only the server writes the store.

What v1 holds:
- **Voxels** stand on the cells of the level whose width is nearest the layers' thickness: level 18 at the default 115 km and 0.5 m, so cells are 0.53–0.75 m wide. A voxel's value is taken at its middle. It holds fixed-point values: distance in millimetres, condition, a grain and a liquid with amounts of the voxel's volume, and temperature in eighths of a kelvin. Distances are kept within a band of four layers either side of a surface; beyond it, only the side counts.
- **Bricks** of 8³ are the unit stored, saved and sent. A brick that is one voxel throughout is held as that voxel. Otherwise each quantity is packed apart, as one value, a palette of 1–16-bit indices, or in full. So a brick the surface passes through keeps its distances in full and a few bits a voxel for the rest.
- **The store** is a hash of changed bricks keyed by face, `i`, `j` and layer. A brick changed anywhere is held whole. One written back as generated is let go. Tiles bigger than a brick, and the tree over them, wait until uniform edits that large appear.
- **Filling:** a brick is filled through `Generator::columns`, where each column takes the ground's slope from the cells beside it. That is 2.2–2.4× cheaper than each measuring its own, and as true against the real distance (about 5% either way). A cell's column comes out the same in any square, so unchanged voxels are the same everywhere.
- **Saving** writes only the bricks changed since the last save, as rows of the world's SQLite file under a header of the layers they stand on. Loading refuses bricks saved on other layers.
- **Edits** so far are balls dug and built. All the generated ground is shaped matter; sand and snow turn loose when the simulation disturbs them.

## The simulation

The simulation runs on the server, only in chunks near players or events:
- **Water and lava** move by pipes between stacked sections in each column, weighted so they spread evenly in every direction.
- **Sand and rubble** slide when steeper than their angle of repose, measured in true distances.
- **Collapse** checks connectivity in a bounded region, plus a load rule that lets arches stand. It's partial and local: what's overloaded sags and cracks first, then the failed part falls as rubble.
- **Wear** lowers condition by each material's own process: weather, water, rot, rust, frost and load. Worn parts get weaker, so neglect ends in collapse. Wear is what makes maintenance jobs: repair, drainage, clearing rubble, roads.
- **Lava** stiffens and freezes as it cools.

Updates are integer, ordered by a hash and repeatable. Clients receive changed bricks within their interest radius and interpolate between updates.

## The client

The client meshes changed chunks on worker threads:
- **Shaped matter** is meshed by dual contouring, so crisp and smooth forms come out of one surface.
- **Loose matter** is meshed by surface nets.

Detail falls off with distance without cracks, and materials blend by triplanar texturing. Positions are 64-bit on the server and camera-relative floats on the GPU, with reversed-Z depth.

What v1 draws:
- **Chunks** of 32 columns square, each meshed by surface nets from its own voxels and one more all round, taken over the cube's edges from the next face. Each edge between voxels is drawn by one chunk: the one holding its lower end, or across the cube's edges, the lower face. So chunks meet without gaps, but for a column-wide one at each of the cube's 8 corners, where three columns meet.
- **Two surfaces:** shaped matter against open space, and liquid against air. Shaped matter counts as full of liquid, so the sea's surface runs into the ground instead of wrapping it. A voxel's material says which side it's on, as a distance under half a millimetre rounds to none.
- **Where:** from 16 m under a chunk's lowest ground to over its highest, around the sea's level apart from that, and through held changes next to either.
- **The client:** `--globe`, or `--globe-planet` with a preset or a planet file, meshes the chunks within 160 m of the eye on the job pool, nearest first, coloured by material and drawn as blocks. Flight keeps above the generated ground.
- **Cost:** 36–76 ms a chunk on one thread, Venus to the Moon. Most of it is the generator's columns: each also takes the ground's slope at 16 m for its soils, nearly another column's work.

## Build order

1. **Cells:** cube-sphere addresses, the tangent mapping and its inverse, exact neighbours across edges and corners, cell areas and volumes, levels; the sparse tree and windows ported from diamonds to faces.
2. **The planet's description:** physical knobs, derived values and the four presets.
3. **Generator v1:** reference level, strata, eroded relief, caves, craters, soils; golden hashes on every platform.
4. **Store:** face trees, tiles, bricks, both kinds of matter, saving and loading.
5. **Mesher v1:** surface nets; the client's `--globe` draws the new world, and the hexel code is removed.
6. **Mesher v2:** dual contouring.
7. **Detail by distance,** crack-free across chunks and faces.
8. **Materials and texturing.**
9. **Server authority and streaming.**
10. **Water.**
11. **Sand and rubble.**
12. **Collapse and wear.**
13. **Lava and magma.**
14. **The drainage solve, rivers, lakes and coasts.**
15. **Life:** plants, animals and people shaping the ground they live on.

Each stage ends in tests and stills. The report's build order says how each one is judged.
