#!/usr/bin/env python3
"""Shared binary STL reader, writer, and manifold mesh checks."""

from collections import defaultdict
from pathlib import Path
import struct
import numpy as np

STL_DTYPE = np.dtype([("n", "<3f4"), ("v", "<9f4"), ("a", "<u2")])


def read_stl(path):
    """Read a binary STL file and return triangles as an (N, 3, 3) float64 array."""
    p = Path(path)
    data = p.read_bytes()
    if data.startswith(b"version https://git-lfs"):
        raise ValueError(f"{p}: Git LFS pointer file, not a binary STL")
    if data.lstrip()[:5].lower() == b"solid" and b"facet normal" in data[:1024]:
        raise ValueError(f"{p}: ASCII STL is not supported (binary STL required)")
    if len(data) < 84:
        raise ValueError(f"{p}: file too short ({len(data)} bytes) for binary STL header")
    tri_count = struct.unpack("<I", data[80:84])[0]
    expected_len = 84 + 50 * tri_count
    if len(data) != expected_len:
        raise ValueError(
            f"{p}: binary STL length mismatch (header declares {tri_count} triangles = "
            f"{expected_len} bytes, got {len(data)} bytes)"
        )
    if tri_count == 0:
        return np.zeros((0, 3, 3), dtype=np.float64)
    arr = np.frombuffer(data[84:], dtype=STL_DTYPE)
    return arr["v"].reshape(-1, 3, 3).astype(np.float64)


def write_stl(path, triangles, header_name=""):
    """Write an (N, 3, 3) triangle array to a binary STL file."""
    t = np.asarray(triangles, dtype=np.float64)
    if t.ndim != 3 or t.shape[1:] != (3, 3):
        raise ValueError(f"Expected (N, 3, 3) triangle array, got {t.shape}")
    v0, v1, v2 = t[:, 0], t[:, 1], t[:, 2]
    normals = np.cross(v1 - v0, v2 - v0)
    lengths = np.linalg.norm(normals, axis=1)
    lengths[lengths == 0.0] = 1.0
    normals /= lengths[:, None]

    rec = np.zeros(len(t), dtype=STL_DTYPE)
    rec["n"] = normals
    rec["v"] = t.reshape(-1, 9)

    header = header_name.encode("ascii", errors="replace")[:80].ljust(80, b" ")
    with open(path, "wb") as f:
        f.write(header)
        f.write(struct.pack("<I", len(t)))
        f.write(rec.tobytes())


def analyze_mesh(triangles, quant_mm=1e-4):
    """Compute bounding box, volume, watertightness, bad edges, and shell count."""
    t = np.asarray(triangles, dtype=np.float64)
    n_tri = len(t)
    if n_tri == 0:
        zero3 = np.zeros(3, dtype=np.float64)
        return {
            "triangles": 0,
            "bbox_min": zero3,
            "bbox_max": zero3,
            "volume": 0.0,
            "watertight": False,
            "bad_edges": 0,
            "shells": 0,
        }

    verts = t.reshape(-1, 3)
    lo = verts.min(axis=0)
    hi = verts.max(axis=0)
    v0, v1, v2 = t[:, 0], t[:, 1], t[:, 2]
    vol = float(np.einsum("ij,ij->i", v0, np.cross(v1, v2)).sum() / 6.0)

    q = np.round(verts / quant_mm).astype(np.int64)
    uniq, inv = np.unique(q, axis=0, return_inverse=True)
    faces = inv.reshape(-1, 3)

    edges = defaultdict(int)
    for a, b, c in faces:
        ia, ib, ic = int(a), int(b), int(c)
        edges[(ia, ib)] += 1
        edges[(ib, ic)] += 1
        edges[(ic, ia)] += 1

    bad_edges = 0
    for (p, r), count in edges.items():
        if p == r or edges.get((r, p), 0) != count or count != 1:
            bad_edges += 1

    parent = list(range(len(uniq)))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for a, b, c in faces:
        ra = find(int(a))
        rb = find(int(b))
        if rb != ra:
            parent[rb] = ra
        rc = find(int(c))
        if rc != ra:
            parent[rc] = ra

    shells = len({find(x) for x in range(len(uniq))})
    return {
        "triangles": int(n_tri),
        "bbox_min": lo,
        "bbox_max": hi,
        "volume": vol,
        "watertight": bad_edges == 0,
        "bad_edges": int(bad_edges),
        "shells": int(shells),
    }
