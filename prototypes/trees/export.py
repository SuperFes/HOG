"""Bakes species into .flora files the engine loads: per species its textures and a set of
variants (seeds at ages), each with its meshes, leaves, branch cards and impostor. The layout
is in docs/flora-assets.md.

    export.py [species ...] [--seeds N] [--ages A,B] [--impostors N] [--out DIR]
    (default: every species, 8 seeds at age 30, impostors for the first 4, into var/flora)"""

import argparse
import hashlib
import os
import struct
import sys
import time

import numpy as np

import lod as lodmod

MAGIC = b"HOGFLORA"
VERSION = 1
COLOUR_RANGE = 2.0  # stored colours are linear colour / COLOUR_RANGE, sRGB encoded

# Texture formats.
RGBA8_SRGB, RGBA8, RG16, R8, RGBA16 = 1, 2, 3, 4, 5
# Mesh roles.
WOOD, FOLIAGE, CARD = 1, 2, 3
FALLEN = 1  # leaf flag: lying on the ground, unfolded
CARD_TEXEL = 0.04  # metres
# Branch cards at coarser and coarser levels, each built of clusters this many metres across,
# with limbs as mesh only where at least as wide as cards.LIMB times it, and texels as much
# coarser: about a quarter of the cards each step.
CARD_LEVELS = (1.0, 2.0, 4.0)


# --- packing -------------------------------------------------------------------------------

def srgb8(c):
    c = np.clip(np.asarray(c, np.float64), 0.0, 1.0)
    s = np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)
    return np.round(s * 255).astype(np.uint8)


def from_srgb8(b):
    s = np.asarray(b, np.float64) / 255
    return np.where(s <= 0.04045, s / 12.92, np.power((s + 0.055) / 1.055, 2.4))


def unorm8(x):
    return np.round(np.clip(x, 0, 1) * 255).astype(np.uint8)


def unorm16(x):
    return np.round(np.clip(x, 0, 1) * 65535).astype(np.uint16)


def snorm8(x):
    return np.round(np.clip(x, -1, 1) * 127).astype(np.int8)


def snorm16(x):
    return np.round(np.clip(x, -1, 1) * 32767).astype(np.int16)


def oct_encode(n):
    """Unit vectors to octahedral coordinates in [-1, 1]^2."""
    n = np.asarray(n, np.float64)
    n = n / np.maximum(np.abs(n).sum(-1, keepdims=True), 1e-12)
    x, y, z = n[..., 0], n[..., 1], n[..., 2]
    fx = np.where(y >= 0, x, (1 - np.abs(z)) * np.sign(x + (x == 0)))
    fz = np.where(y >= 0, z, (1 - np.abs(x)) * np.sign(z + (z == 0)))
    return np.stack([fx, fz], -1)


def oct_decode(e):
    e = np.asarray(e, np.float64)
    x, z = e[..., 0], e[..., 1]
    y = 1 - np.abs(x) - np.abs(z)
    t = np.maximum(-y, 0)
    x = x - np.sign(x + (x == 0)) * t
    z = z - np.sign(z + (z == 0)) * t
    n = np.stack([x, y, z], -1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


def quat_of(frame):
    """Rotation matrices (columns: across, way, face) to unit quaternions (x, y, z, w), w >= 0."""
    m = np.asarray(frame, np.float64)
    tr = m[:, 0, 0] + m[:, 1, 1] + m[:, 2, 2]
    q = np.zeros((len(m), 4))
    for i in range(len(m)):
        a = m[i]
        if tr[i] > 0:
            s = np.sqrt(tr[i] + 1) * 2
            q[i] = [(a[2, 1] - a[1, 2]) / s, (a[0, 2] - a[2, 0]) / s, (a[1, 0] - a[0, 1]) / s, s / 4]
        elif a[0, 0] > a[1, 1] and a[0, 0] > a[2, 2]:
            s = np.sqrt(1 + a[0, 0] - a[1, 1] - a[2, 2]) * 2
            q[i] = [s / 4, (a[0, 1] + a[1, 0]) / s, (a[0, 2] + a[2, 0]) / s, (a[2, 1] - a[1, 2]) / s]
        elif a[1, 1] > a[2, 2]:
            s = np.sqrt(1 + a[1, 1] - a[0, 0] - a[2, 2]) * 2
            q[i] = [(a[0, 1] + a[1, 0]) / s, s / 4, (a[1, 2] + a[2, 1]) / s, (a[0, 2] - a[2, 0]) / s]
        else:
            s = np.sqrt(1 + a[2, 2] - a[0, 0] - a[1, 1]) * 2
            q[i] = [(a[0, 2] + a[2, 0]) / s, (a[1, 2] + a[2, 1]) / s, s / 4, (a[1, 0] - a[0, 1]) / s]
    q *= np.where(q[:, 3:] < 0, -1, 1)
    return q / np.linalg.norm(q, axis=1, keepdims=True)


def quat_matrix(q):
    q = np.asarray(q, np.float64)
    q = q / np.linalg.norm(q, axis=-1, keepdims=True)
    x, y, z, w = q[..., 0], q[..., 1], q[..., 2], q[..., 3]
    return np.stack([
        np.stack([1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)], -1),
        np.stack([2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)], -1),
        np.stack([2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)], -1)], -2)


