# The world tree: generated large first, stored, read without the maths

This joins [the shape tree](octree-generation.md) and [the tree store](tree-store.md). The store,
pieces and streaming built for the second are the container; the first's rule fills it: **play
never runs the generator's maths for a read**. A read walks stored nodes and adds a few seeded
layers under the last one.

What runs now breaks that rule. Forming solves the planet on its points, about 250 m apart, and
everything finer is rebuilt at every sample: `Formed::carved` looks up every valley within 1.8
spacings of the point, walks its course, runs its wander and wear noise, and rounds the walls
together. A column costs 0.2 to 0.55 ms, more for coarse ones than fine, and woods, slopes and
fallen rocks each ask again. A 2 km region takes 10 s to fill.

## Decided

- **Large first, each level from the one over it.** Forming writes the top of the tree at the
  scales it solves at. Every finer node is made from its parent's stored values and the few
  features that reach it, never from searching the planet's points again.
- **A node stores what its parent can't predict**, and a few numbers that steer what's under it:
  slope, roughness, gullying, flow direction, water level, rock strength. Where the parent's
  prediction is close enough, nothing is stored and the node is made when read.
- **Features are primitives with culled lists.** Forming emits valley stretches, channels, lakes,
  coasts and crags as shapes with their numbers, held once in a feature table. A node holds the
  ids of those that reach it, its parent's list cut down by the bounds already written
  (`wall_bounds`, `Smooth::range`, `fbm_bounds`). A node costs the features in it.
- **Coarse nodes are summaries, not samples.** Bounds decide all air or all solid without
  sampling; a crossed node holds its plane, shares and roughness, with detail finer than it as
  its mean and spread. A coarse node is cheaper than a fine one, never dearer.
- **Below the unique scale, seeded layers.** Where nothing is unique (past the finest valley and
  channel), nodes aren't stored: each is the leaf's stored numbers plus one seeded layer per size
  (seeds from position, never chained), gullies from Johansen's filter steered by the stored
  gullying. A read costs at most the stored levels plus a few layers.
- **Water and ground at the same resolution.** Stages write results into the levels they work at,
  so a lake's level and its shore come from the same nodes.

## Made to iterate on

No eight-hour bake before seeing anything. The planet is made where it's looked at, near first,
and a change to the generator remakes only what it touches.

- **On demand, near first.** Nothing is baked ahead unless asked for. A node is made the first
  time anyone wants it, top down, so the first view needs the coarse levels over the whole planet
  and fine ones only near the eye. `hog-bake` stays for filling ahead when wanted.
