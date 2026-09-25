#!/usr/bin/env python3
"""vf_tour.py - the shared camera tour used by record_demo.py --mode dual.

A list of (label, seconds, action) steps. `action` is:

  None            just hold still for `seconds`
  "w"/"a"/"s"/... hold that flight key (WASD, Q/E for down/up)
  ("look", dx, dy)  hold RMB and sweep the pointer by dx,dy px over `seconds`

The village landmarks sit at x 5..23, z 11..26 (see the SHOTS table in
record_demo.py), the default spawn is (1, 2, 1.5) facing (5.3, 1, 11.3), the
flight speed is 4 m/s, and the water plane is y = -0.9. The altitudes below
keep the camera roughly between y 2 and y 9 so it never ends up swimming.

The SAME tour is replayed for every backend, so the only difference between
the two halves of the video is the renderer.
"""

TOUR = [
    ("hold, let it settle",           2.0, None),
    ("rise for a clear view",         2.0, "e"),
    ("turn right toward the tower",   2.2, ("look", 520, -40)),
    ("fly toward the village",        5.0, "w"),
    ("sweep across the skyline",      2.2, ("look", -700, 60)),
    ("strafe along the shore",        3.0, "a"),
    ("descend toward eye level",      1.6, "q"),
    ("advance to the market",         4.0, "w"),
    ("look up at the tower",          2.0, ("look", 240, -320)),
    ("circle the well",               3.2, "d"),
    ("push in on the hall",           3.4, "w"),
    ("hold on the hall facade",       2.4, None),
    ("climb for the overview",        3.2, "e"),
    ("tilt down to read the layout",  2.4, ("look", 0, 300)),
    ("drift back over the water",     4.0, "s"),
    ("pan left over the lake",        2.6, ("look", -620, -60)),
    ("settle wide, village in frame", 3.0, None),
]


def play(inp, log=print, script=None, budget=None, fill=None):
    """Run the tour. Returns the wall-clock seconds it took.

    `fill`    scale every step so the whole tour lasts about this many seconds
              (the script is authored at whatever length read well; a longer
              recording just runs the same path more slowly, and the pointer
              sweeps keep their pixel deltas so the framing is identical).
    `budget`  stop early rather than overrunning the recorder's -t window.
    """
    import time
    steps = list(script or TOUR)
    if fill:
        cur = sum(d for _, d, _ in steps)
        k = fill / cur if cur else 1.0
        steps = [(lb, d * k, a) for lb, d, a in steps]
    t0 = time.time()
    for label, dur, action in steps:
        if budget is not None and (time.time() - t0) + dur > budget:
            break
        log("%s (%.1fs)" % (label, dur))
        if action is None:
            time.sleep(dur)
        elif isinstance(action, tuple) and action[0] == "look":
            _, dx, dy = action
            inp.look_begin()
            inp.look(dx, dy, dur)
            inp.look_end()
        else:
            inp.hold(action, dur)
    return time.time() - t0


def total_seconds(script=None):
    return sum(d for _, d, _ in (script or TOUR))
