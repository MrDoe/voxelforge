#!/usr/bin/env python3
"""vf_nav.py - decision-driven camera navigation for a live Voxelforge window.

One iteration:
  raise (wmctrl) -> capture (x11grab) -> ASCII map (tools/ascii_view.py)
  -> decision (opencode-rag decide: the same tev1 model the make_decision
  tool uses) -> basic move (XTEST via tools/vf_input.py) -> next capture.

Basic move vocabulary (v1):
  forward:2.5 / back:1.5 / strafe_left:2 / strafe_right:2
  up:2 / down:1.5           hold w/s/a/d/e/q for the given seconds
  turn_left / turn_right    yaw 45 deg (an explicit arg overrides)
  pitch_up / pitch_down     pitch 25 deg
  stop                      no motion

Usage (run from the repo root):
  python3 tools/vf_nav.py look --out build/nav/shot.png
  python3 tools/vf_nav.py move --spec "turn_left,forward:3" --out build/nav/shot.png
  python3 tools/vf_nav.py run --out build/nav/nav --steps 3
"""
import argparse
import json
import math
import os
import re
import subprocess
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ascii_view  # noqa: E402
from vf_input import VFInput  # noqa: E402

WINDOW_TITLE = 'Voxelforge'
# Sidebar chrome to crop for the decision map (full sidebar is ~334 px).
SIDEBAR_PX = 340
STALE_IDS = '/tmp/opencode/nav/stale_ids.json'
TMP = '/tmp/opencode/nav'

KEY_HOLDS = {
    'forward': ('w', 2.5), 'back': ('s', 1.5),
    'strafe_left': ('a', 2.0), 'strafe_right': ('d', 2.0),
    'up': ('e', 2.0), 'down': ('q', 1.5),
}
ANGLE_MOVES = {
    'turn_left': (-45.0, 0.0), 'turn_right': (45.0, 0.0),
    'pitch_up': (0.0, 25.0), 'pitch_down': (0.0, -25.0),
}

QUESTIONS = [{
    'id': 'next_move',
    'type': 'choice',
    'instructions': ('Pick the single best next camera move for a tour of '
                     'the voxel hamlet.'),
    'criteria': {
        'forward': 'fly straight ahead along the current view direction',
        'back': 'back away from whatever is close ahead',
        'turn_left': 'turn the view left ~45 degrees',
        'turn_right': 'turn the view right ~45 degrees',
        'strafe_left': 'slide left while keeping the view direction',
        'strafe_right': 'slide right while keeping the view direction',
        'up': 'climb for a higher vantage point',
        'down': 'descend a little',
        'stop': 'hold position - use ONLY when the view already shows the '
                'village, water, or open landscape',
    },
}]


def find_input():
    stale = set()
    try:
        with open(STALE_IDS) as f:
            stale = set(json.load(f))
    except Exception:
        pass
    return VFInput(title=WINDOW_TITLE, exclude=stale)


def activate(inp):
    """Raise + focus through the WM.

    Xlib set_input_focus alone did not survive: another window stayed stacked
    above and x11grab photographed *it* (dark frame, mean luma ~23). wmctrl's
    _NET_ACTIVE_WINDOW goes through the WM and does raise it.
    """
    subprocess.run(['wmctrl', '-i', '-a', '0x%08x' % inp.win.id], check=False)
    time.sleep(0.6)


def capture(inp, out):
    activate(inp)
    x, y, w, h = inp.geometry()
    subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-f', 'x11grab',
                    '-video_size', '%dx%d' % (w, h), '-i', ':0+%d,%d' % (x, y),
                    '-frames:v', '1', out], check=True)
    return out


def execute(inp, spec_item):
    if ':' in spec_item:
        name, arg = spec_item.split(':', 1)
        arg = float(arg)
    else:
        name, arg = spec_item, None
    activate(inp)
    if name == 'stop':
        return
    if name in KEY_HOLDS:
        key, default = KEY_HOLDS[name]
        inp.hold(key, arg if arg is not None else default)
    elif name in ANGLE_MOVES:
        d_yaw, d_pitch = ANGLE_MOVES[name]
        k = (arg / 45.0) if arg is not None else 1.0
        inp.look_begin()
        inp.look_to(math.radians(d_yaw * k), math.radians(d_pitch * k), 1.2)
        inp.look_end()
    else:
        raise SystemExit('vf_nav: unknown move %r' % name)


DECISION_URL = 'http://127.0.0.1:11434/v1/systemone'
DECISION_MODEL = 'tev1:0.8b'


