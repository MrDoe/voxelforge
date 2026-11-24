---
title: What OMO-Slim teaches about extending opencode-crosstalk
tags: [crosstalk, opencode, plugins, multi-agent, coordination, tooling]
sourceRefs: [/home/christoph/code/opencode-crosstalk/README.md, /home/christoph/code/opencode-crosstalk/src/types.ts, /home/christoph/code/opencode-crosstalk/src/deliverer.ts, /home/christoph/code/opencode-crosstalk/src/config.ts, https://raw.githubusercontent.com/alvinunreal/oh-my-opencode-slim/master/README.md, https://raw.githubusercontent.com/alvinunreal/oh-my-opencode-slim/master/docs/background-orchestration.md, https://raw.githubusercontent.com/alvinunreal/oh-my-opencode-slim/master/docs/tools.md]
lastReviewed: 2026-11-24
---

# What OMO-Slim teaches about extending opencode-crosstalk

Scope note: [[sources/oh-my-opencode-slim]] is an **agent-orchestration policy layer** (routing, presets, review gates) over native OpenCode subagents; `opencode-crosstalk` is a **peer-coordination mesh** (presence, names, messages, file leases). Crosstalk should borrow OMO's *coordination discipline*, not its model-routing machinery. With one local provider/model here, the routing half has no payload.

## What crosstalk already does that matches OMO's best rules

Do not "modernise" these into a second state machine:

- **Host owns lifecycle.** OMO explicitly removed its own job board, wall-clock watchdog, concurrency queue and periodic idle TODO wake ("not another job ledger"). Crosstalk's event-reducer registry is necessary because V2 exposes no `session.list()`; it should stay a mesh, not become a scheduler.
- **Mailbox is authoritative; injection is best effort.** OMO's completion delivery is host-owned and at-least-once. Crosstalk's bounded injection retry + TTL mailbox is the same shape.
- **Silence is not evidence.** OMO reports `unknown` rather than inventing status; crosstalk's `stale` (10 min) / `evict` (1 h) thresholds and `addressable` flag do the same.
- **Exact identity, not fuzzy authority.** OMO verifies `parentID` on task controls; crosstalk's name is a label, session ID is authority, and leases normalize to exact paths (no globs, atomic batches).

Highest-value borrowings, in priority order:

1. **A live self-check instead of a static snippet.** OMO registers skills in-process (no disk copy) and ships a skill for tuning the plugin itself; it also has a `ping all agents` verification ritual. Crosstalk's `announce` briefing is already dynamic, but the project AGENTS.md block is a *static copy* that must be hand-replaced and can drift from the tools. Measured here: a stored quirk claimed the seven tools were unavailable in this harness while `status`/`peers`/`claim`/`send` were in fact callable — a `crosstalk_doctor` tool (tool visibility, permission action, `codemode`, scope, peer count/addressability, config warnings) plus a bundled tuning skill would kill that whole failure class. The `<!-- opencode-crosstalk:begin/end -->` markers already make a marker-based snippet updater easy.

2. **Ownership-scoped replies, and fix the scope asymmetry.** OMO's `task_report` takes the recipient from the host-owned `parentID` — callers cannot choose it — and `task_reply` verifies the exact child. In crosstalk, `crosstalk_peers` has a `scope` parameter but `crosstalk_send` does not: a peer in another project can steer you, and you cannot reply (`outside the project scope`) even with `delivery: "queue"`. Add `crosstalk_reply { to: <messageId> }` that resolves the sender from the inbox (the `replyTo` field already exists on `send`), verify scope, and return a concrete relay hint ("not addressable — route via <shared-scope peer>") instead of a bare refusal.

3. **Structured briefs with an evidence contract.** OMO's delegation brief is goal + constraints + paths/lines + allowed edits + validation owner + expected evidence, and "submission is not completion" — summaries are claims, artifacts must be verified. Crosstalk's `status.goal/workingOn/note` and `send.text/topic` are free-form. Add a `kind: "task"` message with typed meta (`writeScope` paths, `validation` owner, `evidence` expected) and tie it to the lease, so `peers` answers "who owns what, with what evidence" — directly useful for this repo's shared surfaces (`build/`, shaders, `scenery/<name>/world.json`, overlays).

4. **Continuation by exact session ID.** OMO continues the same child via `subagent(sessionID, prompt)` and repairs in that context instead of spawning a new lane. Crosstalk has the session ID already; make continuation explicit (`crosstalk_continue`) so steering an idle peer reuses its context rather than looking like a fresh task.

5. **Decline semantics.** OMO's task-fit rejection returns a brief reason and the orchestrator reroutes instead of resending unchanged work. Add `kind: "decline"` with a reason; `renderDelivery` should not demand an answer to a decline, and the sender should be nudged to reroute.

6. **A coordination loop guard.** OMO's guard counts only calls whose results are byte-identical to the previous one (confirmed in `tool.execute.after`, so parallel calls cannot inflate it), warns at 3 and hard-blocks read-only tools at 5 — while exempting status reads so result retrieval cannot deadlock. Crosstalk has bounded delivery retries and `maxWaitMs` but no guard against two polite agents looping `send`/`wait`/`inbox`. Add one, exempting `inbox` and `claim release`.

7. **Optional pane linking.** OMO's multiplexer integration opens each agent in its own tmux/zellij/kitty pane so work is watchable; crosstalk has avatars and summaries but no jump-to-peer. A `crosstalk_focus { sessionID }` TUI action would strengthen the "see each other" half without adding a ledger.

## What not to copy

Multi-model routing, Council consensus, preset switching, marketplace packages, dynamic agent prompts and the autonomous job lifecycle. They solve a different problem, depend on a heterogeneous provider pool this workspace does not have, and would duplicate native OpenCode 2.3+ subagent behaviour. The cross-session value here is *who owns which bytes and what evidence closes the task* — keep crosstalk small around that.

See also [[concepts/decision-driven-world-editing]] (this project's own experiment-loop policy, which a `kind: "task"` brief should interlock with rather than compete against).