def quantize(p, lo, hi):
    return unorm16((np.asarray(p) - lo) / np.maximum(hi - lo, 1e-6))


def dequantize(q, lo, hi):
    return lo + q.astype(np.float64) / 65535 * (hi - lo)


def colour8(c):
    return srgb8(np.asarray(c) / COLOUR_RANGE)


def sunvis8(sv):
    """c0 in [0, 1], c1 within +-0.5 (fits are kept in range), as unorm8."""
    sv = np.asarray(sv, np.float64)
    return unorm8(np.concatenate([sv[:, :1], sv[:, 1:] + 0.5], 1))


def drop_of(size, rank, dims, target):
    """Metres a pixel spans when a piece drops out: it is kept while rank < (size / (px *
    target))^dims (lod.kept), so for px under size / (target * rank^(1/dims))."""
    r = np.maximum(np.asarray(rank, np.float64), 1e-9)
    return np.minimum(size / (target * r ** (1.0 / dims)), 65504.0)


# --- leaves --------------------------------------------------------------------------------

def fit_leaves(P, UV, tris):
    """Leaves as a template and per-leaf frames. P: (leaves, 6, 3) corners laid out as
    foliage.leaves makes them (base +- across, tip +- across, folded base, folded tip).
    Returns the template's corners (in across, way, face units of the leaf's length), the
    frames (leaves, 3, 3), lengths, bases and which leaves lie fallen (unfolded)."""
    base = (P[:, 0] + P[:, 1]) / 2
    tip = (P[:, 2] + P[:, 3]) / 2
    way_v = tip - base
    length = np.linalg.norm(way_v, axis=1)
    way = way_v / length[:, None]
    across = P[:, 1] - P[:, 0]
    across -= way * np.sum(across * way, 1, keepdims=True)
    across /= np.linalg.norm(across, axis=1, keepdims=True)
    face = np.cross(across, way)
    rel = P - base[:, None]
    local = np.stack([np.sum(rel * across[:, None], 2), np.sum(rel * way[:, None], 2),
                      np.sum(rel * face[:, None], 2)], 2) / length[:, None, None]
    folded = local[:, 4, 2]
    fallen = folded < 0.5 * np.median(folded)
    template = np.median(local[~fallen], axis=0) if (~fallen).any() else np.median(local, axis=0)
    frame = np.stack([across, way, face], 2)
    return template, frame, length, base, fallen


def leaf_corners(template, frame, length, base, fallen):
    t = np.repeat(template[None], len(base), 0)
    t[fallen, :, 2] = 0.0
    return base[:, None] + np.einsum("nij,nkj->nki", frame, t) * length[:, None, None]


# --- chunks --------------------------------------------------------------------------------

def chunk(tag, payload):
    payload = bytes(payload)
    pad = (-len(payload)) % 16
    return tag + struct.pack("<I", len(payload) + pad) + bytes(8) + payload + bytes(pad)


