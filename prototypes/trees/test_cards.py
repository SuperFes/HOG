"""Checks for branch cards: `.venv/bin/python -m unittest test_cards`."""
import unittest

import numpy as np

import cards
from render import Material, Scene


def leaf_scene(n=600, seed=1):
    """Square leaves in two clumps, each leaf facing one of three ways, and a limb."""
    rng = np.random.default_rng(seed)
    P, N, I = [], [], []
    axes = np.eye(3)
    for k in range(n):
        centre = rng.normal(0, 0.3, 3) + (0 if k % 2 else 4.0)
        a = axes[k % 3]
        u, v = axes[(k + 1) % 3] * 0.05, axes[(k + 2) % 3] * 0.05
        b = len(P)
        P += [centre - u - v, centre + u - v, centre + u + v, centre - u + v]
        N += [a] * 4
        I += [(b, b + 1, b + 2), (b, b + 2, b + 3)]
    s = Scene()
    limb = (np.array([[0, -2, 0], [0.1, -2, 0], [0, 0, 0]], "f4"), np.tile([1, 0, 0], (3, 1)).astype("f4"),
            np.ones((3, 3), "f4"), np.array([[0, 1, 2]], np.uint32), None, None,
            {"kind": "wood", "size": np.full(3, 0.2, "f4")})
    s.add(limb, Material(cutoff=-1.0))
    s.add((np.array(P, "f4"), np.array(N, "f4"), np.ones((len(P), 3), "f4"),
           np.array(I, np.uint32), None, None, {"kind": "leaf"}), Material(translucency=0.5))
    return s


class PlanTest(unittest.TestCase):
    def test_every_leaf_lies_on_one_card_parallel_to_it(self):
        s = leaf_scene()
        plan = cards.plan(s, across=1.5)
        seen = np.zeros(len(s.parts[1][0][3]), int)
        p, n, _c, idx = s.parts[1][0][:4]
        for card in plan:
            normal = np.cross(card.right, card.up)
            tris = card.part_tris.get(1, [])
            seen[tris] += 1
            if len(tris):
                self.assertGreater(np.abs(n[idx[tris, 0]] @ normal).min(), 0.9)
        np.testing.assert_array_equal(seen, 1)

    def test_limbs_stay_mesh(self):
        plan = cards.plan(leaf_scene(), across=1.5)
        self.assertTrue(all(0 not in c.part_tris for c in plan))

    def test_clusters_are_compact(self):
        rng = np.random.default_rng(2)
        pts = np.concatenate([rng.normal(0, 0.2, (500, 3)), rng.normal(5, 0.2, (500, 3))])
        label = cards.clusters(pts, 1.0)
        for c in range(label.max() + 1):
            m = pts[label == c]
            self.assertLess(np.ptp(m, axis=0).max(), 2.0)


if __name__ == "__main__":
    unittest.main()
