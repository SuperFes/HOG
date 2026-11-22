"""Self-organising trees held to real targets: a species gives the height, crown spread and
branch count it should have at an age, and the growth numbers that reach them are found by
bisection, so species are tuned by what a tree looks like rather than by the model's knobs."""

import dataclasses
import hashlib
import json
import math
from pathlib import Path

import numpy as np

import grow_sot

CACHE = Path(__file__).with_name("out") / "calibrated.json"


@dataclasses.dataclass
class Targets:
    at: int = 30  # years
    height: float = 15.0  # metres
    spread: float = 10.0  # metres across the crown
    nodes: int = 5000  # metamers alive, at the model's branch scale


def measure(params, seed, years, cap):
    """Height, spread and metamers alive at `years`, or None past `cap` metamers."""
    t = grow_sot.Tree(params, seed)
    for _ in range(years):
        t.step()
        if sum(t.alive) > cap:
            return None
    pos = np.array([q for q, a in zip(t.pos, t.alive) if a])
    if len(pos) < 3:
        return 0.0, 0.0, len(pos)
    radial = np.hypot(pos[:, 0], pos[:, 2])
    return float(pos[:, 1].max()), float(2 * np.percentile(radial, 95)), len(pos)


def _key(params, targets, seeds):
    blob = json.dumps([dataclasses.asdict(params), dataclasses.asdict(targets), seeds],
                      sort_keys=True, default=str)
    return hashlib.sha1(blob.encode()).hexdigest()[:16]


def _score(params, targets, seeds):
    """Mean height, spread and metamers over `seeds`; metamers infinite past a cap."""
    cap = targets.nodes * 3
    got = [measure(params, s, targets.at, cap) for s in seeds]
    if any(g is None for g in got):
        return None
    return tuple(float(np.mean([g[k] for g in got])) for k in range(3))


def _bisect(params, field, lo, hi, want, index, targets, seeds, steps=6):
    """Sets `field` so the score's `index` meets `want`, assuming it rises with the field."""
    best = getattr(params, field)
    for _ in range(steps):
        mid = math.sqrt(lo * hi)
        trial = dataclasses.replace(params, **{field: mid})
        s = _score(trial, targets, seeds)
        value = math.inf if s is None else s[index]
        if value > want:
            hi = mid
        else:
            lo = mid
        best = mid
    return dataclasses.replace(params, **{field: best})


def calibrate(params, targets, seeds=(1,), rounds=3, verbose=True):
    key = _key(params, targets, list(seeds))
    cache = json.loads(CACHE.read_text()) if CACHE.exists() else {}
    if key in cache:
        return dataclasses.replace(params, **cache[key])
    p = params
    for r in range(rounds):
        p = _bisect(p, "vigour", 1.0, 5.0, targets.nodes, 2, targets, seeds)
        p = _bisect(p, "order_length", 0.25, 1.4, targets.spread, 1, targets, seeds)
        p = _bisect(p, "internode", 0.08, 0.6, targets.height, 0, targets, seeds)
        if verbose:
            print(f"  round {r + 1}: internode {p.internode:.3f} order_length "
                  f"{p.order_length:.3f} vigour {p.vigour:.3f} -> {_score(p, targets, seeds)}",
                  flush=True)
    found = {k: getattr(p, k) for k in ("internode", "order_length", "vigour")}
    cache[key] = found
    CACHE.parent.mkdir(exist_ok=True)
    CACHE.write_text(json.dumps(cache, indent=1))
    return p
