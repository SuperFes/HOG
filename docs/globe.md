# The globe: a planet made of hexagonal voxels

Oct 31, 2026

The world is voxels: hexagonal prisms standing on radial lines, each holding a real material and its state. The planet is born in them, shaped in them, played in them and dug into. There is no heightfield anywhere. Ground height, coasts, rivers and cliffs are whatever the voxels hold, never a function sampled over the sphere.

This replaces the heightfield path (`Elevation`, `Landform`, river and shore strips) and the square-celled volume of `docs/world-v2.md`. Those stay playable until the globe overtakes them. Nothing new is built on them, and the globe never reads them.

## Rules

- **Voxels are the world.** Every stage reads and writes voxels. A stage that wants a height asks the column where its solid ends.
- **No heightfields, no stamps.** No process places a landform. Cliffs, caves, coasts and volcanoes come from materials, heat, gravity and water acting on voxels.
- **Conserved.** Rock, water and heat are counted, and every step keeps the counts, in tests.
- **Blocks first.** The voxels are drawn as hex prisms coloured by material. The final look comes at the very end.
- **Its own library.** `libs/globe` links `hog::core` and nothing from `hog::planet`, so the old path can't leak in.

## Cells

The sphere is tiled by an icosahedral hex grid: the vertices of an icosahedron whose triangles are split `2^level` times along each edge, projected onto the sphere. Each vertex is a cell, a hexagon of its six neighbours, apart from the icosahedron's twelve corners, which are pentagons. A level has `10 · 4^level + 2` cells. Each flat hexagon is carried onto the sphere by a projection that keeps areas, so every hexagon holds the same share of the globe and a pentagon five sixths of it. Shapes stretch a little by the icosahedron's corners instead: neighbours stand 0.94 to 1.2 steps apart.

The icosahedron is split into ten diamonds of two triangles each. A cell is addressed by level, diamond and `(i, j)` within it, plus the two poles. Neighbours are found exactly, in integers, across diamond edges and around the pentagons.

Outward, each column of cells is split into shells, as the square volume did, so a cell is a hex prism. Gravity points to the centre, straight through every cell's floor.

**Between levels.** A coarse cell's centre is a fine cell, and the six fine cells halfway to its neighbours are shared half and half with them. Moving material down a level hands it to those seven cells; moving it up sums them back, so it's conserved and centred, with no drift. Coarse levels run the planet-scale processes; fine levels run the local ones and the play.

**In memory.** Fine columns are stored in chunks, square blocks of `(i, j)` within a diamond, made on demand and dropped when they hold nothing generation can't make again. Edits and anything simulated are kept.

## What a voxel holds

- **Material:** iron, mantle rock, basalt, granite, sandstone, limestone, shale, sediment, soil, sand, ice, snow, water, magma, air.
- **State:** temperature; for rock, strength and fractures; for water, flow; for magma, how far it has cooled.
- A material counts only through properties that change how the world forms: density, strength, how fast it wears, where it melts, how it flows.

## Birth

The planet forms in the voxels at coarse levels, each step a process acting on what the last one left:

1. **Accretion:** a hot ball of mixed metal, rock, water and gas, its heat from impacts and decay.
2. **Melting and differentiation:** where it's molten, dense metal sinks and light rock rises, so a core, a mantle and a crust sort themselves out.
3. **Cooling:** heat conducts out and radiates from the surface; the magma ocean freezes from the surface down into the first crust.
4. **Outgassing and the first oceans:** gas leaves the rock as an atmosphere; as it cools, water rains out and fills the low ground.
5. **A moving mantle:** heat drives the mantle to overturn; the crust over it cracks, sinks where it's cold and dense, and thickens where it's pushed together. This is where the plate model failed before. Here it's tried as material moving between voxels, not as plates carried in their own frames.
6. **Weathering and water:** rain wears rock to sediment, water carries it downhill and drops it, ice grinds it. Rivers are water voxels flowing in channels they cut.

Each step is judged by stills of the blocks and by its counts.

## The live world

- **Digging and building:** edits remove or place voxels, as server operations every client applies alike.
- **Fractures and rockslides:** rock holds by its strength. Cut away its support, or let water, frost or a quake crack it, and what breaks off falls, tumbles and settles as rubble.
- **Magma:** heat under the crust melts pockets that rise through cracks, flow out as lava, cool and become new rock.
- **Water:** flows, pools and erodes wherever it is, and settles to rest when nothing moves it.
- **At rest costs nothing:** only what moves is simulated, near players, and baked back once it settles.

## Build order

**H · Hex cells**
1. Addresses: level, diamond, `(i, j)`, poles; centre on the sphere; the cell under a direction.
2. Neighbours: six, five at the pentagons, across diamond edges, exact.
3. An equal-area projection; corners, area and a prism's volume between shells.
4. Between levels: the centre and the six half-shared cells, conserved both ways.
5. Columns of shells holding materials; chunks.
6. Blocks: hex prisms meshed by material, drawn in the client.

**P · Birth**
7. Accretion.
8. Heat, melting and differentiation.
9. Cooling and the first crust.
10. Outgassing, rain and oceans.
11. The mantle overturning, the crust cracking and sinking.
12. Weathering, rivers, sediment and ice.

**L · Live**
13. Digging and building.
14. Strength, fractures and falling rock.
15. Magma and eruptions.
16. Water flowing near players.
