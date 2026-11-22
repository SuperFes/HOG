# Room to improve

Places found where the game can get faster, lighter or smoother, kept as they turn up so none is
lost. Each says what was measured and what's thought to help; take one up, measure before and
after, and strike it off.

## Drawing the ground (client)

- **Refine by error, not size.** A node is drawn finer once it spans 16 pixels however flat it is.
  Refining where the ground strays from a coarse node's plane by more than a pixel or two would
  keep flat ground (sea floor, beaches, plains) coarse at any distance. Each crossed node now
  holds its roughness, a bound on that stray (about 2.6 times what its children show), unknown
  in chunks made before it; the client doesn't read it yet.
- **Skip blocks that can't hold a surface.** 58% of the blocks wanted for a view came out empty
  (3,681 of 6,314 on report `20261120-212931`) but cost a full sampling each.
- **Don't re-mesh for detail a block can't show.** A block meshes again when any piece it reads
  changes, finer than its cells or not; as finer pieces land, coarse blocks are remeshed for
  nothing (1,297 of 30,484 over three minutes in `20261120-221605`).
- **Mesh what's in front first.** The queue goes by distance alone; asks already rank what's
  behind lower.
- **Bigger blocks far away.** Every block has the same cells, so far ones pay the same overhead,
  draw call and upload as near ones.
- **Sampling a block.** About 0.5 ms of ground and 0.25 ms of water a block, mostly finding the
  leaves round each corner through hash maps and bisecting crossings down to the finest lattice;
  reading a block's cells once into a flat array would cut it further.
- **A persistent pieces snapshot.** Every drawing plan copies the whole held list (~30k entries),
  four times a second, on the thread drawing.
- **GPU.** The ground pass is 8 ms of a 20 ms frame on the 860M, cloud marching 3 to 11 ms; half
  resolution with temporal reprojection is the usual fix for the clouds.

## Streaming (client and server)

- **Memory budget for held pieces.** Nothing held is ever let go (188 to 321 MB in reports); far
  detail could go, the cache making it a local read to have again.
- **Band overshoot.** A chunk runs to its band's foot, about 4× what was asked; align asks to band
  feet, or have the server cut trees at the depth asked.
- **Ask deep at once.** Detail comes a band a round trip; asking under held coarse leaves, culled
  by their planes, would bring the finest near the eye in two.
- **Water bodies smaller.** A river's course is six doubles a point every 11 m; floats from its
  head, fewer points where it runs straight, about a third the size.
- **Client settings file.** Cache size, node pixels and the like as settings, not options.

## Baking (server)

- ~~**Finding cells per sample.**~~ Each sample's cells are now found once a fill, and steps
  within a face skip the cube's geometry: band 3 went from 994 s to 928 s.
- **Ground cells read again.** A band 3 bake reads 38.3M ground cells for 33.7M distinct ones:
  12% are read again after the kept cells (2^17) let them go, at about 85 µs a read. A bake could
  keep more; or fill regions in an order that keeps neighbours close in time.
- ~~**Valleys out of reach read in full.**~~ A valley whose whole course lies out of its reach
  now returns before its stretch, worn walls and `wall_at`s (0.7 of the 2.8 full reads a cell
  made, adding nothing). Same bits; `formed ground cost` 1–3% faster, within the noise of a bake
  running alongside.
- ~~**Hashing noise corners.**~~ Each octave hashed its eight corners from scratch, three mixes
  each; they now share their first steps (14 mixes, not 24). Same bits; an fBm takes 21% fewer
  instructions and 26% fewer cycles (`fbm cost`, `perf stat`), and noise is about a quarter of a
  fine bake. Each octave's frame and its way back now come from a table built the same way
  rather than a matrix product each octave: 10% fewer instructions again, about 3% fewer cycles.
