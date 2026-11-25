"""Draws the HertzyEVS app icon (hertzyevs.ico + 256 px png + .rc): a sine (Hz) inside a turning arrow (RPM).
Each size is drawn on its own, so the small ones keep fewer and heavier details. Run: python make_icons.py
"""
import math
from pathlib import Path
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
SIZES = [16, 24, 32, 48, 64, 128, 256]
SS = 8                                # supersampling
TOP, BOTTOM = (0x3b, 0x3f, 0x4a), (0x1c, 0x1e, 0x24)   # graphite, like the app
WHITE = (255, 255, 255, 255)
RED = (0xff, 0x61, 0x61, 255)


def tile(n):
    """Rounded square with the vertical blue gradient of the app header; a small transparent margin on big sizes."""
    N = n * SS
    m = 0 if n <= 32 else round(N * 0.04)
    grad = Image.new("RGBA", (N, N))
    d = ImageDraw.Draw(grad)
    for y in range(N):
        t = y / (N - 1)
        d.line([(0, y), (N, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(TOP, BOTTOM)) + (255,))
    mask = Image.new("L", (N, N), 0)
    ImageDraw.Draw(mask).rounded_rectangle([m, m, N - 1 - m, N - 1 - m], radius=round((N - 2 * m) * 0.23), fill=255)
    out = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    out.paste(grad, (0, 0), mask)
    return out


def line(d, pts, w, fill=WHITE):
    d.line(pts, fill=fill, width=round(w), joint="curve")
    for x, y in (pts[0], pts[-1]):
        d.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=fill)


def hertzy(n):
    """a sine wave (frequency) with a turning arrow around it (rotation speed)"""
    im = tile(n)
    d = ImageDraw.Draw(im)
    N = n * SS
    c = N / 2
    small = n <= 32
    # turning arrow: an arc of ~290 degrees with a head
    R = N * 0.34
    w = N * (0.085 if small else 0.06)
    a0, a1 = math.radians(-60), math.radians(225)
    arc = [(c + R * math.cos(a0 + (a1 - a0) * i / 200), c + R * math.sin(a0 + (a1 - a0) * i / 200)) for i in range(201)]
    for x, y in arc:
        d.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=RED)
    hx, hy = arc[-1]
    ta = a1 + math.pi / 2                    # tangent (direction of travel)
    hl = N * (0.16 if small else 0.13)
    tip = (hx + hl * 0.75 * math.cos(ta), hy + hl * 0.75 * math.sin(ta))
    nx, ny = math.cos(a1), math.sin(a1)
    d.polygon([tip, (hx + nx * hl * 0.55, hy + ny * hl * 0.55), (hx - nx * hl * 0.55, hy - ny * hl * 0.55)], fill=RED)
    # sine through the middle
    sw = N * (0.08 if small else 0.055)
    x0, x1, A = c - N * 0.22, c + N * 0.22, N * 0.12
    pts = [(x0 + (x1 - x0) * i / 400, c - A * math.sin(2 * math.pi * 1.5 * i / 400)) for i in range(401)]
    for x, y in pts:
        d.ellipse([x - sw / 2, y - sw / 2, x + sw / 2, y + sw / 2], fill=WHITE)
    return im.resize((n, n), Image.LANCZOS)


for name, draw in [("hertzyevs", hertzy)]:
    images = [draw(n) for n in SIZES]
    images[-1].save(HERE / f"{name}.png")
    images[-1].save(HERE / f"{name}.ico", format="ICO", append_images=images[:-1], sizes=[(n, n) for n in SIZES])
    (HERE / f"{name}.rc").write_text(f'IDI_ICON1 ICON "{name}.ico"\n')
