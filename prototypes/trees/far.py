"""Forests out to the horizon: a land of hills (terrain.Land) drawn with its trees, for
judging far-forest drawing against a truth.

The truth draws every tree as its relit impostor. Trees shade each other and the ground
through their crowns, each crown an ellipsoid of leaves thinning light by its density, traced
from every pixel toward the sun and over the sky: no shadow map holds trees kilometres off.
A tree's own shading is baked into its impostor, so a trace skips the tree it starts in."""

import math
import os

import moderngl
import numpy as np
from PIL import Image

import terrain
from impostor import IMPOSTOR_LIB, Impostor, bake, coverage_mips
from render import _gl, context, look_at, perspective

SKY = (0.60, 0.70, 0.85)  # background, display colour
LIGHT = dict(sun_colour=(3.2, 2.95, 2.6), sky_colour=(0.55, 0.70, 1.0),
             ground_colour=(0.18, 0.20, 0.12), exposure=1.0)
MEADOW = (0.075, 0.10, 0.045)
MATRICES = {"view_proj", "inv_view_proj", "light_proj"}
LITTER = (0.06, 0.05, 0.03)


class Kind:
    """A tree kind as the far drawing needs it: impostor, and crown as an ellipsoid of
    revolution (centre, horizontal and vertical radii) with the leaf density thinning
    light through it."""

    def __init__(self, name, imp, crown_centre, crown_radii, sigma):
        self.name = name
        self.imp = imp
        self.crown_centre = np.asarray(crown_centre, float)
        self.crown_radii = np.asarray(crown_radii, float)
        self.sigma = float(sigma)


def _mean_transmittance(sigma, depth):
    """Mean light through a sphere-like crown of `depth` metres across, over its disc."""
    rho = (np.arange(64) + 0.5) / 64
    return float((np.exp(-sigma * depth * np.sqrt(1 - rho ** 2)) * 2 * rho).mean())


def fit_crown(scene, imp):
    """The ellipsoid holding the bulk of the leaves, and the density giving it the
    impostor's coverage seen from the side."""
    pts = np.concatenate([p for (p, *_), mat in scene.parts if mat.translucency > 0])
    lo_y, hi_y = np.percentile(pts[:, 1], [4, 97])
    axis = np.array([np.median(pts[:, 0]), np.median(pts[:, 2])])
    rh = float(np.percentile(np.hypot(pts[:, 0] - axis[0], pts[:, 2] - axis[1]), 92))
    centre = np.array([axis[0], (lo_y + hi_y) / 2, axis[1]])
    rv = float((hi_y - lo_y) / 2)
    # Coverage over the crown's outline in the horizon views of the atlas.
    v, cell = imp.views, imp.albedo.shape[0] // imp.views
    covs = []
    for j in range(v):
        for i in range(v):
            u = (i + 0.5) / v * 2 - 1
            w = (j + 0.5) / v * 2 - 1
            if abs(1 - abs((u + w) / 2) - abs((u - w) / 2)) > 0.3:
                continue  # not near the horizon
            a = imp.albedo[j * cell:(j + 1) * cell, i * cell:(i + 1) * cell, 3]
            s = (np.arange(cell) + 0.5) / cell * 2 - 1
            X, Y = np.meshgrid(s * imp.radius, s * imp.radius)
            Y = Y + imp.centre[1] - centre[1]
            inside = (X / rh) ** 2 + (Y / rv) ** 2 < 1
            covs.append(a[inside].mean())
    cov = float(np.mean(covs))
    lo, hi = 0.0, 20.0
    for _ in range(40):
        mid = (lo + hi) / 2
        if 1 - _mean_transmittance(mid, 2 * rh) < cov:
            lo = mid
        else:
            hi = mid
    return centre, (rh, rv), lo, cov


def coverage_areas(imp):
    """The impostor's covered area (m^2) seen from the side and from above, averaged over
    the baked views near the horizon and near the top."""
    v, cell = imp.views, imp.albedo.shape[0] // imp.views
    texel = (2 * imp.radius / cell) ** 2
    side, top = [], []
    for j in range(v):
        for i in range(v):
            u, w = (i + 0.5) / v * 2 - 1, (j + 0.5) / v * 2 - 1
            y = 1 - abs((u + w) / 2) - abs((u - w) / 2)
            a = imp.albedo[j * cell:(j + 1) * cell, i * cell:(i + 1) * cell, 3].sum() * texel
            if y < 0.3:
                side.append(a)
            elif y > 0.8:
                top.append(a)
    return float(np.mean(side)), float(np.mean(top))


def match_area(kind):
    """Grow the crown ellipsoid until it stops as much light as the impostor covers from the
    side and from above: the bulk-of-leaves fit leaves out the trunk, stray shoots and the
    crown's ragged edge, and far off a forest of such crowns lets too much ground through."""
    side, top = coverage_areas(kind.imp)
    rh, rv = kind.crown_radii
    for _ in range(30):
        top_now = np.pi * rh * rh * (1 - _mean_transmittance(kind.sigma, 2 * rv))
        side_now = np.pi * rh * rv * (1 - _mean_transmittance(kind.sigma, 2 * rh))
        rh *= (top / top_now) ** 0.25
        rv *= (side / side_now) ** 0.5 * (top_now / top) ** 0.25
    kind.crown_radii = np.array([rh, rv])
    return kind


def load_kinds(kinds=(("oak", 30), ("birch", 30), ("spruce", 30), ("beech", 30)), views=6,
               cell=128, cache="out/far_cache"):
    import species
    import sunvis
    from dress import dressed
    os.makedirs(cache, exist_ok=True)
    out = []
    for name, age in kinds:
        path = f"{cache}/{name}_{age}_{views}_{cell}.npz"
        z = np.load(path) if os.path.exists(path) else {}
        if "moment" in z:
            imp = Impostor(z["centre"], float(z["radius"]), views, z["albedo"], z["normal"],
                           z["depth"])
            imp.sunvis = z["sunvis"]
            imp.moment = z["moment"]
            out.append(match_area(Kind(name, imp, z["crown_centre"], z["crown_radii"],
                                       z["sigma"])))
            continue
        scene, _ = dressed(species.load(name), 1, age)
        scene.sunvis = sunvis.sun_visibility(scene)
        imp = bake(scene, views=views, cell=cell)
        c, r, sigma, cov = fit_crown(scene, imp)
        print(f"{name}: crown at {c.round(1)}, radii {np.round(r, 1)}, "
              f"side coverage {cov:.2f}, density {sigma:.2f}/m", flush=True)
        np.savez(path, centre=imp.centre, radius=imp.radius, albedo=imp.albedo,
                 normal=imp.normal, depth=imp.depth, sunvis=imp.sunvis, moment=imp.moment,
                 crown_centre=c,
                 crown_radii=r, sigma=sigma)
        out.append(match_area(Kind(name, imp, c, r, sigma)))
    return out


