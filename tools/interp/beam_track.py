"""The flashlight's lit area through the in-between frames of an LM_INTERP_TEST run.

    beam_track.py <dump dir> [steps] [--crop x,y,w,h] [--thresh 0.35] [--frames a,b]

For each game frame N with dumps f<N-1>_fresh and f<N>_s1 .. s<steps> (s<steps> = game frame N),
takes the pixels that are much brighter than in a dimmed copy of the frame (the flashlight's pool
and beam: luminance above `thresh` of the frame's maximum, a 640x480 dump, HUD corners excluded) and
tracks their brightness-weighted centroid. A lit area that follows the interpolation moves by
1/steps of its game-frame step at each in-between; one that jumps once per game frame moves at
one in-between only. Reported per frame: the steps (px), how uneven they are (max step / mean
step; 1 = even, `steps` = everything at one in-between), and a summary.
"""
import argparse
import os
import re

import numpy as np
from PIL import Image


def lum(path, crop):
    im = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
    if crop:
        x, y, w, h = crop
        im = im[y:y + h, x:x + w]
    return 0.299 * im[..., 0] + 0.587 * im[..., 1] + 0.114 * im[..., 2]


def centroid(L, thresh):
    m = L >= thresh * 255.0
    if m.sum() < 50:
        return None
    w = (L - thresh * 255.0) * m
    ys, xs = np.indices(L.shape)
    s = w.sum()
    return np.array([(xs * w).sum() / s, (ys * w).sum() / s]), int(m.sum())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("steps", nargs="?", type=int, default=4)
    ap.add_argument("--crop", default="0,40,640,330", help="excludes the HUD (health, Game Boy Horror) at the bottom")
    ap.add_argument("--thresh", type=float, default=0.55)
    ap.add_argument("--frames", default="")
    a = ap.parse_args()
    crop = [int(v) for v in a.crop.split(",")] if a.crop else None
    names = set(os.listdir(a.dir))
    frames = sorted({int(m.group(1)) for n in names if (m := re.match(r"f(\d+)_s1\.ppm$", n))})
    if a.frames:
        lo, hi = (int(v) for v in a.frames.split(","))
        frames = [f for f in frames if lo <= f <= hi]
    uneven = []
    for f in frames:
        seq = [f"f{f - 1}_fresh.ppm"] + [f"f{f}_s{s}.ppm" for s in range(1, a.steps + 1)]
        if not all(n in names for n in seq):
            continue
        cs = [centroid(lum(os.path.join(a.dir, n), crop), a.thresh) for n in seq]
        if any(c is None for c in cs):
            continue
        pts = [c[0] for c in cs]
        steps = [float(np.linalg.norm(pts[i + 1] - pts[i])) for i in range(a.steps)]
        total = float(np.linalg.norm(pts[-1] - pts[0]))
        mean = sum(steps) / len(steps)
        ratio = max(steps) / mean if mean > 0.05 else 1.0
        if total > 1.0:
            uneven.append(ratio)
        print(f"f{f}: steps {' '.join(f'{s:5.2f}' for s in steps)} px  total {total:5.2f}  uneven {ratio:4.2f}  lit px {cs[-1][1]}")
    if uneven:
        u = np.array(uneven)
        print(f"\nframes with the lit area moving > 1 px: {len(u)}; uneven (max step / mean step) p50 {np.median(u):.2f} "
              f"p90 {np.percentile(u, 90):.2f}; frames with one in-between taking > 60% of the step: "
              f"{int((u > 0.6 * a.steps).sum())}")


if __name__ == "__main__":
    main()
