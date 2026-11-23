"""Checks for the .flora exporter: `.venv/bin/python -m unittest test_export`."""
import unittest

import numpy as np

import export as ex
import foliage
import lod as lodmod
from render import Material


def folded_leaves(n, seed=3, width=0.6, fold=0.3, fallen_share=0.1):
    """Leaves laid out as foliage.leaves makes them, some lying fallen and unfolded."""
    rng = np.random.default_rng(seed)
    way = rng.normal(size=(n, 3))
    way /= np.linalg.norm(way, axis=1, keepdims=True)
    face = rng.normal(size=(n, 3))
    face -= way * np.sum(face * way, 1, keepdims=True)
    face /= np.linalg.norm(face, axis=1, keepdims=True)
    across = np.cross(way, face)
    base = rng.uniform(-8, 8, (n, 3)) + [0, 10, 0]
    length = rng.uniform(0.06, 0.16, n)
    half = length * width / 2
    f = np.where(rng.random(n) < fallen_share, 0.0, fold)
    P = np.stack([base - across * half[:, None], base + across * half[:, None],
                  base + way * length[:, None] - across * half[:, None],
                  base + way * length[:, None] + across * half[:, None],
                  base + face * (f * half)[:, None],
                  base + way * length[:, None] + face * (f * half)[:, None]], 1)
    return P


class PackingTest(unittest.TestCase):
    def test_octahedral_normals_round_trip(self):
        rng = np.random.default_rng(1)
        n = rng.normal(size=(2000, 3))
        n /= np.linalg.norm(n, axis=1, keepdims=True)
        back = ex.oct_decode(ex.snorm16(ex.oct_encode(n)) / 32767)
        self.assertLess(np.degrees(np.arccos(np.clip(np.sum(n * back, 1), -1, 1))).max(), 0.01)

    def test_rotations_round_trip(self):
        P = folded_leaves(500)
        _t, frame, *_ = ex.fit_leaves(P, None, None)
        back = ex.quat_matrix(ex.snorm8(ex.quat_of(frame)) / 127)
        self.assertLess(np.abs(back - frame).max(), 0.03)

    def test_colours_keep_their_range(self):
        c = np.array([[0.0, 0.01, 1.17], [1.9, 0.5, 0.002]])
        back = ex.from_srgb8(ex.colour8(c)) * ex.COLOUR_RANGE
        np.testing.assert_allclose(back, c, rtol=0.03, atol=0.002)


class LeafTest(unittest.TestCase):
    def test_leaves_rebuild_from_template_within_a_few_millimetres(self):
        P = folded_leaves(3000)
        template, frame, length, base, fallen = ex.fit_leaves(P, None, None)
        self.assertAlmostEqual(fallen.mean(), 0.1, delta=0.03)
        lo, hi = P.reshape(-1, 3).min(0) - 0.01, P.reshape(-1, 3).max(0) + 0.01
        # As stored: base quantized, rotation snorm8, length half float.
        q_base = ex.dequantize(ex.quantize(base, lo, hi), lo, hi)
        q_frame = ex.quat_matrix(ex.snorm8(ex.quat_of(frame)) / 127)
        q_len = length.astype(np.float16).astype(np.float64)
        back = ex.leaf_corners(template, q_frame, q_len, q_base, fallen)
        self.assertLess(np.linalg.norm(back - P, axis=2).max(), 0.003)


def piece_mesh(n=400, seed=5):
    """Triangles in pieces with pivots, sizes and ranks, as lod.thinned reads them."""
    rng = np.random.default_rng(seed)
    pivots = rng.uniform(-5, 5, (n, 3))
    sizes = rng.uniform(0.01, 0.3, n)
    ranks = rng.random(n)
    P = np.repeat(pivots, 3, 0) + rng.normal(0, 0.05, (n * 3, 3))
    N = np.tile([0.0, 1.0, 0.0], (n * 3, 1))
    C = rng.uniform(0, 1, (n * 3, 3))
    idx = np.arange(n * 3, dtype=np.uint32).reshape(n, 3)
    lod = foliage.thinning("wood", np.repeat(pivots, 3, 0), np.repeat(sizes, 3),
                           np.repeat(ranks, 3), dims=1)
    return (P.astype("f4"), N.astype("f4"), C.astype("f4"), idx, None, None), lod


class MeshTest(unittest.TestCase):
    def test_every_level_of_detail_is_a_prefix(self):
        mesh, lod = piece_mesh()
        lo, hi = mesh[0].min(0) - 0.01, mesh[0].max(0) + 0.01
        data = ex.mesh_chunk(ex.WOOD, mesh, Material(), lod, lo, hi, None, (-1, -1))
        m = ex.read_mesh(data[16:])
        drops = m["pieces"]["drop"].astype(np.float64)
        self.assertTrue(np.all(np.diff(drops) <= 0))
        for px in (0.001, 0.01, 0.03, 0.08, 0.2):
            s = lodmod.kept(lod, px)
            live = int((s[mesh[3]] > 0).any(axis=1).sum())
            k = int((drops > px).sum())
            drawn = int(m["pieces"]["index_end"][k - 1]) // 3 if k else 0
            # Half-float drops may move a piece across px by a hair.
            self.assertLessEqual(abs(drawn - live), 3, px)
            # What a prefix draws is exactly the pieces still kept.
            tri_pieces = m["vertices"]["piece"][m["indices"][:drawn]].min(axis=1)
            self.assertTrue(np.all(tri_pieces < k))


class FileTest(unittest.TestCase):
    def test_chunks_and_textures_round_trip(self):
        w = ex.Writer()
        tex = np.random.default_rng(2).random((8, 8, 4))
        a = w.texture([tex, tex[:4, :4]], ex.RGBA8_SRGB)
        b = w.texture([tex, tex[:4, :4]], ex.RGBA8_SRGB)
        self.assertEqual(a, b)  # deduplicated
        data = w.bytes(ex.species_head("oak", 0, None))
        chunks = ex.read(data)
        self.assertEqual([t for t, _ in chunks], [b"SPEC", b"TEXR"])
        self.assertEqual(chunks[0][1][:3], b"oak")


if __name__ == "__main__":
    unittest.main()