COMMON = """
uniform sampler2D height_tex;
uniform sampler2D density_tex;
uniform sampler2D kinds_tex;
uniform vec2 origin;
uniform float extent;
uniform float cell;
uniform vec3 crown_c[4];
uniform vec2 crown_r[4];
uniform float crown_sigma[4];
uniform float reach;      // farthest a crown spreads from its tree, metres
uniform float top;        // highest a crown reaches over its tree's foot
uniform vec3 sky_dirs[15];
uniform float sky_weights[15];
uniform vec3 eye;
uniform float earth_radius;
uniform float haze_m;
uniform vec3 sky_display;
uniform bool unshadowed;  // trees and ground in full sun and sky, to judge cover apart
""" + terrain.HASH_GLSL + """
vec3 rot_y(vec3 p, float a) {
    float c = cos(a), s = sin(a);
    return vec3(c * p.x + s * p.z, p.y, -s * p.x + c * p.z);
}

bool tree_at(ivec2 c, out vec3 base, out int kind, out float scale, out float yaw) {
    vec2 xz = origin + (vec2(c) + 0.15 + 0.7 * vec2(unit_hash(c, 1u), unit_hash(c, 2u))) * cell;
    vec2 uv = (xz - origin) / extent;
    kind = 0;
    scale = 1.0;
    yaw = 0.0;
    base = vec3(0.0);
    if (unit_hash(c, 0u) >= texture(density_tex, uv).r) return false;
    vec4 ks = texture(kinds_tex, uv);
    float u = unit_hash(c, 3u);
    kind = u < ks.x ? 0 : u < ks.x + ks.y ? 1 : u < ks.x + ks.y + ks.z ? 2 : 3;
    base = vec3(xz.x, texture(height_tex, uv).r, xz.y);
    scale = 0.8 + 0.4 * unit_hash(c, 4u);
    yaw = unit_hash(c, 5u) * 6.2831853;
    return true;
}

// Leaf optical depth from p toward d, out to `limit` metres, through every crown but the
// one in cell `skip`. Cells are walked in order along the ray, column by column across its
// major axis, so once the light is all but gone the walk stops.
float optical_depth(vec3 p, vec3 d, ivec2 skip, float limit) {
    if (unshadowed) return 0.0;
    float tmax = min(limit, (top + 40.0) / max(d.y, 0.05));
    bool xmajor = abs(d.x) >= abs(d.z);
    vec2 a = xmajor ? p.xz : p.zx;
    vec2 dir = xmajor ? d.xz : d.zx;
    vec2 o = xmajor ? origin : origin.yx;
    vec2 b = a + dir * tmax;
    float spread = reach + 0.71 * cell;
    float slope = abs(dir.x) > 1e-6 ? dir.y / dir.x : 0.0;
    int step = dir.x >= 0.0 ? 1 : -1;
    int i0 = int(floor((a.x - float(step) * spread - o.x) / cell));
    int i1 = int(floor((b.x + float(step) * spread - o.x) / cell));
    vec2 ab = b - a;
    float tau = 0.0;
    int i = i0;
    for (int n = 0; n < 48; ++n, i += step) {
        float xa = max(o.x + float(i) * cell - spread, min(a.x, b.x));
        float xb = min(o.x + float(i + 1) * cell + spread, max(a.x, b.x));
        xa = min(xa, xb);
        float za = a.y + slope * (xa - a.x), zb = a.y + slope * (xb - a.x);
        int j0 = int(floor((min(za, zb) - spread - o.y) / cell));
        int j1 = int(floor((max(za, zb) + spread - o.y) / cell));
        for (int j = j0; j <= min(j1, j0 + 24); ++j) {
            ivec2 c = xmajor ? ivec2(i, j) : ivec2(j, i);
            if (c == skip) continue;
            vec2 cc = o + (vec2(i, j) + 0.5) * cell;
            float t = clamp(dot(cc - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
            if (length(cc - a - ab * t) > spread) continue;
            vec3 base;
            int k;
            float sc, yaw;
            if (!tree_at(c, base, k, sc, yaw)) continue;
            vec3 r = vec3(crown_r[k].x, crown_r[k].y, crown_r[k].x) * sc;
            vec3 q = (p - base - rot_y(crown_c[k], yaw) * sc) / r;
            vec3 dd = d / r;
            float qa = dot(dd, dd), qb = dot(q, dd), qc = dot(q, q) - 1.0;
            float disc = qb * qb - qa * qc;
            if (disc <= 0.0) continue;
            float sq = sqrt(disc);
            float t0 = max((-qb - sq) / qa, 0.0), t1 = min((-qb + sq) / qa, tmax);
            tau += crown_sigma[k] * max(t1 - t0, 0.0);
        }
        if (tau > 9.0 || i == i1) break;
    }
    return tau;
}

float sky_open(vec3 p, ivec2 skip) {
    float s = 0.0;
    for (int i = 0; i < 15; ++i) s += exp(-optical_depth(p, sky_dirs[i], skip, 80.0)) * sky_weights[i];
    return s;
}

// The ground's rise per metre along x and z.
vec2 ground_grad(vec2 xz) {
    vec2 uv = (xz - origin) / extent;
    float du = 1.0 / float(textureSize(height_tex, 0).x);
    float texel = extent * du;
    return vec2(texture(height_tex, uv + vec2(du, 0.0)).r - texture(height_tex, uv - vec2(du, 0.0)).r,
                texture(height_tex, uv + vec2(0.0, du)).r - texture(height_tex, uv - vec2(0.0, du)).r)
           / (2.0 * texel);
}

float drop_at(vec3 p) {
    vec2 d = p.xz - eye.xz;
    return dot(d, d) / (2.0 * earth_radius);
}

vec3 hazed(vec3 display, vec3 p) {
    if (haze_m <= 0.0) return display;
    return mix(sky_display, display, exp(-length(p - eye) / haze_m));
}

vec3 tone_map(vec3 c) {
    c = clamp((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0);
    return pow(c, vec3(1.0 / 2.2));
}
"""

CANOPY_LIB = """
uniform vec4 kind_crown[4];   // crown centre height over the foot, horizontal and vertical
                              // radii, and the share of light a crown stops seen through
uniform float crown_reach;    // a typical crown's horizontal radius where rays meet it
uniform vec2 layer_span;      // distances over which trees go from drawn to the layer

uniform vec2 stat_span;       // distances over which the layer's trees become statistics

// Share of the trees at distance d that the layer stands for (the rest are drawn).
float layer_share_at(float d) {
    if (layer_span.y <= 0.0) return 1.0;
    return clamp((d - layer_span.x) / (layer_span.y - layer_span.x), 0.0, 1.0);
}

// Share of them it stands for as statistics, not as each tree's own crown.
float stat_share_at(float d) {
    if (stat_span.y <= 0.0) return layer_share_at(d);
    return min(clamp((d - stat_span.x) / (stat_span.y - stat_span.x), 0.0, 1.0),
               layer_share_at(d));
}
const float S_NODE[3] = float[3](0.845, 1.0, 1.155);
const float S_WEIGHT[3] = float[3](0.27778, 0.44444, 0.27778);

// Per tree per square metre: the rate (per metre of path) at which a ray at height h over
// the ground, going along d, first touches crowns of one size. The crowns it may touch form
// a union of discs (the crown's level slice) about the ray's track; new area comes from the
// disc moving with the ray and from its radius growing.
float meet_rate(float h, vec3 d, float hc, float rh, float rv) {
    float y = (h - hc) / rv;
    if (abs(y) >= 1.0) return 0.0;
    float w = rh * sqrt(1.0 - y * y);
    float ww = -rh * rh * y / rv * d.y;  // w times dw/ds
    float u = length(d.xz);
    if (u * w < 1e-5) return 6.2831853 * max(ww, 0.0);
    float a = ww / (w * u);
    if (a >= 1.0) return 6.2831853 * ww;
    if (a <= -1.0) return 0.0;
    float phi0 = acos(-a);
    return 2.0 * (u * w * sqrt(1.0 - a * a) + ww * phi0);
}

// Light stopped per metre at height h along d, by kind, for `lambda` trees per m^2.
vec4 stop_rate(float h, vec3 d, float lambda, vec4 mix_k) {
    vec4 out_k = vec4(0.0);
    for (int k = 0; k < 4; ++k) {
        if (mix_k[k] <= 0.0) continue;
        vec4 c = kind_crown[k];
        float r = 0.0;
        for (int s = 0; s < 3; ++s)
            r += S_WEIGHT[s] * meet_rate(h, d, c.x * S_NODE[s], c.y * S_NODE[s], c.z * S_NODE[s]);
        out_k[k] = lambda * mix_k[k] * c.w * r;
    }
    return out_k;
}

float lens(float x) {
    if (x >= 1.0) return 0.0;
    return (acos(x) - x * sqrt(1.0 - x * x)) / 1.5707963;
}

// Share of a unit disc lying at or beyond coordinate x along one axis.
float disc_beyond(float x) {
    if (x <= -1.0) return 1.0;
    if (x >= 1.0) return 0.0;
    return (acos(x) - x * sqrt(1.0 - x * x)) / 3.14159265;
}

// The area of ground holding the feet of crowns of one size that a ray from height h0 going
// up along l meets on its way to the sky, exactly, leaving out crowns the start is already
// inside (a point that is seen is not buried in another tree's crown). With heights scaled
// so the crown is a sphere, the feet met lie within the sphere's radius of the ray: a slice
// through the cylinder about it, beyond where it starts, and through the ball about its
// start; those about the start itself are the disc the ball's slice makes there.
float met_area(float h0, vec3 l, float hc, float rh, float rv) {
    float k = rh / rv;
    vec3 e = normalize(vec3(l.x, l.y * k, l.z));
    float up = (hc - h0) * k;  // sphere centre over the ray's start
    float cz = e.y, sz = length(e.xz);
    float tube = 0.0;
    float s_b;  // where, along the ray's heading, the forward half begins
    if (sz < 1e-5) {
        tube = up > 0.0 ? 3.14159265 * rh * rh : 0.0;
        s_b = up > 0.0 ? -1e9 : 1e9;
    } else {
        tube = 3.14159265 * rh * rh / cz * disc_beyond(-up / (rh * sz));
        s_b = -up * cz / sz;
    }
    if (abs(up) < rh) {
        float rho = sqrt(rh * rh - up * up);
        tube -= 3.14159265 * rho * rho * disc_beyond(s_b / rho);
    }
    return max(tube, 0.0);
}

// Light from direction l reaching height h0 in a stand of the same make all round. With
// `toward_eye` given (y > 0), the point is known to be seen from there: crowns the view
// path would have met are known not to be there, and near the point the two paths share
// them, which brightens what is seen looking down-sun (the hotspot).
//
// On a slope (`grad`, the ground's rise per metre) the paths climb out of the canopy as
// fast as they rise over the ground, so a slope facing the light lets more through than
// one facing away; worked in a frame sheared to the slope.
float stand_light(float h0, vec3 l, float lambda, vec4 mix_k, vec3 toward_eye, vec2 grad) {
    if (unshadowed) return 1.0;
    float top_h = top;
    l = vec3(l.x, l.y - dot(grad, l.xz), l.z);
    toward_eye = vec3(toward_eye.x, toward_eye.y - dot(grad, toward_eye.xz), toward_eye.z);
    if (l.y <= 0.02 || h0 >= top_h) return h0 >= top_h ? 1.0 : 0.0;
    l = normalize(l);
    toward_eye = normalize(toward_eye);
    float tau = 0.0;
    for (int k = 0; k < 4; ++k) {
        if (mix_k[k] <= 0.0) continue;
        vec4 c = kind_crown[k];
        float a = 0.0;
        for (int s = 0; s < 3; ++s)
            a += S_WEIGHT[s] * met_area(h0, l, c.x * S_NODE[s], c.y * S_NODE[s], c.z * S_NODE[s]);
        tau += lambda * mix_k[k] * c.w * a;
    }
    if (toward_eye.y > 0.02) {
        // Less what the view path is known to have left empty near the point: the shared
        // share of the light's path, as a ratio over the same steps so their error cancels.
        float sep = length(l.xz / l.y - toward_eye.xz / toward_eye.y);
        float len = (top_h - h0) / l.y;
        float all_r = 0.0, shared_r = 0.0;
        const int N = 24;
        for (int i = 0; i < N; ++i) {
            float dy = (float(i) + 0.5) / float(N) * len * l.y;
            vec4 r = stop_rate(h0 + dy, l, lambda, mix_k);
            float sum = r.x + r.y + r.z + r.w;
            all_r += sum;
            shared_r += sum * lens(dy * sep / (2.0 * crown_reach));
        }
        tau *= 1.0 - shared_r / max(all_r, 1e-9);
    }
    return exp(-max(tau, 0.0));
}

float stand_sky(float h0, float lambda, vec4 mix_k, vec2 grad) {
    float s = 0.0;
    for (int i = 0; i < 15; ++i)
        s += stand_light(h0, sky_dirs[i], lambda, mix_k, vec3(0.0, -1.0, 0.0), grad)
             * sky_weights[i];
    return s;
}
"""