- **Stage versions.** Each stage (forming's parts, valleys, coasts, gullies, materials, woods...)
  has a version: a number bumped when its maths changes, mixed with a hash of the knobs it reads
  and the versions of the stages it reads from. A chunk records the versions of the stages that
  made it. Changing gullies leaves every node over the scale gullies work at untouched.
- **Stale is still shown.** A chunk whose stage versions are behind is still served, so the old
  ground stays on screen, and remade in the background, nearest the players first. Clients are
  sent the new chunk when it's written, as for any change.
- **Forming kept by version.** Forming's results are stored with their versions, so a restart
  doesn't form again unless something it reads changed, and a changed late stage reuses the early
  ones.
- **Generation apart from serving.** Serving only reads and sends. What players want goes in a
  table in the store; a generating process (`hog-bake --serve`) fills it, near first, and the
  server sends what's written. A slow fill never holds up what's already there.

## Edits over what's generated

Generated nodes are a cache: anything made can be thrown away and made again from the levels over
it, as Outerra refines its terrain as it's viewed. The world is remade until it's right; edits
matter once it's settled, and needn't outlive a remake before then. Each edit is kept as a shape
(a ball dug, a box filled, a slab levelled) in its own table, listed on the nodes it touches, and
applied over the generated nodes as they're made, with the summaries over them redone up the
path. Houses and other buildings are objects standing on the ground, not ground; only what they
do to it (the levelled slab, a cellar dug) is an edit.

## Sending what's stored

The server sends chunks as stored, never decoding them: an answer to a node down to a depth is the
bytes of the chunks covering it, already in the mapped file. The client decodes and grafts them,
and a chunk's version is its change stamp. A client's new asks replace its last; leaving drops
them, so nothing is filled for someone gone.

## Steps

Each step is its own commit with tests, and the world stays playable after each.

1. **Chunks as stored on the wire.** Done: protocol 43's `Chunks` carry each kind's chunk as the
   store holds it, its band, root, version and tree's bytes, and version 0 for one let go of.
   `server::ChunkSending` sends, for each node asked in order, every band's chunks over it or
   under it down to the depth asked, read with `ChunkStore::Snapshot::stored` and never decoded;
   each goes once a version, a node the store holds only part of goes as it is and waits on a
   fill, and asks replace the asker's last. Every water body goes before the first chunk. The
   client holds each chunk's tree at its root, a deeper band's over a shallower's. Tests: what's
   sent is what's stored; the rest goes once filled; a chunk changed goes again and one gone is
   let go of; messages keep to their limits; asks replace what's due.
2. **Generation apart.** Done: the server writes what its players wait on to a wants table
   (`baked::WantsTable`, an LMDB environment of its own beside the store, so writing it never
   waits on a fill), and `baked::Generating` fills what it asks for, forgetting what drops out
   of it (`ChunkFiller::forget`), in `hog-bake --serve` or on threads of the server's own
   (`[ground] generate`, on by default). Every write to the store, and every want found to need
   nothing more, goes in the store's log in the same transaction; the server reads the log each
   tick for what changed, whoever wrote it, so a bake in another process wakes what waits on it
   too. Both sides poll, as LMDB has no notify. The server's tick never fills or decodes. Tests:
   the log holds every write in order and keeps its newest; the table holds what it's last
   given; what the table asks for is filled and answered, and what leaves it never is; a server
   that leaves filling to another process sends what's stored when asked.
3. **Stage versions.** Versions per stage; chunks record theirs; stale chunks served and remade
   near first; forming's results stored by version. Tests: bumping a fine stage leaves coarse
   chunks alone and remakes fine ones; stale chunks are still sent; a restart doesn't form again.
4. **Features as primitives.** Forming emits the feature table; nodes carry culled lists; a node
   is made from its parent and its features. The look may move on the way; it's sharpened once
   the structure stands. Measured first: a read ran 16 valleys, 4 of them near, through 128 course
   stretches, 76 of which went through `wall_at` and the pairwise lowest; rounding alone was 40%
   of a fill. In parts:
   1. *A valley costs the stretches that can stand lowest.* Spurs between one valley's bends round
      over 4 m at most, so a stretch standing higher than that over the lowest isn't read; the
      ground's other numbers blend over 15 m of height, not 30. A 32 m region went from 0.96 s
      to 0.35 s, a 2 km one from 10.9 s to 5.6 s.
   2. *Ground cells.* The ground of each of the cube's cells, at every level, made once and kept
      where fills share it; slopes from its gradient, never from reads beside it. Still made by
      `Formed::at`.
   3. *Fields from the parent.* Under forming's spacing, a cell's smooth fields (soils, deposits,
      walls, strength, weathering, the ground before valleys) come from its parent level's cells,
      not from weighing the points again.
   4. *Noise a level at a time.* A cell adds only the octaves of its own size to its parent's.
   5. *Valley lists.* The course table, wander baked in, and each cell's list cut from its
      parent's by `wall_bounds`.
   6. *Gullies, settling, beaches, crags and lakes* made the same way.

   Tests: what's made agrees with `Formed::at` within a tolerance on the test world, over land,
   coast and sea floor; time per region, coarse and fine, against the last part's.
5. **Coarse summaries.** Bounds decide air and solid; planes, shares and roughness for detail
   finer than a node. Tests: a parent agrees with its children combined; a 2 km region fills in
   well under a second.
6. **Seeded layers.** Unique-scale leaves and the layers under them. Tests: a read from any depth
   agrees with one from the root; seams match across neighbours; layers steer by stored numbers.
7. **Woods, rocks and materials from stored numbers.** No `Formed::at` behind them.
8. **Edits as shapes** over generated nodes; a remake may drop them.
9. **Remove** `Formed::at` as a play path, `Generator::columns`, tiles, pages and their stores;
   the bot and walking stand on the tree.

Steps 1 to 3 change nothing about how the planet looks, and make every later step quick to see:
change a stage, and only what it touches is remade around where you're looking.

## Valleys, as others carry them

How step 4 carries valleys from forming's points into finer levels, after Proland, Outerra and
Génevaux, Cordonnier and Peytavie's papers (none tried here yet):

- **Courses stored once, wander baked in.** Forming writes each valley stretch as a course line
  with its wander already in it, and a profile along it: bed level, width, depth and wall shape,
  falling only downstream and smoothed by width (Proland's curve profiles, Peytavie's courses).
  No noise is walked along a course at read time.
- **Lists clipped down the tree.** A node's list is its parent's clipped to it by `wall_bounds`
  plus a margin, its course lines simplified at half the parent's tolerance (Proland's graphs).
