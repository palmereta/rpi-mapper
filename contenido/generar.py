#!/usr/bin/env python3
"""Genera los loops de contenido para el mapper (1280x720, 30 fps, H.264).

Todo es procedural (numpy), así que cada clip es un loop perfecto: el último
cuadro empalma con el primero. Fondo negro (negro = proyector sin luz) para que
sirvan para mapear caras, contornos y objetos.

Salida en <dir>:
  loop_NN_<nombre>.mp4  un período de cada efecto (loop sin cortes si va solo)
  mapping_mix.mp4       todos encadenados con fundidos; también loop sin cortes
  contacto.png          hoja de contacto

Uso: python3 generar.py <dir> [nombre ...]
     JOBS=n procesos en paralelo (por defecto 2; cada uno llega a ~500 MB; ~6 min en un M4)
"""
import os
import subprocess
import sys
from multiprocessing import Pool

import numpy as np

W, H, FPS = 1280, 720, 30
CX, CY = W / 2, H / 2
f32 = np.float32
X, Y = np.meshgrid(np.arange(W, dtype=f32) + 0.5, np.arange(H, dtype=f32) + 0.5)

CYAN = (0.1, 0.85, 1.0)
MAGENTA = (1.0, 0.15, 0.75)
AMBER = (1.0, 0.55, 0.08)
WHITE = (1.0, 1.0, 1.0)
VIOLET = (0.55, 0.25, 1.0)


def col(v, c):
    return v[..., None] * np.array(c, f32)


def glow(d, w):
    return np.exp(-(d / w) ** 2)


def sstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def pal(t, d=(0.0, 0.33, 0.67)):
    return 0.5 + 0.5 * np.cos(2 * np.pi * (t[..., None] + np.array(d, f32)))


def tile_noise(h, w, feat, seed, stretch=(1.0, 1.0)):
    """Ruido suave periódico en los dos ejes (se puede desplazar sin costura).
    feat: tamaño de las manchas en px; stretch: cuánto se alargan en x y en y."""
    n = np.random.default_rng(seed).standard_normal((h, w))
    fy = np.fft.fftfreq(h)[:, None] * stretch[1]
    fx = np.fft.rfftfreq(w)[None, :] * stretch[0]
    n = np.fft.irfft2(np.fft.rfft2(n) * np.exp(-((fx ** 2 + fy ** 2) * feat ** 2) * 4), s=(h, w))
    n = (n - n.mean()) / n.std()
    return np.clip(0.5 + n / 5, 0, 1).astype(f32)


def blur(img, sigma):
    fy = np.fft.fftfreq(img.shape[0])[:, None]
    fx = np.fft.rfftfreq(img.shape[1])[None, :]
    g = np.exp(-2 * (np.pi * sigma) ** 2 * (fx ** 2 + fy ** 2))
    return np.fft.irfft2(np.fft.rfft2(img) * g, s=img.shape).astype(f32)


def seg_dist(px, py, a, b):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    t = np.clip(((px - ax) * dx + (py - ay) * dy) / max(dx * dx + dy * dy, 1e-6), 0, 1)
    return np.hypot(px - ax - t * dx, py - ay - t * dy)


# ---------------------------------------------------------------- efectos
# Cada efecto: nframes (período) y frame(p) con p en [0, 1).

class Contorno:
    """Cometas que recorren dos marcos (el borde de la superficie)."""
    nframes = 240

    def __init__(self):
        self.rings = []
        for m, color, sense in ((14, CYAN, 1), (52, MAGENTA, -1)):
            rw, rh = W - 2 * m, H - 2 * m
            e = np.stack([Y - m, W - m - X, H - m - Y, X - m])
            k = e.argmin(0)
            s = np.choose(k, [np.clip(X - m, 0, rw), rw + np.clip(Y - m, 0, rh),
                              rw + rh + np.clip(W - m - X, 0, rw), 2 * rw + rh + np.clip(H - m - Y, 0, rh)])
            d = np.abs(e.min(0))
            line = glow(d, 2.2) + 0.35 * glow(d, 12)
            self.rings.append((s.astype(f32), line.astype(f32), 2 * (rw + rh), color, sense))

    def frame(self, p):
        out = np.zeros((H, W, 3), f32)
        for s, line, P, color, sense in self.rings:
            v = np.full_like(s, 0.12)
            for k in range(2):
                c = ((p + k / 2) % 1) * P
                delta = ((c - s) * sense) % P
                v += np.exp(-delta / (0.16 * P)) + 1.5 * glow(np.minimum(delta, P - delta), 18)
            out += col(line * v, color)
        return out


