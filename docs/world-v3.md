# World v3: baked, streamed, drawn as meshes

Nov 16, 2026

The world is generated ahead of play into octrees, streamed to clients nearest first, and drawn
by the client as ordinary game meshes. Voxels on a cube-sphere were scaffolding to get the
generator making more than rock; the game ends up shaped like a regular MMO. The generator is
what's kept. What changes is what it fills, how that travels, and how it's drawn.

Labels below: **decided** constrains code, **leaning** is the current proposal, **open** needs an
answer before its step starts.

## Where it stands

Decided:

- The engine ends up a regular MMO: meshes and ordinary distance LOD on the client. Voxels are
  not the shipped representation of what's drawn.
- The generator bakes into octrees, and octrees are what's stored and streamed: they're far
  smaller than meshes to send.
- The client decides how to draw. The server never meshes.
- The game server stays light: it reads the store and streams. While the generator is still
  changing, the server fills what the store lacks through the generator library and keeps it;
  once the generator settles the world is fully baked ahead and the server never generates.
- Streaming goes nearest first on connect, then fills in outward from the player.
- The 3D world drops the cube-sphere for a plain grid: x, y, z from the planet's centre.
- One tree per kind of data over the whole planet. A coarse node is one not yet split; adding
  detail splits it. There are no separate trees per level of detail.
- Water tops are flat at their body's known level; river water moves the way it runs, with the
  wind on top.
- Water reacts to changes live and locally: a change at a water edge wakes a local flow that runs
  until it settles back into bodies. Player changes first; changes from the world itself later,
  through the same path.
- Tests check data. Visual and whole-program tests wait until the engine nears stable.

Leaning:

- Several trees sharing one key space, one per kind of data (below).
- Smooth surfaces meshed from the shape tree's planes (dual contouring, `qef.hpp`).
  A plain grid's axes only line up with "up" in six places, so blocks would stair-step almost
  everywhere.
- Trees cut into pages of 64³ finest nodes (subtrees) for disk and the wire, keyed by depth and grid
  position. Pages are where a tree is cut, not edges in the world.
- Edits as a log over the baked trees, with snapshots and rollback (below).

Open:

- Seam scheme between levels of detail: Transvoxel-style transition cells, or skirts.
- Sea run-up on the shore: a water-surface shader effect, or dropped.
- `river_size` on the working planet: keep 100, or back to 1 until rivers are joined.

## One grid, many trees

The planet sits in one root node centred on its middle: the smallest that halves down to the
finest size and holds the planet and its sky. For the working world (28.8 km radius and 3 km of
sky, a third of a metre) that's about 87 km across and 18 halvings deep. Nothing is cut ahead: a
node splits only where what's under it differs, so open sea, sky and deep rock each stay one large
node, and detail goes only where it's needed. Pages group the nodes that exist for disk and the
wire, under one node and down six levels at most; where the tree stops early there's no page
under it. Each node sums up what's under it (shape as the plane through its
eight, material and water by most), so a node is its own coarse view, and a change under it updates
only the nodes above it. Neighbours are x ± 1: no face edges, no radial layers, no
tapered boxes. "Up" and gravity are the direction from the centre, worked out from position,
never from a grid axis.

