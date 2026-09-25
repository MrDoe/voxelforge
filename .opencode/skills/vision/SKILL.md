---
name: vision
description: Inspect, judge and A/B-compare images and renders with the describe_image vision tool plus the repo's ascii_view.py. Use when asked how something LOOKS, to compare two renders/screenshots, to spot visual artifacts (floating splats, holes, banding, wrong shapes), or when a screenshot/diagram/mockup is referenced.
---

# Vision — judging what a render actually looks like

Two independent instruments. Use the right one, and use both when a claim
matters.

| instrument | good at | blind to |
| --- | --- | --- |
| `describe_image(filePath, systemPrompt?)` | qualitative shape, silhouettes, "are there floating discs", overall composition, artifacts a human would name | exact numbers; it **hallucinates detail** |
| `python3 .opencode/skills/voxel-object/scripts/ascii_view.py` | exact pixel coverage, luma, where in the frame something is | silhouette *shape* (an ellipse and a circle look the same) |
| pixel metrics (numpy on the PPM) | mean/percentile diffs, coverage, gradient energy | whether the result looks *right* |

The classic failure is using only one: ASCII said the reeds were present and
plausible while the vision model immediately reported "oversized lily-pad
discs hovering above the stems". Shape is a vision question; "how much" is a
measurement question.

## Hard limits of `describe_image`

1. **The file must live inside the workspace.** `/home/<user>/Downloads/x.png`
   fails with *"resolves outside the workspace"*. Copy it in first — `build/`
   is gitignored and a good scratch location:
   `cp /path/outside/img.png build/img.png`.
2. **It does not read PPM.** `./build/voxelforge --shot out.ppm` produces PPM,
   so convert before describing:
   ```bash
   python3 -c "from PIL import Image; Image.open('/tmp/opencode/x.ppm').save('build/x.png')"
   ```
   (PIL is available; `tests/*.py` already depend on it.)
3. **Always pass a `systemPrompt`.** Unsteered descriptions are generic. Ask
   for concrete, checkable claims and name the failure you are hunting for:
   *"do you see disc/plate shapes hovering in the air above vertical stems?"*
4. **It reports details that are not there.** Treat every claim as a lead to
   confirm, not a fact. Cross-check anything actionable with `ascii_view.py`
   or a numeric diff.

## Workflow

### Single image
```
describe_image("build/shot.png",
  systemPrompt="Describe X. Specifically: (1) ..., (2) ... . Be concrete
  about frame location (left/right/centre, foreground/background).")
```

### A/B comparison (the common case)
1. Render **both** variants from the *same* camera and resolution.
2. Convert both to PNG in the workspace.
3. Describe both with the *same* `systemPrompt` and compare the answers.
4. Add a numeric diff so the comparison has a number, not just prose:
   ```bash
   python3 -c "
   import numpy as np
   def load(p,w,h):
       d=open(p,'rb').read(); i=0
       for _ in range(3):
           while d[i:i+1].isspace(): i+=1
           while not d[i:i+1].isspace(): i+=1
       i+=1
       return np.frombuffer(d[i:i+w*h*3],dtype=np.uint8).reshape(h,w,3).astype(float)
   a=load('/tmp/opencode/a.ppm',960,540); b=load('/tmp/opencode/b.ppm',960,540)
   print('mean|diff| %.2f  changed>3 %.2f%%'%(np.abs(b-a).mean(), 100*(np.abs(b-a).mean(axis=2)>3).mean()))
   "
   ```

### Pitfalls that have actually cost time

- **Camera mismatch invalidates the comparison.** Two renders from different
  `--cam` values differ everywhere by construction (a bogus "88 % changed"
  that was just a different viewpoint). Check the camera before believing a
  diff.
- **Read the PPM header properly.** Trailing whitespace/newline after the
  pixel data makes `reshape` fail; slice to exactly `w*h*3` bytes.
- **Isolate the region you care about.** A whole-frame diff is dominated by
  whatever covers the most pixels. Build a mask (e.g. render with a layer
  enabled vs disabled) and measure inside it.
- **The vision provider is separate from the app's chat LLM.** `VF_LLM_URL` /
  `VF_LLM_MODEL` configure voxelforge's own chat; `describe_image` uses the
  provider configured for opencode.
- For world-object verification the `voxel-object` skill prefers pixel
  evidence over vision; keep that rule, and use vision to *find* candidate
  problems (then prove them with pixels).

## Related

- `.opencode/skills/voxel-object/SKILL.md` — the authoring loop and
  `ascii_view.py`.
- `tests/*.py` — the repo's own pixel-metric gates (`visual_check`,
  `fog_check`, `ssao_check`, `live_edit_check`, `texture_check`).