class Writer:
    def __init__(self):
        self.parts = []
        self.textures = {}  # content hash -> id

    def texture(self, levels, fmt):
        """`levels`: arrays (h, w, channels) already in the stored range, largest first."""
        data = []
        for lv in levels:
            if fmt == RGBA8_SRGB:
                b = np.concatenate([srgb8(lv[..., :3]), unorm8(lv[..., 3:4])], -1)
            elif fmt == RGBA8:
                b = unorm8(lv)
            elif fmt == R8:
                b = unorm8(lv[..., :1])
            elif fmt == RG16:
                b = unorm16(lv[..., :2])
            else:
                b = unorm16(lv)
            raw = np.ascontiguousarray(b).tobytes()
            data.append(raw + bytes((-len(raw)) % 16))
        key = hashlib.sha1(struct.pack("<I", fmt) + b"".join(data)).hexdigest()
        if key in self.textures:
            return self.textures[key]
        tid = len(self.textures)
        self.textures[key] = tid
        h, w = levels[0].shape[:2]
        self.parts.append(chunk(b"TEXR", struct.pack("<6I", tid, w, h, fmt, len(levels), 0)
                                + bytes(8) + b"".join(data)))
        return tid

    def bytes(self, head):
        return MAGIC + struct.pack("<I", VERSION) + bytes(4) + head + b"".join(self.parts)


def mesh_chunk(role, mesh, mat, lod, lo, hi, sunvis, textures, level=0):
    """A mesh with its pieces in drop order and its triangles sorted to match, so every level
    of detail is a prefix of its indices."""
    p, n, c, idx, uv, autumn = mesh
    used = np.unique(idx)
    if len(used) < len(p):
        remap = np.zeros(len(p), np.int64)
        remap[used] = np.arange(len(used))
        p, n, c, idx = p[used], n[used], c[used], remap[idx]
        uv = uv[used] if uv is not None else None
        autumn = autumn[used] if autumn is not None else None
        sunvis = sunvis[used] if sunvis is not None else None
        if lod is not None:
            per_vertex = ("pivot", "size", "rank")
            lod = {k: v[used] if k in per_vertex else v for k, v in lod.items()}
    nv = len(p)
    if lod is None:
        piece_of = np.zeros(nv, np.int64)
        piv = ((lo + hi) / 2)[None]
        size = np.array([1.0])
        drop = np.array([65504.0])
        dims, target = 0, 0.0
    else:
        dims, target = int(lod["dims"]), float(lodmod.TARGET[lod["kind"]])
        key = np.concatenate([lod["pivot"], lod["size"][:, None], lod["rank"][:, None]], 1)
        uniq, first, piece_of = np.unique(key, axis=0, return_index=True, return_inverse=True)
        piece_of = piece_of.ravel()
        piv, size = uniq[:, :3], uniq[:, 3]
        drop = drop_of(size, uniq[:, 4], dims, target)
    order = np.argsort(-drop, kind="stable")
    rank_of = np.empty_like(order)
    rank_of[order] = np.arange(len(order))
    vpiece = rank_of[piece_of]
    tri_piece = vpiece[idx].min(axis=1)  # stays while any corner's piece does
    tri_order = np.argsort(tri_piece, kind="stable")
    idx = idx[tri_order]
    index_end = np.searchsorted(tri_piece[tri_order], np.arange(len(order)), side="right") * 3
    # Neighbourhood means, toward which thinned pieces are calmed (lod._calmed).
    pivots_v = lod["pivot"] if lod is not None else p
    means = [lodmod.local_mean(pivots_v, a) if a is not None else None for a in (n, c, autumn)]
    first_v = np.zeros(len(order), np.int64)
    first_v[vpiece[::-1]] = np.arange(nv)[::-1]
    uv = np.zeros((nv, 2), np.float32) if uv is None else uv
    uv_scale = np.maximum(uv.max(0), 1e-6)
    autumn = np.concatenate([c, np.zeros((nv, 1))], 1) if autumn is None else autumn
    v = np.zeros(nv, dtype=VERTEX)
    v["pos"][:, :3] = quantize(p, lo, hi)
    v["normal"] = snorm16(oct_encode(n))
    v["uv"] = unorm16(uv / uv_scale)
    v["colour"][:, :3] = colour8(c)
    v["autumn"][:, :3] = colour8(autumn[:, :3])
    v["autumn"][:, 3] = unorm8(autumn[:, 3])
    v["sunvis"] = sunvis8(sunvis) if sunvis is not None else unorm8(np.tile([1, .5, .5, .5], (nv, 1)))
    v["piece"] = vpiece
    pc = np.zeros(len(order), dtype=PIECE)
    pc["pivot"][:, :3] = quantize(piv[order], lo, hi)
    pc["size"] = size[order].astype(np.float16)
    pc["drop"] = drop[order].astype(np.float16)
    mn, mc, ma = (m[first_v] if m is not None else None for m in means)
    pc["mean_colour"][:, :3] = colour8(mc)
    if ma is not None:
        pc["mean_autumn"][:, :3] = colour8(ma[:, :3])
        pc["mean_autumn"][:, 3] = unorm8(ma[:, 3])
    else:
        pc["mean_autumn"][:, :3] = colour8(mc)
    mnn = mn / np.maximum(np.linalg.norm(mn, axis=1, keepdims=True), 1e-9)
    pc["mean_normal"] = snorm8(oct_encode(mnn))
    pc["index_end"] = index_end
    albedo, normal_tex = textures
    head = struct.pack("<3I f 2i 3f 2f 3I", role, dims, level, target, albedo, normal_tex,
                       mat.translucency, mat.cutoff, mat.relief, *uv_scale, nv, len(order),
                       idx.size)
    return chunk(b"MESH", head + bytes((-len(head)) % 16) + v.tobytes() + pc.tobytes()
                 + idx.astype("<u4").tobytes())


