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
- Smooth surfaces meshed from the shape tree's amounts (surface nets or dual contouring, `qef.hpp`).
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

The planet sits in one root cube centred on its middle, about 64 km across for the working
world (28.8 km radius and 3 km of sky). At a third of a metre that's 18 halvings deep; cut into
64³ pages, 12 levels of pages. Empty sky and the deep interior are each a single uniform node, so
only the surface shell costs anything. Each node sums up what's under it (shape averaged,
material and water by most), so a node is its own coarse view, and a change under it updates
only the nodes above it. Neighbours are x ± 1: no face edges, no radial layers, no
tapered boxes. "Up" and gravity are the direction from the centre, worked out from position,
never from a grid axis.

The 2D maps of the surface (weather, climate, formation's sphere) stay what they are. Only the 3D
world moves to the grid.

Every tree shares the root cube and its keys, so reading several at one place is the same key in
each, and walking them together is descending them side by side (as OpenVDB keeps many grids over
one transform, and compressed voxel DAGs keep shape apart from attributes). Each tree collapses
where it alone is uniform, stops at the depth it needs, and is versioned and streamed on its own
rhythm.

| Tree | Holds | Why apart |
| --- | --- | --- |
| Shape | how much of each node is solid | meshing and collision; streamed first |
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
material tree, and caches the mesh until a version or the depth changes. Where depths meet, a seam
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

1. Root cube, node and page keys (depth, x, y, z), Morton order, parent, children and
   neighbours. Tests: round trips, neighbours across page cuts, the key of any point.
2. Shape, material and water trees, each a `VoxelTree` or a leaner kind, with node summaries and
   versions; a page record holding its trees; encode and decode. Tests: round trips, identical
   nodes give identical bytes, a split leaf's summary matches its children.
3. Water body table: sea, lake, river (course, level along it, flow, seasonal rise and peak),
   with ids stable across bakes. Tests: level and flow read at points along a river.
4. Heat as baseline plus deviations. Tests: matches today's temperatures within a step.
5. Edits log: format only (id, author, time, generator version, the nodes left), unused until
   edits come back. Tests: an edit over a baked page reads through.

### 2 · Generator on the grid

1. Fill a node at any depth from the generator at grid points: shape amount, material, water
   body. Tests: a page read against the generator at the same points.
2. Coarse agrees with fine: a node filled coarse equals the fine ones under it summed up, so
   detail filled later never jumps. Tests: every level of a small world.
3. Water bodies emitted by the generator: the sea, lakes from the formed lake fill, rivers from
   the valleys' courses with `river_size` applied. Tests: on the small world, every lake and
   the largest rivers come out as bodies with the right levels.
4. Objects out of voxels: crags, rockfall and boulders, vents, as instances. Tests: the same
   places and sizes as today.

### 3 · Store and baker

1. `libs/baked` on grid keys, per tree, with versions and the generator version; old store files
   refused by format version. Tests: put, get, the holder of any point, versions, a generator
   version mismatch refused.
2. `hog-bake`: coarse for the whole planet, fine round a place, and the formed world. Measure size
   and time on the working world and the small world.
3. Filling on demand: a queue of wanted nodes, the generator library filling it on worker threads
   and writing the store, the store telling readers. Tests: a node asked for appears and is read;
   a stale generator version is filled again.
4. Scratch stores for dev runs.
5. Later, once the generator settles: a full bake ahead, and the baker as its own process.

### 4 · Server streaming

1. The server reads the store per tree, and fills only what it lacks through step 3.3. Tests: a
   server on a fully baked small world serves a page with no generator running.
2. Interest and order: coarse in view first, then nearest fine, then outward refinement within a
   byte budget per client. Tests: order of pages sent for a player standing still.
3. What each client holds, by page and tree version; only newer sent. Tests: a water season
   change sends only water trees.
4. Protocol messages for pages, bodies and version bumps; protocol version bumped. Tests:
   round trips, refusal of a cut message.
5. Bot and load tests streaming the new way.

### 5 · Client

1. One held tree: pages per level in hashes, finest-over lookups, a memory budget and
   eviction farthest first. Tests: lookups and eviction on held data.
2. Depth by distance per page. Tests: depth chosen for a camera path.
3. Smooth mesher over the shape tree at a depth, material weights per vertex. Tests: closed
   surfaces on flat, slope, overhang and cave regions; vertices on the surface within tolerance.
4. Seams between depths. Tests: no gaps along every shared edge of mixed-depth neighbours.
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