def _decide_http(state, questions):
    body = {
        'model': DECISION_MODEL,
        'state': state,
        'questions': {q['id']: {k: v for k, v in q.items() if k != 'id'}
                      for q in questions},
    }
    req = urllib.request.Request(
        DECISION_URL, data=json.dumps(body).encode(),
        headers={'content-type': 'application/json'})
    with urllib.request.urlopen(req, timeout=30) as r:
        ans = json.loads(r.read().decode())
    a = ans['answers'][questions[0]['id']]
    probs = a.get('probabilities')
    if isinstance(probs, list):
        probs = dict(zip(questions[0].get('criteria', {}).keys(), probs))
    if not isinstance(probs, dict):
        probs = {}
    return a.get('choice'), probs


def fast_move(lines, stats, last):
    """Rule-based move with zero model calls - a latency floor and a fallback,
    deliberately dumb. The llm policy is the real navigator."""
    flat = ''.join(lines)
    n = len(flat)
    blue = flat.count('~') / n
    green = flat.count('T') / n
    dark = sum(flat.count(c) for c in ' .:') / n
    bright = sum(flat.count(c) for c in '%@') / n
    close = blue < 0.05 and green < 0.10 and (dark + bright) < 0.35
    if close:
        # against a surface: back off, then turn, then climb, then back
        return {'back': 'turn_right', 'turn_right': 'up',
                'up': 'back'}.get(last, 'back')
    if blue > 0.30 or green > 0.10:
        return 'forward'
    if stats['luma'] < 55:
        return 'back'
    return 'forward'


def decide(state, questions=QUESTIONS):
    """Ask tev1 directly (the same backend the make_decision tool and
    `opencode-rag decide` use). HTTP avoids the node CLI startup; the CLI is
    the fallback."""
    try:
        return _decide_http(state, questions)
    except Exception as e:
        print('decide: http failed (%s), falling back to CLI' % e)
    os.makedirs(TMP, exist_ok=True)
    sp = os.path.join(TMP, '_state.txt')
    qp = os.path.join(TMP, '_questions.json')
    with open(sp, 'w') as f:
        f.write(state)
    with open(qp, 'w') as f:
        json.dump(questions, f)
    r = subprocess.run(['opencode-rag', 'decide', '--state-file', sp,
                        '--questions-file', qp, '--json'],
                       capture_output=True, text=True, check=True)
    m = re.search(r'\n(\{.*\})\s*$', r.stdout, re.S)
    if not m:
        raise SystemExit('vf_nav: cannot parse decide output:\n' + r.stdout)
    a = json.loads(m.group(1))['answers'][0]
    return a['choice'], a.get('probabilities', {})