VERTEX = np.dtype([("pos", "<u2", 4), ("normal", "<i2", 2), ("uv", "<u2", 2),
                   ("colour", "u1", 4), ("autumn", "u1", 4), ("sunvis", "u1", 4),
                   ("piece", "<u4")])
PIECE = np.dtype([("pivot", "<u2", 4), ("size", "<f2"), ("drop", "<f2"),
                  ("mean_colour", "u1", 4), ("mean_autumn", "u1", 4), ("mean_normal", "i1", 2),
                  ("pad", "<u2"), ("index_end", "<u4"), ("pad2", "<u4")])
LEAF = np.dtype([("pivot", "<u2", 3), ("length", "<f2"), ("rotation", "i1", 4),
                 ("drop", "<f2"), ("normal", "i1", 2), ("colour", "u1", 3), ("flags", "u1"),
                 ("autumn", "u1", 4), ("sunvis", "u1", 4), ("mean_colour", "u1", 4),
                 ("mean_autumn", "u1", 4), ("mean_normal", "i1", 2), ("pad", "<u2")])
assert VERTEX.itemsize == 32 and PIECE.itemsize == 32 and LEAF.itemsize == 40


def leaves_chunk(mesh, mat, lod, lo, hi, sunvis, textures):
    """Leaves as template instances in drop order; returns the chunk and the template."""
    p, n, c, idx, uv, autumn = mesh
    k = len(p) // 6
    P = p.reshape(k, 6, 3).astype(np.float64)
    template, frame, length, base, fallen = fit_leaves(P, uv.reshape(k, 6, 2), idx)
    err = np.linalg.norm(leaf_corners(template, frame, length, base, fallen) - P, axis=2).max()
    assert err < 0.005, f"leaves don't fit one template: {err * 1000:.1f} mm off"
    target = float(lodmod.TARGET["leaf"])
    first = np.arange(k) * 6
    drop = drop_of(lod["size"][first], lod["rank"][first], 2, target)
    order = np.argsort(-drop, kind="stable")
    means = [lodmod.local_mean(lod["pivot"], a) for a in (n, c, autumn)]
    L = np.zeros(k, dtype=LEAF)
    sel = first[order]
    L["pivot"] = quantize(base[order], lo, hi)
    L["length"] = length[order].astype(np.float16)
    L["rotation"] = snorm8(quat_of(frame[order]))
    L["drop"] = drop[order].astype(np.float16)
    L["normal"] = snorm8(oct_encode(n[sel]))
    L["colour"] = colour8(c[sel])
    L["flags"] = np.where(fallen[order], FALLEN, 0)
    L["autumn"][:, :3] = colour8(autumn[sel, :3])
    L["autumn"][:, 3] = unorm8(autumn[sel, 3])
    sv = sunvis.reshape(k, 6, 4).mean(1)[order] if sunvis is not None else np.tile([1, 0, 0, 0], (k, 1))
    L["sunvis"] = sunvis8(sv)
    mn, mc, ma = (m[sel] for m in means)
    L["mean_colour"][:, :3] = colour8(mc)
    L["mean_autumn"][:, :3] = colour8(ma[:, :3])
    L["mean_autumn"][:, 3] = unorm8(ma[:, 3])
    L["mean_normal"] = snorm8(oct_encode(mn / np.maximum(np.linalg.norm(mn, axis=1, keepdims=True), 1e-9)))
    albedo, turn = textures
    head = struct.pack("<2I f 2i 2f", k, 0, target, albedo, turn, mat.translucency, mat.cutoff)
    return chunk(b"LEAF", head + bytes((-len(head)) % 16) + L.tobytes()), template


