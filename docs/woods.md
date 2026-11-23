# Woods: where trees grow, and which

Where trees stand and what kinds they are is part of the world the generator makes, worked out
from what formed: climate, height, soil and water. It's a field over the ground: how much of it
the trees' crowns cover, and each kind's share of them. Single trees come later, hash-pinned on
that field once the tree lab has made the kinds to place ([roadmap](roadmap.md), Phase 5). The
old placement and its trees went with the old world.

## Decided

- **Results, not a simulation.** As the formation stages are, the woods are worked out once from
  what the place is like, by rules ecology has measured, never painted. Fire, felling and the
  forest's own life change them later through the same field.
- **Kinds of tree by how they live, not species.** The habitat decides shares of a few kinds that
  live differently; each species the lab makes belongs to one, and picks its own place within it.
  - Needleleaf evergreen: boreal and mountain conifers, hardy to long cold.
  - Broadleaf deciduous: temperate woods that drop their leaves in winter.
  - Broadleaf evergreen: warm, wet woods with no winter.
  - Dry woodland: thorny, small-leaved trees of savanna and dry scrub, standing far apart.
  - Palms: warm and frost-free, most of all on coasts and by water.
  - Wetland: trees that stand with wet feet, by rivers, lakes and in fens: willow, alder, swamp
    cypress, and mangroves in warm salt water.
- **Shares, never yes or no.** Every limit fades over a range, and the boundaries between stands
  are blended and warped, so woods thin into grassland and one kind gives way to another with no
  line drawn ([blend boundaries](world-v3.md)).

## The habitat at a place

What a tree there lives with, read from the formed world at the ground's own height:

- **Warmth:** the yearly mean temperature, lowered by the lapse rate from the formed points'
  height to the ground's own, so treelines follow ridges and valleys rather than the coarse
  climate grid. With it the year's swing by latitude, which sets frost and the growing season.
- **Water:** yearly rain against what the warmth would evaporate (Holdridge's potential
  evapotranspiration, 58.93 mm a year per degree of biotemperature): under a quarter is desert,
  under a half scrub, under one woodland, over one forest. And how far the ground stands over
  the floor of the valley it lies in, the height over the nearest drainage, which stands in for
  the water table: low ground by rivers and lakes is wet, high ground dry. Only where rain or a
  river running the year round keeps the water up: a desert wash's floor is as dry as its sides,
  but a big river's makes a gallery forest through the desert.
- **Ground:** how deep the soil is and what it's made of, bare rock, how steep, ice and snow
  lying the year round.
- **Salt:** near the sea, spray and salt water at the shore, once distances can be asked
  ([queries](queries.md), step 2).

## The woods from it

- **Cover**, the share of ground under crowns, is the least of what each limit allows: cold (no
  trees where the growing season's warmth is under Körner's treeline, about 6.4 °C, thinning over
  the degrees above it), drought (cover rising with rain over evaporation), thin soil and bare
  rock, steepness, ice, and standing water (wetland trees only).
- **Each kind's share** is how well the habitat suits it, against how well it suits the others:
  each kind has a range of warmth, water, wet feet, frost and salt it does best in, fading out
  past it. Shares sum to one where there's any cover.
- **Stands**: the shares wander by noise a few hundred metres across, so kinds gather into
  groves and stands, still mixed but for the strongest; now and then a kind the habitat suits
  only a little stands as an outlying grove (oaks in the conifers' cool margin, never palms
  there); and glades thin the cover (`globe::lean`).

## Steps

1. Habitat at a place from the formed world (`globe::habitat`). Tests: warmth falls with height by
   the lapse rate; wetter low by rivers than on ridges; ice and bare rock read as such. Done:
   besides the rest, the water running down the valleys about at their mean (`flow`); salt
   waits for a distance to the sea.
2. Woods from a habitat (`flora::woods_of`): cover and the kinds' shares. Tests on habitats made
   to order: tropical wet is broadleaf evergreen forest, boreal is needleleaf, desert bare,
   above the treeline none, a fen wetland, a warm salt shore mangrove and palm; shares sum to
   one; every limit fades rather than steps. Done, palms still to tie to coasts with salt.
3. A census and map of the woods on the small world and others: share of land wooded, treeline
   heights, kinds by latitude and height; judged by the map before going on. Done
   (`hog-census --woods FILE`): on the working world a fifth of the land under crowns, in belts
   by climate, treeless over 3.1 km, gallery forest along the big river crossing the desert.
   The belts were too even, so stands lean them as above (`globe::woods`): on the map, three
   tenths of the land wooded, read at the map's own detail.
4. The woods carried in the pages, so clients and the server read the same: a grid kind holding
   cover and the kinds' shares per node, filled by the generator, summed up coarse, versioned to
   change with fire and felling. Done (`grid::Woods`): on the ground through each node, held no
   finer than 16 m, even woods merged coarse; stored, sent and held like the other trees.
5. After the tree lab: single trees hash-pinned on the field at the finest nodes, each kind's
   species chosen within it, standing on the ground surface from the pages. The trees come baked
   from the lab as `.flora` files ([flora assets](flora-assets.md)): seeds at ages per species,
   each with its thinning meshes, leaves, branch cards and impostor.
