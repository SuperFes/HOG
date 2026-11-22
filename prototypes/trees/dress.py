"""Trees with their foliage: wood and leaves for a species at an age, and sheets of them
whole, close up, and backlit."""

import math
import sys
import time

import numpy as np

import bark
import foliage
import species
import textures
from render import Material, Scene, render, sheet
from skeleton import tube_mesh

SUN = np.array([0.45, 0.75, 0.35])
SUN_YAW = math.degrees(math.atan2(SUN[0], SUN[2]))


def foliage_of(sp, sk, seed, season=0.0):
    """(mesh, material) of a species' foliage on `sk`, and points standing for its area."""
    f = sp.foliage
    rng = np.random.default_rng(seed + 1000)
    colour = tuple(f.get("colour", (0.06, 0.12, 0.03)))
    if sp.kind == "broadleaf":
        tex, mask = textures.leaf(f.get("leaf", "ovate"), colour=colour,
                                  lobes=f.get("lobes", 0), serrate=f.get("serrate", 0.0),
                                  aspect=f.get("aspect", 0.4), turn=f.get("leaf_turn", "edges"),
                                  mask=True)
        made = foliage.leaves(sk, rng, per_segment=f.get("per_segment", 10),
                              size=f.get("size", 0.1), width=f.get("aspect", 0.4) * 2.2,
                              spread=f.get("spread", 0.2), droop=f.get("droop", 0.0),
                              season=season, autumn=f.get("autumn"), base_colour=colour,
                              turn_from=f.get("turn_from"), sun=tuple(SUN))
        mat = Material(tex, translucency=0.55, cutoff=0.5, turn_mask=mask)
        if made is not None:
            mesh, area, twiglets = made
            return mesh, mat, area, tube_mesh(twiglets, colour=(0.13, 0.10, 0.08),
                                                   trunk_first=False)
    elif sp.kind == "conifer":
        tex = textures.needle_spray(colour=colour, needle_length=f.get("needle_length", 0.3),
                                    angle=f.get("angle", 55), density=f.get("density", 400),
                                    tufts=f.get("tufts", False),
                                    brush_tip=f.get("brush_tip", 0.0),
                                    side_shoots=f.get("side_shoots", 0),
                                    size=tuple(f.get("texture", (256, 1024))))
        made = foliage.sprays(sk, rng, shoot_radius=f.get("shoot_radius", 0.03),
                              width=f.get("width", 0.16), droop=f.get("droop", 25),
                              layers=f.get("layers", 2), tips_only=f.get("tips_only", 0.0),
                              repeat=f.get("repeat", 0.45), star=f.get("star", False),
                              needle_years=f.get("needle_years", 0))
        mat = Material(tex, translucency=0.25, cutoff=0.45)
    else:
        tex = textures.frond_half(colour=colour, leaflets=f.get("leaflets", 70),
                                  angle=f.get("angle", 60))
        made = foliage.fronds(sk, rng, leaflet=f.get("leaflet", 0.85), fold=f.get("fold", 28))
        mat = Material(tex, translucency=0.45, cutoff=0.5)
    if made is None:
        return None, None, np.zeros((0, 3)), None
    mesh, area = made
    return mesh, mat, area, None


_bark_cache = {}


def bark_of(sp):
    """A species' bark texture, its tile in metres and its relief, made once."""
    if not sp.bark:
        return None
    if sp.name not in _bark_cache:
        tex, tile = bark.make(sp.bark)
        _bark_cache[sp.name] = (tex, tile, sp.bark.get("relief", 0.01))
    return _bark_cache[sp.name]


def _weathered(wood, grime):
    """Grime splashed dark around the foot of the stem, and moss climbing its shaded north
    side, thinning with height; both broken up so they never band."""
    p, n, c = wood[0], wood[1], wood[2]
    rng = np.random.default_rng(3)
    y = p[:, 1].astype(float)
    ragged = 0.35 * np.sin(p[:, 0] * 7.1 + p[:, 2] * 5.3) + 0.25 * np.sin(p[:, 0] * 17 - y * 3)
    foot = np.clip((0.9 + ragged - y) / 0.9, 0, 1)
    north = np.clip(-n[:, 2] * 0.8 + 0.2, 0, 1)
    moss = north * np.clip((2.5 + ragged * 2 - y) / 2.5, 0, 1) * grime
    out = c * (1 - 0.45 * grime * foot)[:, None]
    out = out * (1 - moss[:, None]) + out * np.array([0.55, 0.85, 0.35]) * moss[:, None]
    return (p, n, out.astype("f4"), *wood[3:])


