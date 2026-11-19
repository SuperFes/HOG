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

- **A node holds its own value.** Each node keeps what the generator gives at its own size,
  whether it splits or not, rather than the summary of whatever happens to be filled under it.
  So a node reads the same however deep the tree around it is filled and from whichever node
  over it the fill began, any cut of the tree agrees with any other, and a coarse node never
  changes as detail is added under it: filling finer never rewrites coarser chunks. Edits are
  the one thing that change a node, and they recompute what's over them.
- **Ask for a node to a depth.** Any node, any depth down to the finest. The answer is a
  **piece**: that node's subtree for each kind, cut at that depth. Leaves keep saying whether
  more detail lies under them. Pieces are what goes over the wire and what a client merges into
  the one tree it holds.
- **Asks are sized by the view, not the store.** A client asks for regions whose size grows
  with distance, each to the depth its distance wants, so an ask carries a few thousand nodes
  wherever it is, near or far. A distant kilometre is one ask, not thousands.
- **Storage is grouped by content, not depth.** Depths are taken in **bands** of a few levels
  from the root. A **chunk** holds the nodes of one band under one node, its root, which lies no
  deeper than the band above's foot, so everything the band holds there lies under it. A chunk
  is split into its root's eight children only where it would go past a byte
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

1. **Pieces.** Done: nodes keep their own values (`Tree::Decided`, kept through `trimmed`), and
   `Tree::cut`, `Tree::graft` and `Tree::compact` cut a piece from any node to any depth and
   graft one in at any node, touching only the piece and the way down to it. The coded form is
   `grid::Page`, whose root may be any node; it takes the name piece when pages retire. Tests: a
   fill reads the same at every node whatever its depth or starting node; pieces filled apart
   graft into what's filled whole; graft then cut gives the piece back; a graft changes only
   what's under it and keeps the values over it.
2. **Chunk store.** Done: `baked::ChunkStore` beside the page store. Chunks are keyed by kind,
   band and the root's Morton bits moved up to the deepest depth, so the chunk over any node is
   the last key at or before it, one seek. A chunk carries the tree from its root to its band's
   foot; a piece is cut from the finest band's chunk over the node, or put together from the
   coarser bands with the finer chunks under the node grafted in. Versions come from one counter
   for the whole store, so a key let go and held again never repeats a version. A tree put under
   a held chunk is grafted into it. Tests: pieces read from chunks match the generator's trees
   over sea floor and land for band heights 2 to 4 and budgets from 256 bytes to 1 MB; open sky
   is one chunk a band and smooth ground takes fewer than rugged; versions step only on change;
   another generator's chunks read as not held.
3. **Filling chunks.** Done: a fill is a **region**, one band's nodes under a root set
   `fill_levels` over the band's foot (no deeper than its roots), so one fill is never more than
   that many levels however coarse its band, and its chunks can be wide. A band's regions under a
   node are those the band above leaves open, so nothing is filled under ground the generator
   closes. `baked::ChunkFiller` answers a node asked down to a depth, band by band from the one
   holding the node, ranked, written on its writer thread; `hog-bake` fills regions depth first
   into a chunk store, down through the band holding the level `--coarse` rounds to. Tests:
   regions read back as the generator fills them; asking deeper fills only the bands under and
   leaves the versions over it alone; a node in closed ground fills nothing under it; fills
   start by rank; a bake keeps what it holds and deepens without filling again.
4. **The server answers asks with pieces.** Done, beside the page path: `grid::Piece` is the
   coded form, any depth under any node, and `AskPieces`/`Pieces` carry asks and answers.
   `server::PieceSource` reads a node down to a depth from the chunk store, as far as it's held,
   asking the filler for the rest; answers are kept for the next asker and checked against the
   versions of the chunks they were read from, and let go where the filler writes under them.
   `server::PieceSending` sends each client what it asks for in its order, remembers a stamp of
   what each piece it holds was read from, and on a touch sends only the kinds whose stamp moved;
   a piece short of its depth waits until something is written under it. Tests: answers match the
   store and the generator; a chunk written lets go of what was read from it; a client is sent a
   changed piece once, with only the kinds changed; a short piece goes again once filled.
5. **The client asks by region and holds one tree.** Done: `client::HeldPieces` keeps each piece
   as it came and reads them as one tree, the piece reaching deepest at a node answering there;
   meshing reads nodes through it. `client::pieces_to_ask` goes down from the root through what's
   held: a node wanted no more than `region_levels` under it is one region, asked to the depth its
   distance wants; one wanted deeper is gone down through, child by child, and asked
   `region_levels` deep where nothing's held under it yet, so coarse comes first and finer is
   asked only where ground lies. Behind the eye ranks lower. The server always serves pieces from
   the chunk store `[ground] store` names, or a scratch one, filling on demand, and the client
   always draws from them; tiles still go to whoever doesn't ask, so the bot walks on them, and
   the client flies until walking reads the pieces. Protocol 42. Tests: asks never
   overlap and each lies under what's held; asking and answering until nothing's left holds the
   ground near and far as deep as the view wants; turning re-ranks.
6. **Pages retire:** the page store, page filling, page sending and held pages, and the page
   cut in the grid.

Steps 1 to 3 don't touch what runs now. Step 5 switched the server and the client over
together; a store is a chunk store, baked or filled as asked.

## Open

- The band height and the chunk budget, from counts on the working world: probably three levels
  and some tens of kilobytes. On the test world a 64-node-wide shape fill is about 70 kB over
  sea floor and 117 kB over land: every surface node holds its own plane, so even smooth ground
  isn't shared, only empty and solid space is.
- Whether chunks well under budget are ever joined back into one under their parent, or a bake
  just fills at a sensible root to begin with.
- Whether identical pieces travel once, by hash, for clients far apart seeing the same sea.
- The client keeps every piece it's sent, and the server remembers each as held; letting go of
  far pieces needs the client to say so, so the server sends them again when asked.
- A piece over 512 KiB can't go in one message though a piece may be 1 MB; regions of five
  levels stay far under it so far.
- A bake in another process doesn't wake pieces waiting on fills; they go when next touched.
- Walking reads the old tiles, which a client asking for pieces isn't sent: the player should
  stand and collide on the held pieces.
