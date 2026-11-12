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
2. **Bounds for the ground's pieces**: the smooth field, fbm, valley walls, gullies, crags, each
   with a value and an interval over a cube, tested against dense sampling.
3. **Formed features as primitives**: forming emits them and builds the tree before play. The
   world must read as it does now at the finest depth, and forming's time is measured.
4. **Coarse summaries**: right averages and roughness for detail finer than a node.
5. **Game queries**: point, ray, box.
6. **Wire and client**: summaries sent by depth wanted, drawn as cubes.
7. **Edits as shapes.**
8. **Remove** `Generator::columns`, `generate_stack`, `Land`'s tiles, `TileSending` and tile
   meshing.

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