GROUND_VS = """
#version 330
uniform mat4 view_proj;
uniform sampler2D height_tex;
uniform vec2 origin;
uniform float extent;
uniform vec3 eye;
uniform float earth_radius;
in vec2 in_uv;
out vec3 v_pos;
void main() {
    float h = textureLod(height_tex, in_uv, 0.0).r;
    v_pos = vec3(origin.x + in_uv.x * extent, h, origin.y + in_uv.y * extent);
    vec2 d = v_pos.xz - eye.xz;
    gl_Position = view_proj * vec4(v_pos - vec3(0.0, dot(d, d) / (2.0 * earth_radius), 0.0), 1.0);
}
"""

GROUND_FS = """
#version 330
uniform vec3 sun;
uniform vec3 sun_colour;
uniform vec3 sky_colour;
uniform vec3 ground_colour;
uniform float exposure;
uniform int mode;          // 0 truth (trees drawn apart), 1 painted, 2 under a stand
uniform vec3 meadow;
uniform vec3 litter;
uniform vec3 canopy_albedo[4];
uniform bool depth_only;
uniform int debug;  // 4: coverage only, trees white over black ground; 7: sun reaching, 8: sky open, grey
in vec3 v_pos;
out vec4 f_colour;
""" + COMMON + CANOPY_LIB + """
void main() {
    if (depth_only || debug == 4 || (debug >= 1 && debug <= 3)) {
        f_colour = vec4(0.0, 0.0, 0.0, depth_only ? 0.0 : 1.0);
        return;
    }
    vec2 uv = (v_pos.xz - origin) / extent;
    float texel = extent / float(textureSize(height_tex, 0).x);
    float hx = texture(height_tex, uv + vec2(1.0, 0.0) / float(textureSize(height_tex, 0).x)).r
             - texture(height_tex, uv - vec2(1.0, 0.0) / float(textureSize(height_tex, 0).x)).r;
    float hz = texture(height_tex, uv + vec2(0.0, 1.0) / float(textureSize(height_tex, 0).y)).r
             - texture(height_tex, uv - vec2(0.0, 1.0) / float(textureSize(height_tex, 0).y)).r;
    vec3 n = normalize(vec3(-hx, 2.0 * texel, -hz));
    float dens = texture(density_tex, uv).r;
    vec3 base = mix(meadow, litter, clamp(dens * 1.5, 0.0, 1.0));
    float shade = 1.0, sky = 1.0;
    if (mode == 0) {
        shade = exp(-optical_depth(v_pos, sun, ivec2(-1000000), 150.0));
        sky = sky_open(v_pos, ivec2(-1000000));
    } else if (mode == 2) {
        // Drawn trees' own shadows near, the stand's statistics where the layer takes over.
        float share = layer_share_at(length(v_pos - eye));
        if (share < 1.0) {
            shade = exp(-optical_depth(v_pos, sun, ivec2(-1000000), 150.0));
            sky = sky_open(v_pos, ivec2(-1000000));
        }
        if (share > 0.0) {
            float lambda = dens / (cell * cell);
            vec4 mix_k = texture(kinds_tex, uv);
            vec2 grad = vec2(hx, hz) / (2.0 * texel);
            shade = mix(shade, stand_light(0.0, sun, lambda, mix_k, normalize(eye - v_pos), grad),
                        share);
            sky = mix(sky, stand_sky(0.0, lambda, mix_k, grad), share);
        }
    }
    if (debug == 7 || debug == 8) {
        f_colour = vec4(vec3(debug == 7 ? shade : sky), 1.0);
        return;
    }
    vec3 ambient = mix(ground_colour, sky_colour, n.y * 0.5 + 0.5);
    // Under trees the ground is sunflecks and shade, each through the tone curve on its own:
    // tone-mapping their mean comes out brighter than the ground looks.
    vec3 dark = base * ambient * sky;
    vec3 lit = dark + base * sun_colour * max(dot(n, sun), 0.0);
    vec3 shown = tone_map(lit * exposure) * shade + tone_map(dark * exposure) * (1.0 - shade);
    if (mode == 1) {
        vec3 c = base * (sun_colour * max(dot(n, sun), 0.0) * shade + ambient * sky);
        // The forest as paint: canopy colour over the ground by cover, lit as the ground.
        vec4 ks = texture(kinds_tex, uv);
        vec3 alb = canopy_albedo[0] * ks.x + canopy_albedo[1] * ks.y
                 + canopy_albedo[2] * ks.z + canopy_albedo[3] * ks.w;
        vec3 canopy = alb * (sun_colour * max(dot(n, sun), 0.0) + ambient);
        c = mix(c, canopy, clamp(dens * 1.2, 0.0, 1.0));
        shown = tone_map(c * exposure);
    }
    f_colour = vec4(hazed(shown, v_pos), 1.0);
}
"""

TREE_VS = """
#version 330
uniform mat4 view_proj;
uniform vec3 eye;
uniform float earth_radius;
uniform vec3 k_centre[4];
uniform float k_radius[4];
in vec2 in_corner;
in vec4 in_base;   // foot x, y, z; yaw
in vec4 in_info;   // kind, scale, cell i, cell j
out vec3 v_pos;
flat out vec4 v_base;
flat out vec4 v_info;
vec3 rot_y(vec3 p, float a) {
    float c = cos(a), s = sin(a);
    return vec3(c * p.x + s * p.z, p.y, -s * p.x + c * p.z);
}
void main() {
    int k = int(in_info.x);
    vec3 centre = in_base.xyz + rot_y(k_centre[k], in_base.w) * in_info.y;
    float r = k_radius[k] * in_info.y;
    vec3 look = normalize(eye - centre);
    vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), look));
    vec3 up = cross(look, right);
    v_pos = centre + (right * in_corner.x + up * in_corner.y) * r;
    v_base = in_base;
    v_info = in_info;
    vec2 d = v_pos.xz - eye.xz;
    gl_Position = view_proj * vec4(v_pos - vec3(0.0, dot(d, d) / (2.0 * earth_radius), 0.0), 1.0);
}
"""