The 2D maps of the surface (weather, climate, formation's sphere) stay what they are. Only the 3D
world moves to the grid.

Every tree shares the root node and its keys, so reading several at one place is the same key in
each, and walking them together is descending them side by side (as OpenVDB keeps many grids over
one transform, and compressed voxel DAGs keep shape apart from attributes). Each tree collapses
where it alone is uniform, stops at the depth it needs, and is versioned and streamed on its own
rhythm.

| Tree | Holds | Why apart |
| --- | --- | --- |
| Shape | the surface through each node as a plane, or all solid or all open | meshing and collision; streamed first |
| Material | an index into the world's short list of materials | texturing; broad strata collapse |
| Water | which water body a node's basin belongs to, or loose water's amount | exact levels, flow; changes with the seasons |
| Edits | changes players make, over the baked trees | later; kept off the main path |

Not trees:

- **Water bodies**: a small table. The sea and each lake have one level, a river its course with
  the level and flow along it, each with how it rises through the year. The water tree marks a
  body's whole basin; the level says how full it is, so a lake rising or draining changes one
  number, not the tree.
- **Heat**: worked out from the planet's baseline (`baseline_temperature`), with sparse
  deviations only where something is hot.
- **Weather**: its own system and grid, as now.
- **Rocks, crags, boulders, plants, buildings**: object instances in a spatial index, as in any
  MMO; not voxels.

## Baking

The generator answers what's at any 3D point and resolution, so it can fill any node at any
depth directly; coarse nodes agree with the fine ones under them. A full fine bake isn't
practical while the generator is changing: scaled from the store's Nov 12 measurements (113 s
on 8 threads and ~35 MB for a 1.6 km² coastal patch at 0.5 m), the working planet is roughly
200 hours and 230 GB at 0.5 m, and about twice that at 0.33 m. Mostly sea could make it 2–3×
less; rivers added since could make it more. So, for now:

- Coarse levels are baked for the whole planet ahead of play. They're few, the same for every
  player, and the part that's slow live: in the Nov 10 first-view benchmark, levels 7–13 took
  ~66 s of 73 s for ~330 tiles, while fine levels round the player took ~0.6 s each.
- The formed world is baked too, so the generator library starts without forming (~12.6 s).
- Fine detail is filled when a player nears, by the generator library in the server on worker
  threads, and written to the store so it's made once.
- Everything stored carries the generator version that made it; the version is bumped whenever
  the generator's output changes, since `world_id` covers only the settings. On a mismatch the
  server generates live and rebakes coarse in the background. Dev runs use a scratch store, so a
  forgotten bump can't serve stale ground.

Once the generator settles, the world is fully baked ahead, the baker becomes its own process,
and the server only reads. The code path is the same either way: the store holds what was baked,
the generator fills what wasn't.

## Editing while playing

Edits are a log over the baked trees, so the bake and play never fight.

- Each edit has an id, who made it, when, and the generator version it was made against. It's
  kept as the nodes it leaves, so it can be checked against the ground under it.
- Edits copy only the nodes they change (the trees share identical nodes), so a snapshot is a kept
  root and rollback is pointing back at one. Old nodes no kept snapshot holds are swept up; how
  many snapshots to keep is a setting.
- Removing one edit from the middle puts its nodes back if nothing later touched them, and
  otherwise replays the log without it from the snapshot before it.
- Edits are written to the store in one batch per server tick (LMDB has one writer); each keeps
  its own id inside the batch.
- An edit touching several trees (shape and material) commits them together under one version.
  The nodes above it re-sum; far clients get an update only if a node's bytes change.
- Changing the generator after a live world has edits is a migration, run on purpose, never a
  casual rebake.

### Water against edits

- An edit that changes shape next to water, or opens its containment, is found by looking up the
  water tree at the same keys.
- That wakes a live flow over a small patch round the change: water held there as amounts, moving
  much like Minecraft's, until it settles; then it's turned back into bodies with flat levels and
  the patch stops.
- A breached lake loses what flows out through the patch, and its level drops over time; the rest
  of the lake is just the lower number. The sea never runs short: a breach into a dry hollow below
  sea level fills it and joins it to the sea. A trench off a river fills to the river's level
  where they meet; moving a river's course waits.
- The water changes an edit causes are logged under its id, so rolling the edit back rolls them
  back too.
- Changes from the world itself (rockfall, seasons, erosion) later use the same path; player
  changes come first.

## Streaming

