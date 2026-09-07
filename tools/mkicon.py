"""Bakes the supplied 240x240 SVG into an Android adaptive-icon foreground.

Android VectorDrawable has no <circle>/<rect rx>/<use>/stroke-dasharray, and
nested SVG transforms don't map cleanly onto <group>, so every shape is
converted to explicit path data with its transform already applied. The one
thing that survives as-is is stroking (colour/width/round caps), which
VectorDrawable does support.
"""
import math

# 240 source units -> 108dp viewport, then 0.70 so the art sits inside the
# adaptive icon's 66% safe zone (outer ring gets masked/parallaxed away).
S = (108.0 / 240.0) * 0.70
OFF = 54.0 - 120.0 * S

def px(x, y):
    return (x * S + OFF, y * S + OFF)

def fmt(v):
    return f"{v:.2f}".rstrip('0').rstrip('.')

def circle(cx, cy, r):
    cx, cy = px(cx, cy)
    r = r * S
    return (f"M{fmt(cx-r)},{fmt(cy)} a{fmt(r)},{fmt(r)} 0 1,0 {fmt(2*r)},0 "
            f"a{fmt(r)},{fmt(r)} 0 1,0 {fmt(-2*r)},0 Z")

def rrect(x, y, w, h, rx, tx=0.0, ty=0.0):
    x += tx; y += ty
    x0, y0 = px(x, y)
    ww, hh, r = w * S, h * S, rx * S
    return (f"M{fmt(x0+r)},{fmt(y0)} h{fmt(ww-2*r)} a{fmt(r)},{fmt(r)} 0 0,1 {fmt(r)},{fmt(r)} "
            f"v{fmt(hh-2*r)} a{fmt(r)},{fmt(r)} 0 0,1 {fmt(-r)},{fmt(r)} "
            f"h{fmt(-(ww-2*r))} a{fmt(r)},{fmt(r)} 0 0,1 {fmt(-r)},{fmt(-r)} "
            f"v{fmt(-(hh-2*r))} a{fmt(r)},{fmt(r)} 0 0,1 {fmt(r)},{fmt(-r)} Z")

def line(x1, y1, x2, y2):
    a = px(x1, y1); b = px(x2, y2)
    return f"M{fmt(a[0])},{fmt(a[1])} L{fmt(b[0])},{fmt(b[1])}"

def bez(t, p0, c1, c2, p3):
    u = 1 - t
    return (u*u*u*p0[0] + 3*u*u*t*c1[0] + 3*u*t*t*c2[0] + t*t*t*p3[0],
            u*u*u*p0[1] + 3*u*u*t*c1[1] + 3*u*t*t*c2[1] + t*t*t*p3[1])

paths = []
def add(d, fill=None, stroke=None, width=None, cap=None):
    paths.append((d, fill, stroke, width, cap))

# Craters, largest first.
add(circle(120, 116, 58), fill="#5E2B1E")
add(circle(115, 109, 42), fill="#8E4128")
add(circle(110, 102, 26), fill="#C4623A")
add(circle(105, 95, 10),  fill="#D98A5C")
# Target reticle.
add(circle(142, 140, 20),   fill="#1B1424")
add(circle(142, 140, 17.5), stroke="#C4623A", width=5*S)

# The trajectory: stroke-dasharray "2 15" with round caps is a dotted line,
# which VectorDrawable can't express - so it's flattened into the dots it
# would have drawn, spaced by arc length along the curve.
P0, C1, C2, P3 = (85,88), (108,100), (122,116), (132,128)
samples = [bez(i/400.0, P0, C1, C2, P3) for i in range(401)]
acc, dots, travelled = 0.0, [], 0.0
for i in range(1, len(samples)):
    seg = math.dist(samples[i-1], samples[i])
    travelled += seg
    acc += seg
    if acc >= 17.0 or i == 1:      # 2 on + 15 off
        acc = 0.0
        dots.append(samples[i])
for d in dots:
    add(circle(d[0], d[1], 4), fill="#B8E04A")   # stroke-width 8 -> radius 4

# Three tanks: a rotated barrel plus the shared hull symbol, transforms baked.
for tx, ty, rot in ((56, 72, 28), (188, 96, 162), (86, 192, -58)):
    a = math.radians(rot)
    add(line(tx, ty, tx + 33*math.cos(a), ty + 33*math.sin(a)),
        stroke="#F2E4CF", width=7*S, cap="round")
    add(rrect(-18, -15, 36, 30, 9, tx, ty), fill="#F2E4CF")
    add(circle(tx, ty, 7), fill="#1B1424")

out = ['<?xml version="1.0" encoding="utf-8"?>',
       '<!-- Generated from design/icons/scorched3d-icon-1024.svg - see',
       '     tools/mkicon.py. Do not hand-edit: regenerate instead. -->',
       '<vector xmlns:android="http://schemas.android.com/apk/res/android"',
       '    android:width="108dp"',
       '    android:height="108dp"',
       '    android:viewportWidth="108"',
       '    android:viewportHeight="108">']
for d, fill, stroke, width, cap in paths:
    out.append('    <path')
    if fill:   out.append(f'        android:fillColor="{fill}"')
    if stroke:
        out.append(f'        android:strokeColor="{stroke}"')
        out.append(f'        android:strokeWidth="{fmt(width)}"')
        if cap: out.append(f'        android:strokeLineCap="{cap}"')
    out.append(f'        android:pathData="{d}" />')
out.append('</vector>')
open('app/src/main/res/drawable/ic_launcher_foreground.xml','w').write('\n'.join(out) + '\n')
print(f"wrote foreground: {len(paths)} paths, {len(dots)} trajectory dots")
