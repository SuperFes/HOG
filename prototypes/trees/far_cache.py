"""How far the eye may move before a cached far view shows: the far forest drawn once, then
the eye moved; the old frame reprojected by its depth into the new view (and, for scale, the
old frame reused as it is) against a fresh frame, over what lies past `NEAR` metres.

    far_cache.py [view]    (default: summit)"""

import math
import sys

import numpy as np
from PIL import Image, ImageFilter

import far
import far_sheet as fs
import terrain
from render import look_at, perspective

NEAR = 3000.0
MOVES = (5.0, 20.0, 50.0, 200.0)


def matrices(eye, target, size=fs.SIZE):
    return perspective(math.radians(60.0), size[0] / size[1], 1.0, 60000.0) @ look_at(eye, target)


def reproject(img, dist, eye0, target0, eye1, target1, size=fs.SIZE):
    """`img` seen from eye0 (with per-pixel distances), splatted into the view from eye1,
    nearest first; holes take their nearest filled neighbour along the row."""
    w, h = size
    src = np.asarray(img, np.float32)
    ys, xs = np.mgrid[0:h, 0:w]
    ndc = np.stack([(xs + 0.5) / w * 2 - 1, 1 - (ys + 0.5) / h * 2,
                    np.ones_like(xs, float), np.ones_like(xs, float)], -1)
    inv = np.linalg.inv(matrices(eye0, target0))
    far_pts = ndc.reshape(-1, 4) @ inv.T
    d = far_pts[:, :3] / far_pts[:, 3:] - eye0
    d /= np.linalg.norm(d, axis=1, keepdims=True)
    t = dist.reshape(-1)
    live = t < 1e8
    p = eye0 + d[live] * t[live, None]
    c = np.hstack([p, np.ones((len(p), 1))]) @ matrices(eye1, target1).T
    x = ((c[:, 0] / c[:, 3] + 1) / 2 * w).astype(int)
    y = ((1 - c[:, 1] / c[:, 3]) / 2 * h).astype(int)
    ok = (c[:, 3] > 0) & (x >= 0) & (x < w) & (y >= 0) & (y < h)
    depth = np.linalg.norm(p - eye1, axis=1)[ok]
    x, y, col = x[ok], y[ok], src.reshape(-1, 3)[live][ok]
    order = np.argsort(-depth)  # far first, so near ones overwrite
    out = np.full((h, w, 3), np.nan, np.float32)
    out[y[order], x[order]] = col[order]
    sky = np.array(fs.far.SKY) * 255
    for row in out:
        bad = np.isnan(row[:, 0])
        if bad.all():
            row[:] = sky
            continue
        idx = np.where(~bad, np.arange(w), 0)
        np.maximum.accumulate(idx, out=idx)
        first = np.argmax(~bad)
        idx[:first] = first
        row[:] = row[idx]
    return Image.fromarray(np.clip(out + 0.5, 0, 255).astype(np.uint8))


def score(img, ref, mask):
    a, b = np.asarray(img, float), np.asarray(ref, float)
    sharp = np.abs(a - b).mean(-1)[mask].mean()
    blur = np.abs(np.asarray(img.filter(ImageFilter.GaussianBlur(1.5)), float)
                  - np.asarray(ref.filter(ImageFilter.GaussianBlur(1.5)), float)).mean(-1)
    return sharp, blur[mask].mean()


if __name__ == "__main__":
    vname = sys.argv[1] if len(sys.argv) > 1 else "summit"
    land = terrain.Land()
    F = far.Far(land, far.load_kinds())
    view = fs.VIEWS[vname]
    draw = fs.CANDIDATES["crowns"]
    eye0, target0 = view.eye(land), view.target(land)
    img0 = draw(F, view)
    dist0 = F.distances(eye0, target0)
    look = view.look / np.linalg.norm(view.look)
    side = np.cross(look, [0.0, 1.0, 0.0])
    side /= np.linalg.norm(side)
    for name, axis in (("sideways", side), ("forward", look * [1, 0, 1])):
        for move in MOVES:
            at = tuple(np.array(view.at) + axis[[0, 2]] * move)
            # At the same height: flying, so the near ground and trees stay out of the way.
            over = eye0[1] - land.sample(land.height, *at)
            moved = fs.View(at, over, view.look, view.sun)
            eye1, target1 = moved.eye(land), moved.target(land)
            fresh = draw(F, moved)
            mask = F.distances(eye1, target1) > NEAR
            mask &= F.distances(eye1, target1) < 1e8
            re = score(reproject(img0, dist0, eye0, target0, eye1, target1), fresh, mask)
            same = score(img0, fresh, mask)
            print(f"{vname} {name} {move:5.0f} m: reprojected {re[0]:.1f}/{re[1]:.1f}, "
                  f"reused as is {same[0]:.1f}/{same[1]:.1f} (per pixel / blurred 1.5 px)",
                  flush=True)
            if move == MOVES[-1] and name == "sideways":
                reproject(img0, dist0, eye0, target0, eye1, target1).save(
                    "out/far_cache_reprojected.png")
                fresh.save("out/far_cache_fresh.png")
