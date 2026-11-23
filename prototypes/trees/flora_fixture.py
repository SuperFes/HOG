"""Writes libs/flora/tests/flora_fixture.hpp: a tiny .flora file made by the exporter, with the
leaf corners it was made from and the draw counts it should give, so the engine's reader is
checked against what the lab writes.

    flora_fixture.py"""

import os
import struct

import numpy as np

import export as ex
import foliage
from render import Material
from test_export import folded_leaves, piece_mesh

PX = (0.002, 0.01, 0.03, 0.1, 1.0)


def leaf_mesh(P):
    k = len(P)
    rng = np.random.default_rng(9)
    N = np.repeat(rng.normal(size=(k, 1, 3)), 6, 1).reshape(-1, 3)
    N /= np.linalg.norm(N, axis=1, keepdims=True)
    C = np.repeat(rng.uniform(0, 1.2, (k, 1, 3)), 6, 1).reshape(-1, 3)
    A = np.repeat(rng.uniform(0, 1, (k, 1, 4)), 6, 1).reshape(-1, 4)
    UV = np.tile(np.array([(0, 0), (1, 0), (0, 1), (1, 1), (0.5, 0), (0.5, 1)], float), (k, 1))
    tri = np.array([(0, 4, 2), (4, 5, 2), (4, 1, 5), (1, 3, 5)])
    idx = (np.arange(k)[:, None, None] * 6 + tri[None]).reshape(-1, 3)
    length = np.linalg.norm((P[:, 2] + P[:, 3] - P[:, 0] - P[:, 1]) / 2, axis=1)
    pivot = np.repeat(((P[:, 0] + P[:, 1]) / 2)[:, None], 6, 1).reshape(-1, 3)
    rank = rng.random(k)
    lod = foliage.thinning("leaf", pivot, np.repeat(length, 6), np.repeat(rank, 6), dims=2)
    mesh = (P.reshape(-1, 3).astype("f4"), N.astype("f4"), C.astype("f4"), idx.astype("u4"),
            UV.astype("f4"), A.astype("f4"))
    order = np.argsort(-ex.drop_of(length, rank, 2, 3.0), kind="stable")
    return mesh, lod, order


def impostor_chunk(w, lo, hi, views=2, cell=4):
    """An impostor of a crown filling its square from every view: leaves facing up, all lit."""
    size = views * cell
    def flat(*value):
        return [np.broadcast_to(np.array(value, float), (size, size, len(value))).copy()]
    ids = [w.texture(flat(0.2, 0.4, 0.1, 1.0), ex.RGBA8_SRGB),
           w.texture(flat(0.5, 1.0, 0.5, 1.0), ex.RGBA8),
           w.texture(flat(0.5, 0.5), ex.RG16),
           w.texture(flat(1.0, 0.5, 0.5, 0.5), ex.RGBA8),
           w.texture(flat(1 / 3, 1 / 3, 0.5, 0.5), ex.RGBA16)]
    centre = (lo + hi) / 2
    radius = float(np.linalg.norm(hi - lo) / 2)
    head = struct.pack("<2I 4f 3f 2f f 5i", views, cell, *centre, radius, *centre,
                       radius / 2, radius / 2, 1.0, *ids)
    return ex.chunk(b"IMPO", head)


def build():
    P = folded_leaves(12, seed=11)
    lmesh, llod, order = leaf_mesh(P)
    wmesh, wlod = piece_mesh(n=20, seed=12)
    pts = np.concatenate([P.reshape(-1, 3), wmesh[0]])
    lo, hi = pts.min(0) - 0.01, pts.max(0) + 0.01
    w = ex.Writer()
    tex = w.texture([np.full((4, 4, 4), 0.5), np.full((2, 2, 4), 0.5)], ex.RGBA8_SRGB)
    leaves, template = ex.leaves_chunk(lmesh, Material(translucency=0.5), llod, lo, hi, None,
                                       (tex, -1))
    mesh = ex.mesh_chunk(ex.WOOD, wmesh, Material(), wlod, lo, hi, None, (tex, -1))
    impostor = impostor_chunk(w, lo, hi)
    textures = w.parts
    data = (ex.MAGIC + struct.pack("<I", ex.VERSION) + bytes(4)
            + ex.species_head("fixture", 1, template) + b"".join(textures)
            + ex.chunk(b"VARI", struct.pack("<2I 3f 3f", 7, 30, *lo, *hi)) + mesh + leaves
            + impostor)
    m = ex.read_mesh(mesh[16:])
    drops = m["pieces"]["drop"].astype(np.float64)
    counts = []
    for px in PX:
        k = int((drops > px).sum())
        counts.append((k, int(m["pieces"]["index_end"][k - 1]) if k else 0))
    leaf_drops = ex.read_leaves(leaves[16:])["leaves"]["drop"].astype(np.float64)
    kept = [int((leaf_drops > px).sum()) for px in PX]
    return data, P[order], counts, kept


def header(data, corners, counts, kept):
    def floats(a):
        return ", ".join(f"{x:.6f}F" for x in np.asarray(a).ravel())
    body = ",".join(("\n    " if i % 16 == 0 else " ") + f"0x{b:02x}" for i, b in enumerate(data))
    return f"""#pragma once

// Made by prototypes/trees/flora_fixture.py; regenerate rather than edit.

#include <array>
#include <cstddef>
#include <cstdint>

// clang-format off
namespace flora_fixture {{

inline constexpr std::array<std::uint8_t, {len(data)}> bytes{{{body}}};

// The leaves' corners as made, in the order they are stored.
inline constexpr std::array<float, {corners.size}> corners{{{floats(corners)}}};

inline constexpr std::array<float, {len(PX)}> metres_per_pixel{{{floats(PX)}}};
// At each: pieces kept and the indices drawing them, and leaves kept.
inline constexpr std::array<std::uint32_t, {len(PX)}> pieces{{{", ".join(str(k) for k, _ in counts)}}};
inline constexpr std::array<std::uint32_t, {len(PX)}> indices{{{", ".join(str(i) for _, i in counts)}}};
inline constexpr std::array<std::uint32_t, {len(PX)}> leaves{{{", ".join(map(str, kept))}}};

}} // namespace flora_fixture
// clang-format on
"""


if __name__ == "__main__":
    out = os.path.join(os.path.dirname(__file__), "../../libs/flora/tests/flora_fixture.hpp")
    with open(out, "w") as f:
        f.write(header(*build()))
    print(out)
