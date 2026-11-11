#!/usr/bin/env python3
"""Generate a combined build plate STL containing all 3 printable enclosure parts.

Part offsets are computed dynamically from the loaded STL bounding boxes so the
configured inter-part gap is preserved regardless of enclosure dimensions.
"""

import argparse
from pathlib import Path
import sys
import numpy as np

_HERE = str(Path(__file__).resolve().parent)
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from stl_io import read_stl, write_stl

ENCLOSURE_DIR = Path(__file__).resolve().parent.parent
DEFAULT_STL_DIR = ENCLOSURE_DIR / "stl"
DEFAULT_PLATE_NAME = "Power_Outage_Monitor_All_Parts_Plate.stl"
DEFAULT_GAP_MM = 16.0


def center_part_xy_on_z0(triangles):
    """Translate a part so its XY bounding-box center is (0, 0) and min Z is 0."""
    verts = triangles.reshape(-1, 3)
    lo = verts.min(axis=0)
    hi = verts.max(axis=0)
    shift = np.array([(lo[0] + hi[0]) / 2.0, (lo[1] + hi[1]) / 2.0, lo[2]])
    out = triangles - shift
    size = hi - lo
    return out, size


def build_plate(stl_dir, out_path, gap_mm=DEFAULT_GAP_MM):
    """Combine Case_Bottom, Case_Lid, and Cable_Clamp into one build plate STL."""
    sdir = Path(stl_dir)
    t_bottom, s_bottom = center_part_xy_on_z0(read_stl(sdir / "Case_Bottom.stl"))
    t_lid, s_lid = center_part_xy_on_z0(read_stl(sdir / "Case_Lid.stl"))
    t_clamp, s_clamp = center_part_xy_on_z0(read_stl(sdir / "Cable_Clamp.stl"))

    # Place Case_Bottom below Y=0 and Case_Lid above Y=0 with gap_mm between them
    t_bottom[:, :, 1] -= s_bottom[1] / 2.0 + gap_mm / 2.0
    t_lid[:, :, 1] += s_lid[1] / 2.0 + gap_mm / 2.0

    # Place Cable_Clamp to the right of the wider body with gap_mm clearance
    max_body_x = max(s_bottom[0], s_lid[0])
    t_clamp[:, :, 0] += max_body_x / 2.0 + gap_mm + s_clamp[0] / 2.0

    all_t = np.concatenate([t_bottom, t_lid, t_clamp], axis=0)
    all_verts = all_t.reshape(-1, 3)
    min_xyz = all_verts.min(axis=0)
    max_xyz = all_verts.max(axis=0)
    center_xy = 0.5 * (min_xyz[:2] + max_xyz[:2])

    all_t[:, :, 0] -= center_xy[0]
    all_t[:, :, 1] -= center_xy[1]
    all_t[:, :, 2] -= min_xyz[2]

    out_file = Path(out_path)
    out_file.parent.mkdir(parents=True, exist_ok=True)
    write_stl(out_file, all_t, out_file.stem)

    verts = all_t.reshape(-1, 3)
    bmin = verts.min(axis=0)
    bmax = verts.max(axis=0)
    size = bmax - bmin
    print(f"[OK] Generated: {out_file}")
    print(f"  Triangles: {len(all_t):,}")
    print(f"  Footprint: {size[0]:.1f} x {size[1]:.1f} x {size[2]:.1f} mm")
    print(
        f"  Bounds: X=[{bmin[0]:.1f}, {bmax[0]:.1f}], "
        f"Y=[{bmin[1]:.1f}, {bmax[1]:.1f}], Z=[{bmin[2]:.3f}, {bmax[2]:.3f}]"
    )


def main():
    ap = argparse.ArgumentParser(description="Generate combined 3-part build plate STL.")
    ap.add_argument("--stl-dir", default=str(DEFAULT_STL_DIR), help="Directory with oriented part STLs")
    ap.add_argument(
        "--out",
        default=None,
        help="Output plate STL path (default: <stl-dir>/Power_Outage_Monitor_All_Parts_Plate.stl)",
    )
    ap.add_argument(
        "--gap",
        type=float,
        default=DEFAULT_GAP_MM,
        help=f"Clearance gap between parts in mm (default: {DEFAULT_GAP_MM})",
    )
    args = ap.parse_args()

    out_path = args.out if args.out else str(Path(args.stl_dir) / DEFAULT_PLATE_NAME)
    try:
        build_plate(args.stl_dir, out_path, gap_mm=args.gap)
    except (OSError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