TREE_FS = """
#version 400
uniform sampler2D atlas_albedo;
uniform sampler2D atlas_normal;
uniform sampler2D atlas_depth;
uniform sampler2D atlas_sunvis;
uniform sampler2D atlas_moment;
uniform int views;
uniform int kinds;
uniform vec3 k_centre[4];
uniform float k_radius[4];
uniform vec3 sun_world;
uniform vec3 sun_colour;
uniform vec3 sky_colour;
uniform vec3 ground_colour;
uniform float exposure;
uniform mat4 view_proj;
uniform bool own_shadow;
uniform int debug;
uniform bool depth_only;
uniform int samples;  // per pixel, multisampled
in vec3 v_pos;
flat in vec4 v_base;
flat in vec4 v_info;
out vec4 f_colour;
// Set per fragment for the shared impostor code: the tree's own frame and atlas.
vec3 centre;
float radius;
vec3 sun;
int kind;
#define ATLAS(c) vec2(((c).x + float(kind)) / float(kinds), (c).y)
#define FETCH(t, c) texture(t, c)
""" + COMMON + IMPOSTOR_LIB + """
void main() {
    kind = int(v_info.x);
    float sc = v_info.y;
    float yaw = v_base.w;
    centre = k_centre[kind];
    radius = k_radius[kind];
    sun = rot_y(sun_world, -yaw);
    // Into the tree's own frame, at its baked size.
    vec3 ro = rot_y(eye - v_base.xyz, -yaw) / sc;
    vec3 rd = normalize(rot_y(v_pos - v_base.xyz, -yaw) / sc - ro);
    vec2 g = encode(normalize(ro - centre));
    vec2 g0 = clamp(floor(g), vec2(0.0), vec2(float(views) - 1.0));
    vec2 f = clamp(g - g0, 0.0, 1.0);
    vec4 sa[4];
    vec4 sn[4];
    vec4 ssv[4];
    vec4 sm[4];
    float syz[4];
    float sk[4];
    float acc_a = 0.0, wsum = 0.0;
    vec3 acc_hit = vec3(0.0);
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            int s = j * 2 + i;
            vec2 gg = min(g0 + vec2(i, j), vec2(float(views) - 1.0));
            float w = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
            vec3 hit;
            sample_view(gg, ro, rd, sa[s], sn[s], ssv[s], sm[s], syz[s], hit);
            sk[s] = w * sa[s].a;
            acc_a += sk[s];
            acc_hit += hit * sk[s];
            wsum += w;
        }
    vec3 local_pos = rot_y(v_pos - v_base.xyz, -yaw) / sc;
    float alpha = impostor_alpha(acc_a / max(wsum, 1e-4), local_pos, 0.5);
    if (alpha <= (depth_only ? 0.5 : 0.0)) discard;
    vec3 hit_local = acc_hit / max(acc_a, 1e-4);
    vec3 world = v_base.xyz + rot_y(hit_local * sc, yaw);
    vec4 clip = view_proj * vec4(world - vec3(0.0, drop_at(world), 0.0), 1.0);
    gl_FragDepth = clip.z / clip.w * 0.5 + 0.5;
    if (depth_only) {
        f_colour = vec4(0.0);
        return;
    }
    ivec2 own = ivec2(v_info.zw);
    // Coverage as a sample mask drawn per tree and pixel: alpha to coverage gives every
    // tree the same mask for the same alpha, so far trees overlapping in a pixel cover only
    // as much as one of them; independent masks add up as leaves do.
    uint hsh = pcg(uint(gl_FragCoord.x) + pcg(uint(gl_FragCoord.y) + cell_hash(own, 9u)));
    int n = int(floor(alpha * float(samples) + float(hsh >> 8u) / 16777216.0));
    int start = int(hsh % uint(samples));
    int mask = 0;
    for (int i = 0; i < n; ++i) mask |= 1 << ((start + i) % samples);
    if (mask == 0) discard;
    gl_SampleMask[0] = mask;
    if (debug == 4) {
        f_colour = vec4(1.0);
        return;
    }
    float shade = exp(-optical_depth(world, sun_world, own, 150.0));
    if (debug == 7) {
        f_colour = vec4(vec3(shade), 1.0);
        return;
    }
    float sky = sky_open(world, own);
    if (debug == 8) {
        f_colour = vec4(vec3(sky), 1.0);
        return;
    }
    float dbg = 0.0;
    vec3 c = light_views(sa, sn, ssv, sm, syz, sk, acc_a, normalize(ro - hit_local), shade, sky,
                         dbg);
    if (debug > 0) {
        f_colour = vec4(vec3(dbg / max(acc_a, 1e-4)), 1.0);
        return;
    }
    f_colour = vec4(hazed(c, world), 1.0);
}
"""



CANOPY_VS = """
#version 330
in vec2 in_corner;
out vec2 v_ndc;
void main() {
    v_ndc = in_corner;
    gl_Position = vec4(in_corner, 0.0, 1.0);
}
"""

SHELL_VS = """
#version 330
uniform mat4 view_proj;
uniform sampler2D height_tex;
uniform vec2 origin;
uniform float extent;
uniform vec3 eye;
uniform float earth_radius;
uniform float top;
in vec2 in_uv;
void main() {
    float h = textureLod(height_tex, in_uv, 0.0).r + top;
    vec3 p = vec3(origin.x + in_uv.x * extent, h, origin.y + in_uv.y * extent);
    vec2 d = p.xz - eye.xz;
    gl_Position = view_proj * vec4(p - vec3(0.0, dot(d, d) / (2.0 * earth_radius), 0.0), 1.0);
}
"""

SHELL_FS = """
#version 330
out vec4 f_colour;
void main() { f_colour = vec4(0.0); }
"""

DIST_FS = """
#version 330
uniform vec3 eye;
in vec3 v_pos;
out vec4 f_colour;
void main() { f_colour = vec4(length(v_pos - eye), 0.0, 0.0, 1.0); }
"""

