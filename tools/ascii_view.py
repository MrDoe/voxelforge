#!/usr/bin/env python3
"""ascii_view.py - render a PNG as an ASCII block map for text-only eyes.

describe_image captions are semantic but coarse; a terminal (or an agent
that cannot open the image at all) needs a layout read. This downsamples
the image through ffmpeg and prints one character per block:

    '~'  blue-dominant   (water / sky)
    'T'  green-dominant  (foliage)
    ' .:-=+*#%@'         luminance ramp for everything else, bright at '@'

The footer reports mean luma and blue/green coverage, so "the frame went
dark" is a number here, not an impression. --skip-left crops fixed UI
chrome before sampling (the Voxelforge sidebar is ~334 px wide).

block_map() is the importable form (vf_nav.py uses it); the CLI below is
for ad-hoc inspection.

Usage:
    python3 tools/ascii_view.py build/nav/shot.png
    python3 tools/ascii_view.py shot.png 120 --skip-left 340
"""
import argparse
import subprocess


def probe_size(path):
    out = subprocess.run(
        ['ffprobe', '-v', 'error', '-select_streams', 'v:0',
         '-show_entries', 'stream=width,height', '-of', 'csv=p=0', path],
        capture_output=True, text=True, check=True).stdout.strip()
    w, h = out.split(',')
    return int(w), int(h)


def block_map(path, cols=100, skip_left=0, ramp=' .:-=+*#%@'):
    """Return (lines, stats) for the image at path.

    lines: list of `cols`-wide rows, top to bottom.
    stats: dict(luma, blue, green, width, height, cols, rows).
    """
    W, H = probe_size(path)
    w = W - skip_left
    if w <= 0:
        raise ValueError('ascii_view: --skip-left larger than the image')
    rows = max(4, int(round(cols * H / w * 0.5)))
    filt = 'scale=%d:%d:flags=area' % (cols, rows)
    if skip_left:
        filt = 'crop=%d:%d:%d:0,' % (w, H, skip_left) + filt
    raw = subprocess.run(
        ['ffmpeg', '-loglevel', 'error', '-i', path, '-vf', filt,
         '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
        capture_output=True, check=True).stdout
    if len(raw) < cols * rows * 3:
        raise ValueError('ascii_view: short decode (%d bytes)' % len(raw))

    nblue = ngreen = 0
    total = 0.0
    lines = []
    for y in range(rows):
        line = []
        for x in range(cols):
            i = (y * cols + x) * 3
            r, g, b = raw[i], raw[i + 1], raw[i + 2]
            l = 0.299 * r + 0.587 * g + 0.114 * b
            total += l
            if b > r * 1.15 and b > g * 1.05 and b > 60:
                line.append('~')
                nblue += 1
            elif g > r * 1.08 and g > b * 1.08:
                line.append('T')
                ngreen += 1
            else:
                line.append(ramp[min(len(ramp) - 1,
                                     int(l / 256 * len(ramp)))])
        lines.append(''.join(line))
    n = cols * rows
    stats = dict(luma=total / n, blue=100.0 * nblue / n,
                 green=100.0 * ngreen / n, width=W, height=H,
                 cols=cols, rows=rows)
    return lines, stats


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image')
    ap.add_argument('cols', nargs='?', type=int, default=100,
                    help='characters per row (default 100)')
    ap.add_argument('--skip-left', type=int, default=0,
                    help='crop N px off the left edge before sampling')
    ap.add_argument('--ramp', default=' .:-=+*#%@')
    args = ap.parse_args()

    lines, stats = block_map(args.image, args.cols, args.skip_left, args.ramp)
    print('\n'.join(lines))
    print('mean luma %.1f  blue %.0f%%  green %.0f%%  '
          '(%dx%d -> %dx%d, skip-left %d)'
          % (stats['luma'], stats['blue'], stats['green'], stats['width'],
             stats['height'], stats['cols'], stats['rows'], args.skip_left))


if __name__ == '__main__':
    main()
