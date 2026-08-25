#!/usr/bin/env python3
"""ASCII viewer for voxelforge PPM renders (no vision model needed).
Usage: ascii_view.py <out.ppm> [cols] [rows] [--crop x y w h]
Prints a luminance+hue character map so a text model can judge composition,
texture, artifacts, and coverage.
"""
import sys, struct

def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    idx = 2
    vals = []
    while len(vals) < 3:
        while data[idx:idx+1].isspace(): idx += 1
        if data[idx:idx+1] == b'#':
            while data[idx:idx+1] not in (b'\n', b''): idx += 1
            continue
        st = idx
        while not data[idx:idx+1].isspace(): idx += 1
        vals.append(int(data[st:idx]))
    idx += 1
    w, h, _ = vals
    return w, h, data[idx:idx + w*h*3]

def char_for(r, g, b):
    lum = (r + g + b) / 3.0
    # hue buckets
    if g > r + 20 and g > b + 20:
        base = 'g'                      # green
    elif b > r + 20 and b > g:
        base = 'b'                      # blue/sky
    elif r > g + 20 and g > b + 10:
        base = 'y'                      # warm (sand/wood/brown)
    elif abs(r-g) < 20 and abs(g-b) < 20:
        base = '#'                      # neutral
    else:
        base = '.'                      # mixed
    # intensity sub-levels
    if lum < 40:    return 'k' if base == '.' else base
    if lum < 70:    return base.lower()
    if lum < 110:   return base.lower()
    if lum < 150:   return base.upper()
    if lum < 190:   return base.upper()
    if lum < 230:   return base.upper()
    if lum < 255:   return base.upper()
    return 'W'

def main():
    path = sys.argv[1]
    cols = int(sys.argv[2]) if len(sys.argv) > 2 else 100
    rows = int(sys.argv[3]) if len(sys.argv) > 3 else 46
    crop = None
    if len(sys.argv) > 5 and sys.argv[4] == '--crop':
        crop = tuple(int(x) for x in sys.argv[5:9])
    w, h, px = read_ppm(path)
    if crop:
        cx, cy, cw, ch = crop
        w0, h0 = w, h
        px = px[(cy*w0)*3:(cy*w0 + ch*w0)*3]
        w, h = cw, ch
    n = w * h
    # average blocks: scale to cols x rows
    for yy in range(rows):
        line = ''
        for xx in range(cols):
            x0 = int(xx * w / cols); x1 = max(x0 + 1, int((xx+1) * w / cols))
            y0 = int(yy * h / rows); y1 = max(y0 + 1, int((yy+1) * h / rows))
            sr = sg = sb = 0; cnt = 0
            for y in range(y0, y1):
                base = y * w
                for x in range(x0, x1):
                    i = (base + x) * 3
                    sr += px[i]; sg += px[i+1]; sb += px[i+2]; cnt += 1
            line += char_for(sr//cnt, sg//cnt, sb//cnt)
        print(line)

if __name__ == '__main__':
    main()