- **The ground under the valleys comes down unmodified.** A node's ground is the interpolating
  upsample of its parent's ground before valleys, plus a stored correction where there is one,
  plus noise scaled by stored slope, curvature and roughness (Proland's elevation, Outerra's
  noise). The valleys are laid over it again at each level from the list, by Génevaux's replace
  blend, whose weight is zero past a valley's reach, so bounds cull it. So a valley is never
  smeared down through the upsampling. A read is a distance to a course and a profile lookup:
  about a microsecond in Génevaux's measurements, against 200 to 550 here now.
- **Narrower than the node, a valley is its mean and spread** (step 5).
- **Edits are shapes laid on in the same stage** as a node is made, as Outerra lays craters, and
  versions decide what's remade, as Proland's graph versions do.

Sources: [Proland terrain](https://proland.inria.fr/doc/proland-4.0/terrain/html/index.html),
[Proland graphs](https://proland.inria.fr/doc/proland-4.0/graph/html/index.html),
[Outerra's terrain](https://outerra.blogspot.com/2009/02/procedural-terrain-algorithm.html),
[Outerra's roads](https://outerra.blogspot.com/2009/06/roads.html),
[Génevaux et al. 2013](https://www.cs.purdue.edu/homes/bbenes/papers/Genevaux13ToG.pdf),
[Cordonnier et al. 2016](https://www.cs.purdue.edu/homes/bbenes/papers/Cordonier16CGF.pdf),
[Peytavie et al. 2019](https://www.cs.purdue.edu/homes/bbenes/papers/Peytavie19CGF.pdf).

## Versions that hold

A version bumped by hand is only as good as remembering to bump it, so a stage's version is its
hand-kept number mixed with the hash of its knobs and its inputs' versions, and checked: the
generating process remakes a few random stored chunks now and then, and one that comes out
different under the same versions is reported as a stage changed without its number. During
development a stage can also be marked to remake everywhere it's read.

## Open

- How deep the stored levels go: as deep as it takes to look right, with the look below that
  from the ground's materials as drawn rather than stored shape.
- Three tests find their spots elsewhere since valleys moved (step 4.1), and fail there: a rock
  on a 50° slope whose top's node the fill holds solid, a crag placed 0.3 m lower than formed,
  and inland water 36 m up that the old voxel world's lookup misses a metre under its top.
  Marked may-fail; rocks and crags come from stored numbers in step 7, the voxel world goes in 9.
- Waking across processes: both sides poll now (the server each tick, the generator every 50 ms
  while idle). A socket or event file could wake them instead if polling ever shows.