On connect the client gets coarse pages for all it can see first (they're cheap), then fine
pages nearest first. After that the server refines outward by what improves the view most per
byte, within a bandwidth budget. It remembers which version of each tree's page each client
holds, and sends only what's newer: a season's water, an edit. The client asks for nothing it
can coarsen itself.

## Drawing

The client keeps the pages it's been sent in one tree, with a hash per level for neighbours and
the finest page over a point. Per page it picks a depth by distance (fine near the camera,
halving outward), meshes the shape tree at that depth into a smooth surface, textures it by the
material tree, blending each node's material into its neighbours' by distance so nature shows no
borders (hard edges only where the data has them: rock out of soil, or a player's sidewalk), and caches the mesh until a version or the depth changes. Where depths meet, a seam
scheme closes the gap and the swap morphs or fades, so nothing pops.

Water is drawn from its bodies, not voxel amounts: the sea and lakes as a surface at their level
where their water tree says water is, rivers as ribbons along their course with their flow for
the shader to move along, the wind's ripples on top.

## Steps

Each step comes with tests on data, and none starts before the one it needs is done. Where a
step replaces something, the old and new are compared before the old goes.

### 0 · Measure and decide

1. **Split trees or one**: count nodes on real baked tiles held as one `Cube` tree against shape,
   material and water trees apart. Decides the leaning above. Measured on today's tiles (small
   world, 64 places, four depths): apart costs 1.4–1.7× one tree while water holds amounts, which
   trace the ground; water as only a body id is ~500 nodes against ~160 000, and then apart is
   ~1.03× the nodes and ~5% fewer bytes. Holds with water as bodies.
2. **Meshing cost now**: a hidden benchmark of `mesh_tile` on real tiles (block fill, column
   steps, faces), as the baseline the new mesher must beat. Measured: 25–38 ms a 64³ tile on one
   core, 7 500–12 400 quads, four vertices a quad, none shared.
3. Confirm page size from step 1's counts.

### 1 · Grid keys and trees

1. Root node, node and page keys (depth, x, y, z), Morton order, parent, children and
   neighbours. Tests: round trips, neighbours across page cuts, the key of any point.
2. Shape, material and water trees (`grid::Tree`, one value a node: the surface's plane, a
   16-bit material index, a water body id), with node summaries and versions; a page record holding its
   trees; encode and decode. Tests: round trips, identical nodes give identical bytes, a split
   leaf's summary matches its children. Loose water's amount waits for live water.
   Shape leaves hold a plane (normal, distance, and the material the surface shows) so flat
   and straight ground stays coarse:
   eight merge where one plane runs within a quarter of a finest node of theirs. Measured on a
   ball 40 finest nodes round: within 0.4 of a finest node of the true surface. Curvature per
   leaf is an idea to try if gentle curves still split too much.
3. Water body table: sea, lake, river (course, level along it, flow, seasonal rise and peak),
   with ids stable across bakes. Tests: level and flow read at points along a river.
4. Heat as baseline plus deviations: the baseline stays the planet's (`baseline_temperature`),
   and only hot things are kept, as sources falling off to none at their reach. Today's only
   heat over the baseline is live (lava flow, vents), so nothing fills them until those move
   over. Tests: falloff, overlapping sources add, round trips.
5. Edits log: format only (id, author, time, generator version, the nodes left), unused until
   edits come back. Tests: an edit over a baked page reads through.

### 2 · Generator on the grid

1. Fill a node at any depth from the generator at grid points: shape plane, material, water
   body. Materials are a palette of derived mixes, each named and made of others (loam of sand,
   silt and clay), so one index says what digging gives; the generator steps through the mixes
   between neighbours rather than jumping. Tests: a page read against the generator at the
   same points. Done top down (`globe::fill`): each depth's nodes are probed together, and only
   those the surface may cross split. The generator is asked in batches of columns on its own
   cells, read between the four about each node: one point alone costs 0.73 ms (its column),
   a batched column 0.025 ms, a point along a made column 0.9 µs. A 32³ node on the ground
   fills in ~0.2 s on one core. Water marks only the sea's basin until 2.3.
2. Coarse agrees with fine: a node filled coarse equals the fine ones under it summed up, so
   detail filled later never jumps. Tests: every level of a small world. Measured from 1 m to
   2 km nodes: the coarse plane lies within 0.1 of a side of the fine ones' summary on average,
   0.76 at worst, and no node left closed coarse gains detail finer. Found on the way: the cave
   term held the distance at -50 m anywhere in solid, so coarse nodes deep in rock looked near
   a surface; caves now count only within their reach, and a node is left whole only where none
   shows at its detail, kept open where one may show finer. Materials and water bodies sum up
   as the most common but none (air, no body), so a coarse node shows what's in it. Still apart:
   - Caves showing only finer turn a few summaries' normals (cave roofs average in with the
     ground). Idea: sum planes up by how much of each child is solid, so a cave counts little.
   - Decided: shape leaves carry the material their surface shows, so ground looks alike at
     every depth; the material tree holds what digging gives, a coarse node the most common
     inside. Neither keeps eight from merging. What shows sums up as a blend: palette entries
     past the first 64 blend two of those by sixteenths, so a coarse node shows the mix of what
     the crossed children show, which the client blends as an overlay. Up to 8 m nodes they
     agree (shown under 0.25 unlike by shares, 23 of 26 held). Coarser, the generator itself
     disagrees: it lays cover by its slope smoothed to the detail asked, so ground bare and
     steep at 32 m and finer reads as gentle clay-covered ground at 128 m.
   - Tried: a coarse node blending what finer columns show at 4×4 points over its plane. Taken
     at the finest detail each point is a lone column (~0.7 ms), 12× the fill; taken two levels
     finer, 6×, and nodes of 128 m and over still differ by half their shares, as the finer
     columns are still smoothed and two entries hold too little of a km-wide node. Dropped.
     Decided: the blend is enough for now. Later, a bake can sum coarse nodes up from finer
     ones where it matters; idea: the generator gives the share of coarse ground steep enough
     to stay bare, so the fill blends cover and rock from one column.
