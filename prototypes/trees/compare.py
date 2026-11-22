"""One sheet: each growth model's 30-year tree side by side, per archetype."""
import grow_param
import grow_sc
import grow_sot
from render import Scene, render, sheet
from skeleton import tube_mesh

MODELS = (("Self-organising", grow_sot), ("Rule sets", grow_param),
          ("Space colonisation", grow_sc))

if __name__ == "__main__":
    rows, labels = [], []
    for kind in ("broadleaf", "conifer"):
        for seed in (1, 2):
            row = []
            for _name, m in MODELS:
                sk = m.grow(getattr(m, kind.upper()), seed, {30})[30]
                s = Scene()
                s.add(tube_mesh(sk))
                row.append(render(s, size=(560, 720), yaw=35 + 40 * seed))
            rows.append(row)
            labels.append(f"{kind}\nseed {seed}")
    sheet(rows, "Round 1: branch structure at 30 years, wood only (each panel framed to its tree)",
          labels, [n for n, _ in MODELS], "out/compare.png")
