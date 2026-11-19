# The tree, stored and sent without tiles

The world's ground is one octree per kind (shape, material, water, woods) over the grid: every
node holds a correct summary of what's under it at its own size, identical nodes are shared, and
the generator fills any node to any depth. That stays. What goes is cutting it into fixed pages
every six levels and making that cut the unit of everything: storing, filling, asking and
sending. Fixed pages tied detail to where pages happen to end, turned distant ground into tens of
thousands of tiny requests and fills, and refilled a page each time it was wanted deeper.

Here nothing about the cut is visible to anyone using the tree, and nothing depends on a fixed
size: any grid depth, any finest size, any detail asked for.

## Decided

- **Ask for a node to a depth.** Any node, any depth down to the finest. The answer is a
  **piece**: that node's subtree for each kind, cut at that depth. Leaves keep saying whether
  more detail lies under them. Pieces are what goes over the wire and what a client merges into
  the one tree it holds.
- **Asks are sized by the view, not the store.** A client asks for regions whose size grows
  with distance, each to the depth its distance wants, so an ask carries a few thousand nodes
  wherever it is, near or far. A distant kilometre is one ask, not thousands.
- **Storage is grouped by content, not depth.** Depths are taken in **bands** of a few levels.
  A **chunk** holds the nodes of one band under one node, its root, which lies at or above the
  band's top. A chunk is split into its root's eight children only where it would go past a byte
  budget. Shared nodes make uniform ground tiny: open sea, flat plains and solid rock stay one
  small chunk over kilometres even at the finest band, and rugged ground splits finer. A chunk
  is one record, keyed by kind, band and root in Z order, so the chunk holding any node is one
  seek.
- **Reading a piece reads only the bands it reaches.** A distant ask reads coarse bands, whose
  chunks are wide, and never touches the finer ones under them. That's what grouping by bands
  buys over grouping whole subtrees.
- **Filling goes chunk by chunk, top down, at the rank asked.** A missing chunk is filled from
  its root down to its band's foot, keeping the band's nodes; the bands above come out the same
  because generation is deterministic. A chunk is written, then split where it's over budget.
  Fills keep the writer thread and the ranking they have now.
- **Every chunk has a version** that steps whenever its bytes change. A piece kept for the next
  asker records the chunks it was read from, and their versions. Each asking checks those
  versions without decoding anything, so any writer is caught: the filler, a bake in another
  process, edits. Writes the server makes itself let go of what they touch at once. A byte
  budget lets go the least lately asked for.
- **Edits rewrite one chunk per band.** Changing the finest nodes changes the summaries up the
  tree, so an edit rewrites the chunk holding the changed nodes in each band above it. Their
  versions step, and clients holding pieces over them are sent what changed.
- **Clients hold one tree per kind,** merging pieces as they come. Their level of detail is how
  deep they read it. Meshing reads nodes, never pieces or chunks.
- **The band height and the chunk budget only tune storage.** They change no answer. Tests
  check that pieces read from chunks match the generator's tree directly, for several of each.

## Steps

1. **Pieces.** A piece is a node and its trees cut at a depth, coded like a page is now but
   rooted at any node; grafting a piece into a held tree and cutting one out of it. Tests:
   graft then cut gives the piece back; grafting a deeper piece over a shallower one replaces
   only its subtree; summaries above stay consistent.
2. **Chunk store.** Bands, chunks keyed by kind, band and root, split by budget, found by one
   seek, versioned; reading any piece from them. Tests: a piece read from chunks matches the
   tree the generator fills directly, over flat and rugged ground, for several band heights and
   budgets; flat ground takes few chunks; versions step only on change.
3. **Filling chunks.** On demand, ranked, written on the writer thread; the bake moves onto
   chunks, depth first in bounded memory, to the detail asked for. Tests: what's filled matches
   the generator; asking deeper grows what's held without refilling the bands above.
4. **The server answers asks with pieces.** Kept pieces checked by their chunks' versions;
   changes sent to the clients holding them. Tests: answers match the store; a chunk written
   lets go of what was read from it; a client is sent a changed piece once.
5. **The client asks by region and holds one tree.** Regions sized by distance, the depth the
   view wants, what's ahead first; meshing reads the merged tree. Tests: asks cover what the
   view needs with no overlap; detail arrives coarse first; turning re-ranks.
6. **Pages retire:** the page store, page filling, page sending and held pages, and the page
   cut in the grid.

Steps 1 to 3 don't touch what runs now. Steps 4 and 5 switch the server and the client over
together, and the store is rebuilt by baking.

## Open

- The band height and the chunk budget, from counts on the working world: probably three levels
  and some tens of kilobytes.
- Whether chunks well under budget are ever joined back into one under their parent, or a bake
  just fills at a sensible root to begin with.
- Whether identical pieces travel once, by hash, for clients far apart seeing the same sea.