class Barrido:
    """Una barra horizontal que baja y después una vertical que cruza."""
    nframes = 240

    def frame(self, p):
        if p < 0.5:
            pos, a, color = -60 + (p / 0.5) * (H + 360), Y, CYAN
        else:
            pos, a, color = -60 + ((p - 0.5) / 0.5) * (W + 360), X, MAGENTA
        d = pos - a
        v = glow(d, 3) + 0.5 * glow(d, 18) + 0.3 * np.where(d > 0, np.exp(-d / 80), 0)
        return col(v, color) + col(glow(d, 2.5), WHITE) * 0.6


class Grilla:
    """Grilla de neón con ondas que salen del centro."""
    nframes = 300

    def __init__(self):
        g = 80
        dx = np.abs((X - CX + g / 2) % g - g / 2)
        dy = np.abs((Y - CY + g / 2) % g - g / 2)
        self.line = np.maximum(glow(dx, 1.6), glow(dy, 1.6)) + 0.3 * np.maximum(glow(dx, 7), glow(dy, 7))
        self.dot = glow(np.hypot(dx, dy), 5)
        self.r = np.hypot(X - CX, Y - CY)

    def frame(self, p):
        wave = (0.5 + 0.5 * np.cos(2 * np.pi * (self.r / 360 - 2 * p))) ** 3
        v = self.line * (0.1 + 0.9 * wave) + 1.2 * self.dot * wave
        return v[..., None] * pal(self.r / 1500 - p, (0.5, 0.75, 0.95))


class Tunel:
    """Rectángulos concéntricos que avanzan hacia afuera."""
    nframes = 240

    def __init__(self):
        self.d = np.maximum(np.abs(X - CX) / CX, np.abs(Y - CY) / CY)
        self.L = np.log2(self.d + 1e-4) * 3
        self.fade = sstep(0.02, 0.3, self.d)
        self.cols = np.array([CYAN, MAGENTA, AMBER], f32)

    def frame(self, p):
        phi = self.L - 3 * p
        e = np.abs((phi + 0.5) % 1 - 0.5)
        n = np.floor(phi).astype(int) % 3
        v = (glow(e, 0.05) + 0.25 * glow(e, 0.18)) * self.fade
        return v[..., None] * self.cols[n]


class Plasma:
    """Plasma de color a pantalla completa (para bañar una superficie)."""
    nframes = 360

    def frame(self, p):
        a = 2 * np.pi * p
        v = (np.sin(X / 97 + a) + np.sin(Y / 71 - 2 * a) + np.sin((X + Y) / 131 + a)
             + np.sin(np.hypot(X - CX - 200 * np.cos(a), Y - CY - 120 * np.sin(a)) / 59 - 3 * a))
        return pal(v / 8 + p) * 0.9


