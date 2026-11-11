#!/usr/bin/env python3
"""STL print gate (numpy only): bbox, volume, triangles, shells, watertightness,
bed contact check, and downward-facing overhang report.

Exit code 0 only when:
- the mesh is watertight (every directed edge used once with one opposite edge),
- the mesh has the expected number of shells (default 1), and
- the lowest point along the given up axis is within 0.01 mm of zero.
"""

import argparse
from collections import defaultdict
import json
from pathlib import Path
import sys
import numpy as np

_HERE = str(Path(__file__).resolve().parent)
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from stl_io import analyze_mesh, read_stl

BED_ZERO_TOL_MM = 0.01
DEFAULT_UP_AXIS = "+Y"
DEFAULT_BED_TOL_MM = 0.05
DEFAULT_EXPECTED_SHELLS = 1


def evaluate_stl(
    path,
    up_axis=DEFAULT_UP_AXIS,
    bed_tol=DEFAULT_BED_TOL_MM,
    expected_shells=DEFAULT_EXPECTED_SHELLS,
):
    """Run mesh integrity, bed contact, and overhang analysis on an STL file."""
    t = read_stl(path)
    metrics = analyze_mesh(t)
    lo = metrics["bbox_min"]
    hi = metrics["bbox_max"]

    ax = {"X": 0, "Y": 1, "Z": 2}[up_axis[1]]
    sg = 1 if up_axis[0] == "+" else -1
    bed = float(lo[ax] if sg > 0 else -hi[ax])

    if len(t) > 0:
        v0, v1, v2 = t[:, 0], t[:, 1], t[:, 2]
        cr = np.cross(v1 - v0, v2 - v0)
        area = np.linalg.norm(cr, axis=1) / 2.0
        nrm = cr / np.maximum(np.linalg.norm(cr, axis=1)[:, None], 1e-12)
        up = nrm[:, ax] * sg
        h = t[:, :, ax].mean(axis=1) * sg
        zmax_tri = (t[:, :, ax] * sg).max(axis=1)
        on_bed = (zmax_tri - bed) < bed_tol
        over = (up < -np.cos(np.radians(45)) - 1e-6) & ~on_bed
        flat = (up < -0.999) & ~on_bed
        bins = defaultdict(float)
        for hh, ar in zip(h[over] - bed, area[over]):
            bins[round(float(hh), 1)] += float(ar)
        over_area = float(area[over].sum())
        flat_area = float(area[flat].sum())
        over_by_h = sorted(((k, round(v, 2)) for k, v in bins.items()))[:60]
    else:
        over_area = 0.0
        flat_area = 0.0
        over_by_h = []

    failures = []
    if metrics["triangles"] == 0:
        failures.append("mesh has 0 triangles")
    if not metrics["watertight"]:
        failures.append(f"mesh is not watertight (bad_edges={metrics['bad_edges']})")
    if metrics["shells"] != expected_shells:
        noun = "shell" if expected_shells == 1 else "shells"
        failures.append(f"expected {expected_shells} {noun}, got {metrics['shells']}")
    if abs(bed) > BED_ZERO_TOL_MM:
        failures.append(
            f"lowest point along {up_axis} is {bed:.4f} mm (exceeds +/-{BED_ZERO_TOL_MM:.2f} mm)"
        )

    res = {
        "file": str(path),
        "triangles": metrics["triangles"],
        "bbox_min": [round(float(x), 3) for x in lo],
        "bbox_max": [round(float(x), 3) for x in hi],
        "size": [round(float(x), 3) for x in (hi - lo)],
        "volume": round(metrics["volume"], 2),
        "watertight": metrics["watertight"],
        "bad_edges": metrics["bad_edges"],
        "shells": metrics["shells"],
        "bed_min_up": round(bed, 3),
        "overhang_area_gt45": round(over_area, 2),
        "flat_downfacing_area": round(flat_area, 2),
        "overhang_by_height": over_by_h,
        "passed": len(failures) == 0,
        "failures": failures,
    }
    return res


def main():
    ap = argparse.ArgumentParser(description="Verify binary STL manifoldness, shell count, and bed contact.")
    ap.add_argument("stl", help="Path to binary STL file")
    ap.add_argument(
        "--up",
        default=DEFAULT_UP_AXIS,
        choices=["+X", "-X", "+Y", "-Y", "+Z", "-Z"],
        help=f"Print bed upward axis (default: {DEFAULT_UP_AXIS})",
    )
    ap.add_argument(
        "--bed-tol",
        type=float,
        default=DEFAULT_BED_TOL_MM,
        help=f"Distance from bed plane (mm) excluded from overhang checks (default: {DEFAULT_BED_TOL_MM})",
    )
    ap.add_argument(
        "--expected-shells",
        type=int,
        default=DEFAULT_EXPECTED_SHELLS,
        help=f"Expected disconnected manifold shell count (default: {DEFAULT_EXPECTED_SHELLS})",
    )
    ap.add_argument("--json", help="Optional path to write JSON summary")
    args = ap.parse_args()
    if args.expected_shells < 1:
        ap.error("--expected-shells must be >= 1")

    try:
        res = evaluate_stl(
            args.stl,
            up_axis=args.up,
            bed_tol=args.bed_tol,
            expected_shells=args.expected_shells,
        )
    except (OSError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)

    out_text = json.dumps(res, indent=1)
    print(out_text)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            f.write(out_text + "\n")

    if not res["passed"]:
        print(f"FAIL: {args.stl}: {'; '.join(res['failures'])}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
