"""A pine grown a year at a time, by how pines grow:

- the leader puts on one shoot a year and sets a whorl of buds at its tip, so limbs come in
  tiers; it slows with age while the top limbs don't, so old crowns flatten;
- limbs grow from their tips, each year setting a few side shoots there, so foliage gathers
  in clouds at the limbs' ends and the inner wood is bare;
- side shoots live a few years and die, and whole limbs die as the crown rises over them;
- needles last a few years, so only each shoot's newest growth carries them."""

import math
from dataclasses import dataclass

import numpy as np

from skeleton import Skeleton

UP = np.array([0.0, 1.0, 0.0])


@dataclass
class Params:
    leader: float = 0.55  # metres of leader a year when young
    leader_old: float = 0.12  # ... and in old age
    slowing: float = 35.0  # years for the leader to slow most of the way
    whorl: int = 5  # limbs set at each year's whorl
    whorl_v: int = 1
    limb_angle: float = 60.0  # degrees from upright a new limb starts at
    limb_angle_old: float = 80.0  # ... at the crown's foot, where limbs have sagged
    limb_rate: float = 0.42  # a limb's yearly growth over the leader's
    limb_rise: float = 0.10  # how far a limb's tip turns up each year
    side_shoots: int = 2  # side shoots set at a limb's tip each year
    side_angle: float = 45.0
    side_rate: float = 0.55  # a side shoot's growth over its limb's
    side_life: int = 7  # years a side shoot lives (bare inner limbs)
    twig_shoots: int = 2  # third-order shoots each side shoot sets a year
    twig_life: int = 4
    crooked: float = 0.18  # random turn of each year's shoot
    crown_share: float = 0.5  # share of the height the live crown keeps
    crown_share_young: float = 0.95
    needle_years: int = 3
    tip_radius: float = 0.004
    pipe_exponent: float = 2.6
    bark_low: tuple = (0.12, 0.10, 0.09)  # thick grey-brown bark at the foot
    bark_high: tuple = (0.42, 0.20, 0.09)  # thin orange bark up the trunk and limbs


class Shoot:
    __slots__ = ("nodes", "dir", "order", "born", "alive", "base_height", "parent", "kids")

    def __init__(self, node, d, order, born, base_height, parent):
        self.nodes = [node]
        self.dir = d
        self.order = order
        self.born = born
        self.alive = True
        self.base_height = base_height
        self.parent = parent
        self.kids = []
        if parent is not None:
            parent.kids.append(self)


