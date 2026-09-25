#!/usr/bin/env python3
"""Dump a .vxw layer's voxel records and reconstruct its box decomposition.

VXW layout (little-endian, src/voxel/worldfile.{hpp,cpp}):
  header 64 B: "VXWF", u32 version, f32 worldSize/voxelSize/waterLevel,
               u32 gridN, u32 brickN, u32 crc32(payload)
  v1 payload : legacy SVO arrays (chunkGrid/childBase/payload/handles/bricks,
               each u64 count + data) then records (u64 count + 16 B each)
  v2 payload : u32 sectionCount, then sections {u32 type, u64 size, data};
               type 0 = records, type 1 = legacy SVO

Record 16 B: u16 x,y,z; u8 r,g,b,a; u8 refl; u8 rough; u8 mat; u8 reserved;
             u16 pad. `reserved` is the per-cell texture override.

This tool is a read-only companion to `vf_mcp read_object` (which caps its
listing at 2000 voxels): it parses every record and clusters them back into
axis-aligned boxes, so an authored object can be inspected, diffed after a
rasterizer change, or reconstructed as a `write_object` shapes list.

Usage:
  tools/vxw_dump.py assets/hamlet_hall.vxw            # full decomposition
  tools/vxw_dump.py --layer thin_probe                 # resolves assets/
  tools/vxw_dump.py file.vxw --cells                  # one JSON line per cell
  tools/vxw_dump.py file.vxw --probe 612 530 452      # field-style probe
"""
import argparse
import collections
import json
import struct
import sys

VOXEL = 0.1
WORLD = 102.4
HEADER = 64
MAGIC = b"VXWF"


def u32(d, o):
    return int.from_bytes(d[o:o + 4], "little")


def f32(d, o):
    return struct.unpack_from("<f", d, o)[0]


def u64(d, o):
    return int.from_bytes(d[o:o + 8], "little")


def read_records(raw):
    """Return (records, version, meta). Handles v1 and v2."""
    if raw[:4] != MAGIC:
        raise SystemExit("not a VXW file (bad magic)")
    version = u32(raw, 4)
    meta = dict(world=f32(raw, 8), voxel=f32(raw, 12), water=f32(raw, 16),
                gridN=u32(raw, 20), brickN=u32(raw, 24))
    body = raw[HEADER:]

    def section(off, kind):
        """kind 0 = records, 1 = legacy SVO. Returns (next_off, records|None)."""
        stype = u32(body, off)
        size = u64(body, off + 4)
        data = body[off + 12:off + 12 + size]
        recs = None
        if stype == kind == 0:
            recs = parse_records(data)
        elif stype == kind == 1:
            pass  # SVO buffers: skipped, this tool is record-oriented
        return off + 12 + size, recs

    if version == 1:
        # walk the five legacy arrays, then the record block (its u64 count
        # stays in front of the records: parse_records consumes it itself)
        off = 0
        for _ in range(5):
            n = u64(body, off)
            off += 8 + 4 * n
        recs = parse_records(body[off:])
        return recs, version, meta
    if version == 2:
        count = u32(body, 0)
        off = 4
        recs = None
        for _ in range(count):
            off, r = section(off, 0)
            if r is not None:
                recs = r
        if recs is None:
            raise SystemExit("v2 file has no records section")
        return recs, version, meta
    raise SystemExit(f"unknown VXW version {version}")


def parse_records(d):
    n = u64(d, 0)
    out = []
    for i in range(n):
        o = 8 + 16 * i
        if o + 16 > len(d):
            break
        x, y, z = struct.unpack_from("<HHH", d, o)
        out.append(dict(x=x, y=y, z=z, r=d[o + 6], g=d[o + 7], b=d[o + 8],
                        a=d[o + 9], refl=d[o + 10], rough=d[o + 11],
                        mat=d[o + 12], tex=d[o + 13]))
    return out


def rects(pairs, gap=25):
    """Maximal axis-aligned rectangles from (x,z) cells, splitting on x-gaps."""
    S = set(pairs)
    out = []
    while S:
        x0, z0 = min(S)
        x1 = x0
        while (x1 + 1, z0) in S:
            x1 += 1
        z1 = z0
        while all((x, z1 + 1) in S for x in range(x0, x1 + 1)):
            z1 += 1
        for z in range(z0, z1 + 1):
            for x in range(x0, x1 + 1):
                S.discard((x, z))
        out.append((x0, x1, z0, z1))
    return sorted(out)