# The forest as a stand of crowns placed at random by the density field: marched along each
# pixel's ray through the canopy layer, from where it enters to the ground (or past it).
CANOPY_FS = """
#version 330
uniform mat4 inv_view_proj;
uniform sampler2D ground_depth;
uniform sampler2D shell_depth;
uniform sampler2D crown_albedo;
uniform sampler2D crown_normal;
uniform sampler2D crown_sunvis;
uniform sampler2D crown_moment;
uniform sampler2D crown_depth;
uniform sampler2D full_albedo;  // the impostor atlases, for crowns seen in part
uniform sampler2D full_normal;
uniform sampler2D full_sunvis;
uniform sampler2D full_moment;
uniform sampler2D full_depth;
uniform float atlas_cell;       // texels across one baked view
uniform float pixel_angle;  // radians a pixel spans
uniform float grain;        // the stand's pixel-to-pixel spread, per 1/sqrt(trees in a pixel)
uniform int views;
uniform int kinds;
uniform vec3 k_centre[4];
uniform float k_radius[4];
uniform vec3 sun_world;
uniform vec3 sun_colour;
uniform vec3 sky_colour;
uniform vec3 ground_colour;
uniform float exposure;
uniform bool own_shadow;
uniform int debug;
in vec2 v_ndc;
out vec4 f_colour;
vec3 centre;
float radius;
vec3 sun;
int kind;
// Crowns seen in part read the impostor atlases at the mip their pixel spans: neighbouring
// pixels see other trees, so the atlas's own derivatives mean nothing here.
#define atlas_albedo full_albedo
#define atlas_normal full_normal
#define atlas_sunvis full_sunvis
#define atlas_depth full_depth
#define atlas_moment full_moment
float atlas_lod;
#define FETCH(t, c) textureLod(t, c, atlas_lod)
#define ATLAS(c) vec2(((c).x + float(kind)) / float(kinds), (c).y)
""" + COMMON + IMPOSTOR_LIB + CANOPY_LIB + """
vec3 unproject(vec2 ndc, float depth) {
    vec4 p = inv_view_proj * vec4(ndc, depth * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}

// One kind's crown as a far pixel sees it from to_eye_w, the tree turned by yaw: every
// baked view's coverage-weighted average, lit as the impostor is.
vec3 crown_seen(int k, float yaw, vec3 to_eye_w, float shade, float sky) {
    kind = k;
    centre = k_centre[k];
    radius = k_radius[k];
    sun = rot_y(sun_world, -yaw);
    vec3 te = rot_y(to_eye_w, -yaw);
    vec2 g = encode(te);
    vec2 g0 = clamp(floor(g), vec2(0.0), vec2(float(views) - 1.0));
    vec2 f = clamp(g - g0, 0.0, 1.0);
    vec4 sa[4];
    vec4 sn[4];
    vec4 ssv[4];
    vec4 sm[4];
    float syz[4];
    float sk[4];
    float acc = 0.0;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            int s = j * 2 + i;
            vec2 gg = min(g0 + vec2(i, j), vec2(float(views) - 1.0));
            vec2 cell = (gg + 0.5) / float(views);
            sa[s] = texture(crown_albedo, ATLAS(cell));
            sn[s] = texture(crown_normal, ATLAS(cell));
            ssv[s] = texture(crown_sunvis, ATLAS(cell));
            sm[s] = texture(crown_moment, ATLAS(cell));
            syz[s] = texture(crown_depth, ATLAS(cell)).g;
            sk[s] = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y) * sa[s].a;
            acc += sk[s];
        }
    float dbg = 0.0;
    return light_views(sa, sn, ssv, sm, syz, sk, acc, te, shade, sky, dbg);
}

float g_dbg;  // the last crown_at's debug term

// A crown that the ray rd meets at `entry` (world), as the impostor shows it: stepping into
// the crown by each of the four nearest views' depth maps to the leaves seen there, read at
// the mip a pixel spans, and lit as the impostor is with the stand's light where they are.
// Rays grazing a stand see only crown tops, lit, where one colour for the whole crown would
// paint them dark. The leaves seen sit deeper than the crown's outline, and more shaded.
vec3 crown_at(int k, float yaw, float sc, vec3 base, vec3 entry, vec3 rd, float dist,
              float max_lod) {
    kind = k;
    centre = k_centre[k];
    radius = k_radius[k];
    sun = rot_y(sun_world, -yaw);
    vec3 ro = rot_y(entry - base, -yaw) / sc;
    vec3 rl = rot_y(rd, -yaw);
    atlas_lod = clamp(log2(max(dist * pixel_angle * atlas_cell / (2.0 * radius * sc), 1e-3)),
                      0.0, max_lod);
    vec2 g = encode(-rl);
    vec2 g0 = clamp(floor(g), vec2(0.0), vec2(float(views) - 1.0));
    vec2 f = clamp(g - g0, 0.0, 1.0);
    vec4 sa[4];
    vec4 sn[4];
    vec4 ssv[4];
    vec4 sm[4];
    float syz[4];
    float sk[4];
    float acc = 0.0;
    vec3 acc_hit = vec3(0.0);
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            int s = j * 2 + i;
            vec2 gg = min(g0 + vec2(i, j), vec2(float(views) - 1.0));
            vec3 hit;
            sample_view(gg, ro, rl, sa[s], sn[s], ssv[s], sm[s], syz[s], hit);
            sk[s] = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y) * sa[s].a;
            acc += sk[s];
            acc_hit += hit * sk[s];
        }
    vec3 seen = acc > 1e-4 ? base + rot_y(acc_hit / acc * sc, yaw) : entry;
    vec2 uv = (seen.xz - origin) / extent;
    float lambda = texture(density_tex, uv).r / (cell * cell);
    vec4 mix_k = texture(kinds_tex, uv);
    vec2 grad = ground_grad(seen.xz);
    float h = max(seen.y - base.y, 0.0);
    float shade = stand_light(h, sun_world, lambda, mix_k, -rd, grad);
    float sky = stand_sky(h, lambda, mix_k, grad);
    g_dbg = debug == 7 ? shade : sky;
    if (acc < 1e-4) return crown_seen(k, yaw, -rd, shade, sky);
    float dbg = 0.0;
    vec3 c = light_views(sa, sn, ssv, sm, syz, sk, acc, -rl, shade, sky, dbg);
    if (debug >= 1 && debug <= 3) g_dbg = dbg / acc;
    return c;
}

// The same over trees turned every way.
vec3 crown_colour(int k, vec3 to_eye_w, float shade, float sky) {
    vec3 c = vec3(0.0);
    for (int y = 0; y < 4; ++y) c += crown_seen(k, float(y) * 1.5707963 + 0.4, to_eye_w, shade, sky);
    return c * 0.25;
}

// Each tree's crown met between flat-world points a and b (a at eye distance ta), as an
// ellipsoid of leaves at the tree's own place, kind, size and turn, composited front to
// back: the stand's grain and edges, where single trees still show. Trees drawn as
// impostors, and those the statistics stand for, are left out.
void walk_crowns(vec3 a, vec3 b, float ta, vec3 rd, inout vec3 col, inout float T) {
    vec3 d = normalize(b - a);
    float tmax = length(b - a);
    bool xmajor = abs(d.x) >= abs(d.z);
    vec2 a2 = xmajor ? a.xz : a.zx;
    vec2 dir = xmajor ? d.xz : d.zx;
    vec2 o = xmajor ? origin : origin.yx;
    vec2 b2 = a2 + dir * tmax;
    float spread = reach + 0.71 * cell;
    float slope = abs(dir.x) > 1e-6 ? dir.y / dir.x : 0.0;
    int step = dir.x >= 0.0 ? 1 : -1;
    int i0 = int(floor((a2.x - float(step) * spread - o.x) / cell));
    int i1 = int(floor((b2.x + float(step) * spread - o.x) / cell));
    vec2 ab = b2 - a2;
    int i = i0;
    for (int n = 0; n < 64; ++n, i += step) {
        float xa = max(o.x + float(i) * cell - spread, min(a2.x, b2.x));
        float xb = min(o.x + float(i + 1) * cell + spread, max(a2.x, b2.x));
        xa = min(xa, xb);
        float za = a2.y + slope * (xa - a2.x), zb = a2.y + slope * (xb - a2.x);
        int j0 = int(floor((min(za, zb) - spread - o.y) / cell));
        int j1 = int(floor((max(za, zb) + spread - o.y) / cell));
        for (int j = j0; j <= min(j1, j0 + 24); ++j) {
            ivec2 c = xmajor ? ivec2(i, j) : ivec2(j, i);
            vec2 cc = o + (vec2(i, j) + 0.5) * cell;
            float tc = clamp(dot(cc - a2, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
            if (length(cc - a2 - ab * tc) > spread) continue;
            vec3 base;
            int k;
            float sc, yaw;
            if (!tree_at(c, base, k, sc, yaw)) continue;
            float rank = unit_hash(c, 6u);
            float dist = ta + tc * tmax;
            if (rank >= layer_share_at(dist) || rank < stat_share_at(dist)) continue;
            vec3 r = vec3(crown_r[k].x, crown_r[k].y, crown_r[k].x) * sc;
            vec3 q = (a - base - rot_y(crown_c[k], yaw) * sc) / r;
            vec3 dd = d / r;
            float qa = dot(dd, dd), qb = dot(q, dd), qc = dot(q, q) - 1.0;
            float disc = qb * qb - qa * qc;
            if (disc <= 0.0) continue;
            float sq = sqrt(disc);
            float t0 = max((-qb - sq) / qa, 0.0), t1 = min((-qb + sq) / qa, tmax);
            if (t1 <= t0) continue;
            float alpha = 1.0 - exp(-crown_sigma[k] * (t1 - t0));
            if (debug == 4) {
                col += T * alpha;
                T *= 1.0 - alpha;
                continue;
            }
            vec3 cc3 = crown_at(k, yaw, sc, base, a + d * t0, rd, ta + t0, log2(atlas_cell));
            if ((debug >= 1 && debug <= 3) || debug == 7 || debug == 8) cc3 = vec3(g_dbg);
            else cc3 = hazed(cc3, eye + rd * (ta + t0));
            col += T * alpha * cc3;
            T *= 1.0 - alpha;
        }
        if (T < 0.003 || i == i1) break;
    }
}

// A pixel of stand holds a handful of crowns and gaps, never quite the average: noise tied
// to the ground at the pixel's own scale, spread as 1/sqrt(crowns it holds), so it fades as
// pixels take in more trees and never swims as the eye moves.
float lattice(vec2 xz, float size, uint level) {
    vec2 g = xz / size;
    ivec2 i = ivec2(floor(g));
    vec2 f = g - vec2(i);
    f = f * f * (3.0 - 2.0 * f);
    float n00 = unit_hash(i, 40u + level), n10 = unit_hash(i + ivec2(1, 0), 40u + level);
    float n01 = unit_hash(i + ivec2(0, 1), 40u + level), n11 = unit_hash(i + ivec2(1, 1), 40u + level);
    return mix(mix(n00, n10, f.x), mix(n01, n11, f.x), f.y) * 2.0 - 1.0;
}

float stand_noise(vec3 p, float dist, vec3 rd, float lambda) {
    float w = dist * pixel_angle;
    // Across the ray a pixel is w wide; along it, as deep as sight reaches into the stand.
    float deep = min(w / max(abs(rd.y), 0.02), 2.0 * top / max(abs(rd.y), 0.05));
    float crowns = max(lambda * w * deep, 1.0);
    float lv = log2(max(w, 1.0));
    float l0 = floor(lv);
    float n = mix(lattice(p.xz, exp2(l0), uint(l0)), lattice(p.xz, exp2(l0 + 1.0), uint(l0) + 1u),
                  lv - l0);
    return n * 1.7 / sqrt(crowns);
}

void main() {
    vec2 tex_uv = v_ndc * 0.5 + 0.5;
    float zs = texture(shell_depth, tex_uv).r;
    if (zs >= 1.0) discard;
    float zg = texture(ground_depth, tex_uv).r;
    vec3 entry = unproject(v_ndc, zs);
    vec3 rd = normalize(entry - eye);
    float t0 = length(entry - eye);
    // With no ground behind, on until the far plane: a ray grazing one hill may still pass
    // through the trees on a crest many kilometres on.
    float t1 = zg < 1.0 ? length(unproject(v_ndc, zg) - eye) : 60000.0;
    if (t1 <= t0) discard;
    // Statistical crowns: their share of light stopped, and where and which they are.
    float T = 1.0;
    float hit_h = 0.0, hit_h2 = 0.0, hit_t = 0.0;
    vec4 hit_k = vec4(0.0);
    // Explicit crowns, in front.
    vec3 col_e = vec3(0.0);
    float T_e = 1.0;
    float open_t = -1.0;  // where the ray last came into the canopy, while in it
    float t = t0;
    int n_in = 0;
    float min_h = 1e9;
    for (int i = 0; i < 400 && t < t1 && T * T_e > 0.003; ++i) {
        vec3 q = eye + rd * t;
        q.y += drop_at(q);  // back to the flat world the stand lives in
        float above = q.y - texture(height_tex, (q.xz - origin) / extent).r - top;
        // Well over the canopy, stride on: the ground rises no steeper than this.
        float ds = min(max(max(1.0, 0.004 * t), 0.7 * above), t1 - t);
        vec3 p = eye + rd * (t + 0.5 * ds);
        p.y += drop_at(p);
        vec2 uv = (p.xz - origin) / extent;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;  // off the land
        float h = p.y - texture(height_tex, uv).r;
        bool inside = h < top + 2.0 && h > -2.0;
        if (inside && open_t < 0.0 && stat_share_at(t) < 1.0) open_t = t;
        bool last = t + ds >= t1 || i == 399;
        if (open_t >= 0.0 && (!inside || last || (t + ds - open_t) * length(rd.xz) > 300.0)) {
            vec3 a = eye + rd * open_t;
            a.y += drop_at(a);
            vec3 b = eye + rd * (t + ds);
            b.y += drop_at(b);
            walk_crowns(a, b, open_t, rd, col_e, T_e);
            open_t = inside && !last ? t + ds : -1.0;
        }
        min_h = min(min_h, h);
        if (h < top && h > 0.0) {
            ++n_in;
            float lambda = texture(density_tex, uv).r / (cell * cell) * stat_share_at(t);
            vec4 r = stop_rate(h, rd, lambda, texture(kinds_tex, uv));
            float mu = r.x + r.y + r.z + r.w;
            if (mu > 0.0) {
                float hit = T * (1.0 - exp(-mu * ds));
                hit_h += hit * h;
                hit_h2 += hit * h * h;
                hit_t += hit * (t + 0.5 * ds);
                hit_k += hit * r / mu;
                T *= exp(-mu * ds);
            }
        }
        t += ds;
    }
    float covered = 1.0 - T;
    vec3 c = vec3(0.0);
    if (covered > 1e-4) {
        hit_h /= covered;
        hit_t /= covered;
        hit_k /= covered;
        vec3 p = eye + rd * hit_t;
        p.y += drop_at(p);
        vec2 uv = (p.xz - origin) / extent;
        float lambda = texture(density_tex, uv).r / (cell * cell);
        vec4 mix_k = texture(kinds_tex, uv);
        vec2 grad = ground_grad(p.xz);
        // Light climbs faster than linearly with height in the stand, so lighting the mean
        // height met comes out dark: average two heights spanning the spread met.
        float spread = sqrt(max(hit_h2 / covered - hit_h * hit_h, 0.0));
        float shade = 0.0, sky = 0.0;
        for (int s = -1; s <= 1; s += 2) {
            float hs = max(hit_h + float(s) * spread, 0.0);
            shade += 0.5 * stand_light(hs, sun_world, lambda, mix_k, -rd, grad);
            sky += 0.5 * stand_sky(hs, lambda, mix_k, grad);
        }
        for (int k = 0; k < 4; ++k)
            if (hit_k[k] > 0.001) c += hit_k[k] * crown_colour(k, -rd, shade, sky);
        if (debug == 2) c = vec3(shade);
        if (debug == 3) c = vec3(sky);
        c *= 1.0 + grain * stand_noise(p, hit_t, rd, lambda);
        c = hazed(c, p);
        if (debug == 4) c = vec3(1.0);
        if (debug == 7) c = vec3(shade);
        if (debug == 8) c = vec3(sky);
    }
    float alpha = 1.0 - T_e * T;
    if (debug == 6) {
        f_colour = vec4(1.0 - T, float(n_in) / 64.0, clamp(min_h / 64.0 + 0.5, 0.0, 1.0), 1.0);
        return;
    }
    if (alpha < 1e-4) discard;
    f_colour = vec4(col_e + T_e * c * covered, alpha);
}
"""


