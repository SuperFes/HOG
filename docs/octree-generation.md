# The shape tree

Proposed 2026-11-12. The world is one spatial tree that can be cut into anywhere and split as far
as detail is wanted. Every node, at any depth, says how it looks and what it means to the game. No
tiles, stacks, columns or tile workers: those go.

It works like a rope in a text editor. A rope is a tree whose every node carries a summary of the
text under it (length, line count), so any position is found by walking down, and an edit only
touches one path. Here each node is a cube of space and its summary is what's in that cube.

The generator finishes before play. Forming does all the searching and solving (courses, lakes,
walls, which feature wins where) and leaves its result as primitives: fixed shapes with their
numbers, cheap to evaluate. It then builds the whole tree, splitting until each node is decided
(all air, or solid of one material) or holds only a few primitives. Play only reads: it walks down
to a stored node and evaluates that node's few primitives, as a font's outlines are drawn at any
size without designing the font again.

## A node

- **Its cube**: on a cube face of the planet, a cell and a span of radius, so depth means detail
  everywhere on the sphere.
- **Its shapes**: the formed features whose reach touches it (ground field, valley stretches,
  channels, lakes, crags, fallen blocks, beds, coast, edits), pruned to what can matter inside it.
  A child keeps only its parent's shapes that reach the child (Dreams' culled edit lists), and
  each shape's expression is simplified for the cube where interval bounds settle a branch
  (Keeter's MPR: a `max` whose one side always wins collapses to that side).
- **Its summary**, which combines from children to parent the way a rope's lengths add up:
  - bounds of the surface: all air, all solid, all water, or crossed;
  - shares of each material and of water, so it can be drawn and mined at its size;
  - the surface as it averages over the cube: a plane (point and normal) and how rough it is
    under that, the detail the node is too coarse to show;
  - what it means to the game: can be stood on, dug and how hard, liquid, temperature range,
    the domains present (later habitats, weather, caves).

## Cutting in

To know any place at any detail: start at the top, walk down. Where a node's summary already
answers the question (all air, all rock of one kind, coarse enough for this viewer), stop. Below
the stored leaves, finer detail is the leaf's primitives evaluated, the same at any depth, so a
place reads the same however it was reached.

A node's summary comes from its primitives where it isn't split, not from sampling its middle. Detail finer than
the node is left out with its right average and counted as roughness: noise octaves finer than the
cube contribute their mean (zero) and their spread, channels narrower than the cube their share of
water and lowered ground. Splitting replaces the estimate with the children's combined summaries.

## What uses it

- **The game** asks by walking down: a point (what's here), a ray (sight, sun, digging), a box
  (can this stand here, what's under this building). Each stops as high as the summaries allow.
- **The client** is sent node summaries down to the depth it wants for each place and draws them
  however it likes: cubes for now, a plane patch per node later, anything that reads the summary.
  It never evaluates shapes. Moving sends only the nodes newly wanted, coarse before fine.
- **Edits** are shapes added to the nodes they touch; summaries are redone up that one path, as
  a rope edit does.

## What's kept

The tree forming built, with each leaf's primitives, and edit shapes. Its size follows how many
primitives there are, not the planet's area: empty sky and deep rock are a few large nodes.
Nothing is stored as tiles or voxels.

Radius is the axis where the ground's structure lies (it's mostly one surface with beds under it),
so a leaf can hold its contents as a few layers along radius (Benes and Forsbach's layered terrain)
instead of splitting all the way down. Caves and overhangs are where a node holds more than one
crossing, and only there does it need to split in all three directions.

## What's hard

Interval bounds for the ground's maths. The smooth field between formation points, valley walls
blended with smooth minimums, fbm and gullies all need a bound over a cube, not just a value at a
point. Each piece has a known slope limit (fbm's amplitude per octave sums to a bound; blends are
no steeper than their parts), so bounds are value at the middle ± slope × half the cube, tightened
where a piece knows better. These bounds decide how many nodes get split, so how tight they are
decides how fast this is.

## Steps

Each step comes with tests. Every split is checked against the shapes evaluated directly, and a
parent's summary against its children's combined.

1. **Node, summary, build** over toy primitives (half-space, ball, box) (done, `ShapeTree`):
   built until nodes are decided or hold few primitives, read anywhere, summaries combine, any
   depth reads the same as the primitives evaluated directly. A 50 m ball split until decided
   sums to its volume within 0.01%.
2. **Bounds for the ground's pieces** (done), each rigorous and tested against dense sampling,
   with how much of the bounds the samples span:
   - fbm over a ball (`fbm_bounds`): per octave the tighter of interval arithmetic over the
     lattice cells it touches and the middle's value ± the steepest it gets there; 23–31% over a
     few metres, 13–16% over hundreds.
   - Smooth fields over a cap (`Smooth::range`): exact, between the points within reach.
   - Valley walls (`wall_bounds`): floor, steepness and channel each bounded along the stretch;
     71%.
   - Gullies (`gully_bounds`): each octave at most half its deepest; 5%, and not shrinking with
     the ball, since it ignores how finer gullies stay within the crests of coarser ones.
   - Crags: from nothing to the most rotting over the node (`rotted_by` only grows with how fast
     and how long the rock rots, and cores stand no higher than they rotted).
   Walls and gullies live with the uncommitted noise work they bound.
3. **Formed features as primitives**: forming emits them and builds the tree before play. The
   world must read as it does now at the finest depth, and forming's time is measured.
4. **Coarse summaries**: right averages and roughness for detail finer than a node.
5. **Game queries**: point, ray, box.
6. **Wire and client**: summaries sent by depth wanted, drawn as cubes.
7. **Edits as shapes.**
8. **Remove** `Generator::columns`, `generate_stack`, `Land`'s tiles, `TileSending` and tile
   meshing.

## Ideas, not decided (2026-11-12)

Nothing may be generated at play: a read is a walk of stored data, never `Formed` or its maths.
Writing out every metre of an Earth-sized world is petabytes (Flight Simulator streams over 2 PB),
because detail that's pure noise holds nothing beyond its seed. So the unique part is stored and
the noise part is made from stored numbers.

- **Corrections, coarse to fine.** A node stores only the correction to what its parent predicts,
  plus a few numbers (roughness, slope direction, gullying). Nodes exist only where something
  can't be predicted from the parent, so the tree's size follows the features, not the area.
  Reading anywhere is the parent's prediction plus the stored corrections, so every depth knows
  how it looks. Wavelet terrain (C-BDAM, ~0.25 bits a sample for real global terrain) and
  geometry clipmaps work this way.
- **Seeds by position, in blocks, never chains.** Counter-based randomness (Philox, "as easy as
  1, 2, 3"): a seed is `hash(world, size, lattice point)`, computed directly. A stored block owns
  a band of sizes: under it, its stored corners blended plus one seeded layer per size, scaled by
  its stored numbers. Layers don't depend on each other, and made-up detail never feeds the next
  level; only stored data does. A read costs at most ~20 stored levels plus ~8 layers wherever
  it cuts in. Neighbours share the lattice, so no seams.
- **Gullies from layers.** Johansen's erosion filter (2026, MPL-2.0): each layer carries its slope
  to the next, so small gullies branch off larger ones and run downhill, each point computed
  alone. Steered by the block's stored slope and gullying; no real drainage, which the stored
  valleys and rivers already give.
- **Processes write the tree.** The tree is forming's working state as well as play's world.
  Stages run in order, each at the scale it works at, reading what earlier stages stored and
  writing corrections: tectonics at km nodes, then snow, then ice flowing over ~100 m nodes and
  carving its bed (Cordonnier et al. 2023), then the sea flooding it into a fjord, then rivers in
  what the ice left (hanging valleys from the order). The tree splits only where a result has
  edges (fjord walls finer than its floor).
- **Processes steer the seeds.** A stage also writes block numbers: ice flow direction, polish,
  plucked slopes, walls left too steep. The seeded layers read them: striations along the flow,
  smooth polished walls, jagged plucked sides, talus at the foot.
- **Forming decides where a block is enough**: where the stored corners plus layers stay within
  tolerance of the stage's result (the step 2 bounds tell without sampling), stop; elsewhere
  store a correction and go finer.

Open: how much land counts as unique (sets bytes and forming time); forming time for ice over
every range; how the existing `libs/formation` stages write corrections instead of results.
Possible first slices: one mountain range as stages in the tree until a fjord forms; seeded
blocks with erosion-style gullies against `Formed::at` for how often a block is enough.

## Measured before (2026-11-11)

Why the tile path is slow, for comparison: a 64×64 tile costs ~0.2 s at every level, ~85% of it the
ground maths at ~36 µs a column; every column repeats its neighbours' valley, noise and point
searches, and an all-air tile costs as much as a ground tile.

## References

- Keeter 2020, [Massively parallel rendering of complex closed-form implicit surfaces](https://www.mattkeeter.com/research/mpr/keeter_mpr20.pdf):
  interval arithmetic per region prunes the expression for everything inside it.
- Evans 2015, [Learning from Failure](https://advances.realtimerendering.com/s2015/AlexEvans_SIGGRAPH-2015-sml.pdf):
  Dreams' lists of SDF edits, culled per brick.
- Frisken et al. 2000, Adaptively sampled distance fields: an octree split only where the field
  has detail.
- Crassin et al. 2009, GigaVoxels: nodes made only when the renderer asks for them, coarse
  first.
- Benes and Forsbach 2001, Layered data representation for visual simulation of terrain erosion:
  terrain as layers of material along the vertical.
- Génevaux et al. 2013 and 2015, terrain from hydrology and feature primitives: the land as a
  construction tree of features, water as its own.
- Stolte and Kaufman, [parallel octree voxelization](https://nilostolte.github.io/research/parvox.pdf):
  an interval test skips octants that can't hold the surface.
- Gobbetti et al. 2006, [C-BDAM](https://vcg-legacy.isti.cnr.it/Publications/2006/GMCDG06/eg2006-cbdam.pdf):
  planet terrain as compressed wavelet corrections. Losasso and Hoppe 2004, geometry clipmaps:
  stored terrain plus noise matched to its roughness.
- Takikawa et al. 2022, [Variable Bitrate Neural Fields](https://research.nvidia.com/labs/toronto-ai/vqad):
  a multiresolution octree streamed coarse first.
- Salmon et al. 2011, [Parallel random numbers: as easy as 1, 2, 3](https://users.cs.utah.edu/~hari/teaching/bigdata/random123sc11.pdf):
  random values computed straight from their counter.
- Johansen 2026, [erosion filter](https://blog.runevision.com/2026/03/): branching gullies from
  noise layers, each point alone.
- Cordonnier et al. 2023, [Forming terrains by glacial erosion](https://people.cs.uct.ac.za/~jgain/wp-content/papercite-data/pdf/cordonnier2023.pdf):
  ice flow and its carving at mountain-range scale.
- Villanueva et al. 2016, [symmetry-aware sparse voxel DAGs](https://www.crs4.it/vic/data/papers/i3d2016-symmetry-dags.pdf):
  repeated subtrees stored once.
