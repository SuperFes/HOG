# Tree prototypes

Out-of-engine tree experiments, rendered as stills to judge side by side before anything
goes into `libs/flora` or the in-engine plant lab.

Setup (once): `python3 -m venv .venv && .venv/bin/pip install numpy pillow moderngl`
Renders headless on the GPU through EGL. Run from this directory:

- `.venv/bin/python compare.py`: every growth model's 30-year tree side by side
- `.venv/bin/python try_model.py grow_sot|grow_param|grow_sc [broadleaf,conifer]`: one model
  by seed and age (8, 18, 30 years), each row to one scale
- `palm.py`: a palm by rules; rendered with `try_model.row`
- `.venv/bin/python species_sheet.py [names]`: every species in `species/` by age, wood only
- `.venv/bin/python dress.py [names] [age]`: species with foliage: whole, close up, backlit

Sheets land in `out/`.

| File | What |
|---|---|
| `skeleton.py` | nodes and parents, pipe-model radii, wood as parallel-transported tubes |
| `render.py` | sun + sky + bounce, 4k PCF shadow map, MSAA, ACES; contact sheets |
| `grow_sot.py` | self-organising trees (Palubicki 2009): buds compete for light a year at a time |
| `grow_param.py` | rule sets per branch level (Weber & Penn 1995), crown shape functions |
| `grow_sc.py` | space colonisation (Runions 2007): crown points pull branch tips |
| `palm.py` | rosette, leaning stem curving up, fronds arching more with age |
| `calibrate.py` | holds self-organising species to height, spread and branch-count targets |
| `species.py`, `species/*.toml` | a species = growth model + its numbers + foliage |
| `textures.py` | leaves, needle sprays (branchlets, tufts, brushes), frond halves, drawn in code |
| `foliage.py` | leaves on generated twiglets, crossed needle ribbons, frond strips; sky visibility |
| `dress.py` | wood + foliage per species, and the foliage sheets |
| `lod.py` | continuous thinning: pieces drop in rank order by pixel size, survivors grow to keep cover |
| `sunvis.py` | each vertex's own visibility toward any light, fitted as 4 spherical-harmonic numbers |
| `lod_sheet.py` | one tree at true size over distances: truth, mesh, thinned, impostors by view count |
| `forest_sheet.py` | a 1080p field of trees, all full mesh vs all LOD, and a flick GIF between them |
| `terrain.py` | 24 km of hills, forest density and kind fields, trees hashed one per 7 m cell |
| `far.py` | far forest: impostor truth with traced crown shadows; canopy layer of explicit crowns and stand statistics |
| `far_sheet.py` | seven views to 20 km at 1080p: truth vs painted, layer, hybrid, crowns, by distance band |
| `far_cache.py` | how far the eye may move before a cached far view shows, reprojected or reused |
| `test_far.py` | `.venv/bin/python -m unittest test_far`: hash, stand maths, mips, leaf lighting, crown fit |
| `cards.py` | branch cards: crossed planes per leaf cluster, leaves on the plane they lie along |
| `card_sheet.py` | one tree at true size: mesh, thinned, cards, impostor against the truth |
| `test_cards.py` | `.venv/bin/python -m unittest test_cards`: every leaf on one card parallel to it |

Self-organising species calibrate once (tens of seconds each); results cache in
`out/calibrated.json`, keyed by the species' numbers.
