"""Checks for the far-forest pieces: `.venv/bin/python -m unittest test_far`."""
import math
import unittest

import numpy as np

import far
import terrain
from impostor import coverage_mips
from render import context


def glsl(defs, expr, n, uniforms=None):
    """`expr` (a float, using the index `i`) for i in 0..n-1, worked out on the GPU after
    the GLSL in `defs`."""
    ctx = context()
    prog = ctx.program(vertex_shader="""
#version 330
in vec2 in_corner;
void main() { gl_Position = vec4(in_corner, 0.0, 1.0); }
""", fragment_shader="#version 330\nout vec4 f_out;\n" + defs + """
void main() {
    int i = int(gl_FragCoord.x);
    f_out = vec4(""" + expr + """, 0.0, 0.0, 1.0);
}
""")
    for k, v in (uniforms or {}).items():
        if k in prog:
            if isinstance(v, np.ndarray):
                prog[k].write(np.ascontiguousarray(v, "f4")[:prog[k].array_length].tobytes())
            else:
                prog[k].value = v
    tex = ctx.texture((n, 1), 4, dtype="f4")
    fbo = ctx.framebuffer(tex)
    quad = ctx.buffer(np.array([-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1], "f4").tobytes())
    vao = ctx.vertex_array(prog, [(quad, "2f", "in_corner")])
    fbo.use()
    vao.render()
    out = np.frombuffer(tex.read(), np.float32).reshape(n, 4)[:, 0].copy()
    for r in (vao, quad, fbo, tex, prog):
        r.release()
    return out


class HashTest(unittest.TestCase):
    def test_gpu_and_numpy_find_the_same_trees(self):
        ci = np.arange(256) * 37 + 11
        cj = np.arange(256) * 53 + 7
        want = terrain.unit_hash(ci, cj, 3)
        got = glsl(terrain.HASH_GLSL, "unit_hash(ivec2(i * 37 + 11, i * 53 + 7), 3u)", 256)
        np.testing.assert_allclose(got, want, atol=1e-6)


MEET = """
uniform float top;
uniform vec3 sky_dirs[15];
uniform float sky_weights[15];
""" + far.CANOPY_LIB


