#!/usr/bin/env python3
"""Rotate assembly-frame STL exports into print orientation, place floor at Z = 0,
and centre XY at (0, 0).
"""

import argparse
from pathlib import Path
import sys
import numpy as np

_HERE = str(Path(__file__).resolve().parent)
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from stl_io import analyze_mesh, read_stl, write_stl

ENCLOSURE_DIR = Path(__file__).resolve().parent.parent
DEFAULT_SRC = ENCLOSURE_DIR / "stl-assembly-frame"
DEFAULT_OUT = ENCLOSURE_DIR / "stl"

# up: which source axis becomes print +Z ('+y' = floor at min Y on the bed, '-y' = max Y on the bed)
PARTS = [
    ("Case_Bottom", "+y", "Case_Bottom"),
    ("Case_Lid", "-y", "Case_Lid"),
    ("Cable_Clamp", "-y", "Cable_Clamp"),
]

ROT = {
    "+y": np.array([[1, 0, 0], [0, 0, -1], [0, 1, 0]], dtype=np.float64),   # about X +90: z' = y
    "-y": np.array([[1, 0, 0], [0, 0, 1], [0, -1, 0]], dtype=np.float64),   # about X -90: z' = -y
    "+z": np.eye(3, dtype=np.float64),
    "-z": np.array([[1, 0, 0], [0, -1, 0], [0, 0, -1]], dtype=np.float64),  # about X 180: z' = -z
}


def orient_parts(src_dir, out_dir):
    """Orient all printable parts from src_dir and write them into out_dir."""
    src_path_dir = Path(src_dir)
    out_path_dir = Path(out_dir)
    out_path_dir.mkdir(parents=True, exist_ok=True)

    print(f"{'Part':20s} {'Dimensions (mm)':25s} {'Volume':14s} {'Watertight':10s}")
    print("-" * 75)
    ok = True
    for src, up, dst in PARTS:
        src_file = src_path_dir / f"{src}.STL"
        if not src_file.exists():
            src_file = src_path_dir / f"{src}.stl"
        if not src_file.exists():
            raise FileNotFoundError(f"Missing assembly-frame STL for {src} in {src_path_dir}")

        t = read_stl(src_file)
        t = t @ ROT[up].T
        lo = t.reshape(-1, 3).min(axis=0)
        hi = t.reshape(-1, 3).max(axis=0)
        t = t - np.array([(lo[0] + hi[0]) / 2.0, (lo[1] + hi[1]) / 2.0, lo[2]])

        metrics = analyze_mesh(t)
        size = metrics["bbox_max"] - metrics["bbox_min"]
        out_file = out_path_dir / f"{dst}.stl"
        write_stl(out_file, t, dst)

        dim_str = f"{size[0]:.1f} x {size[1]:.1f} x {size[2]:.1f}"
        vol_str = f"{metrics['volume'] / 1000.0:.2f} cm3"
        wt = metrics["watertight"] and metrics["shells"] == 1
        print(f"{dst:20s} {dim_str:25s} {vol_str:14s} {str(wt):10s}")
        if not wt:
            ok = False
            print(
                f"FAIL: {dst}: watertight={metrics['watertight']}, "
                f"bad_edges={metrics['bad_edges']}, shells={metrics['shells']}",
                file=sys.stderr,
            )
    if not ok:
        raise RuntimeError("One or more oriented STLs failed mesh integrity check")


def main():
    ap = argparse.ArgumentParser(description="Orient assembly-frame STLs for 3D printing.")
    ap.add_argument("src", nargs="?", default=str(DEFAULT_SRC), help="Input assembly-frame STL directory")
    ap.add_argument("out", nargs="?", default=str(DEFAULT_OUT), help="Output print STL directory")
    args = ap.parse_args()

    try:
        orient_parts(args.src, args.out)
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
