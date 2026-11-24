---
title: oh-my-opencode-slim (OMO-Slim) — assessment for this workspace
tags: [opencode, plugin, multi-agent, tooling, sources]
sourceRefs: [https://github.com/alvinunreal/oh-my-opencode-slim, https://raw.githubusercontent.com/alvinunreal/oh-my-opencode-slim/master/README.md, /home/christoph/.config/opencode/opencode.json]
lastReviewed: 2026-11-24
---

# oh-my-opencode-slim (OMO-Slim)

An **OpenCode plugin**, not a model. It adds a six-agent orchestration layer: an Orchestrator plans a work graph and dispatches background specialists (Explorer, Oracle, Council, Designer, Fixer, optional Observer), each assignable a different model. Its pitch is routing each task to the best quality/speed/cost trade rather than forcing one model to do everything. MIT-licensed; main repo `alvinunreal/oh-my-opencode-slim` (~10.3k stars, actively pushed 2026-11-23). Requires OpenCode ≥ 2.3.0. Install: `bunx oh-my-opencode-slim@latest install` (or `npx ...`); config lives at `~/.config/opencode/oh-my-opencode-slim.json`, with presets switchable via `/preset`. Bundled skills include `deepwork`, `codemap`, `simplify`, `verification-planning`, `worktrees`, `step-back`, `reflect`.

## Fit with this workspace — marginal

Measured against the local config on 2026-11-24: OpenCode v2.3.2 (version OK), one provider `strata` at `127.0.0.1:8081/v1` with a single local model `qwen3.8-flash-next-q2_0`, and no `plugins` array in the global config. Consequences:

- **The core value evaporates with one model.** Multi-model routing, the Council's cross-model consensus, and cheap-explorer/strong-oracle splitting all assume heterogeneous providers. Pointing all six agents at the same local Qwen flash model adds delegation and context overhead while every specialist reasons identically. Native OpenCode subagents (already available via the `subagent` tool) provide delegation without the extra layer.
- **It would be a second orchestration system on top of an already customized one.** This project has its own experiment loop — OpenCodeRAG retrieval, the shared quirk store, crosstalk multi-session coordination, project-specific agents/skills, and the in-app AI panel's dedicated session. See [[concepts/decision-driven-world-editing]] and [[entities/app-subsystems]]. Another Orchestrator that plans its own background jobs can duplicate or fight those conventions.
- **Context cost is real here.** The compaction buffer is 60,548 tokens on a 131k context; Slim's agent prompts, delegation rules and bundled skills consume budget that the project's many domain skills already compete for. The upstream "slim" claim is relative to the much larger full oh-my-opencode, not small in absolute terms.
- **Default presets don't match the setup.** The generated presets target OpenAI/OpenCode Go models; all agents would have to be repointed at the `strata` provider manually.

## When it would be worth trying

If the goal is autonomous, multi-phase work with build/review gates on a large codebase — `deepwork` phases with Oracle review, automatic `verification-planning` before non-trivial changes — Slim is free, mature, and actively maintained, and its `worktrees` skill fits this repo's shared-tree hazards. If adopted here: configure every agent to the local model explicitly, disable unneeded agents/hooks (e.g. `disabled_hooks: ["council-inject"]`), and check that background jobs don't collide with crosstalk's session leases before running parallel edits.

Related: [[sources/brain-doom-decision-sample]] (the other external agent-loop source in this wiki).
