# Tree prototypes

Out-of-engine tree experiments, rendered as stills to judge side by side before anything
goes into `libs/flora` or the in-engine plant lab.

Setup (once): `python3 -m venv .venv && .venv/bin/pip install numpy pillow moderngl`
Renders headless on the GPU through EGL. Run from this directory:

- `.venv/bin/python compare.py`: every growth model's 30-year tree side by side
- `.venv/bin/python try_model.py grow_sot|grow_param|grow_sc [broadleaf,conifer]`: one model
  by seed and age (8, 18, 30 years), each row to one scale
- `palm.py`: a palm by rules; rendered with `try_model.row`

Sheets land in `out/`.

| File | What |
|---|---|
| `skeleton.py` | nodes and parents, pipe-model radii, wood as parallel-transported tubes |
| `render.py` | sun + sky + bounce, 4k PCF shadow map, MSAA, ACES; contact sheets |
| `grow_sot.py` | self-organising trees (Palubicki 2009): buds compete for light a year at a time |
| `grow_param.py` | rule sets per branch level (Weber & Penn 1995), crown shape functions |
| `grow_sc.py` | space colonisation (Runions 2007): crown points pull branch tips |
| `palm.py` | rosette, leaning stem curving up, fronds arching more with age |