3. Water bodies emitted by the generator: the sea, lakes from the formed lake fill, rivers from
   the valleys' courses with `river_size` applied. Tests: on the small world, every lake and
   the largest rivers come out as bodies with the right levels. Done: a column says which body
   its water is (`WaterBody`), and the water tree holds it over the ground up to the water's
   highest in the year: the sea 1, lakes after it by index, rivers by the point their main
   stem ends at with the top bit set. A river keeps its name down through each confluence from
   whichever brings the most water, until the sea or a lake takes it. Found on the way:
   - Most of the small world's rivers stand in no water at the default `river_size`; the
     test sizes them up. Creeks narrower or shallower than a node can wind between the nodes'
     middles, so they show only in places.
   - The voxels can hold a lake lower than the generator's column has it (36.5 m against
     60.85 m at one spot): the voxel fill's water and the column's disagree. Left for the
     store and baker, which read water from the bodies.
4. Objects out of voxels: crags, rockfall and boulders, vents, as instances. Tests: the same
   places and sizes as today. Done: a fill places them apart from the shape (`Placed`: a
   rounded box or an opening, keyed so the same object has the same key from any node):
   - Fallen rocks big enough for the finest detail filled; smaller ones stay rubble in the
     ground, as today.
   - Crags: the ground leaves them out, and each is its joint block's core, rotted in and
     rounded, lowered so its peak over the columns it stands on is today's. It is taken from
     the columns the fill already makes; reading the formed world apart for it cost 3.5 s a
     fill. Today a crag is cut flat where its height caps it; the placed block is rigid, so
     off its peak it can stand a little higher or lower.
   - Vents and pools, at their openings, however deep a pool's cavern lies. Their search runs
     over the scatter cells near a node rather than the whole planet.

### 3 · Store and baker

1. `libs/baked` on grid keys, per tree, with versions and the generator version; old store files
   refused by format version. Tests: put, get, the holder of any point, versions, a generator
   version mismatch refused. Done (`baked::PageStore`): each tree of each page is its own
   record, keyed by kind and its page's root, holding the generator version that made it and its
   own version, which steps on only when its bytes change. A store is of one world and grid and
   refuses others, and refuses the face store's files. Trees another generator version made stay
   held but read as stale, never as current, for 3.3 to fill again. The holder of a node is
   found by trying each page over it, finest first, at most four. The face store stays until
   `hog-bake` moves over in 3.2. Still to come: the water bodies table and placed objects.
