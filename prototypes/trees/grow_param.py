"""Rule-set trees after Weber & Penn 1995, "Creation and Rendering of Realistic Trees": each
branch level has its own count, length, angles, curve and taper, and a crown shape scales
the first level's lengths along the trunk. Age scales the tree and thins its levels."""

import math
from dataclasses import dataclass, field

import numpy as np

from skeleton import Skeleton


def shape_ratio(shape, r):
    """Weber-Penn crown shapes: how long first-level branches are at height share r."""
    r = min(max(r, 0.0), 1.0)
    return {
        "conical": 0.2 + 0.8 * r,
        "spherical": 0.2 + 0.8 * math.sin(math.pi * r),
        "hemispherical": 0.2 + 0.8 * math.sin(0.5 * math.pi * r),
        "cylindrical": 1.0,
        "tapered": 0.5 + 0.5 * r,
        "flame": r / 0.7 if r <= 0.7 else (1.0 - r) / 0.3,
        "inverse_conical": 1.0 - 0.8 * r,
        "tend_flame": 0.5 + 0.5 * r / 0.7 if r <= 0.7 else 0.5 + 0.5 * (1.0 - r) / 0.3,
    }[shape]


@dataclass
class Level:
    length: float  # share of parent length (trunk: metres at full age)
    length_v: float = 0.1
    curve_res: int = 6
    curve: float = 0.0  # degrees bent over the stem
    curve_back: float = 0.0  # degrees bent back over its second half
    curve_v: float = 10.0  # degrees of random bend per segment
    down: float = 45.0  # degrees from the parent's axis
    down_v: float = 10.0
    rotate: float = 140.0  # degrees about the parent between children
    rotate_v: float = 15.0
    branches: int = 0  # children per stem at full count
    splits: float = 0.0  # forks per segment, kept fractional by an error carry
    split_angle: float = 0.0
    up: float = 0.0  # degrees per segment bent toward up (<0 toward down)


@dataclass
class Params:
    shape: str = "tend_flame"
    base_size: float = 0.3  # share of the trunk bare of branches
    scale: float = 14.0  # trunk metres at full age
    scale_v: float = 2.0
    ratio: float = 0.018  # trunk radius per metre of trunk
    ratio_power: float = 1.3  # child radius = parent's * (child/parent length)^power
    tip_radius: float = 0.004
    levels: list = field(default_factory=list)
    flare: float = 0.5  # trunk widens this share at its foot


