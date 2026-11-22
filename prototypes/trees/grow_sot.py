"""Self-organising trees, after Palubicki et al. 2009, "Self-organizing tree models for image
synthesis": buds compete for light in a shadow grid, resource flows from the base by the
Borchert-Honda split, and a tree grows a year at a time, so every age comes from one run."""

import math
from dataclasses import dataclass, field

import numpy as np

from skeleton import Skeleton

GOLDEN = math.radians(137.5)


@dataclass
class Params:
    internode: float = 0.12  # metres a metamer at full vigour
    branch_angle: float = 45.0  # degrees between a lateral bud and its shoot
    phyllotaxis: str = "alternate"  # alternate | opposite | whorl
    whorl: int = 5  # buds in a whorl
    side_pairs: bool = False  # orders past the trunk make a pair of side buds, flat sprays
    laterals_at: str = "every"  # every node | shoot_end (pseudo-whorls each year)
    apical: float = 0.52  # Borchert-Honda lambda: share favouring the main axis
    apical_old: float | None = None  # lambda once the tree's years are past `apical_years`
    apical_years: int = 0
    vigour: float = 2.0  # resource at the base per unit of light gathered
    max_metamers: int = 6  # a shoot's metamers in a year at most
    order_vigour: float = 1.0  # apical control: a bud of order k grows on vigour * this^k
    order_length: float = 1.0  # a metamer of order k is internode * this^k long
    shade_weight: float = 0.25  # pull toward light (xi)
    tropism: tuple = (-0.15, 0.02)  # gravitropism by axis order: <0 up, >0 down (eta)
    tropism_decay: float = 1.0  # tropism of orders past the table: last * decay^(extra)
    wobble: float = 0.06  # random turn per metamer
    straight: float = 1.0  # weight of a shoot's own heading
    # Shadow grid.
    voxel: float = 0.15
    shadow_a: float = 1.0
    shadow_b: float = 1.8
    shadow_depth: int = 6
    full_light: float = 6.0
    shadow_spread: float = 1.0  # voxels the shadow widens per voxel down
    self_shade: float = 0.25  # weight of a bud's shadow in its own voxel, against those below
    # Shedding: lateral branches gathering less light than this per metamer drop off.
    shed: float = 0.12
    shed_age: int = 3
    bud_life: int = 4  # years a dormant bud keeps
    tip_radius: float = 0.0035
    pipe_exponent: float = 2.4
    trunk_start: float = 0.0  # metres of bare trunk before laterals form (lower ones shed later)
    extra: dict = field(default_factory=dict)