def crown_views(imp, weighted=True):
    """Per baked view, the crown's colour, normal and sun visibility, and its coverage: the
    tree as one far pixel sees it from there, as the impostor's last mip level holds it
    (plain means, or coverage-weighted). (views, views) per map."""
    v = imp.views
    cell = imp.albedo.shape[0] // v

    def per_view(img):
        return img.reshape(v, cell, v, cell, img.shape[-1])

    cov = per_view(imp.albedo)[..., 3:4]
    w = cov.sum(axis=(1, 3))
    out = []
    for img in (imp.albedo, imp.normal, imp.sunvis, imp.moment, imp.depth):
        if weighted:
            out.append((per_view(img) * cov).sum(axis=(1, 3)) / np.maximum(w, 1e-6))
        else:
            out.append(per_view(img).mean(axis=(1, 3)))
    out[0] = np.concatenate([out[0][..., :3], cov.mean(axis=(1, 3))], -1)
    return out


def sky_directions():
    """Directions over the sky and their cosine weights, for the open-sky share."""
    dirs, weights = [], []
    for elev, count in ((18.0, 6), (45.0, 6), (75.0, 3)):
        e = math.radians(elev)
        for k in range(count):
            a = 2 * math.pi * (k + (0.5 if elev == 45.0 else 0.0)) / count
            dirs.append((math.cos(e) * math.cos(a), math.sin(e), math.cos(e) * math.sin(a)))
            # Each ring stands for a band of the sky, weighted by cosine and its solid angle.
            weights.append(math.sin(e) * math.cos(e) / count)
    w = np.array(weights)
    return np.array(dirs, "f4"), (w / w.sum()).astype("f4")


def tile_projection(proj, tiles, tx, ty):
    """The part of `proj` seen by tile (tx, ty) of tiles x tiles."""
    m = np.eye(4)
    m[0, 0] = m[1, 1] = tiles
    m[0, 3] = tiles - 1 - 2 * tx
    m[1, 3] = tiles - 1 - 2 * ty
    return m @ proj