class Builder:
    def __init__(self, p: Params, seed: int, age: float):
        self.p = p
        self.rng = np.random.default_rng(seed)
        self.age = age
        self.sk = Skeleton()
        self.radius = []

    def v(self, x):
        return (self.rng.random() * 2 - 1) * x

    def add(self, pos, parent, axis, r):
        n = self.sk.add(pos, parent, axis)
        self.radius.append(r)
        return n

    def build(self):
        p = self.p
        grow = 1.0 - math.exp(-3.0 * self.age)
        length = (p.scale + self.v(p.scale_v)) * grow
        levels = len(p.levels)
        if self.age < 0.25:
            levels = min(levels, 2)
        elif self.age < 0.5:
            levels = min(levels, 3)
        self.levels = levels
        root = self.add(np.zeros(3), -1, True, p.ratio * length * (1 + p.flare))
        self.stem(0, root, np.zeros(3), np.array([0.0, 1.0, 0.0]), np.array([1.0, 0.0, 0.0]),
                  length, p.ratio * length, 0.0, None)
        self.sk.radius = np.array(self.radius)
        return self.sk

    def stem(self, depth, parent_node, start, heading, side, length, radius, offset, parent_len):
        p = self.p
        lv = p.levels[depth]
        res = max(lv.curve_res, 2)
        seg = length / res
        d = heading / np.linalg.norm(heading)
        node = parent_node
        pos = start.copy()
        split_carry = self.rng.random()
        children = []
        for k in range(res):
            t0 = k / res
            # Bend: the stem's curve over its first half, curve_back over its second.
            bend = lv.curve / res if (lv.curve_back == 0 or k < res / 2) else lv.curve_back / res
            bend += self.v(lv.curve_v) / res
            d = rotate(d, side, math.radians(bend))
            if lv.up:
                d = toward(d, np.array([0.0, 1.0, 0.0]), math.radians(lv.up))
            pos = pos + d * seg
            r = radius * (1 - 0.85 * (k + 1) / res) + p.tip_radius
            if depth == 0 and k == 0:
                r = radius * (1 + p.flare * 0.4)
            node = self.add(pos, node, True if k else depth == 0 or k > 0, r)
            if k == 0 and depth > 0:
                self.sk.axis[node] = False
            children.append((node, pos.copy(), d.copy(), t0 + 1 / res))
            # Forks: a split leaves this stem and runs as a sibling of equal standing.
            if lv.splits and k < res - 1:
                split_carry += lv.splits
                if split_carry >= 1:
                    split_carry -= 1
                    ang = math.radians(lv.split_angle + self.v(10))
                    axis = rotate(side, d, self.rng.random() * 2 * math.pi)
                    fork = rotate(d, axis, ang)
                    d = rotate(d, axis, -ang * 0.6)
                    self.stem_rest(depth, node, pos, fork, axis, length * (1 - t0) * 0.9,
                                   r, res - k - 1)
        if depth + 1 < self.levels:
            self.branch_out(depth, children, length, radius, d)

    def stem_rest(self, depth, node, pos, d, side, length, r, segs):
        p = self.p
        lv = p.levels[depth]
        if segs <= 0:
            return
        seg = length / segs
        children = []
        first = True
        for k in range(segs):
            d = rotate(d, side, math.radians(lv.curve / max(lv.curve_res, 2)
                                             + self.v(lv.curve_v) / max(lv.curve_res, 2)))
            if lv.up:
                d = toward(d, np.array([0.0, 1.0, 0.0]), math.radians(lv.up))
            pos = pos + d * seg
            rr = r * (1 - 0.85 * (k + 1) / segs) + p.tip_radius
            node = self.add(pos, node, not first, rr)
            first = False
            children.append((node, pos.copy(), d.copy(), 0.5 + 0.5 * (k + 1) / segs))
        if depth + 1 < self.levels:
            self.branch_out(depth, children, length, r, d)

    def branch_out(self, depth, points, length, radius, last_dir):
        p = self.p
        child = p.levels[depth + 1]
        if depth == 0:
            count = child.branches
        else:
            count = child.branches * 0.8
        count *= 0.35 + 0.65 * min(self.age * 1.4, 1.0)
        count = int(round(count))
        if count <= 0:
            return
        base = p.base_size if depth == 0 else 0.0
        rot = self.rng.random() * 360
        for i in range(count):
            t = base + (1 - base) * (i + 0.5) / count
            # Nearest segment point at or past t.
            idx = min(int(t * len(points)), len(points) - 1)
            node, pos, d, _ = points[idx]
            if depth == 0:
                ratio = shape_ratio(p.shape, (1 - t) / max(1 - base, 1e-6))
                clen = length * (child.length + self.v(child.length_v)) * ratio
            else:
                clen = (child.length + self.v(child.length_v)) * (length * (1 - 0.6 * t))
            if clen <= 0.12 * (depth + 1):
                continue
            rot += child.rotate + self.v(child.rotate_v)
            down = math.radians(child.down + self.v(child.down_v))
            a = np.array([0.0, 1.0, 0.0]) if abs(d[1]) < 0.9 else np.array([1.0, 0.0, 0.0])
            u = np.cross(d, a)
            u /= np.linalg.norm(u)
            u = rotate(u, d, math.radians(rot))
            cd = rotate(d, u, down)
            side = u
            crad = max(radius * (clen / length) ** p.ratio_power, p.tip_radius)
            self.stem(depth + 1, node, pos, cd, side, clen, crad, t, length)


def rotate(v, axis, ang):
    axis = axis / np.linalg.norm(axis)
    return (v * math.cos(ang) + np.cross(axis, v) * math.sin(ang)
            + axis * np.dot(axis, v) * (1 - math.cos(ang)))


def toward(d, target, ang):
    axis = np.cross(d, target)
    n = np.linalg.norm(axis)
    if n < 1e-6:
        return d
    return rotate(d, axis / n, min(ang, math.acos(np.clip(np.dot(d, target), -1, 1))))


def grow(params, seed, ages, full=30):
    """Skeletons at each of `ages` years, `full` years being a share of 1 of the tree's growth."""
    return {a: Builder(params, seed, a / full).build() for a in ages}


BROADLEAF = Params(
    shape="tend_flame", base_size=0.25, scale=15.0, scale_v=2.0, ratio=0.011,
    levels=[
        Level(length=1.0, curve_res=8, curve=0, curve_v=25, splits=0.25, split_angle=28),
        Level(length=0.42, length_v=0.08, curve_res=6, curve=-25, curve_back=20, curve_v=40,
              down=55, down_v=12, rotate=140, branches=24, up=2.5),
        Level(length=0.45, length_v=0.1, curve_res=4, curve=-10, curve_v=60, down=45,
              down_v=12, rotate=140, branches=12),
        Level(length=0.5, length_v=0.1, curve_res=2, curve=0, curve_v=30, down=45, down_v=10,
              rotate=140, branches=8),
    ])

CONIFER = Params(
    shape="conical", base_size=0.06, scale=19.0, scale_v=2.0, ratio=0.008, flare=0.25,
    levels=[
        Level(length=1.0, curve_res=10, curve_v=6),
        # Long limbs droop from the trunk and turn up at their tips, as a spruce's do.
        Level(length=0.28, length_v=0.05, curve_res=6, curve=18, curve_back=-30, curve_v=10,
              down=95, down_v=8, rotate=72, rotate_v=25, branches=150),
        Level(length=0.35, length_v=0.08, curve_res=3, curve=0, curve_v=15, down=60, down_v=10,
              rotate=180, rotate_v=30, branches=14),
        Level(length=0.45, length_v=0.1, curve_res=2, curve_v=15, down=50, down_v=10,
              rotate=180, branches=6),
    ])