2. `hog-bake`: coarse for the whole planet, fine round a place, and the formed world. Measure size
   and time on the working world and the small world. Done for the pages (`bake::bake`): from the
   top page down, a depth of pages at a time, each leading to the pages under its open leaves.
   Under a leaf open for the ground passing, every page goes in whose finest nodes are `--coarse`
   metres or wider; within `--across` of `--at` every page does, caves too, down to the finest.
   Pages are filled in batches across threads and each batch written together, so a stopped bake
   carries on; pages held as this generator makes them are read, not filled. Small world (12 km,
   0.5 m), 4 threads on a busy machine: coarse to 32 m (the top page and those rooted at depth 4)
   is 2327 pages in 424 s, 59 MB (shape 31, material 18, water 6); fine round a place 200 m
   across is 202 more pages in 6 s, 2 MB. A coarse page costs some 0.7 s of a thread, a fine one
   0.03 s. Found on the way: the top page's middle is the planet's centre, which has no up; and
   fallen rocks were scattered for nodes too coarse to hold any. Working world (28.8 km, 1/3 m):
   coarse to 16 m (the top page and those rooted at depth 6, feet 21 m) is 124994 pages in 74
   min on 4 busy threads, 939 MB (shape 464, material 290, water 110), some 0.09 s of a thread a
   page. Left: the
   formed world (most of `formation::World` would be stored, so it waits on measuring what reads
   it), and retiring the face store and `hog-baked-bench` (step 7). Ground open for caves near
   the surface is followed only round a place, so the coarse bake leaves it to be filled live.
3. Filling on demand: a queue of wanted nodes, the generator library filling it on worker threads
   and writing the store, the store telling readers. Tests: a node asked for appears and is read;
   a stale generator version is filled again. Done (`baked::Filler`): nodes are wanted from any
   thread; the owner pumps it (a server tick), which writes the fills finished in one batch, says
   which pages were written, and starts what each wanted node needs next. A node walks the pages
   over it from the top, stopping at the first not held whole as this generator makes it (filled
   on the workers, the node waiting on it), at the one holding its depth, or at one whose leaf over
   it isn't open. Pages are filled a depth at a time, never past what the page above says is
   there; two nodes needing one page share its fill. Stale pages fill again and keep their
   versions where the bytes come out the same. Since 4.2 a page is filled only as deep as the
   nodes asked for under it reach, and deeper once one reaches further.
4. Scratch stores for dev runs. Done (`PageStore::scratch`): a store in a new directory of its
   own under the system's temporary one, removed once the store closes. The server picks it up in
   step 4.
5. Later, once the generator settles: a full bake ahead, and the baker as its own process.

### 4 · Server streaming

1. The server reads the store per tree, and fills only what it lacks through step 3.3. Tests: a
   server on a fully baked small world serves a page with no generator running. Done
   (`server::PageSource`): pages held whole as this generator makes them are read from the store;
   one missing is asked of the filler where there is one, and read once a pump has written it.
   Without a filler the store is all there is. The filler moved into `libs/baked` so the server
   links it without the bake tool. The server app opens a store with 4.4.
2. Interest and order: coarse in view first, then nearest fine, then outward refinement within a
   byte budget per client. Tests: order of pages sent for a player standing still. Done
   (`server::PageSending`): from the top page down, under each open leaf of a page wanted whole,
   the pages whose root node spans more than 8 pixels from the player (as `TileSending` judged
   voxels); ground open for caves only within the finest nodes' reach. Decided: a page goes only
   as deep as its nodes should be seen from the player, cut with `Tree::trimmed`, and deeper as
   the player nears; the filler likewise fills a page only as deep as asked and deepens it later.
   Whole six-level pages were far too much: one player on the small world wanted 57k fine pages
   (~400 MB, ~30 min to fill). Cut to depth, the same 59k go 1 level (727), 2 (30k) or 3+ (29k)
   deep. Order: by depth, then nearest first; `next` sends in order within a byte budget, one
   page at least. A leaf above a page's foot is open only in case the ground lies nearer than its
   distance says, so it's followed only near, as caves are: followed like the ground, it made
   2.1 M pages (174 MB, 2¼ h to fill) for one player, nearly all air. Measured (hidden
   `[.bench]` test): a player arriving on an empty small world store is sent 15304 pages, 16.6
   MB, in 302 s of filling on 4 threads (mostly the 2326 coarse pages a coarse bake makes ahead);
   the four follows took 0.07 s together.