def is_leaves(mesh, lod):
    return (lod is not None and lod["kind"] == "leaf" and len(mesh[0]) % 6 == 0
            and len(mesh[3]) == len(mesh[0]) // 6 * 4 and mesh[4] is not None)


# --- species -------------------------------------------------------------------------------

def material_textures(w, mat, leafy):
    if mat.texture is None:
        return -1, -1
    tex = mat.texture.astype(np.float64)
    albedo = w.texture([tex], RGBA8_SRGB)
    if leafy and mat.turn_mask is not None:
        return albedo, w.texture([np.asarray(mat.turn_mask, np.float64)[..., None]], R8)
    return albedo, -1


def impostor_chunk(w, scene, name):
    import far
    import sunvis
    from impostor import bake, coverage_mips
    scene.sunvis = sunvis.sun_visibility(scene)
    imp = bake(scene, views=6, cell=128)
    c, r, sigma, _cov = far.fit_crown(scene, imp)
    kind = far.match_area(far.Kind(name, imp, c, r, sigma))
    depth = imp.depth.copy()
    depth[..., 1] += 0.5
    sv = imp.sunvis.copy()
    sv[..., 1:] += 0.5
    mo = imp.moment.copy()
    mo[..., 2:] += 0.5
    chains = coverage_mips([imp.albedo, imp.normal, depth, sv, mo])
    ids = [w.texture(chains[0], RGBA8_SRGB), w.texture(chains[1], RGBA8),
           w.texture(chains[2], RG16), w.texture(chains[3], RGBA8),
           w.texture(chains[4], RGBA16)]
    head = struct.pack("<2I 4f 3f 2f f 5i", imp.views, imp.albedo.shape[0] // imp.views,
                       *imp.centre, imp.radius, *kind.crown_centre, *kind.crown_radii,
                       kind.sigma, *ids)
    return chunk(b"IMPO", head)


def variant_chunks(w, name, sp, seed, age, impostor):
    import cards
    from dress import dressed
    scene, _ = dressed(sp, seed, age)
    import sunvis
    sv = sunvis.sun_visibility(scene)
    lo, hi = scene.bounds()
    lo, hi = lo - 0.01, hi + 0.01
    out = [chunk(b"VARI", struct.pack("<2I 3f 3f", seed, age, *lo, *hi))]
    template = None
    for (mesh, mat), lod, s in zip(scene.parts, scene.lod, sv):
        leafy = mat.translucency > 0
        tex = material_textures(w, mat, leafy)
        if is_leaves(mesh, lod):
            c, template = leaves_chunk(mesh, mat, lod, lo, hi, s, tex)
            out.append(c)
        else:
            out.append(mesh_chunk(FOLIAGE if leafy else WOOD, mesh, mat, lod, lo, hi, s, tex))
    for level, across in enumerate(CARD_LEVELS, 1):
        carded, _n = cards.carded(scene, across=across, texel=CARD_TEXEL * across,
                                  limb=cards.LIMB * across)
        for (mesh, mat), lod in zip(carded.parts, carded.lod):
            if mat.normal_map is not None:
                alb = w.texture(mat.levels[0], RGBA8_SRGB)
                nrm = w.texture(mat.levels[1], RGBA8)
                out.append(mesh_chunk(CARD, mesh, mat, None, lo, hi, None, (alb, nrm), level))
            else:
                out.append(mesh_chunk(WOOD, mesh, mat, None, lo, hi, None,
                                      material_textures(w, mat, False), level))
    if impostor:
        out.append(impostor_chunk(w, scene, name))
    return out, template


def species_head(name, variants, template):
    t = np.zeros((6, 3)) if template is None else template
    return chunk(b"SPEC", struct.pack("<32s 2I f I", name.encode()[:32], variants, 0,
                                      COLOUR_RANGE, 0 if template is None else 6)
                 + bytes(12) + t.astype("<f4").tobytes()
                 + np.array([(0, 0), (1, 0), (0, 1), (1, 1), (0.5, 0), (0.5, 1)], "<f4").tobytes()
                 + np.array([0, 4, 2, 4, 5, 2, 4, 1, 5, 1, 3, 5], "<u2").tobytes())


def export(name, seeds, ages, impostors, out_dir):
    import species
    sp = species.load(name)
    w = Writer()
    body, template, n = [], None, 0
    for age in ages:
        for seed in range(1, seeds + 1):
            t = time.time()
            chunks, tp = variant_chunks(w, name, sp, seed, age, seed <= impostors)
            template = tp if tp is not None else template
            body += chunks
            n += 1
            print(f"  {name} seed {seed} age {age}: {sum(map(len, chunks)) / 1e6:.1f} MB, "
                  f"{time.time() - t:.0f}s", flush=True)
    textures = w.parts
    w.parts = []
    data = (MAGIC + struct.pack("<I", VERSION) + bytes(4) + species_head(name, n, template)
            + b"".join(textures) + b"".join(body))
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"{name}.flora")
    with open(path, "wb") as f:
        f.write(data)
    print(f"{path}: {n} variants, {len(textures)} textures, {len(data) / 1e6:.1f} MB", flush=True)
    return path