class Tree:
    def __init__(self, params: Params, seed: int):
        self.p = params
        self.rng = np.random.default_rng(seed)
        self.pos = [np.zeros(3)]
        self.parent = [-1]
        self.axis = [True]
        self.order = [0]
        self.born = [0]
        self.alive = [True]
        self.year = 0
        # Buds: node, heading, terminal, order, born.
        self.buds = [dict(node=0, dir=np.array([0.0, 1.0, 0.0]), terminal=True, order=0,
                          born=0, phase=0)]
        self._new_buds = []

    # --- light ---------------------------------------------------------------------------
    def _shadow(self, bud_pos):
        p = self.p
        lo = bud_pos.min(axis=0) - p.voxel * (p.shadow_depth + 2)
        cells = np.floor((bud_pos - lo) / p.voxel).astype(np.int64)
        dims = cells.max(axis=0) + p.shadow_depth + 3
        grid = np.zeros(dims)
        offs, weights = [], []
        for q in range(p.shadow_depth + 1):
            w = p.shadow_a * p.shadow_b ** (-q) * (p.self_shade if q == 0 else 1.0)
            r = int(round(q * p.shadow_spread))
            for di in range(-r, r + 1):
                for dk in range(-r, r + 1):
                    offs.append((di, -q, dk))
                    weights.append(w)
        offs = np.array(offs)
        weights = np.array(weights)
        tgt = (cells[:, None, :] + offs[None, :, :]).reshape(-1, 3)
        tgt = np.clip(tgt, 0, dims - 1)
        np.add.at(grid, (tgt[:, 0], tgt[:, 1], tgt[:, 2]), np.tile(weights, len(cells)))
        return grid, cells

    def _light(self, bud_pos):
        p = self.p
        grid, cells = self._shadow(bud_pos)
        s = grid[cells[:, 0], cells[:, 1], cells[:, 2]] - p.shadow_a * p.self_shade
        q = np.clip(1.0 - s / p.full_light, 0.0, 1.0)
        # Toward less shadow, from central differences.
        g = np.zeros((len(cells), 3))
        for axis in range(3):
            up = cells.copy()
            dn = cells.copy()
            up[:, axis] = np.minimum(up[:, axis] + 1, grid.shape[axis] - 1)
            dn[:, axis] = np.maximum(dn[:, axis] - 1, 0)
            g[:, axis] = grid[up[:, 0], up[:, 1], up[:, 2]] - grid[dn[:, 0], dn[:, 1], dn[:, 2]]
        v = -g
        n = np.linalg.norm(v, axis=1, keepdims=True)
        v = np.where(n > 1e-9, v / np.maximum(n, 1e-9), np.array([0.0, 1.0, 0.0]))
        return q, v

    # --- one year ------------------------------------------------------------------------
    def step(self):
        p = self.p
        self.year += 1
        buds = [b for b in self.buds if self.alive[b["node"]]]
        if not buds:
            return
        # Each bud senses light where its shoot would go, so buds at one node don't crowd.
        bud_pos = np.array([self.pos[b["node"]] + b["dir"] * p.internode for b in buds])
        light, toward = self._light(bud_pos)
        count = len(self.pos)
        kids = [[] for _ in range(count)]
        for n in range(1, count):
            if self.alive[n]:
                kids[self.parent[n]].append(n)
        buds_at = [[] for _ in range(count)]
        for k, b in enumerate(buds):
            buds_at[b["node"]].append(k)
        # Light gathered by each subtree, base-ward.
        q_node = np.zeros(count)
        for n in range(count - 1, -1, -1):
            if not self.alive[n]:
                continue
            q_node[n] = sum(light[k] for k in buds_at[n]) + sum(q_node[c] for c in kids[n])
        # Resource handed out from the base, tip-ward.
        lam = p.apical
        if p.apical_old is not None and self.year > p.apical_years:
            lam = p.apical_old
        v_node = np.zeros(count)
        v_bud = np.zeros(len(buds))
        v_node[0] = p.vigour * q_node[0]
        for n in range(count):
            if not self.alive[n] or v_node[n] <= 0:
                continue
            main, lateral = [], []  # (kind, index, q)
            for c in kids[n]:
                (main if self.axis[c] else lateral).append(("node", c, q_node[c]))
            for k in buds_at[n]:
                (main if buds[k]["terminal"] else lateral).append(("bud", k, light[k]))
            qm = sum(x[2] for x in main)
            ql = sum(x[2] for x in lateral)
            denom = lam * qm + (1 - lam) * ql
            if denom <= 0:
                continue
            vm = v_node[n] * lam * qm / denom
            vl = v_node[n] - vm
            for group, total, share in ((main, qm, vm), (lateral, ql, vl)):
                for kind, idx, q in group:
                    give = share * q / total if total > 0 else 0.0
                    if kind == "node":
                        v_node[idx] += give
                    else:
                        v_bud[idx] += give
        # Buds with enough resource grow shoots; the rest wait, or die of age.
        kept = []
        for k, b in enumerate(buds):
            # Rounded at random, so a bud's expected growth is its resource, without a cliff at 1.
            v = v_bud[k] * p.order_vigour ** b["order"]
            n_met = min(int(math.floor(v + self.rng.random())), p.max_metamers)
            if n_met >= 1:
                length = (p.internode * p.order_length ** min(b["order"], 2)
                          * min(max(v / n_met, 0.5), 1.6) ** 0.5)
                self._shoot(b, n_met, length, toward[k])
            elif self.year - b["born"] < p.bud_life:
                kept.append(b)
        self.buds = kept + self._new_buds
        self._new_buds = []
        self._shed(q_node, kids)

    def _tropism(self, order):
        t = self.p.tropism
        if order < len(t):
            return t[order]
        return t[-1] * self.p.tropism_decay ** (order - len(t) + 1)

    def _shoot(self, bud, n_met, length, toward):
        p = self.p
        d = bud["dir"].copy()
        node = bud["node"]
        order = bud["order"]
        g = np.array([0.0, -1.0, 0.0])
        phase = bud.get("phase", 0)
        for m in range(n_met):
            wob = self.rng.normal(size=3) * p.wobble
            d = p.straight * d + p.shade_weight * toward + self._tropism(order) * g + wob
            d /= np.linalg.norm(d)
            q = self.pos[node] + d * length
            first = m == 0
            node = self._add(q, node, bud["terminal"] or not first, order)
            last = m == n_met - 1
            makes_laterals = p.laterals_at == "every" or last
            if order == 0 and q[1] < p.trunk_start:
                makes_laterals = False
            if makes_laterals:
                phase = self._laterals(node, d, order, phase)
        self._new_buds.append(dict(node=node, dir=d, terminal=True, order=order,
                                   born=self.year, phase=phase))

    def _laterals(self, node, d, order, phase):
        p = self.p
        if p.side_pairs and order >= 1:
            # Left and right of the shoot, about level, as a conifer's sprays lie.
            up = np.array([0.0, 1.0, 0.0])
            side = np.cross(d, up)
            ns = np.linalg.norm(side)
            side = side / ns if ns > 1e-6 else np.array([1.0, 0.0, 0.0])
            th = math.radians(p.branch_angle * 0.7 + self.rng.normal() * 6.0)
            for sgn in (1.0, -1.0):
                bd = d * math.cos(th) + sgn * side * math.sin(th)
                self._new_buds.append(dict(node=node, dir=bd / np.linalg.norm(bd),
                                           terminal=False, order=order + 1, born=self.year,
                                           phase=0))
            return phase
        if p.phyllotaxis == "opposite":
            angles = [phase * math.pi / 2, phase * math.pi / 2 + math.pi]
            phase += 1
        elif p.phyllotaxis == "whorl":
            off = self.rng.random() * 2 * math.pi
            angles = [off + 2 * math.pi * i / p.whorl for i in range(p.whorl)]
        else:
            angles = [phase * GOLDEN]
            phase += 1
        a = np.array([0.0, 1.0, 0.0]) if abs(d[1]) < 0.9 else np.array([1.0, 0.0, 0.0])
        u = np.cross(d, a)
        u /= np.linalg.norm(u)
        v = np.cross(d, u)
        th = math.radians(p.branch_angle + self.rng.normal() * 6.0)
        for ang in angles:
            side = math.cos(ang) * u + math.sin(ang) * v
            bd = d * math.cos(th) + side * math.sin(th)
            self._new_buds.append(dict(node=node, dir=bd / np.linalg.norm(bd), terminal=False,
                                       order=order + 1, born=self.year, phase=0))
        return phase

    def _add(self, q, parent, axis, order):
        self.pos.append(q)
        self.parent.append(parent)
        self.axis.append(axis)
        self.order.append(order if axis else order + 1)
        self.born.append(self.year)
        self.alive.append(True)
        return len(self.pos) - 1

    def _shed(self, q_node, kids):
        p = self.p
        count = len(q_node)
        size = np.zeros(count)
        for n in range(count - 1, -1, -1):
            if self.alive[n]:
                size[n] = 1 + sum(size[c] for c in kids[n])
        for n in range(1, count):
            if (self.alive[n] and not self.axis[n] and self.year - self.born[n] >= p.shed_age
                    and q_node[n] / max(size[n], 1) < p.shed):
                self._kill(n, kids)

    def _kill(self, n, kids):
        stack = [n]
        while stack:
            k = stack.pop()
            if self.alive[k]:
                self.alive[k] = False
                stack.extend(kids[k])

    # --- out -----------------------------------------------------------------------------
    def skeleton(self):
        sk = Skeleton()
        remap = {}
        for n in range(len(self.pos)):
            if not self.alive[n]:
                continue
            par = remap.get(self.parent[n], -1)
            remap[n] = sk.add(self.pos[n], par, self.axis[n])
        sk.pipe_radii(tip=self.p.tip_radius, exponent=self.p.pipe_exponent)
        return sk


def grow(params, seed, ages):
    """Skeletons of one tree at each of `ages` years."""
    t = Tree(params, seed)
    out = {}
    for year in range(1, max(ages) + 1):
        t.step()
        if year in ages:
            out[year] = t.skeleton()
    return out


BROADLEAF = Params(internode=0.25, voxel=0.25, branch_angle=42, phyllotaxis="alternate",
                   apical=0.53, vigour=2.2, max_metamers=2, shade_weight=0.22,
                   tropism=(-0.12, -0.02, 0.03), wobble=0.07, trunk_start=1.2, shed=0.10,
                   shadow_b=2.0, shadow_depth=5, full_light=2.0)

CONIFER = Params(internode=0.25, voxel=0.25, branch_angle=70, phyllotaxis="whorl", whorl=5,
                 side_pairs=True, laterals_at="shoot_end", apical=0.70, vigour=2.0,
                 shade_weight=0.10, tropism=(-0.35, 0.10, 0.06), wobble=0.04, max_metamers=2,
                 shed=0.10, shadow_b=2.0, shadow_depth=5, full_light=2.0, order_vigour=0.7,
                 order_length=0.5, shadow_spread=0.5)