- ~~**Valley reads' rounding and exponentials.**~~ `Lowered::lowest` rounded every way twice and
  a valley's stretches each took an `exp` to find most count none; `maths::exp` called libm's
  `ldexp` and `nearbyint`. Rounding is now kept from the first pass, far stretches skip the
  `exp`, exp scales by a power of two exactly and rounds by the shift trick, and a stretch's
  floor is read from its own stretch, not a binary search. Same bits; a formed read takes 8–9%
  fewer instructions at every detail (`formed ground cost`, 400k at 2.5 m from 430k).
- ~~**Coarse columns' rubble.**~~ A coarse column reckoned its rubble over rock cells finer than
  its own (18 m under 37 m cells on the test world), so rockfall's walk read the ground of four
  times its cells: 99% of a column from 64 m up, 150 µs a column against 44 µs at the finest.
  Rubble is now reckoned over the cells read at its detail; a 2 km region (6 levels, 32 m leaves)
  fills in 0.43 s from 1.07 s, a coarse column 26 µs. What's left is mostly their ground.
- **Layered fine bands.** A fill can read nodes under a base size from the base's columns plus
  seeded layers (`Layered`, off): a 32 m land node down to 0.5 m fills in 50 ms against 194 ms
  with a quarter of the shape bytes, but strays 0.27 m (median) from the generator's finest
  ground against 0.03 m. Near channels, shores, crags and caves it now takes the generator's own
  reads: over a river with an 8 m base, 0.10 m off in 239 ms against 0.20 in 139 (plain 0.009 in
  355). Narrower margins, or a channel's own width rather than a base's, would cut the cost there.
- **Nodes missed beside river channels.** A node is taken as missed only past its margin plus
  how far the four columns it's read between stray (64 land spots, 5 levels: 1.5% more nodes,
  17.8 s from 17.2 s). Left: on the 8–24 steepest small-world cells, 1–28 missed leaves still
  have ground through them, all within about 250 m of a channel, where the column at a node's
  middle sits up to 900 m off columns a few dozen metres away. Reading a level finer doesn't
  catch it and costs 2.3×. Wants a channel's cut in `rest`, or the generator's own read near
  channels above the base as layered fills take below it.
- **Valleys still dominate a read.** `carved` is 60–75% of `Formed::at` at every detail, nearly all
  `valley_wall`: of about 15 valleys asked a read, 4 pass the bound and 3.5 reach, and each scans
  its whole course (about 31 stretches) three times: nearest 17%, floors and walls 11%, `wall_at` on
  about 6.6 of them 16%, rounding them to the lowest 25%, channels (`stretch_of`, its `pow`s) 9%,
  worn walls' fBm up to 8% at 2.5 m. Channels per point, kept, would save the 9% at about 128 bytes
  a point; a per-cell list of the stretches near would cut the scans.
- **Seams over cube edges.** Weights there are weighed, not read between seed cells, so the
  ground can step by up to the reads' 8 cm along the cube's edges.
- **Weighing forming's points.** `Smooth::weigh` walks its neighbours with a linear `find` and
  an `atan2` for each point it passes; comparing cosines would skip most of the `atan2`s.
- **Woods read their own ground.** Each woods node reads the ground cell at its own detail
  (`globe::woods`), which the shape's columns don't share: a 2 km region (6 levels) spends 70 ms
  of 418 on woods, against 5 ms on the shape, 3 on materials and 1 on rocks and crags;
  `climate_at` is about 2 µs of that a node. Most of the rest (about 340 ms) is making the
  columns. Woods read from stored ground numbers (strata, height over the valley floor, bare
  rock) would cost about what materials do.

## Server and tooling

- **Server teardown in tests waits minutes** on fills still in flight (about 4 min in
  `a player asking for the root…`).
- **Bot test fails on CI** (`bot_tests.cpp:170`, on the ground); not yet looked at.
- **Single-file LMDB stores** (`chunks.mdb`, `wants.mdb`).

## Look

- **Water seam holes** in report `20261120-063135`: likely seam triangles wound the wrong way.
- **Big flat triangles in shallow sea** in reports `20261120-235912`, `20261121-000207`,
  `-000210`, `-000212` and `-000419`: sea floor shows through the water top as straight-edged
  triangles tens of metres across, near the eye, in shallows a few metres deep. Not yet looked at.