class Bloques:
    """Grilla de 10x5 bloques que se encienden a 120 BPM (fachada)."""
    nframes = 240

    def __init__(self):
        tw, th = W // 10, H // 5
        self.idx = ((Y // th) * 10 + X // tw).astype(int)
        self.mask = ((X % tw > 6) & (X % tw < tw - 6) & (Y % th > 6) & (Y % th < th - 6)).astype(f32)
        cols = np.array([CYAN, MAGENTA, AMBER, WHITE], f32)
        ti, tj = np.meshgrid(np.arange(10), np.arange(5))
        ti, tj = ti.ravel(), tj.ravel()
        rng = np.random.default_rng(7)
        trig = []                                   # (segundo, bloques, color)
        for s in range(10):
            trig.append((s * 0.25, ti == s, cols[0]))
        for s in range(10, 15):
            trig.append((s * 0.25, tj == s - 10, cols[1]))
        for s in range(16, 23):
            trig.append((s * 0.25, (ti + tj) // 2 == s - 16, cols[2]))
        for s in range(24, 31):
            m = np.zeros(50, bool)
            m[rng.choice(50, 8, replace=False)] = True
            trig.append((s * 0.25, m, cols[3]))
        self.trig = trig

    def frame(self, p):
        t = p * self.nframes / FPS
        T = self.nframes / FPS
        tile = np.full((50, 3), 0.03, f32)
        for t0, m, c in self.trig:
            tile[m] += np.exp(-((t - t0) % T) / 0.35) * c
        return tile[self.idx] * self.mask[..., None]


class Rayos:
    """Dos coronas de rayos girando en sentidos opuestos."""
    nframes = 300

    def __init__(self):
        self.th = np.arctan2(Y - CY, X - CX) / (2 * np.pi)
        self.r = np.hypot(X - CX, Y - CY)
        self.fade = sstep(20, 160, self.r)

    def frame(self, p):
        a = (0.5 + 0.5 * np.cos(2 * np.pi * (self.th * 16 + 2 * p))) ** 4
        b = (0.5 + 0.5 * np.cos(2 * np.pi * (self.th * 10 - p))) ** 6
        pulse = 0.75 + 0.25 * np.cos(2 * np.pi * 4 * p)
        out = (col(a, AMBER) + col(b, MAGENTA) * 0.7) * (self.fade * pulse)[..., None]
        return out + col(glow(self.r, 60 + 15 * pulse), (1, 0.85, 0.6))


class Fuego:
    """Llamas desde abajo (ruido periódico que sube)."""
    nframes = 180

    def __init__(self):
        self.n1 = tile_noise(H, W, 45, 1, (1, 3))
        self.n2 = tile_noise(H, W, 16, 2, (1, 2.5))
        self.g = Y / H

    def frame(self, p):
        a = np.roll(self.n1, -round(p * H), 0)
        b = np.roll(self.n2, -round(p * 2 * H) % H, 0)
        n = 0.6 * a + 0.4 * b
        heat = np.clip(self.g * 1.5 - 0.72 + (n - 0.5) * 2.2 * (0.3 + 0.7 * self.g), 0, 1)
        return np.stack([np.clip(heat * 3, 0, 1), np.clip(heat * 3 - 1, 0, 1) * 0.85,
                         np.clip(heat * 3 - 2, 0, 1) * 0.6], -1)


class Ojos:
    """Ojos que se abren en la oscuridad, miran, parpadean y se cierran."""
    nframes = 360
    EV = [  # inicio, duración, x, y, tamaño, color
        (0.5, 3.5, 0.22, 0.35, 34, (1.0, 0.85, 0.1)),
        (2.0, 3.0, 0.70, 0.25, 26, (1.0, 0.12, 0.05)),
        (3.8, 4.0, 0.42, 0.62, 44, (0.7, 1.0, 0.1)),
        (6.0, 3.0, 0.12, 0.72, 22, (1.0, 0.12, 0.05)),
        (7.0, 3.5, 0.78, 0.62, 36, (1.0, 0.85, 0.1)),
        (8.8, 3.6, 0.40, 0.18, 24, (0.7, 1.0, 0.1)),
        (10.5, 3.0, 0.60, 0.42, 30, (1.0, 0.12, 0.05)),
    ]

    def frame(self, p):
        T = self.nframes / FPS
        t = p * T
        out = np.zeros((H, W, 3), f32)
        for t0, D, ex, ey, a, c in self.EV:
            tl = (t - t0) % T
            if tl >= D:
                continue
            o = min(sstep(0, 0.3, tl), sstep(D, D - 0.3, tl))
            bt = abs(tl - D * 0.55)
            o *= 1 - 0.95 * float(sstep(0.12, 0.0, bt))
            o = max(float(o), 0.02)
            look = 0.35 * np.sin(2 * np.pi * tl / D)
            for sx in (-1.3, 1.3):
                cx, cy = ex * W + sx * a, ey * H
                x0, x1 = int(max(cx - 2.5 * a, 0)), int(min(cx + 2.5 * a, W))
                y0, y1 = int(max(cy - 2.5 * a, 0)), int(min(cy + 2.5 * a, H))
                xs, ys = X[y0:y1, x0:x1], Y[y0:y1, x0:x1]
                b = 0.5 * a
                ux, uy = (xs - cx) / a, (ys - cy) / (b * o)
                e = np.hypot(ux, uy)
                iris = sstep(1.0, 0.82, e) * (0.75 + 0.5 * (1 - np.minimum(e, 1)))
                pe = np.hypot((xs - cx - look * a) / (0.16 * a), (ys - cy) / (0.85 * b * o))
                v = iris * (1 - 0.92 * sstep(1.0, 0.75, pe))
                halo = 0.22 * glow(np.hypot(ux, (ys - cy) / b), 1.7) * o
                out[y0:y1, x0:x1] += col(v + halo, c)
        return out


class Tormenta:
    """Nubes que pasan y relámpagos."""
    nframes = 320

    def __init__(self):
        self.cl = 0.6 * tile_noise(H, W, 220, 3, (2, 1)) + 0.4 * tile_noise(H, W, 70, 4, (1.7, 1))
        rng = np.random.default_rng(11)
        self.bolts = [self.bolt(rng, x) for x in (0.3 * W, 0.68 * W)]
        self.fl = [(1.8, 1.0, 0), (1.95, 0.7, 0), (2.3, 0.45, None), (6.9, 1.0, 1), (7.05, 0.8, 1)]

    def bolt(self, rng, x0):
        segs = []

        def grow(x, y, ang, steps, w):
            for _ in range(steps):
                ang2 = ang + rng.uniform(-0.6, 0.6)
                ln = rng.uniform(18, 40)
                nx, ny = x + ln * np.sin(ang2), y + ln * np.cos(ang2)
                segs.append(((x, y), (nx, ny), w))
                if w > 0.5 and rng.random() < 0.12:
                    grow(nx, ny, ang2 + rng.choice([-0.9, 0.9]), int(rng.integers(4, 9)), w * 0.5)
                x, y = nx, ny
                if y > H * 0.8:
                    break

        grow(x0, -10, 0.0, 40, 1.0)
        img = np.zeros((H, W), f32)
        for a, b, w in segs:
            x0_, x1_ = int(max(min(a[0], b[0]) - 30, 0)), int(min(max(a[0], b[0]) + 30, W))
            y0_, y1_ = int(max(min(a[1], b[1]) - 30, 0)), int(min(max(a[1], b[1]) + 30, H))
            if x1_ <= x0_ or y1_ <= y0_:
                continue
            d = seg_dist(X[y0_:y1_, x0_:x1_], Y[y0_:y1_, x0_:x1_], a, b)
            np.maximum(img[y0_:y1_, x0_:x1_], w * (glow(d, 1.8) + 0.3 * glow(d, 12)),
                       out=img[y0_:y1_, x0_:x1_])
        return img

    def frame(self, p):
        T = self.nframes / FPS
        t = p * T
        cl = np.roll(self.cl, -round(p * W), 1)
        out = col(0.03 + 0.32 * cl ** 2, (0.35, 0.4, 0.6))
        F, B = 0.0, [0.0, 0.0]
        for t0, amp, b in self.fl:
            dt = (t - t0) % T
            F += amp * np.exp(-dt / 0.09)
            if b is not None:
                B[b] = max(B[b], amp * np.exp(-dt / 0.15))
        out += col((0.12 + 0.9 * cl) * F, (0.8, 0.85, 1.0))
        for img, k in zip(self.bolts, B):
            if k > 0.01:
                out += col(img * k * 1.4, (0.85, 0.9, 1.0))
        return out


class Estrellas:
    """Viaje entre estrellas (hiperespacio suave)."""
    nframes = 240

    def __init__(self):
        rng = np.random.default_rng(5)
        n = 2500
        d = rng.uniform(-1, 1, (n, 2))
        d /= np.maximum(np.abs(d).max(1, keepdims=True), 1e-6)
        d *= rng.uniform(0.1, 1.0, (n, 1)) ** 0.5
        self.d, self.z0 = d.astype(f32), rng.random(n).astype(f32)
        self.c = (0.75 + 0.25 * rng.random((n, 3))).astype(f32)

    def frame(self, p):
        img = np.zeros((H + 2, W + 2, 3), f32)
        for i in range(6):
            z = (self.z0 - p + i * 0.0025) % 1
            z = np.maximum(z, 1e-3)
            x = CX + self.d[:, 0] * CX * 0.08 / z
            y = CY + self.d[:, 1] * CY * 0.08 / z
            b = np.clip((1 - z) * 1.25, 0, 1) ** 2 * (1 - i / 6) * (1 + 1.5 * (z < 0.2))
            ok = (x >= 0) & (x < W - 1) & (y >= 0) & (y < H - 1) & (b > 0.01)
            x, y, b, c = x[ok], y[ok], b[ok], self.c[ok]
            xi, yi = x.astype(int), y.astype(int)
            fx, fy = (x - xi)[:, None], (y - yi)[:, None]
            v = b[:, None] * c
            for ox, oy, wgt in ((0, 0, (1 - fx) * (1 - fy)), (1, 0, fx * (1 - fy)),
                                (0, 1, (1 - fx) * fy), (1, 1, fx * fy)):
                np.add.at(img, (yi + oy + 1, xi + ox + 1), v * wgt)
        return img[1:-1, 1:-1] * 1.6


class Colores:
    """Baño de color lento a pantalla completa."""
    nframes = 480

    def frame(self, p):
        return pal(p + X / W * 0.2 + Y / H * 0.1) * 0.9


class Calabaza:
    """Cara de calabaza con luz de vela que titila."""
    nframes = 180

    def __init__(self):
        S = H * 0.42
        xs, ys = np.meshgrid((np.arange(2 * W) + 0.5) / 2, (np.arange(2 * H) + 0.5) / 2)
        u, v = (xs - CX) / S, (ys - CY) / S

        def tri(a, b, c):
            def side(p1, p2):
                return (u - p2[0]) * (p1[1] - p2[1]) - (p1[0] - p2[0]) * (v - p2[1])
            d1, d2, d3 = side(a, b), side(b, c), side(c, a)
            return ~(((d1 < 0) | (d2 < 0) | (d3 < 0)) & ((d1 > 0) | (d2 > 0) | (d3 > 0)))

        m = tri((-0.62, -0.12), (-0.14, -0.12), (-0.42, -0.55))
        m |= tri((0.62, -0.12), (0.14, -0.12), (0.42, -0.55))
        m |= tri((-0.11, 0.14), (0.11, 0.14), (0.0, -0.06))
        q = (u / 0.7) ** 2
        mouth = (np.abs(u) < 0.7) & (v > 0.36 - 0.18 * q) & (v < 0.66 - 0.46 * q)
        mouth &= ~((u > -0.32) & (u < -0.16) & (v < 0.36 - 0.18 * q + 0.13))
        mouth &= ~((u > 0.14) & (u < 0.30) & (v > 0.66 - 0.46 * q - 0.13))
        m |= mouth
        self.mask = m.reshape(H, 2, W, 2).mean((1, 3)).astype(f32)
        self.halo = blur(self.mask, 28)
        self.grad = np.clip(0.7 + 0.45 * (Y - CY + 0.6 * S) / (1.3 * S), 0.6, 1.2)

    def frame(self, p):
        a = 2 * np.pi * p
        f = (0.82 + 0.06 * np.sin(7 * a + 1) + 0.05 * np.sin(13 * a + 2)
             + 0.04 * np.sin(23 * a + 0.5) + 0.03 * np.sin(41 * a + 3))
        inner = self.mask * self.grad * f
        out = np.stack([inner, inner * (0.62 + 0.25 * f), inner * 0.2], -1)
        return out + col(self.halo * f * 1.4, (1.0, 0.42, 0.04))


CLIPS = [("contorno", Contorno), ("barrido", Barrido), ("grilla", Grilla), ("tunel", Tunel),
         ("bloques", Bloques), ("rayos", Rayos), ("plasma", Plasma), ("estrellas", Estrellas),
         ("colores", Colores), ("calabaza", Calabaza), ("ojos", Ojos), ("fuego", Fuego),
         ("tormenta", Tormenta)]

SEG, XF = 20 * FPS, 45          # el mix: 20 s por efecto, fundido de 1,5 s

_cache = {}


def clip(i):
    if i not in _cache:
        _cache[i] = CLIPS[i][1]()
    return _cache[i]


def render(i, f):
    c = clip(i)
    return c.frame((f % c.nframes) / c.nframes)


def encoder(path):
    return subprocess.Popen(
        ["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
         "-s", f"{W}x{H}", "-r", str(FPS), "-i", "-",
         "-vf", "scale=out_color_matrix=bt709:out_range=tv:flags=bicubic+accurate_rnd+full_chroma_int,format=yuv420p",
         "-c:v", "libx264", "-preset", "slow", "-crf", "21", "-maxrate", "6M", "-bufsize", "12M",
         "-profile:v", "high", "-level", "4.0", "-g", "60", "-threads", "2",
         "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709",
         "-movflags", "+faststart", "-an", path], stdin=subprocess.PIPE)


def to8(img):
    return (np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)


def job(args):
    kind, i, out = args
    enc = encoder(out)
    if kind == "loop":
        for f in range(clip(i).nframes):
            enc.stdin.write(to8(render(i, f)).tobytes())
    else:  # segmento i del mix: arranca con el fundido desde el efecto anterior
        prev = (i - 1) % len(CLIPS)
        for f in range(SEG):
            img = render(i, f)
            if f < XF:
                a = sstep(0, 1, np.float32((f + 0.5) / XF))
                img = a * img + (1 - a) * render(prev, SEG + f)
            enc.stdin.write(to8(img).tobytes())
    enc.stdin.close()
    if enc.wait():
        raise SystemExit(f"ffmpeg falló en {out}")
    return out


def main():
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "contenido-generado")
    only = set(sys.argv[2:])
    os.makedirs(out, exist_ok=True)
    jobs = [("mix", i, f"{out}/.seg{i:02d}.mp4") for i in range(len(CLIPS))] if not only else []
    jobs += [("loop", i, f"{out}/loop_{i + 1:02d}_{n}.mp4")
             for i, (n, _) in enumerate(CLIPS) if not only or n in only]
    with Pool(int(os.environ.get("JOBS", "2"))) as pool:
        for r in pool.imap_unordered(job, jobs):
            print("listo", os.path.basename(r), flush=True)
    if not only:
        lst = f"{out}/.segs.txt"
        with open(lst, "w") as fh:
            fh.writelines(f"file '.seg{i:02d}.mp4'\n" for i in range(len(CLIPS)))
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i", lst,
                        "-c", "copy", "-movflags", "+faststart", f"{out}/mapping_mix.mp4"], check=True)
        for i in range(len(CLIPS)):
            os.remove(f"{out}/.seg{i:02d}.mp4")
        os.remove(lst)
    # hoja de contacto: un cuadro de cada efecto
    from PIL import Image, ImageDraw
    sheet = Image.new("RGB", (4 * 320, 4 * 180), (40, 40, 40))
    for i, (n, _) in enumerate(CLIPS):
        im = Image.fromarray(to8(render(i, int(clip(i).nframes * 0.37)))).resize((316, 176))
        ImageDraw.Draw(im).text((6, 4), f"{i + 1:02d} {n}", fill=(255, 255, 255))
        sheet.paste(im, ((i % 4) * 320 + 2, (i // 4) * 180 + 2))
    sheet.save(f"{out}/contacto.png")
    print("listo contacto.png")


if __name__ == "__main__":
    main()
