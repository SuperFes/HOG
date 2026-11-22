"""A tree as nodes joined to their parents, and its wood meshed as tubes."""

from dataclasses import dataclass, field

import numpy as np


@dataclass
class Skeleton:
    pos: list = field(default_factory=list)  # (3,) arrays, metres, y up
    parent: list = field(default_factory=list)  # -1 for the root
    # Whether a node continues its parent's axis; a branch starts where it doesn't.
    axis: list = field(default_factory=list)
    radius: np.ndarray | None = None

    def add(self, p, parent, axis=True):
        self.pos.append(np.asarray(p, dtype=np.float64))
        self.parent.append(parent)
        self.axis.append(axis)
        return len(self.pos) - 1

    def __len__(self):
        return len(self.pos)

    def children(self):
        kids = [[] for _ in self.pos]
        for n, p in enumerate(self.parent):
            if p >= 0:
                kids[p].append(n)
        return kids

    def pipe_radii(self, tip=0.004, exponent=2.4, cap=None):
        """Radii by the pipe model: a node's cross-section is the sum of its children's."""
        kids = self.children()
        r = np.zeros(len(self))
        for n in range(len(self) - 1, -1, -1):
            if not kids[n]:
                r[n] = tip
            else:
                r[n] = sum(r[k] ** exponent for k in kids[n]) ** (1.0 / exponent)
        if cap is not None:
            r = np.minimum(r, cap)
        self.radius = r
        return r

    def chains(self):
        """Node runs along each axis, each starting at the node it branches from."""
        kids = self.children()
        out = []
        starts = [n for n in range(len(self)) if self.parent[n] < 0 or not self.axis[n]]
        for s in starts:
            run = [self.parent[s]] if self.parent[s] >= 0 else []
            n = s
            while True:
                run.append(n)
                nxt = [k for k in kids[n] if self.axis[k]]
                if not nxt:
                    break
                n = nxt[0]
            out.append(run)
        return out

    def bounds(self):
        p = np.array(self.pos)
        return p.min(axis=0), p.max(axis=0)


def _frame(t):
    a = np.array([0.0, 1.0, 0.0]) if abs(t[1]) < 0.9 else np.array([1.0, 0.0, 0.0])
    u = np.cross(t, a)
    u /= np.linalg.norm(u)
    return u, np.cross(t, u)


def tube_mesh(sk, colour=(0.16, 0.12, 0.09), min_radius=0.0015, sides_for=None, tile=0.6):
    """Positions, normals, colours, indices and bark coordinates of the wood as tubes along
    each chain: u around the stem, v along it, both in `tile` metres, so bark keeps its size.

    Rings follow parallel-transported frames so tubes don't twist; a branch's first ring sits
    at its parent node, sunk into the parent's tube, and takes its own radius. Each ring has a
    seam vertex twice, so the bark wraps without smearing."""
    if sk.radius is None:
        sk.pipe_radii()
    if sides_for is None:
        def sides_for(r):
            return 16 if r > 0.15 else 10 if r > 0.04 else 5 if r > 0.012 else 3
    P, N, C, I, UV = [], [], [], [], []
    count = 0
    rng = np.random.default_rng(7)
    base = np.array(colour)
    for run in sk.chains():
        pts = np.array([sk.pos[n] for n in run])
        if len(pts) < 2:
            continue
        rad = np.array([sk.radius[n] for n in run])
        # A branch's first ring takes its own size, not its parent's.
        if not sk.axis[run[1]]:
            rad[0] = rad[1]
        if rad.max() < min_radius:
            continue
        sides = sides_for(rad[1])
        tang = np.zeros_like(pts)
        tang[1:-1] = pts[2:] - pts[:-2]
        tang[0] = pts[1] - pts[0]
        tang[-1] = pts[-1] - pts[-2]
        lens = np.linalg.norm(tang, axis=1, keepdims=True)
        tang = np.where(lens > 1e-9, tang / np.maximum(lens, 1e-9), np.array([0.0, 1.0, 0.0]))
        u, v = _frame(tang[0])
        tint = base * (0.85 + 0.3 * rng.random())
        colours = getattr(sk, "colour", None)
        # Bark wraps a whole number of times around, as near its true size as that allows.
        wraps = max(1, int(round(2 * np.pi * rad[1] / tile)))
        along = np.concatenate([[0.0], np.cumsum(np.linalg.norm(np.diff(pts, axis=0), axis=1))])
        offset = rng.random()
        ang = np.linspace(0, 2 * np.pi, sides + 1)
        for k in range(len(pts)):
            if k > 0:
                # Parallel transport u along the curve.
                u = u - tang[k] * np.dot(u, tang[k])
                nu = np.linalg.norm(u)
                u = u / nu if nu > 1e-9 else _frame(tang[k])[0]
                v = np.cross(tang[k], u)
            ring_n = np.cos(ang)[:, None] * u + np.sin(ang)[:, None] * v
            P.append(pts[k] + ring_n * rad[k])
            N.append(ring_n)
            here = tint if colours is None else colours[run[k]] * (0.95 + 0.1 * rng.random())
            C.append(np.tile(here, (sides + 1, 1)))
            UV.append(np.stack([ang / (2 * np.pi) * wraps,
                                np.full(sides + 1, along[k] / tile + offset)], 1))
        ring = sides + 1
        k = np.arange(len(pts) - 1)[:, None]
        sidx = np.arange(sides)[None, :]
        a = count + k * ring + sidx
        b = a + ring
        I.append(np.stack([a, b, b + 1], -1).reshape(-1, 3))
        I.append(np.stack([a, b + 1, a + 1], -1).reshape(-1, 3))
        count += len(pts) * ring
    if not P:
        z = np.zeros((0, 3), np.float32)
        return z, z, z, np.zeros((0, 3), np.uint32), np.zeros((0, 2), np.float32)
    return (np.concatenate(P).astype(np.float32), np.concatenate(N).astype(np.float32),
            np.concatenate(C).astype(np.float32), np.concatenate(I).astype(np.uint32),
            np.concatenate(UV).astype(np.float32))