- **Rubble** wants per-cell values with hard edges.
- **Ground textures at 2.5 m** want smoothing and averaging across cells, and their own rules
  on cliff sides. The shapes hold up at that detail; only the texturing lets them down.

## Trees (prototypes/trees)

What the tree prototypes still lack, from their sheets in `prototypes/trees/out/`. Species are
rule files in `prototypes/trees/species/`; a fix that's data belongs there, not in code.

- **Distance detail, what's left** (`lod.py`, `lod_sheet.py`, `forest_sheet.py`): thinning,
  relit impostors lined up by depth, baked own-sun visibility and two-tone shading are in; a
  1080p field of 34 trees is 3.2M triangles + 16 impostors against 20.8M, mean difference
  4.5/255, and 6x6 impostor views do as well as 12x12 there. Still:
  - full-mesh leaves vanish far off (5 cm birch leaves at 400 m read as bare twigs): the alpha
    mips fall under the cutoff; mips need coverage kept per level (scale alpha per mip);
  - thinned crowns at 100-180 m read lighter and yellower than the mesh: calming normals toward
    the local mean tilts them sunward; calm toward a lit-share-preserving normal instead;
  - the crossfade is an image blend here; in the engine a screen-door dither, per tree;
  - impostors stay 5-10 levels brighter than the mesh with the sun off too, since the depth
    step and the full-resolution cutoff in the bake; not yet traced;
  - cells are 256 px and soft at 40 m, which is still mesh range; a season's colour is baked
    in, where a turn mask and palette in the atlas would let one bake cover autumn.
- **Light from anywhere**: own visibility is baked over the whole sphere, so a city's lights
  under a forest, fires or lightning can light far trees from below as the sun does from
  above; the impostor's lighting needs those lights added as further terms.
- **Shadows by distance**: own shading of far trees comes from the baked visibility; the shadow
  map then has to skip a tree's own crown (here 2 radii past the first hit, which also skips a
  neighbour that close; a tree id in the map would not). Then impostor casters in far cascades
  refreshed every few seconds, past them a march toward the sun through the woods field's canopy
  density (as cloud shadows), and sky occlusion under woods baked once.
- **Wind**: no sway yet. Trunk, limbs, twigs and leaf flutter in a hierarchy, driven by the gust
  field the grass uses; fronds bend along their strips.
- **Falling leaves** as particles spawned from crown leaf positions at the drop rate; litter is a
  solid, too saturated carpet that should decay and drift.
- **Spruce up close** looks papery: sprays too large and solid. Finer needles, smaller cards,
  more layers near.
- **Pine needles up close** are flat star cards; needle-bundle strips within a few metres.
- **Narrow-leaved species sparse from afar** (willow, eucalyptus): lanceolate leaves cover little,
  so the crown reads bare at 30 m. More leaves per twiglet, or cover kept by the thinning above.
- **Trunks too thin for their age** on self-organising species (oak at 30 years): the pipe model
  alone sets girth; a basal-area rule by age would. Oak's calibration hits the vigour bound (5).
- **Bark by age**: furrows stretch where a thin stem wraps the tile once; young oaks should be
  smooth and furrow with age. Relief is shading only: trunk silhouettes, root flare and branch
  collars (limb tubes just intersect) want geometry near.
- **Oak autumn** stops at yellow; it should go russet. Palettes progress too slowly past the
  first colour.
- **Leaf undersides** paler and matte, per-species wax sheen; flowers, catkins, cones, coconuts.
- **Beech and larch**: beech's crown is thin and open where it should be dense, larch's sprays
  too thin to read whole.
- **Into the engine**: growth to `libs/flora` in C++ with golden tests (same seed, same tree on
  every platform), the species files read as they are, then the persistent plant store
  (`docs/woods.md` step 5) and profiling every LOD step on the 860M.
