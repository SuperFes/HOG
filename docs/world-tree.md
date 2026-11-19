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

1. **Chunks as stored on the wire.** Protocol 43. The server answers asks with chunk bytes; the
   client holds chunks and reads them as one tree. Asks replace the asker's last; leaving drops
   them. Tests: what's sent is what's stored; a client holds what it'd hold from pieces; the
   first answer goes the tick it's asked whatever is being filled.
2. **Generation apart.** The wants table, `hog-bake --serve` filling it near first, the server
   picking up written chunks. Tests: a server with no generator sends what's stored at once and
   the rest as the generator writes it; asks no one holds are dropped.
3. **Stage versions.** Versions per stage; chunks record theirs; stale chunks served and remade
   near first; forming's results stored by version. Tests: bumping a fine stage leaves coarse
   chunks alone and remakes fine ones; stale chunks are still sent; a restart doesn't form again.
4. **Features as primitives.** Forming emits the feature table; nodes carry culled lists; a node
   is made from its parent and its features. Tests: the finest nodes agree with `Formed::at`
   within a tolerance on the test world, over land, coast and sea floor; time per node, coarse and
   fine, against now.
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

## Versions that hold

A version bumped by hand is only as good as remembering to bump it, so a stage's version is its
hand-kept number mixed with the hash of its knobs and its inputs' versions, and checked: the
generating process remakes a few random stored chunks now and then, and one that comes out
different under the same versions is reported as a stage changed without its number. During
development a stage can also be marked to remake everywhere it's read.

## Open

- How forming's stages write features and corrections instead of fields read by `Formed::at`;
  valleys first, as they're most of the cost now. Look at how others refine coarse terrain on
  demand (Proland's producers, Outerra) before settling.
- How deep the stored levels go: as deep as it takes to look right, with the look below that
  from the ground's materials as drawn rather than stored shape.
- Waking across processes: LMDB has no notify, so something beside it (a socket, an event file)
  says when wants or chunks are written. Any technology that does the job is fair.
