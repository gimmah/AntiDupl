#!/usr/bin/env python3
"""Generate a deterministic test dataset of images for decoder benchmarking.

Produces N images (default 500) with a balanced mix of formats
(JPEG, PNG, WEBP, BMP) and varied resolutions (500x500 up to 4K UHD).

Usage:
    python3 scripts/generate_dataset.py [--out DIR] [--count 500] [--seed 1234]

Requires: Pillow, numpy.

Images are synthetic but photo-like (smooth multi-scale noise + gradients +
a few shapes + mild sensor noise) so that lossy codecs behave realistically
instead of compressing to nothing. A manifest.json describing every file is
written next to the images.
"""
import argparse
import json
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from PIL import Image, ImageDraw

FORMATS = ("JPEG", "PNG", "WEBP", "BMP")
EXT = {"JPEG": "jpg", "PNG": "png", "WEBP": "webp", "BMP": "bmp"}

# (width, height, weight). Weights keep the dataset size sane (BMP is raw)
# while still covering the whole 500x500 .. 3840x2160 range.
RESOLUTIONS = (
    (500, 500, 20),
    (800, 600, 20),
    (1280, 720, 20),
    (1600, 1200, 15),
    (1920, 1080, 15),
    (2560, 1440, 7),
    (3840, 2160, 3),
)


def build_plan(count, seed):
    """Return a list of (index, format, width, height) with formats balanced
    and the resolution mix identical inside every format."""
    rng = np.random.default_rng(seed)
    per_format = [count // len(FORMATS) + (1 if i < count % len(FORMATS) else 0)
                  for i in range(len(FORMATS))]
    weights = np.array([r[2] for r in RESOLUTIONS], dtype=float)
    weights /= weights.sum()
    plan = []
    idx = 0
    for fmt, n in zip(FORMATS, per_format):
        # Deterministic stratified allocation of resolutions, then shuffle.
        counts = np.floor(weights * n).astype(int)
        for k in np.argsort(-(weights * n - counts))[: n - counts.sum()]:
            counts[k] += 1
        sizes = [RESOLUTIONS[k][:2] for k, c in enumerate(counts) for _ in range(c)]
        rng.shuffle(sizes)
        for w, h in sizes:
            plan.append((idx, fmt, int(w), int(h)))
            idx += 1
    return plan


def synth_image(width, height, seed):
    """Photo-like RGB image as uint8 array (h, w, 3)."""
    rng = np.random.default_rng(seed)
    img = np.zeros((height, width, 3), dtype=np.float32)
    # Multi-scale smooth noise: small random grids upscaled bilinearly.
    for scale, amp in ((6, 90.0), (24, 45.0), (96, 18.0)):
        gh, gw = max(2, height // scale + 2), max(2, width // scale + 2)
        grid = rng.random((gh, gw, 3), dtype=np.float32)
        layer = np.asarray(
            Image.fromarray((grid * 255).astype(np.uint8)).resize(
                (width, height), Image.BILINEAR),
            dtype=np.float32) / 255.0
        img += layer * amp
    # Diagonal gradient.
    yy = np.linspace(0, 1, height, dtype=np.float32)[:, None]
    xx = np.linspace(0, 1, width, dtype=np.float32)[None, :]
    base = rng.random(3, dtype=np.float32) * 60
    img += ((xx + yy) * 0.5 * 80.0)[..., None] + base
    # Sensor-like noise.
    img += rng.normal(0, 3.0, size=img.shape).astype(np.float32)
    out = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")
    # A few shapes so there are real edges.
    draw = ImageDraw.Draw(out)
    for _ in range(int(rng.integers(4, 10))):
        x0, y0 = int(rng.integers(0, width)), int(rng.integers(0, height))
        x1 = x0 + int(rng.integers(width // 20 + 1, width // 4 + 2))
        y1 = y0 + int(rng.integers(height // 20 + 1, height // 4 + 2))
        color = tuple(int(c) for c in rng.integers(0, 256, 3))
        if rng.random() < 0.5:
            draw.rectangle([x0, y0, x1, y1], fill=color)
        else:
            draw.ellipse([x0, y0, x1, y1], fill=color)
    return out


def write_one(job):
    out_dir, idx, fmt, w, h, seed = job
    path = os.path.join(out_dir, f"img_{idx:04d}_{fmt.lower()}_{w}x{h}.{EXT[fmt]}")
    img = synth_image(w, h, seed + idx)
    if fmt == "JPEG":
        img.save(path, "JPEG", quality=85, optimize=False, subsampling="4:2:0")
    elif fmt == "PNG":
        # Low compress level: generating 500 images stays fast; still real zlib data.
        img.save(path, "PNG", compress_level=3)
    elif fmt == "WEBP":
        img.save(path, "WEBP", quality=80, method=2)
    else:
        img.save(path, "BMP")
    return {"file": os.path.basename(path), "format": fmt, "width": w, "height": h,
            "bytes": os.path.getsize(path)}


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=os.path.join(here, "..", "testdata", "dataset"),
                    help="output directory (default: testdata/dataset)")
    ap.add_argument("--count", type=int, default=500)
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    args = ap.parse_args()

    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)
    plan = build_plan(args.count, args.seed)
    jobs = [(out_dir, i, f, w, h, args.seed) for i, f, w, h in plan]

    t0 = time.time()
    with ProcessPoolExecutor(max_workers=args.jobs) as ex:
        entries = list(ex.map(write_one, jobs, chunksize=4))
    manifest = {"seed": args.seed, "count": len(entries), "images": entries}
    with open(os.path.join(out_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)

    total = sum(e["bytes"] for e in entries)
    print(f"Generated {len(entries)} images in {time.time() - t0:.1f}s -> {out_dir}")
    print(f"Total size: {total / 1e6:.1f} MB")
    for fmt in FORMATS:
        es = [e for e in entries if e["format"] == fmt]
        print(f"  {fmt:5s} {len(es):4d} files  {sum(e['bytes'] for e in es) / 1e6:8.1f} MB")
    for w, h, _ in RESOLUTIONS:
        n = sum(1 for e in entries if (e["width"], e["height"]) == (w, h))
        print(f"  {w}x{h}: {n}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
