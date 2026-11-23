# Flora assets: the .flora file

Trees (later bushes and other plants) come into the engine as baked assets, one `.flora` file
per species: its textures, and a set of variants (seeds at ages), each with its meshes, its
leaves, its branch cards and, for some, its impostor. The tree lab writes them
(`prototypes/trees/export.py`, into `var/flora/`); a generator in the engine may later write the
same files on the client behind a loading screen, so the renderer never cares which.

Variety: every placed tree picks a variant by hash, and on top of that its own turn, size, lean,
tint and autumn timing, so neighbours rarely match. Impostors are the costly part, so only the
first few variants of a species carry one (4 by default); trees past impostor range pick among
those.

## Levels of detail

Pieces (leaves, needle ribbons, twigs, wood segments) drop out with distance in a fixed random
order and the ones left grow to keep the crown's area (`prototypes/trees/lod.py`). In the file
every part's pieces are sorted by the size of a pixel (metres) at which they drop, and triangles
follow their pieces, so a level of detail is one prefix: find how many pieces have
`drop > metres per pixel` and draw up to that piece's `index_end`. In the vertex shader a kept
piece grows about its pivot by `max(1, px * target / size)` (`lod.kept`), and its normal, colour and autumn colour are pulled toward its neighbourhood's means
by `sqrt(share)` so fewer, larger pieces keep a pixel's variance (`lod._calmed`).

Then: branch cards in coarser and coarser levels (clusters 1, 2 and 4 m across, each about a
quarter of the cards of the last, their limbs thinning with them), the impostor (from a few
hundred metres), the far forest. Files written before levels were stored hold 0 for every mesh;
their cards are the meshes after the whole tree's, one level.

## Layout

Little-endian. A 16-byte header (`HOGFLORA`, u32 version 1, 4 zero bytes), then chunks, each a
16-byte header (4-byte tag, u32 payload size, 8 zero bytes) and a payload padded to 16 bytes.
Order: `SPEC`, every `TEXR`, then per variant a `VARI` followed by its parts.

Colours are linear colour / `colour_range` (2.0), sRGB encoded. Positions are unorm16 over the
variant's bounds. Normals are octahedral. Sun visibility (the leaves' own shadowing, c0 and c1
of a first-order fit) is unorm8 as (c0, c1 + 0.5).

- **SPEC**: name (32 bytes), u32 variants, u32 0, f32 colour_range, u32 template corners (6, or 0
  for species without broad leaves), 12 zero bytes; the leaf template: 6 corners (f32 x3, in
  units of the leaf's length along its across, way and face axes), their uvs (f32 x2), and 4
  triangles (u16 x12).
- **TEXR**: u32 id, width, height, format, mip levels, 0; 8 zero bytes; the levels, largest
  first, each padded to 16. Formats: 1 RGBA8 sRGB (alpha linear), 2 RGBA8, 3 RG16, 4 R8,
  5 RGBA16. Textures are shared between variants (same content, same id).
- **VARI**: u32 seed, u32 age, f32 lo[3], hi[3] (the bounds positions are quantized over).
- **MESH**: u32 role (1 wood, 2 foliage, 3 card), u32 dims (0: never thins), u32 level (0 the
  whole tree; 1, 2, ... its branch cards, coarser each step, with the limbs carrying them), f32 target
  (pixels a piece is kept at), i32 albedo texture, i32 second texture (card normals, -1), f32
  translucency, cutoff, relief, f32 uv scale[2], u32 vertices, pieces, indices; padded to 64.
  Then:
  - vertices, 32 bytes: u16 pos[4] (w unused), snorm16 normal[2], unorm16 uv[2] (times the uv
    scale), u8 colour[4], u8 autumn[4] (rgb, turned share), u8 sun visibility[4], u32 piece;
  - pieces, 32 bytes, in drop order: u16 pivot[4], f16 size, f16 drop, u8 mean colour[4], u8
    mean autumn[4], snorm8 mean normal[2], u16 0, u32 index end, u32 0;
  - u32 indices, three a triangle.
  Card textures: albedo with coverage in alpha; normals packed 0-1 with leafiness in alpha;
  both with coverage-weighted mips.
- **LEAF**: u32 leaves, u32 0, f32 target, i32 albedo texture, i32 turn mask (R8: where on the
  leaf autumn reaches first), f32 translucency, cutoff; padded to 32. Then leaves, 40 bytes, in
  drop order: u16 base[3], f16 length, snorm8 rotation[4] (quaternion x, y, z, w; its columns
  are the across, way and face axes), f16 drop, snorm8 normal[2], u8 colour[3], u8 flags (1:
  fallen, lying unfolded), u8 autumn[4], u8 sun visibility[4], u8 mean colour[4], u8 mean
  autumn[4], snorm8 mean normal[2], u16 0. A leaf's corners are base + rotation × template ×
  length (fallen: the template's face offsets zeroed); the exporter checks they land within
  5 mm of the leaves it baked.
- **IMPO**: u32 views, u32 cell (texels a view), f32 centre[3], radius, the fitted crown (f32
  centre[3], radii[2], leaf density per metre) the far forest traces, i32 textures: albedo
  (RGBA8 sRGB), normal and leafiness (RGBA8), depth and moment yz + 0.5 (RG16), sun visibility
  (RGBA8, c1 + 0.5), moments xx, yy, xy + 0.5, xz + 0.5 (RGBA16); all with coverage-weighted
  mips (`impostor.coverage_mips`).

## Size

An oak variant at 30 years: ~15 MB of meshes and leaves (93k leaves, 3.7 MB of them; twigs are
the largest part), 22 MB of card atlases (4 cm texels), 19 MB more with an impostor; all
uncompressed. Block compression (BC7 colour, BC5 normals) would cut textures about four times.