class Pine:
    def __init__(self, p, seed):
        self.p = p
        self.rng = np.random.default_rng(seed)
        self.pos = [np.zeros(3)]
        self.parent = [-1]
        self.axis = [True]
        self.grown = [0]
        self.dead = [False]
        self.year = 0
        self.shoots = [Shoot(0, UP.copy(), 0, 0, 0.0, None)]

    def _node(self, q, parent, axis):
        self.pos.append(q)
        self.parent.append(parent)
        self.axis.append(axis)
        self.grown.append(self.year)
        self.dead.append(False)
        return len(self.pos) - 1

    def _turn(self, d, amount):
        d = d + self.rng.normal(size=3) * amount
        return d / np.linalg.norm(d)

    def _side(self, d, angle, azimuth):
        a = UP if abs(d[1]) < 0.95 else np.array([1.0, 0.0, 0.0])
        u = np.cross(d, a)
        u /= np.linalg.norm(u)
        v = np.cross(d, u)
        side = math.cos(azimuth) * u + math.sin(azimuth) * v
        out = d * math.cos(angle) + side * math.sin(angle)
        return out / np.linalg.norm(out)

    def step(self):
        p = self.p
        self.year += 1
        age = self.year
        slow = math.exp(-age / p.slowing)
        leader_len = p.leader_old + (p.leader - p.leader_old) * slow
        height = max(q[1] for q in self.pos)
        crown_share = p.crown_share + (p.crown_share_young - p.crown_share) * slow
        crown_foot = height * (1 - crown_share)
        new = []
        for s in self.shoots:
            if not s.alive:
                continue
            # Limbs under the live crown die, and with them all they hold.
            if s.order == 1 and s.base_height < crown_foot:
                self._kill(s)
                continue
            if s.order == 2 and age - s.born > p.side_life:
                self._kill(s)
                continue
            if s.order == 3 and age - s.born > p.twig_life:
                self._kill(s)
                continue
            tip = s.nodes[-1]
            if s.order == 0:
                length = leader_len
                d = self._turn(s.dir * 3 + UP, p.crooked * 0.3)
            elif s.order == 1:
                # Limbs keep growing as the leader slows, which flattens an old crown.
                limb_age = age - s.born
                length = (p.leader * p.limb_rate * (0.55 + 0.45 * math.exp(-limb_age / 25.0))
                          * (0.8 + 0.4 * self.rng.random()))
                # Turning up less once a limb lies level, so limbs don't curl upright.
                rise = p.limb_rise * max(0.0, 0.35 - s.dir[1])
                d = self._turn(s.dir + UP * rise, p.crooked)
            else:
                length = (p.leader * p.limb_rate * p.side_rate ** (s.order - 1)
                          * (0.7 + 0.5 * self.rng.random()))
                d = self._turn(s.dir + UP * p.limb_rise * 0.5, p.crooked)
            s.dir = d
            # Each year's shoot in two pieces, so it can bend.
            mid = self._node(self.pos[tip] + d * length * 0.5, tip, True)
            end = self._node(self.pos[mid] + d * length * 0.5, mid, True)
            s.nodes += [mid, end]
            top = self.pos[end]
            if s.order == 0:
                n = max(p.whorl + int(self.rng.integers(-p.whorl_v, p.whorl_v + 1)), 2)
                off = self.rng.random() * 2 * math.pi
                share = min(age / 25.0, 1.0)
                angle = math.radians(p.limb_angle + (p.limb_angle_old - p.limb_angle)
                                     * share * 0.3)
                for k in range(n):
                    az = off + 2 * math.pi * k / n + self.rng.normal() * 0.25
                    new.append(Shoot(end, self._side(UP, angle, az), 1, age, top[1], s))
            elif s.order == 1:
                for k in range(p.side_shoots):
                    az = self.rng.random() * 2 * math.pi
                    new.append(Shoot(end, self._side(d, math.radians(p.side_angle), az), 2,
                                     age, top[1], s))
            elif s.order == 2:
                for k in range(p.twig_shoots):
                    az = self.rng.random() * 2 * math.pi
                    new.append(Shoot(end, self._side(d, math.radians(p.side_angle), az), 3,
                                     age, top[1], s))
        for s in new:
            first = s.nodes[0]
            s.nodes = [first]
            # A new shoot's first piece starts its own axis off the node it grew from.
            q = self.pos[first] + s.dir * 0.02
            n0 = self._node(q, first, False)
            s.nodes.append(n0)
        self.shoots += new
        self.shoots = [s for s in self.shoots if s.alive]

    def _kill(self, s):
        s.alive = False
        for n in s.nodes[1:]:
            self.dead[n] = True
        # What grew from a dead shoot dies too.
        stack = list(s.kids)
        while stack:
            o = stack.pop()
            if o.alive:
                o.alive = False
                for n in o.nodes[1:]:
                    self.dead[n] = True
            stack.extend(o.kids)

    def skeleton(self):
        sk = Skeleton()
        remap = {}
        grown = []
        for n in range(len(self.pos)):
            if self.dead[n] or (self.parent[n] >= 0 and self.parent[n] not in remap):
                continue
            remap[n] = sk.add(self.pos[n], remap.get(self.parent[n], -1), self.axis[n])
            grown.append(self.grown[n])
        sk.pipe_radii(tip=self.p.tip_radius, exponent=self.p.pipe_exponent)
        sk.grown = np.array(grown)
        sk.year = self.year
        # Thick dark bark low on the trunk, thin orange bark higher up and on the limbs.
        h = max(q[1] for q in sk.pos)
        y = np.array([q[1] for q in sk.pos])
        t = np.clip((y / max(h, 1e-3) - 0.25) / 0.35, 0, 1) * np.clip(1 - sk.radius / 0.25, 0, 1)
        t = np.where(sk.radius < 0.015, 0.4, t)
        sk.colour = (np.array(self.p.bark_low)[None] * (1 - t[:, None])
                     + np.array(self.p.bark_high)[None] * t[:, None])
        return sk


def grow(params, seed, ages):
    t = Pine(params, seed)
    out = {}
    for year in range(1, max(ages) + 1):
        t.step()
        if year in ages:
            out[year] = t.skeleton()
    return out
