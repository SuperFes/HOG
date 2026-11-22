"""Space colonisation after Runions, Lane & Prusinkiewicz 2007, "Modeling trees with a space
colonization algorithm": the crown is a cloud of points that pull the nearest branch tips
toward them and are used up as wood reaches them. Age grows the envelope and its points."""

import math
from dataclasses import dataclass

import numpy as np

from skeleton import Skeleton


@dataclass
class Params:
    envelope: str = "ellipsoid"  # ellipsoid | cone
    height: float = 15.0  # metres at full age
    crown_base: float = 0.3  # share of the height under the crown
    width: float = 0.55  # crown width per metre of height
    points: int = 4000  # attraction points at full age
    step: float = 0.25  # metres a node grows each pass
    influence: float = 2.0  # metres a point pulls from
    kill: float = 0.5  # metres from wood a point is used up
    tropism: float = 0.0  # pull up (<0 down) per step
    wobble: float = 0.05
    tip_radius: float = 0.004
    pipe_exponent: float = 2.3
    passes: int = 140
    top_heavy: float = 0.0  # cone: points denser toward the top (0 = even)


def _points(p, rng, age):
    grow = 1.0 - math.exp(-3.0 * age)
    h = p.height * grow
    base = h * p.crown_base
    r = h * p.width / 2
    n = max(int(p.points * grow ** 1.5), 60)
    out = []
    while len(out) < n:
        q = rng.uniform(-1, 1, size=(n, 3))
        if p.envelope == "cone":
            y = rng.random(n)
            if p.top_heavy:
                y = y ** (1 + p.top_heavy)
            rad = (1 - y) * r
            ang = rng.random(n) * 2 * math.pi
            dist = np.sqrt(rng.random(n)) * rad
            pts = np.stack([np.cos(ang) * dist, base + y * (h - base), np.sin(ang) * dist], -1)
        else:
            keep = (q ** 2).sum(axis=1) <= 1
            q = q[keep]
            pts = np.stack([q[:, 0] * r, base + (q[:, 1] * 0.5 + 0.5) * (h - base),
                            q[:, 2] * r], -1)
        out.extend(pts.tolist())
    return np.array(out[:n]), base, h


def grow_one(p, seed, age):
    rng = np.random.default_rng(seed)
    pts, base, h = _points(p, rng, age)
    sk = Skeleton()
    node = sk.add((0, 0, 0), -1)
    # A trunk to the crown's base.
    y = 0.0
    while y < base:
        y = min(y + p.step, base)
        node = sk.add((rng.normal() * 0.02, y, rng.normal() * 0.02), node)
    nodes = np.array(sk.pos)
    heading = [np.array([0.0, 1.0, 0.0])] * len(nodes)
    alive = np.ones(len(pts), bool)
    has_axis_child = [False] * len(nodes)
    has_axis_child[:-1] = [True] * (len(nodes) - 1)
    for _ in range(p.passes):
        live = pts[alive]
        if len(live) == 0:
            break
        # Nearest node to each live point.
        best = np.full(len(live), np.inf)
        near = np.zeros(len(live), int)
        for start in range(0, len(nodes), 2048):
            chunk = nodes[start:start + 2048]
            d2 = ((live[:, None, :] - chunk[None, :, :]) ** 2).sum(-1)
            k = d2.argmin(axis=1)
            dk = d2[np.arange(len(live)), k]
            better = dk < best
            best[better] = dk[better]
            near[better] = k[better] + start
        pulling = best <= p.influence ** 2
        if not pulling.any():
            # Nothing within reach: the leader keeps on toward the crown.
            tip = len(nodes) - 1
            q = nodes[tip] + np.array([0, p.step, 0])
            sk.add(q, tip)
            nodes = np.vstack([nodes, q])
            heading.append(np.array([0.0, 1.0, 0.0]))
            has_axis_child.append(False)
            continue
        pull = np.zeros_like(nodes)
        for i in np.nonzero(pulling)[0]:
            d = live[i] - nodes[near[i]]
            pull[near[i]] += d / max(np.linalg.norm(d), 1e-9)
        new = []
        for n in np.nonzero(np.linalg.norm(pull, axis=1) > 0)[0]:
            d = pull[n] / np.linalg.norm(pull[n])
            d = d + np.array([0.0, p.tropism, 0.0]) + rng.normal(size=3) * p.wobble
            d /= np.linalg.norm(d)
            axis = not has_axis_child[n] and np.dot(d, heading[n]) > 0.5
            if axis:
                has_axis_child[n] = True
            new.append((nodes[n] + d * p.step, n, axis, d))
        for q, parent, axis, d in new:
            sk.add(q, int(parent), bool(axis))
            heading.append(d)
            has_axis_child.append(False)
        nodes = np.array(sk.pos)
        # Points reached by wood are used up.
        fresh = np.array([q for q, *_ in new])
        idx = np.nonzero(alive)[0]
        d2 = ((pts[idx][:, None, :] - fresh[None, :, :]) ** 2).sum(-1).min(axis=1)
        alive[idx[d2 <= p.kill ** 2]] = False
    sk.pipe_radii(tip=p.tip_radius, exponent=p.pipe_exponent)
    return sk


def grow(params, seed, ages, full=30):
    return {a: grow_one(params, seed, a / full) for a in ages}


BROADLEAF = Params(envelope="ellipsoid", height=16.0, crown_base=0.22, width=0.75,
                   points=5000, step=0.3, influence=2.2, kill=0.55, tropism=0.05)

CONIFER = Params(envelope="cone", height=19.0, crown_base=0.06, width=0.42, points=5000,
                 step=0.3, influence=1.6, kill=0.45, tropism=-0.12, top_heavy=0.0)
