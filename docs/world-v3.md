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
- The game server stays light: it reads the baked store and streams. Generating detail is not
  its job.
- Streaming goes nearest first on connect, then fills in outward from the player.
- The 3D world drops the cube-sphere for a plain grid: x, y, z from the planet's centre.
- Water tops are flat at their body's known level; river water moves the way it runs, with the
  wind on top.
- Tests check data. Visual and whole-program tests wait until the engine nears stable.

Leaning:

- Several octrees sharing one key space, one per kind of data (below).
- Smooth surfaces meshed from the voxels' amounts (surface nets or dual contouring, `qef.hpp`).
  A plain grid's axes only line up with "up" in six places, so blocks would stair-step almost
  everywhere.
- Regions of 64³ voxels, each a `VoxelTree`, keyed by level and grid position.
- A baker process apart from the game server, baking coarse everywhere ahead and fine detail on
  demand, kept once baked.

Open:

- Baker as its own process from the start, or a library the server calls until it moves out.
- Seam scheme between levels of detail: Transvoxel-style transition cells, or skirts.
- Sea run-up on the shore: a water-surface shader effect, or dropped.
- `river_size` on the working planet: keep 100, or back to 1 until rivers are joined.

## One grid, many trees

The planet sits in one root cube centred on its middle, about 64 km across for the working
world (28.8 km radius and 3 km of sky). At a third of a metre that's 18 halvings deep; with 64³
regions, 12 levels of regions. Empty sky and the deep interior are each a single uniform node, so
only the surface shell costs anything. Neighbours are x ± 1: no face edges, no radial layers, no
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
| Shape | how much of each voxel is solid | meshing and collision; streamed first |
| Material | an index into the world's short list of materials | texturing; broad strata collapse |
| Water | which water body a voxel is in | exact levels, flow; changes with the seasons |
| Edits | changes players make, over the baked trees | later; kept off the main path |

Not trees:

- **Water bodies**: a small table. The sea and each lake have one level, a river its course with
  the level and flow along it, each with how it rises through the year.
- **Heat**: worked out from the planet's baseline (`baseline_temperature`), with sparse
  deviations only where something is hot.
- **Weather**: its own system and grid, as now.
- **Rocks, crags, boulders, plants, buildings**: object instances in a spatial index, as in any
  MMO; not voxels.

## Baking

The generator answers what's at any 3D point and resolution, so it can fill any region at any
level directly; coarse regions agree with the fine ones under them. A full fine bake isn't
practical: the store's measurements put 0.5 m at about 22 MB a km², which is roughly 230 GB and
200 hours on 8 threads for the working planet. So:

- Coarse levels are baked for the whole planet ahead of play.
- Fine levels are baked ahead round the spawn, and elsewhere on demand when a player nears,
  then kept.
- The baker writes the store (`libs/baked`, LMDB); the game server only reads it, and asks the
  baker for regions it lacks.

## Streaming

On connect the client gets coarse regions for all it can see first (they're cheap), then fine
regions nearest first. After that the server refines outward by what improves the view most per
byte, within a bandwidth budget. It remembers which version of each tree's region each client
holds, and sends only what's newer: a season's water, an edit. The client asks for nothing it
can coarsen itself.

## Drawing

The client keeps the regions it's been sent in one tree, with a hash per level for neighbours and
the finest region over a point. Per region it picks a depth by distance (fine near the camera,
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
   material and water trees apart. Decides the leaning above.
2. **Meshing cost now**: a hidden benchmark of `mesh_tile` on real tiles (block fill, column
   steps, faces), as the baseline the new mesher must beat.
3. Answer the open questions above that step 1 of the next stage needs (region size, baker
   form).

### 1 · Grid keys and trees

1. Root cube, region keys (level, x, y, z), Morton order, parent, children and neighbours.
   Tests: round trips, neighbours across region edges, the key of any point.
2. Shape, material and water trees per region, each a `VoxelTree` or a leaner kind, with
   versions; a region record holding its trees; encode and decode. Tests: round trips,
   identical voxels give identical bytes.
3. Water body table: sea, lake, river (course, level along it, flow, seasonal rise and peak),
   with ids stable across bakes. Tests: level and flow read at points along a river.
4. Heat as baseline plus deviations. Tests: matches today's voxel temperatures within a step.
5. Edits overlay: format only, unused until edits come back. Tests: an edit over a baked region
   reads through.

### 2 · Generator on the grid

1. Fill a region at any level from the generator at grid points: shape amount, material, water
   body. Tests: a region read against the generator at the same points.
2. Coarse from fine: a coarse region equals the fine ones under it combined (shape averaged,
   material and water by most). Tests: every level of a small world.
3. Water bodies emitted by the generator: the sea, lakes from the formed lake fill, rivers from
   the valleys' courses with `river_size` applied. Tests: on the small world, every lake and
   the largest rivers come out as bodies with the right levels.
4. Objects out of voxels: crags, rockfall and boulders, vents, as instances. Tests: the same
   places and sizes as today.

### 3 · Store and baker

1. `libs/baked` on grid keys, per tree, with versions; old store files refused by format
   version. Tests: put, get, the holder of any point, versions.
2. `hog-bake`: coarse for the whole planet, fine round a place. Measure size and time on the
   working world and the small world.
3. Baking on demand: a queue of wanted regions, the baker filling it and the store telling
   readers. Tests: a region asked for appears and is read.
4. The baker as its own process (or library, per step 0.3).

### 4 · Server streaming

1. The server reads the store per tree and never generates. Tests: a server on a baked small
   world serves a region with no generator running.
2. Interest and order: coarse in view first, then nearest fine, then outward refinement within a
   byte budget per client. Tests: order of regions sent for a player standing still.
3. What each client holds, by region and tree version; only newer sent. Tests: a water season
   change sends only water trees.
4. Protocol messages for regions, bodies and version bumps; protocol version bumped. Tests:
   round trips, refusal of a cut message.
5. Bot and load tests streaming the new way.

### 5 · Client

1. One held tree: regions per level in hashes, finest-over lookups, a memory budget and
   eviction farthest first. Tests: lookups and eviction on held data.
2. Depth by distance per region. Tests: depth chosen for a camera path.
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

- Edits on the overlay tree, sent as version bumps.
- Hand regions to separate servers by subtree, for the big world.
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
  bakes regions quickly; its wire, client and removal steps are this plan's steps 4, 5 and 7.
- [Planet layers](planet-layers.md): the formation backlog and hero features are unchanged.