class Far:
    def __init__(self, land, kinds, weighted_mips=True):
        self.land = land
        self.kinds = kinds
        ctx = self.ctx = context()

        def tex(a, comps):
            h, w = a.shape[:2]
            t = ctx.texture((w, h), comps, np.ascontiguousarray(a, "f4").tobytes(), dtype="f4")
            t.repeat_x = t.repeat_y = False
            return t

        self.height = tex(land.height, 1)
        # The shell over the canopy is drawn on the coarser ground grid, whose triangles cut
        # under the height grid's crests: lift it by the highest ground about each point.
        pad = np.pad(land.height, 2, mode="edge")
        n = land.height.shape[0]
        hmax = np.max([pad[j:j + n, i:i + n] for j in range(5) for i in range(5)], axis=0)
        self.height_max = tex(hmax, 1)
        self.density = tex(land.density, 1)
        self.kind_tex = tex(land.kinds, 4)
        for t in (self.height, self.height_max, self.density, self.kind_tex):
            t.filter = (moderngl.LINEAR, moderngl.LINEAR)
        self.atlases = []
        maps = [np.concatenate([getattr(kd.imp, n) for kd in kinds], axis=1)
                for n in ("albedo", "normal", "depth", "sunvis", "moment")]
        for k, levels in enumerate(coverage_mips(maps) if weighted_mips else
                                   [[m] for m in maps]):
            a = levels[0]
            t = ctx.texture((a.shape[1], a.shape[0]), 4,
                            np.ascontiguousarray(a.astype("f2")).tobytes(), dtype="f2")
            t.build_mipmaps()
            for lvl, m in enumerate(levels[1:], 1):
                t.write(np.ascontiguousarray(m.astype("f2")).tobytes(), level=lvl)
            t.filter = (moderngl.LINEAR_MIPMAP_LINEAR, moderngl.LINEAR)
            self.atlases.append(t)
        self.ground_prog = ctx.program(vertex_shader=GROUND_VS, fragment_shader=GROUND_FS)
        self.tree_prog = ctx.program(vertex_shader=TREE_VS, fragment_shader=TREE_FS)
        n = 1024
        s = (np.arange(n + 1) / n).astype("f4")
        U, V = np.meshgrid(s, s)
        uv = np.stack([U.ravel(), V.ravel()], 1)
        q = np.arange(n * (n + 1)).reshape(n, n + 1)[:, :n].ravel()
        idx = np.stack([q, q + 1, q + n + 1, q + 1, q + n + 2, q + n + 1], 1).astype("u4")
        self.ground_vbo = ctx.buffer(uv.tobytes())
        self.ground_ibo = ctx.buffer(idx.tobytes())
        self.ground_vao = ctx.vertex_array(self.ground_prog, [(self.ground_vbo, "2f", "in_uv")],
                                           self.ground_ibo)
        quad = np.array([-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1], "f4")
        self.quad = ctx.buffer(quad.tobytes())
        self.shell_prog = ctx.program(vertex_shader=SHELL_VS, fragment_shader=SHELL_FS)
        self.shell_vao = ctx.vertex_array(self.shell_prog, [(self.ground_vbo, "2f", "in_uv")],
                                          self.ground_ibo)
        self.canopy_prog = ctx.program(vertex_shader=CANOPY_VS, fragment_shader=CANOPY_FS)
        self.canopy_vao = ctx.vertex_array(self.canopy_prog, [(self.quad, "2f", "in_corner")])
        self.crowns = []
        views = [crown_views(kd.imp, weighted_mips) for kd in kinds]
        for k in range(5):
            a = np.concatenate([v[k] for v in views], axis=1)
            t = ctx.texture((a.shape[1], a.shape[0]), 4, np.ascontiguousarray(a, "f4").tobytes(),
                            dtype="f4")
            t.filter = (moderngl.NEAREST, moderngl.NEAREST)
            self.crowns.append(t)

    def distances(self, eye, target, size=(1920, 1080), fov_deg=60.0):
        """Per pixel (top row first), metres from the eye to the ground; 1e9 for sky."""
        ctx = self.ctx
        if not hasattr(self, "dist_prog"):
            self.dist_prog = ctx.program(vertex_shader=GROUND_VS, fragment_shader=DIST_FS)
            self.dist_vao = ctx.vertex_array(self.dist_prog, [(self.ground_vbo, "2f", "in_uv")],
                                             self.ground_ibo)
        w, h = size
        proj = perspective(math.radians(fov_deg), w / h, 1.0, 60000.0)
        vp = proj @ look_at(np.asarray(eye, float), np.asarray(target, float))
        self.height.use(0)
        self._uniforms(self.dist_prog, dict(view_proj=vp, eye=tuple(eye), height_tex=0,
                                            origin=tuple(terrain.ORIGIN),
                                            extent=terrain.EXTENT,
                                            earth_radius=terrain.EARTH_RADIUS))
        tex = ctx.texture((w, h), 1, dtype="f4")
        z = ctx.depth_renderbuffer((w, h))
        fbo = ctx.framebuffer(tex, z)
        fbo.use()
        fbo.clear(1e9, depth=1.0)
        ctx.enable(moderngl.DEPTH_TEST)
        self.dist_vao.render()
        out = np.frombuffer(tex.read(), np.float32).reshape(h, w)[::-1].copy()
        for r in (fbo, z, tex):
            r.release()
        return out

    def _uniforms(self, prog, values):
        for k, v in values.items():
            if k not in prog:
                continue
            if k in MATRICES:
                prog[k].write(_gl(v))
            elif isinstance(v, np.ndarray):
                # The compiler may keep only the leading elements of an array it uses.
                v = np.asarray(v, "f4").reshape(len(v), -1)[:prog[k].array_length]
                prog[k].write(np.ascontiguousarray(v).tobytes())
            else:
                prog[k].value = v

    def _bins(self, trees, view_proj, proj, grid):
        """Per cell of a grid x grid split of the screen, the trees whose quads reach it."""
        kinds = self.kinds
        cy = np.array([kd.imp.centre[1] for kd in kinds])[trees["kind"]] * trees["scale"]
        r = np.array([kd.imp.radius for kd in kinds])[trees["kind"]] * trees["scale"]
        c = np.hstack([trees["pos"] + np.stack([0 * cy, cy, 0 * cy], 1),
                       np.ones((len(cy), 1))]) @ view_proj.T
        wc = np.maximum(c[:, 3], 1.0)
        rx = r * proj[0, 0] / wc * 1.1
        ry = r * proj[1, 1] / wc * 1.1

        def cells(lo, hi):
            return (np.clip(np.floor((lo + 1) / 2 * grid), 0, grid - 1).astype(int),
                    np.clip(np.floor((hi + 1) / 2 * grid), 0, grid - 1).astype(int))
        gx0, gx1 = cells(c[:, 0] / wc - rx, c[:, 0] / wc + rx)
        gy0, gy1 = cells(c[:, 1] / wc - ry, c[:, 1] / wc + ry)
        nx, ny = gx1 - gx0 + 1, gy1 - gy0 + 1
        count = nx * ny
        idx = np.repeat(np.arange(len(cy)), count)
        k = np.arange(count.sum()) - np.repeat(np.cumsum(count) - count, count)
        gx = gx0[idx] + k % nx[idx]
        gy = gy0[idx] + k // nx[idx]
        key = gy * grid + gx
        order = np.argsort(key, kind="stable")
        key, idx = key[order], idx[order]
        starts = np.flatnonzero(np.r_[True, key[1:] != key[:-1]])
        ends = np.r_[starts[1:], len(key)]
        return {(int(key[a] % grid), int(key[a] // grid)): idx[a:b] for a, b in zip(starts, ends)}

    def frame(self, eye, target, trees=None, size=(1920, 1080), fov_deg=60.0,
              sun=(0.45, 0.75, 0.35), mode=0, supersample=1, haze_m=25000.0, debug=0, chunks=8,
              layer=None, stat=None, grain=0.0, unshadowed=False):
        """A view, `trees` (terrain.Land.trees arrays) drawn as impostors, with the canopy
        layer standing for the rest when `layer` = (near, far) is given: the share of trees
        it stands for rises from 0 at `near` to 1 at `far` ((0, 0): the layer is every tree).
        Of those, each tree's own crown is traced until `stat` = (near, far) hands them to the
        stand's statistics (None: statistics throughout).
        Rendered in tiles at `supersample` times the size, then shrunk in display space."""
        ctx = self.ctx
        self.timings = {}  # GPU nanoseconds by pass
        eye = np.asarray(eye, float)
        sun = np.asarray(sun, float) / np.linalg.norm(sun)
        kinds = self.kinds
        dirs, weights = sky_directions()
        reach = max(kd.crown_radii[0] for kd in kinds) * 1.2
        top = max(kd.crown_centre[1] + kd.crown_radii[1] for kd in kinds) * 1.2
        canopy = np.array([kd.imp.albedo[..., :3][kd.imp.albedo[..., 3] > 0.5].mean(0)
                           for kd in kinds])
        common = dict(origin=tuple(terrain.ORIGIN), extent=terrain.EXTENT, cell=terrain.CELL,
                      crown_c=np.array([kd.crown_centre for kd in kinds]),
                      crown_r=np.array([kd.crown_radii for kd in kinds]),
                      crown_sigma=np.array([kd.sigma for kd in kinds]), reach=reach, top=top,
                      sky_dirs=dirs, sky_weights=weights, eye=tuple(eye),
                      earth_radius=terrain.EARTH_RADIUS, haze_m=haze_m, sky_display=SKY,
                      height_tex=0, density_tex=1, kinds_tex=2, debug=debug,
                      unshadowed=unshadowed, **LIGHT)
        self.height.use(0)
        self.density.use(1)
        self.kind_tex.use(2)
        # Atlases: albedo, normal, depth, sunvis at 3-6, moment at 13; crown views
        # albedo, normal, sunvis at 7-9, moment and depth at 14-15.
        for unit, t in zip((3, 4, 5, 6, 13), self.atlases):
            t.use(unit)
        gp, tp = self.ground_prog, self.tree_prog
        kc = np.array([[kd.crown_centre[1], kd.crown_radii[0], kd.crown_radii[1],
                        1 - _mean_transmittance(kd.sigma, 2 * kd.crown_radii[0])]
                       for kd in kinds])
        common.update(kind_crown=kc, crown_reach=float(kc[:, 1].mean() * 0.8),
                      layer_span=tuple(float(x) for x in (layer or (0.0, 0.0))),
                      stat_span=tuple(float(x) for x in (stat or (0.0, 0.0))))
        self._uniforms(gp, dict(common, sun=tuple(sun), mode=2 if layer else mode,
                                meadow=MEADOW, litter=LITTER, canopy_albedo=canopy,
                                depth_only=False))
        cp = self.canopy_prog
        self._uniforms(cp, dict(common, sun_world=tuple(sun), views=kinds[0].imp.views,
                                kinds=len(kinds),
                                k_centre=np.array([kd.imp.centre for kd in kinds]),
                                k_radius=np.array([kd.imp.radius for kd in kinds]),
                                crown_albedo=7, crown_normal=8, crown_sunvis=9,
                                crown_moment=14, crown_depth=15, full_moment=13, full_depth=5,
                                own_shadow=True,
                                ground_depth=10, shell_depth=11, grain=float(grain),
                                full_albedo=3, full_normal=4, full_sunvis=6,
                                atlas_cell=float(kinds[0].imp.albedo.shape[0]
                                                 // kinds[0].imp.views),
                                pixel_angle=2 * math.tan(math.radians(fov_deg) / 2)
                                / (size[1] * supersample)))
        for unit, t in zip((7, 8, 9, 14, 15), self.crowns):
            t.use(unit)
        self._uniforms(self.shell_prog, dict(common, height_tex=12))
        self.height_max.use(12)
        self._uniforms(tp, dict(common, sun_world=tuple(sun), views=kinds[0].imp.views,
                                kinds=len(kinds),
                                k_centre=np.array([kd.imp.centre for kd in kinds]),
                                k_radius=np.array([kd.imp.radius for kd in kinds]),
                                atlas_albedo=3, atlas_normal=4, atlas_depth=5, atlas_sunvis=6,
                                atlas_moment=13,
                                own_shadow=True, depth_only=False, samples=4))
        tree_vao = inst = None
        w, h = size
        proj = perspective(math.radians(fov_deg), w / h, 1.0, 60000.0)
        view = look_at(eye, np.asarray(target, float))
        view_proj = proj @ view
        tiles = supersample
        grid = tiles * chunks
        if trees is not None and len(trees["kind"]):
            data = np.hstack([trees["pos"], trees["yaw"][:, None], trees["kind"][:, None],
                              trees["scale"][:, None], trees["cell"]]).astype("f4")
            bins = self._bins(trees, view_proj, proj, grid)
            inst = ctx.buffer(reserve=max(len(b) for b in bins.values()) * 32 if bins else 32)
            tree_vao = ctx.vertex_array(tp, [(self.quad, "2f", "in_corner"),
                                             (inst, "4f 4f/i", "in_base", "in_info")])
        colour = ctx.renderbuffer((w, h), components=4, samples=4)
        zbuf = ctx.depth_renderbuffer((w, h), samples=4)
        fbo = ctx.framebuffer(colour, zbuf)
        out_tex = ctx.texture((w, h), 4)
        resolved = ctx.framebuffer(out_tex)
        if layer:
            gdepth = ctx.depth_texture((w, h))
            sdepth = ctx.depth_texture((w, h))
            for t in (gdepth, sdepth):
                t.compare_func = ""
                t.filter = (moderngl.NEAREST, moderngl.NEAREST)
            gfbo = ctx.framebuffer(depth_attachment=gdepth)
            sfbo = ctx.framebuffer(depth_attachment=sdepth)
            gdepth.use(10)
            sdepth.use(11)
        big = np.zeros((h * tiles, w * tiles, 3), np.uint8)

        def pieces():
            # Small pieces, each finished before the next: one long draw trips the GPU's
            # hang watchdog.
            for cy in range(chunks):
                for cx in range(chunks):
                    x0, y0 = cx * w // chunks, cy * h // chunks
                    ctx.scissor = (x0, y0, (cx + 1) * w // chunks - x0,
                                   (cy + 1) * h // chunks - y0)
                    yield cx, cy
                    ctx.finish()
            ctx.scissor = None

        for ty in range(tiles):
            for tx in range(tiles):
                vp = tile_projection(proj, tiles, tx, ty) @ view
                for prog in (gp, tp, self.shell_prog):
                    self._uniforms(prog, {"view_proj": vp})
                ctx.enable(moderngl.DEPTH_TEST)
                ctx.disable(moderngl.CULL_FACE)
                fbo.use()
                ctx.scissor = None
                fbo.clear(*SKY, 1.0, depth=1.0)
                for cx, cy in pieces():
                    self.ground_vao.render()
                    mine = bins.get((tx * chunks + cx, ty * chunks + cy)) if tree_vao else None
                    if mine is not None:
                        inst.write(data[mine].tobytes())
                        tree_vao.render(instances=len(mine))
                ctx.copy_framebuffer(resolved, fbo)
                if layer:
                    # Where the march stops (ground or a drawn tree) and where it starts.
                    gp["depth_only"].value = True
                    tp["depth_only"].value = True
                    gfbo.use()
                    gfbo.clear(depth=1.0)
                    for cx, cy in pieces():
                        self.ground_vao.render()
                        mine = bins.get((tx * chunks + cx, ty * chunks + cy)) if tree_vao else None
                        if mine is not None:
                            inst.write(data[mine].tobytes())
                            tree_vao.render(instances=len(mine))
                    gp["depth_only"].value = False
                    tp["depth_only"].value = False
                    sfbo.use()
                    sfbo.clear(depth=1.0)
                    self.shell_prog["top"].value = common["top"]
                    self.shell_vao.render()
                    ctx.finish()
                    resolved.use()
                    ctx.disable(moderngl.DEPTH_TEST)
                    ctx.enable(moderngl.BLEND)
                    ctx.blend_func = moderngl.ONE, moderngl.ONE_MINUS_SRC_ALPHA
                    self._uniforms(cp, {"inv_view_proj": np.linalg.inv(vp)})
                    for cx, cy in pieces():
                        q = ctx.query(time=True)
                        with q:
                            self.canopy_vao.render()
                        ctx.finish()
                        self.timings["canopy"] = self.timings.get("canopy", 0) + q.elapsed
                    ctx.disable(moderngl.BLEND)
                img = np.frombuffer(out_tex.read(), np.uint8).reshape(h, w, 4)[..., :3]
                y0 = (tiles - 1 - ty) * h
                big[y0:y0 + h, tx * w:(tx + 1) * w] = img[::-1]
        for r in (colour, zbuf, fbo, resolved, out_tex):
            r.release()
        if layer:
            for r in (gfbo, sfbo, gdepth, sdepth):
                r.release()
        if tree_vao is not None:
            tree_vao.release(), inst.release()
        out = Image.fromarray(big)
        if tiles > 1:
            from lod_sheet import shrink
            out = shrink(out, tiles)
        self.view_proj = view_proj
        return out


def thinned(trees, eye, span):
    """The trees still drawn at their distance when the layer takes them over across
    `span` = (near, far): those ranked above the layer's share there."""
    d = np.linalg.norm(trees["pos"] - eye, axis=1)
    share = np.clip((d - span[0]) / max(span[1] - span[0], 1e-6), 0, 1)
    keep = trees["rank"] >= share
    return {k: v[keep] for k, v in trees.items()}


def visible(trees, eye, target, size=(1920, 1080), fov_deg=60.0, near=0.0, far=1e9):
    """The trees inside the view (with a margin for their crowns), nearer than `far`."""
    view_proj = perspective(math.radians(fov_deg), size[0] / size[1], 1.0, 60000.0) @ look_at(
        np.asarray(eye, float), np.asarray(target, float))
    p = np.hstack([trees["pos"] + [0, 8.0, 0], np.ones((len(trees["kind"]), 1))]) @ view_proj.T
    w = np.maximum(p[:, 3], 1e-6)
    d = np.linalg.norm(trees["pos"] - eye, axis=1)
    margin = 1 + 15.0 / w
    keep = (p[:, 3] > 0) & (np.abs(p[:, 0] / w) < margin) & (np.abs(p[:, 1] / w) < margin)
    keep &= (d >= near) & (d < far)
    return {k: v[keep] for k, v in trees.items()}
