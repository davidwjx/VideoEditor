#!/usr/bin/env python3
"""Reproduce the hole-PSNR baselines quoted in the M4 acceptance report.

The high-frequency testsrc2 acceptance plate is a pathological ground truth:
a smooth inpaint fill cannot reproduce the exact phase of the pattern. This
script measures, for one frame:

  * the hole bbox from the mask video,
  * mean-colour-fill PSNR inside the hole,
  * ideal smooth-fill PSNR (Gaussian-blurred true GT) inside the hole,
  * source-vs-clean PSNR outside the hole (alignment sanity check).

Requirements: numpy, opencv-python (the M4 spike venv at C:/m4-spike/venv has
both). Run with an isolated interpreter so no local module shadows a package:

  C:/m4-spike/venv/Scripts/python.exe -I tools/m4_spike/hole_psnr_baseline.py \
      --video C:/m4-spike/acceptance/static_logo_1080p_10s.mp4 \
      --mask  C:/m4-spike/acceptance/static_logo_mask_1080p_10s.mp4 \
      --clean C:/m4-spike/acceptance/clean_background_1080p_10s.mp4 \
      --frame 0

For the smooth acceptance set use static_logo_smooth_1080p_10s.mp4,
static_logo_smooth_mask_1080p_10s.mp4 and clean_gradient_1080p_10s.mp4.
"""

import argparse
import sys

import cv2
import numpy as np


def read_frame(path: str, index: int):
    capture = cv2.VideoCapture(path)
    if not capture.isOpened():
        raise SystemExit(f"cannot open {path}")
    frame = None
    for _ in range(index + 1):
        ok, candidate = capture.read()
        if not ok:
            break
        frame = candidate
    capture.release()
    if frame is None:
        raise SystemExit(f"cannot decode frame {index} of {path}")
    return frame


def psnr(a: np.ndarray, b: np.ndarray, selector: np.ndarray) -> float:
    diff = a[selector].astype(np.float64) - b[selector].astype(np.float64)
    mse = float(np.mean(diff * diff))
    return 100.0 if mse <= 1e-9 else float(10.0 * np.log10(255.0 * 255.0 / mse))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--video", required=True)
    parser.add_argument("--mask", required=True)
    parser.add_argument("--clean", required=True)
    parser.add_argument("--frame", type=int, default=0)
    args = parser.parse_args()

    source = read_frame(args.video, args.frame)
    mask_frame = read_frame(args.mask, args.frame)
    clean = read_frame(args.clean, args.frame)
    if not (source.shape == mask_frame.shape == clean.shape):
        raise SystemExit("video/mask/clean geometry mismatch")

    mask = cv2.cvtColor(mask_frame, cv2.COLOR_BGR2GRAY) > 127
    ys, xs = np.nonzero(mask)
    if len(xs) == 0:
        raise SystemExit("mask is empty")
    print(
        f"frame={args.frame} mask_bbox=x{min(xs)}..{max(xs)} "
        f"y{min(ys)}..{max(ys)} area={int(mask.sum())}"
    )

    hole_mean = clean[mask].mean(axis=0)
    mean_fill = np.broadcast_to(hole_mean, clean.shape)
    blurred = cv2.GaussianBlur(clean, (51, 51), 0)
    outside = ~mask
    print(f"outside-mask source-vs-clean PSNR: {psnr(source, clean, outside):.2f} dB")
    print(f"inside-hole mean-fill PSNR: {psnr(mean_fill, clean, mask):.2f} dB")
    print(f"inside-hole smooth-fill cap (51px blur): {psnr(blurred, clean, mask):.2f} dB")
    print(
        "hole BGR mean/std: "
        + ", ".join(
            f"{name}={hole_mean[i]:.1f}/{clean[mask][:, i].std():.1f}"
            for i, name in enumerate(("B", "G", "R"))
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