def decompose(recs):
    """Group by material + contiguous y-bands, then x/z rectangles."""
    byMat = collections.defaultdict(list)
    for r in recs:
        byMat[(r["mat"], r["tex"])].append(r)
    shapes = []
    for (mat, tex), group in sorted(byMat.items()):
        byY = collections.defaultdict(list)
        for r in group:
            byY[r["y"]].append((r["x"], r["z"]))
        years = sorted(byY)
        bands = []
        i = 0
        while i < len(years):
            j = i
            while j + 1 < len(years) and years[j + 1] == years[j] + 1:
                j += 1
            cells = [c for y in years[i:j + 1] for c in byY[y]]
            for (x0, x1, z0, z1) in rects(cells):
                bands.append(dict(y0=years[i], y1=years[j],
                                  x0=x0, x1=x1, z0=z0, z1=z1))
            i = j + 1
        shapes.append(dict(mat=mat, tex=tex, bands=bands))
    return shapes


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", help=".vxw path, or a bare layer name with --layer")
    ap.add_argument("--layer", action="store_true",
                    help="resolve <name> to assets/<name>.vxw")
    ap.add_argument("--cells", action="store_true",
                    help="emit one JSON object per cell instead of boxes")
    ap.add_argument("--probe", nargs=3, type=int, metavar=("X", "Y", "Z"),
                    help="report the record at one lattice cell")
    args = ap.parse_args()

    path = f"assets/{args.file}.vxw" if args.layer else args.file
    raw = open(path, "rb").read()
    recs, version, meta = read_records(raw)
    print(f"# {path}: version {version}, {len(recs)} records, "
          f"meta world={meta['world']} voxel={meta['voxel']} "
          f"gridN={meta['gridN']} crc-stored={u32(raw, 28)}")

    if args.probe:
        px, py, pz = args.probe
        hit = [r for r in recs if (r["x"], r["y"], r["z"]) == (px, py, pz)]
        if not hit:
            print(f"  ({px},{py},{pz}): EMPTY (no record)")
        for r in hit:
            w = (-WORLD / 2 + (r["x"] + 0.5) * VOXEL,
                 -WORLD / 2 + (r["y"] + 0.5) * VOXEL,
                 -WORLD / 2 + (r["z"] + 0.5) * VOXEL)
            print(f"  ({px},{py},{pz}): mat={r['mat']} tex={r['tex']} "
                  f"rgb=({r['r']},{r['g']},{r['b']}) refl={r['refl']} "
                  f"rough={r['rough']} world=({w[0]:.2f},{w[1]:.2f},{w[2]:.2f})")
        return

    if args.cells:
        for r in recs:
            print(json.dumps(r))
        return

    if not recs:
        print("# no records (SVO-only / empty layer)")
        return
    mn = [min(r[k] for r in recs) for k in "xyz"]
    mx = [max(r[k] for r in recs) for k in "xyz"]
    dim = [mx[i] - mn[i] + 1 for i in range(3)]
    print(f"# bounds x[{mn[0]}..{mx[0]}] y[{mn[1]}..{mx[1]}] z[{mn[2]}..{mx[2]}] "
          f"= {dim[0]}x{dim[1]}x{dim[2]} cells "
          f"(~{dim[0] * VOXEL:.1f}m x {dim[1] * VOXEL:.1f}m x {dim[2] * VOXEL:.1f}m)")
    mats = collections.Counter(r["mat"] for r in recs)
    print("# materials: " + ", ".join(f"{k}->{v}" for k, v in sorted(mats.items())))
    # a box decomposition is only meaningful for a single connected object
    shapes = decompose(recs)
    for sh in shapes:
        tag = f"mat {sh['mat']}" + (f" tex {sh['tex']}" if sh["tex"] else "")
        print(f"\n# {tag}: {len(sh['bands'])} box(es)")
        for b in sh["bands"]:
            w = (b["x1"] - b["x0"] + 1, b["y1"] - b["y0"] + 1, b["z1"] - b["z0"] + 1)
            print(f"  x[{b['x0']:4d}..{b['x1']:4d}] y[{b['y0']:4d}..{b['y1']:4d}] "
                  f"z[{b['z0']:4d}..{b['z1']:4d}]  {w[0]:3d}x{w[1]:3d}x{w[2]:3d}")


if __name__ == "__main__":
    main()
