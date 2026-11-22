"""A palm by rules: a rosette while it establishes, then a stem leaning from its foot and
curving back up, crowned by fronds whose rachises arch more the older they are."""

import math
from dataclasses import dataclass

import numpy as np

from skeleton import Skeleton

GOLDEN = math.radians(137.5)


@dataclass
class Params:
    establish: float = 5.0  # years as a stemless rosette
    rise: float = 0.55  # metres of stem a year after that
    max_height: float = 22.0
    stem_radius: float = 0.16
    bole: float = 0.5  # the foot swells by this share
    lean: float = 18.0  # degrees at the foot, at most
    recover: float = 0.6  # share of the lean the stem curves back toward up
    fronds: int = 26
    frond_length: float = 4.8  # metres at full age
    rachis_radius: float = 0.035
    droop: float = 95.0  # degrees the oldest frond's tip bends down over its length
    segments: int = 14


def grow_one(p, seed, age_years):
    rng = np.random.default_rng(seed)
    sk = Skeleton()
    radii = []

    def add(q, parent, axis, r):
        radii.append(r)
        return sk.add(q, parent, axis)

    height = min(max(age_years - p.establish, 0.0) * p.rise, p.max_height)
    maturity = min(age_years / (p.establish + 12.0), 1.0)
    lean = math.radians(p.lean * rng.random())
    lean_dir = rng.random() * 2 * math.pi
    side = np.array([math.cos(lean_dir), 0.0, math.sin(lean_dir)])
    node = add(np.zeros(3), -1, True, p.stem_radius * (1 + p.bole))
    pos = np.zeros(3)
    d = np.array([0.0, 1.0, 0.0])
    steps = max(int(height / 0.4), 1) if height > 0 else 0
    for k in range(steps):
        t = (k + 1) / steps
        # Lean at the foot, recovering toward up as the stem rises.
        ang = lean * (1 - p.recover * t)
        d = np.array([0.0, math.cos(ang), 0.0]) + side * math.sin(ang)
        d /= np.linalg.norm(d)
        pos = pos + d * (height / steps)
        r = p.stem_radius * (1 + p.bole * max(0.0, 1 - t * 6)) * (0.92 + 0.08 * rng.random())
        node = add(pos.copy(), node, True, r)
    crown = node
    top = pos.copy()
    # Fronds, newest first: upright, then out, then the oldest hanging under the crown.
    count = max(int(p.fronds * (0.35 + 0.65 * maturity)), 5)
    length = p.frond_length * (0.35 + 0.65 * maturity)
    for f in range(count):
        share = f / max(count - 1, 1)  # 0 newest, 1 oldest
        az = f * GOLDEN + rng.normal() * 0.1
        out = np.array([math.cos(az), 0.0, math.sin(az)])
        elev = math.radians(75 - 95 * share + rng.normal() * 6)
        fd = np.array([0.0, math.sin(elev), 0.0]) + out * math.cos(elev)
        fd /= np.linalg.norm(fd)
        droop = math.radians(p.droop * (0.3 + 0.7 * share))
        q = top + fd * p.stem_radius
        prev = add(q.copy(), crown, False, p.rachis_radius)
        seg = length / p.segments
        for s in range(p.segments):
            t = (s + 1) / p.segments
            # Bend down about the frond's own sideways axis, more toward the tip.
            axis = np.cross(fd, np.array([0.0, 1.0, 0.0]))
            n = np.linalg.norm(axis)
            if n > 1e-6:
                axis /= n
                a = droop / p.segments * (0.5 + t)
                fd = (fd * math.cos(-a) + np.cross(axis, fd) * math.sin(-a)
                      + axis * np.dot(axis, fd) * (1 - math.cos(-a)))
            q = q + fd * seg
            prev = add(q.copy(), prev, True, p.rachis_radius * (1 - 0.85 * t) + 0.004)
    sk.radius = np.array(radii)
    return sk


def grow(params, seed, ages):
    return {a: grow_one(params, seed, a) for a in ages}


PALM = Params()