3. What each client holds, by page and tree version; only newer sent. Tests: a water season
   change sends only water trees. Done: each player's holding of a page is its levels and each tree's version.
   A page deeper than held goes whole; as deep, only its trees with newer versions go, and
   nothing if none is. `changed(pages)` puts pages written since (an edit, a season's water) first
   in line for every player holding them. A player never tells the server what it let go of yet;
   that comes with the client's memory budget (5.1).
4. Protocol messages for pages, bodies and version bumps; protocol version bumped. Tests:
   round trips, refusal of a cut message. Done (protocol 41): `Pages` carries pages as
   `grid::Page::encode` codes them, only the trees the client didn't hold as they are now;
   `Bodies` carries water bodies as `grid::WaterBodies::encode` codes them, for when a bodies
   table is made (none is yet). No version bump message: a changed tree is sent in place, first in
   line. The server streams pages where its config has a `[pages]` table (`store = "..."` or
   `scratch = true`): it pumps the filler each tick, follows a player where it moved, or once a
   second while pages were filled, and sends within the queue room tiles use. Tiles still go
   alongside until the client draws pages (step 5).
5. Bot and load tests streaming the new way. Done for the bot: the client session takes `Pages` and `Bodies` in, and a bot
   decodes the pages sent it and counts them (`hog-bot --run-for` logs each bot's at the end); a
   test has a bot sent pages, the top page first. Found on the way: the filler started every fill
   at once, and the pool's queue, once full, ran them on the pumping thread (one pump took 69 s);
   now a few per thread run, the rest wait their turn first asked first, and a node waiting on a
   page is gone over again only once that page is written. The bot still stands on tiles until
   the client's physics reads the shape tree (5.9). Load numbers with many bots wait on that too.

### 5 · Client

1. One held tree: pages per level in hashes, finest-over lookups, a memory budget and
   eviction farthest first. Tests: lookups and eviction on held data. Done (`client::HeldPages`):
   pages by their root's depth in hash maps; a page deeper than held replaces it, one as deep
   replaces only the trees it carries; `at<Kind>(node)` answers from the finest page over the node
   holding that kind, as deep as it's held, saying which page. `evict` lets go of the farthest
   pages from the eye until under a byte budget, never the top page, and gives their roots for
   telling the server once that message exists. Water bodies sent are held by id.
2. Depth by distance per page. Tests: depth chosen for a camera path. Done
   (`client::drawn_pages`, `grid::View`): the pixel rule is shared with the server, so a page is
   drawn as deep as its nodes still span more than the pixels allowed from the eye, never deeper
   than held. Where a page is drawn whole, the held pages under its open leaves that the view
   wants are drawn in place of those leaves and listed under it; the rest of the page is drawn at
   its own depth.
3. Smooth mesher over the shape tree at a depth, material weights per vertex. Tests: closed
   surfaces on flat, slope, overhang and cave regions; vertices on the surface within tolerance.
   Done (`client::mesh_shape`): adaptive dual contouring over a page's leaves, drawn no wider
   than four nodes of the drawn depth. Each vertex sits on its leaf's plane, nearest the crossings
   around it, and shows its leaf's blend of two materials. A corner's side comes only from the
   page's own leaves: solid where it touches solid and nothing open, and the other way round;
   otherwise from the smallest planes over it. So a page's mesh depends on its tree and depth
   alone. Edges touching the page's sides make no polygons; they're left for the seams.
4. Seams between depths. Tests: no gaps along every shared edge of mixed-depth neighbours.
   Done (`client::mesh_seam`, `client::ShapesDrawn`): a page's leaves split round the pages drawn
   under it, and its mesh leaves out every edge touching its sides or theirs. Its seam draws those
   edges inside it, and those on its high sides, each edge on a side drawn by the place holding
   the lowest of the four leaves round it, over the drawn leaves of both sides together. A corner
   on a side reads the leaves either side; one inside reads the page's alone, as its mesh does.
   Vertices are the meshes' own, from each page's border, so seams meet the meshes exactly. A
   page's mesh never changes for its neighbours; its seam is meshed again when a page touching
   it changes (`ShapesDrawn::touching`). Tests: a cave across eight pages at mixed depths, and
   round finer pages drawn in a coarse one, closes with no gap and nothing drawn twice. Where
   pages at three depths meet at a corner, a leaf may join two sheets at its vertex.
5. Morph or fade between depths. Tests: vertex positions at the start and end of a swap.
6. Mesh cache and jobs: meshed once, kept until a version or depth changes, nearest first.
7. Water surfaces from bodies: level surfaces clipped to the water tree, river ribbons with flow,
   the water shader moving along the flow with wind on top. Tests: tops at the body's level;
   flow along the course.
8. Grass, plant cover and anything else placed on the ground read the new surface.
9. Physics and the player's footing on the shape tree or collision meshes; the bot too. Tests:
   standing, walking a slope, falling into water.
10. Rays: picking, the sun ray and sight lines through the held tree.

### 6 · Port what else reads the 3D world

Each either moves to the grid trees or is parked with a note saying so.

1. `libs/flow`: creep, glacier, lava, runoff, slump, soil, support, wear, vents and their
   regions (all on cube-sphere sites and columns).
2. `libs/server`: `HeldGround` (stores columns: replaced by the store and edits overlay),
   `ground_wetting`, `ice_bake`, `tile_sending` (replaced by step 4).
3. `libs/physics` ground, `apps/bot`, `libs/skies` weather keeper's ground reads, `libs/store`'s
   SQLite volume archive (retired).
4. Tools: `hog-bake` (step 3), `hog-build-world` (retired), `hog-stills` and `hog-census` on the
   new trees, `hog-formation` unchanged.
5. The player record's place in the store's players table: still latitude, longitude, height.

### 7 · Retire the cube-sphere 3D world

Only once steps 5 and 6 are at parity: `Layers` for 3D, voxel tiles and `TileKey`'s face form,
`HeldTiles`, `voxel_mesh` and the water `Sheet`, `Volume`, `TileColumn` and the volume archive,
bricks, `Generator::columns` and `generate_stack`, and the shape tree plan's own removal step
([octree-generation.md](octree-generation.md), step 8). Docs that describe them are marked
superseded.

### 8 · After parity

- Edits on the log ([above](#editing-while-playing)): batching per tick, snapshots, rollback,
  removing one edit, sent as version bumps. Tests: rollback restores a snapshot's bytes; removing
  a middle edit equals the log replayed without it.
- Live local water from edits: detection at water edges, the patch flow, settling into bodies,
  lake levels drained through a breach. Tests: a breached lake's level falls by what flowed out; a
  hollow opened below sea level fills; rolling back the edit restores the water.
- Changes from the world itself through the same water path.
- Hand subtrees to separate servers, for the big world.
- A whole-program smoke test once the engine nears stable.

## Carried over

Open from the river work, to fold in where they fit:

- About one in seven of the largest rivers runs dry, perched over ground formed lower than its
  course; the generator needs rivers to follow or fill that ground (step 2.3).
- Rivers still small: joining catchments and melt from frozen peaks; the small world lost its
  ice cap.
- The sea's run-up over the shore (open question above).

## Relation to the other plans

- [The globe](globe.md): its generator and rules stay; its voxel shell, tiles and meshing are
  replaced here.
- [The shape tree](octree-generation.md): forming's primitives and node summaries can be what
  bakes pages quickly; its wire, client and removal steps are this plan's steps 4, 5 and 7.
- [Planet layers](planet-layers.md): the formation backlog and hero features are unchanged.