# --- reading (tests, checks) ---------------------------------------------------------------

def read(path):
    """The chunks of a .flora file as (tag, payload) pairs."""
    data = open(path, "rb").read() if isinstance(path, str) else path
    assert data[:8] == MAGIC, "not a flora file"
    version, = struct.unpack_from("<I", data, 8)
    assert version == VERSION
    pos, out = 16, []
    while pos < len(data):
        tag = data[pos:pos + 4]
        size, = struct.unpack_from("<I", data, pos + 4)
        out.append((tag, data[pos + 16:pos + 16 + size]))
        pos += 16 + size
    return out


def read_mesh(payload):
    head = struct.unpack_from("<3I f 2i 3f 2f 3I", payload)
    role, dims, level, target, albedo, normal, trans, cutoff, relief, us, vs, nv, npc, ni = head
    off = 64
    v = np.frombuffer(payload, VERTEX, nv, off)
    off += nv * VERTEX.itemsize
    pc = np.frombuffer(payload, PIECE, npc, off)
    off += npc * PIECE.itemsize
    idx = np.frombuffer(payload, "<u4", ni, off).reshape(-1, 3)
    return dict(role=role, dims=dims, level=level, target=target, textures=(albedo, normal),
                translucency=trans, cutoff=cutoff, relief=relief, uv_scale=(us, vs),
                vertices=v, pieces=pc, indices=idx)


def read_leaves(payload):
    k, _, target, albedo, turn, trans, cutoff = struct.unpack_from("<2I f 2i 2f", payload)
    return dict(target=target, textures=(albedo, turn), translucency=trans, cutoff=cutoff,
                leaves=np.frombuffer(payload, LEAF, k, 32))


if __name__ == "__main__":
    import species
    ap = argparse.ArgumentParser()
    ap.add_argument("species", nargs="*")
    ap.add_argument("--seeds", type=int, default=8)
    ap.add_argument("--ages", default="30")
    ap.add_argument("--impostors", type=int, default=4)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "../../var/flora"))
    a = ap.parse_args()
    for name in a.species or species.names():
        export(name, a.seeds, [int(x) for x in a.ages.split(",")], a.impostors, a.out)
    sys.exit(0)
