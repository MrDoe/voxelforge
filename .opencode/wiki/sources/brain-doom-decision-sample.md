---
title: "swedishembedded/brain — decision model learning to play DOOM from the game's own state"
tags: [external-source, decision-model, agent-loop, mission, reward, archive]
sourceRefs: [https://github.com/swedishembedded/brain/tree/main/samples/decision/doom]
lastReviewed: 2026-11-24
---

External reference for [[concepts/decision-driven-world-editing]]. The 1993
engine (a Chocolate Doom fork with an HTTP API in its game loop) runs in
**lockstep** — it advances only when the agent sends a step. Every decision
reads a JSON observation rendered as **prose**; no pixels reach the model.
The frame in the window is for the human.

## The load-bearing mechanisms

- **A decision model, not a policy network.** A normal RL policy ends in a
  layer whose width is the action space, so actions must be known at training
  time and stable across steps. Here the **option list is rebuilt from the
  state on every decision and delivered as sentences**: "attack the former
  human sergeant 180 units away, 12 degrees left" exists only while that
  sergeant is alive and in sight. The model reads what an option *means*, so
  an option it never trained on still works. Each option also carries a hidden
  `tag` (intent class), engine commands, duration and geometry numbers for the
  inspector and the scripted teacher — the model sees only the sentence.
- **The mission is prepended to every option.** `clear`, `speedrun`,
  `survive`, `uvmax` each change reward weights; `--mix` samples a mission per
  episode so one policy must read what it was asked to do. The mission rides
  with each decision, not just the episode start.
- **Honest observation.** Facts, never advice; no answer written into the
  question. The exit's position was withheld until the player had seen it; a
  distance field flooded from the exit through unopened doors was replaced by
  one flooding only over sectors whose walls the player had looked at, leading
  to the **frontier** ("ground nobody has looked at") until the exit was found.
  Only what a player can perceive is reported: wall texture name **and
  alignment offset**, not the linedef special; "a push reached a wall", not
  where the secrets are. An agent given the answer plays well and learns
  nothing transferable.
- **Bearings, not map coordinates.** "12 degrees left" is actionable; "at 143
  degrees" makes the reader subtract its own facing. Memory stores positions
  but reports bearings; monsters go stale after ten decisions out of sight,
  items only on evidence (you reached the spot and found nothing).
- **History is in the observation** because each decision is an independent
  forward pass: `stuck`, `visits_here`, `patches`, `seeing_through` (a
  commitment to keep going), `already_tried`. Removing it cost **2.67 of
  return** on its own: the scripted teacher used history the policy couldn't
  see, so cloning taught a mapping that did not exist; a greedy policy without
  it loops forever.
- **Turning and moving are separate decisions.** Doing both in one step walks
  the arc of the turn into the inside corner; measured 13 % of the way to the
  exit and then 400 decisions going nowhere. Face first, then move.
- **Prefix-computable reward (`--reward gauge`).** Pay decision *t* exactly
  `M(h_t+1) − M(h_t)` for a score computable on a prefix (running maximum or
  monotone counter), so `return == progress` algebraically. The older shaped
  scheme had ~92 % of an episode's return in an exploration bonus the score
  did not read — trained on one number, kept on another.
- **Search, then compression.** An archive of reached states (quality-diversity
  niches include keys, **weapons and health band**; omitting those two was
  worth 2.6× on an otherwise identical campaign), operators
  `wander/probe/commit/chase/frisk`, a bandit allocating budget by measured
  gain per second, then cloning the archive into weights. The policy also
  rejoins search as a *sampled* proposal, not an argmax, or each generation
  walks the same path.
- **"Search may use snapshots. The artifact may not."** Restoring an engine
  snapshot makes search affordable, but the produced action list must replay
  from the level's own start and reproduce kills/secrets/exit/tic count. An
  action recorded merely as the bytes sent to the game named more than one
  option ("walk toward the shotgun" and "advance" both send `forward 8`), so
  replay took the first one and diverged; the format is now `tag|tics|commands`.
- **Gate by paired sign test** over the *same* episodes for two heads, not a
  single run each. One row per level, because an average hides which moved.
- Measured decision latency: **28–33 ms end to end** on one Tesla P40 (~28 ms
  model, ~5 ms game), about 30 decisions/s — above human play rate.

## Why it matters here

The sample is the source for the design in [[concepts/decision-driven-world-editing]]:
state-built option sentences, mission-conditioned option families and scoring,
honest observation, history in the state, one-engine-path action compilation,
and replayable option identity. It also supplies the caution: almost every
advance in the sample came from fixing the *observation/option representation*,
not from changing the model.