def summarise(lines, stats):
    """Short verbal digest of the block map - the 0.8b model reads this
    far better than the raw map (measured: raw maps gave near-identical
    posteriors for visibly different views)."""
    cols = len(lines[0])
    flat = ''.join(lines)
    n = len(flat)
    water = flat.count('~') / n
    green = flat.count('T') / n
    dark = sum(flat.count(c) for c in ' .:') / n
    bright = sum(flat.count(c) for c in '%@') / n
    thirds = []
    for k in range(3):
        seg = ''.join(l[k * cols // 3:(k + 1) * cols // 3] for l in lines)
        thirds.append((seg.count('~') / len(seg), seg.count('T') / len(seg)))
    side = ('left', 'centre', 'right')[max(range(3), key=lambda k: thirds[k][1])]
    p = []
    p.append('water ' + ('fills much of the frame' if water > 0.25 else
                         'visible in part of the frame' if water > 0.05 else
                         'not visible'))
    if green > 0.25:
        p.append('foliage fills much of the frame, heaviest %s' % side)
    elif green > 0.05:
        p.append('some foliage (%s)' % side)
    else:
        p.append('no foliage')
    if water < 0.05 and green < 0.10 and (dark + bright) < 0.35:
        p.append('close-up surfaces fill most of the frame')
    p.append('frame is %s' % ('bright' if stats['luma'] > 130 else
                              'dim' if stats['luma'] < 90 else 'mid-toned'))
    return 'View digest: ' + '; '.join(p) + '.'


def describe_state(shot, cols=48):
    lines, stats = ascii_view.block_map(shot, cols=cols, skip_left=SIDEBAR_PX)
    header = ('Voxelforge lakeside hamlet, live camera. '
              + summarise(lines, stats) + '\nASCII map of the same view '
              "(screen blocks; '~' water/sky blue, 'T' foliage green, "
              "' .:-=+*#%@' brightness ramp, top row first):")
    footer = ('stats: mean luma %.0f, blue %.0f%%, green %.0f%%.'
              % (stats['luma'], stats['blue'], stats['green']))
    return header + '\n' + '\n'.join(lines) + '\n' + footer, lines, stats


def one_step(inp, out, cols=48, policy='llm', last=None):
    capture(inp, out)
    state, lines, stats = describe_state(out, cols)
    t0 = time.time()
    if policy == 'fast':
        choice, probs = fast_move(lines, stats, last), {}
    else:
        state += '\nLast move: %s. Prefer a move that changes the view ' \
                 'unless a landmark is clearly visible.' % (last or 'none')
        choice, probs = decide(state)
        # Anti-stuck guard: the 0.8b model has a stop bias on ambiguous
        # frames; two stops in a row would freeze the tour, so escape with a
        # deterministic turn instead.
        if choice == 'stop' and last == 'stop':
            print('guard: repeated stop on an unchanging view -> turn_right')
            choice, probs = 'turn_right', {}
    dt = time.time() - t0
    top = sorted(probs.items(), key=lambda kv: -kv[1])[:3]
    print('view (%s): luma %.0f blue %.0f%% green %.0f%%'
          % (out, stats['luma'], stats['blue'], stats['green']))
    print('\n'.join(lines))
    print('decision [%s %.2fs]: %s  p=%.2f  top3=%s'
          % (policy, dt, choice, probs.get(choice, 0.0),
             ', '.join('%s=%.2f' % kv for kv in top)))
    with open(os.path.join(TMP, 'nav_log.txt'), 'a') as f:
        f.write('%s -> %s p=%.2f top3=%s (decide %.2fs)\n'
                % (out, choice, probs.get(choice, 0.0),
                   ','.join('%s=%.2f' % kv for kv in top), dt))
    execute(inp, choice)
    capture(inp, out[:-4] + 'b.png')  # post-move view for review
    return choice


def scan(inp, out, step=45.0):
    """Capture a full turn, one shot per heading, and report digests."""
    n = max(1, int(round(360.0 / step)))
    for k in range(n):
        shot = '%s%02d.png' % (out, k)
        capture(inp, shot)
        state, lines, stats = describe_state(shot, cols=40)
        print('%s heading %d: %s' % (shot, k * step, state.split('\n')[0]))
        execute(inp, 'turn_right:%g' % step)


def probe(inp, log_path, out=None):
    """Ctrl+LMB at the window centre; report the app's pick line.

    The pick ray runs against the records-derived VoxelField, so it names the
    world position/material/layer ahead - or reports that the centre misses
    all geometry (looking at nothing / outside the world)."""
    activate(inp)
    x, y, w, h = inp.geometry()
    inp.warp_to(x + w // 2, y + h // 2)
    time.sleep(0.4)
    inp.focus()
    size = os.path.getsize(log_path) if os.path.exists(log_path) else 0
    inp.key('ctrl', True)
    time.sleep(0.2)
    inp.click(1, hold=0.15)
    inp.key('ctrl', False)
    time.sleep(0.8)
    hit = None
    if os.path.exists(log_path):
        with open(log_path, 'rb') as f:
            f.seek(size)
            new = f.read().decode(errors='replace')
        for line in new.splitlines():
            if 'pick selected' in line:
                hit = line
    print('probe:', hit or 'no pick hit (nothing pickable at the centre)')
    return hit


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)

    p = sub.add_parser('look')
    p.add_argument('--out', required=True)

    p = sub.add_parser('move')
    p.add_argument('--spec', required=True)
    p.add_argument('--out')

    p = sub.add_parser('run')
    p.add_argument('--out', required=True, help='output prefix')
    p.add_argument('--steps', type=int, default=1)
    p.add_argument('--cols', type=int, default=48)
    p.add_argument('--policy', default='llm', choices=['llm', 'fast'])

    p = sub.add_parser('scan')
    p.add_argument('--out', required=True, help='output prefix')
    p.add_argument('--step', type=float, default=45.0, help='degrees per shot')

    p = sub.add_parser('probe')
    p.add_argument('--log', default='/tmp/opencode/nav/app.log',
                   help='app log to scan for the pick line')
    args = ap.parse_args()

    inp = find_input()
    if args.cmd == 'look':
        capture(inp, args.out)
        print('captured', args.out)
    elif args.cmd == 'move':
        for item in args.spec.split(','):
            execute(inp, item.strip())
        if args.out:
            capture(inp, args.out)
            print('captured', args.out)
    elif args.cmd == 'run':
        last = None
        for i in range(args.steps):
            last = one_step(inp, '%s%02d.png' % (args.out, i), args.cols,
                            args.policy, last)
    elif args.cmd == 'probe':
        probe(inp, args.log)
    else:
        scan(inp, args.out, args.step)


if __name__ == '__main__':
    main()