class StandTest(unittest.TestCase):
    HC, RH, RV = 10.0, 4.0, 6.0

    def crossing(self, zenith_deg, steps=4000):
        """meet_rate summed along a line going down through the whole crown layer."""
        z = math.radians(zenith_deg)
        d = (math.sin(z), -math.cos(z), 0.0)
        length = 2 * self.RV / math.cos(z)
        ds = length / steps
        expr = (f"meet_rate({self.HC + self.RV} - (float(i) + 0.5) * {ds} * {math.cos(z)}, "
                f"vec3({d[0]}, {d[1]}, {d[2]}), {self.HC}, {self.RH}, {self.RV}) * {ds}")
        return float(glsl(MEET, expr, steps).sum())

    def test_crowns_met_crossing_the_layer_match_the_crown_shadow(self):
        for zenith in (0.0, 30.0, 60.0):
            z = math.radians(zenith)
            shadow = math.pi * self.RH * math.sqrt(
                (self.RH * math.cos(z)) ** 2 + (self.RV * math.sin(z)) ** 2) / math.cos(z)
            self.assertAlmostEqual(self.crossing(zenith) / shadow, 1.0, delta=0.01)

    def test_met_area_is_the_crowns_a_ray_comes_to(self):
        for h0 in (0.0, 6.0, 10.0, 13.0, 15.5):
            for zenith in (0.0, 30.0, 60.0, 80.0):
                z = math.radians(zenith)
                l = (math.sin(z), math.cos(z), 0.0)
                exact = glsl(MEET, f"met_area({h0}, vec3({l[0]}, {l[1]}, {l[2]}), "
                                   f"{self.HC}, {self.RH}, {self.RV})", 1)[0]
                n = 8000
                ds = (self.HC + self.RV + 1 - h0) / l[1] / n
                summed = glsl(MEET, f"meet_rate({h0} + (float(i) + 0.5) * {ds * l[1]}, "
                                    f"vec3({l[0]}, {l[1]}, {l[2]}), {self.HC}, {self.RH}, "
                                    f"{self.RV}) * {ds}", n).sum()
                self.assertAlmostEqual(exact, summed, delta=0.02 * max(summed, 5.0))

    def test_light_through_a_stand_is_poisson(self):
        lam, alpha = 0.02, 0.7
        top = 30.0
        crown = np.array([[self.HC, self.RH, self.RV, alpha]] + [[0, 1, 1, 0]] * 3, "f4")
        got = glsl(MEET, "stand_light(0.0, vec3(0.0, 1.0, 0.0), 0.02, vec4(1, 0, 0, 0), "
                         "vec3(0.0, -1.0, 0.0))", 1,
                   {"kind_crown": crown, "top": top, "crown_reach": 3.0})[0]
        # Crown sizes spread 0.8-1.2: the mean shadow grows by the mean of size squared.
        mean_sq = (1.2 ** 3 - 0.8 ** 3) / (3 * 0.4)
        want = math.exp(-lam * alpha * math.pi * self.RH ** 2 * mean_sq)
        self.assertAlmostEqual(got, want, delta=0.005)

    def test_looking_down_sun_sees_lit_ground(self):
        crown = np.array([[self.HC, self.RH, self.RV, 0.7]] + [[0, 1, 1, 0]] * 3, "f4")
        u = {"kind_crown": crown, "top": 30.0, "crown_reach": 3.0}
        sun = "normalize(vec3(0.5, 1.0, 0.0))"
        apart = glsl(MEET, f"stand_light(0.0, {sun}, 0.02, vec4(1, 0, 0, 0), "
                           "normalize(vec3(-0.5, 1.0, 0.0)))", 1, u)[0]
        along = glsl(MEET, f"stand_light(0.0, {sun}, 0.02, vec4(1, 0, 0, 0), {sun})", 1, u)[0]
        self.assertAlmostEqual(along, 1.0, delta=1e-4)
        self.assertLess(apart, 0.8)


class MipTest(unittest.TestCase):
    def test_shrunk_texels_hold_only_what_covers_them(self):
        albedo = np.zeros((2, 2, 4), np.float32)
        albedo[0, 0] = (1.0, 0.0, 0.0, 1.0)
        albedo[1, 1] = (0.0, 0.0, 1.0, 0.25)
        albedo[0, 1, :3] = albedo[1, 0, :3] = 0.5  # padding, uncovered
        normal = np.zeros((2, 2, 4), np.float32)
        normal[0, 0, 1] = 1.0
        (a0, a1), (_n0, n1) = coverage_mips([albedo, normal])
        np.testing.assert_allclose(a1[0, 0], [0.8, 0.0, 0.2, 0.3125], atol=1e-6)
        np.testing.assert_allclose(n1[0, 0, 1], 0.8, atol=1e-6)


class CrownTest(unittest.TestCase):
    def test_matched_crown_stops_what_the_impostor_covers(self):
        class Imp:
            views = 6
            radius = 5.0
        imp = Imp()
        cell = 32
        imp.albedo = np.zeros((6 * cell, 6 * cell, 4), np.float32)
        s = (np.arange(cell) + 0.5) / cell * 2 - 1
        X, Y = np.meshgrid(s, s)
        for j in range(6):
            for i in range(6):
                imp.albedo[j * cell:(j + 1) * cell, i * cell:(i + 1) * cell, 3] = (
                    X ** 2 + Y ** 2 < 0.8) * 0.9
        kind = far.match_area(far.Kind("disc", imp, (0, 5, 0), (2.0, 2.0), 0.5))
        side, top = far.coverage_areas(imp)
        rh, rv = kind.crown_radii
        got = math.pi * rh * rv * (1 - far._mean_transmittance(0.5, 2 * rh))
        self.assertAlmostEqual(got / side, 1.0, delta=0.01)


if __name__ == "__main__":
    unittest.main()