def dressed(sp, seed, age, season=0.0):
    sk = sp.grow(seed, {age})[age]
    made = bark_of(sp)
    if made is None:
        wood = tube_mesh(sk, colour=(0.13, 0.10, 0.08))
        wood_mat = None
    else:
        tex, tile, relief = made
        if sp.bark.get("tint_by_stem") and getattr(sk, "colour", None) is not None:
            # The texture carries the plates; the stem's own colour (orange high on a pine,
            # grey low down) tints it.
            sk.colour = sk.colour * sp.bark.get("tint_gain", 2.5)
        else:
            # The bark as drawn on trunk and limbs, the twigs their own darker shade of it.
            twig = np.array(sp.bark.get("twig", (0.6, 0.5, 0.45)))
            t = np.clip((sk.radius - 0.012) / 0.05, 0, 1)[:, None]
            sk.colour = twig[None] * (1 - t) + np.ones(3)[None] * t
        wood = tube_mesh(sk, colour=(1.0, 1.0, 1.0), tile=tile)
        wood_mat = Material(tex, cutoff=-1.0, relief=relief)
        wood = _weathered(wood, sp.bark.get("grime", 0.5))
    leaf_mesh, mat, area, twigs = foliage_of(sp, sk, seed, season)
    if len(area) and len(wood[0]):
        # The wood inside the crown sees little sky.
        vis = foliage.sky_visibility(wood[0].astype(float), area, cell=0.4, k=0.3)
        wood = (wood[0], wood[1], (wood[2] * (0.3 + 0.7 * vis)[:, None]).astype("f4"),
                *wood[3:])
    scene = Scene()
    scene.add(wood, wood_mat)
    if twigs is not None:
        scene.add(twigs)
    if leaf_mesh is not None:
        scene.add(leaf_mesh, mat)
    return scene, area


def close_target(area, yaw, height_share=0.6):
    """A point on the crown's edge toward the camera, about `height_share` up it."""
    if len(area) == 0:
        return np.zeros(3)
    toward = np.array([math.sin(math.radians(yaw)), 0.0, math.cos(math.radians(yaw))])
    lo, hi = area[:, 1].min(), area[:, 1].max()
    band = np.abs(area[:, 1] - (lo + (hi - lo) * height_share)) < (hi - lo) * 0.12
    pts = area[band] if band.any() else area
    return pts[np.argmax(pts @ toward)]


if __name__ == "__main__":
    which = sys.argv[1].split(",") if len(sys.argv) > 1 else species.names()
    age = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    rows, labels = [], []
    for name in which:
        t = time.time()
        sp = species.load(name)
        a = 40 if sp.model == "palm" else age
        scene, area = dressed(sp, 1, a)
        tris = sum(len(m[3]) for m, _ in scene.parts)
        front = 35.0
        back = SUN_YAW + 180 + 25
        if sp.kind == "palm":
            top = area[np.argmax(area[:, 1])]
            near, under = top + np.array([0, -1.0, 0]), top + np.array([0, -0.5, 0])
        else:
            near, under = close_target(area, front), close_target(area, back, 0.75)
        row = [render(scene, size=(480, 620), yaw=front),
               render(scene, size=(480, 620), yaw=front, pitch=5,
                      view=(near, 2.6)),
               render(scene, size=(480, 620), yaw=back, pitch=-12,
                      view=(under, 3.5))]
        rows.append(row)
        labels.append(f"{name}\n{a} years\n{tris // 1000}k tris")
        print(f"{name}: {time.time() - t:.0f}s, {tris} triangles", flush=True)
    sheet(rows, "Foliage, round 1: whole tree, close up, and looking up toward the sun",
          labels, ["whole", "close (2.6 m)", "backlit, from below"], "out/foliage.png")
