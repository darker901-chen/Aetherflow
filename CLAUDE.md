# AetherFlow Claude Contract

Claude Code should use the same mental model as Codex and other file-based
agents in this repository. Read these first when working here:

- `protocol/AGENT_OPERABLE_ARCHITECTURE.md`
- `protocol/AGENT_COMMANDS.md`
- `protocol/AGENT_PROTOCOL.md`
- `protocol/COMPONENT_INDEX.md`
- `docs/DOCUMENTATION_INDEX.md`
- `docs/2-agent-system/AGENT_ARCHITECTURE.md`
- `docs/1-status/PROJECT_STATUS.md`
- `docs/4-qa-debugging/VERIFICATION_HISTORY.md`

For runtime, encoder, scene, ROI, QP, privacy-mask, or architecture design
work, also use `docs/3-product/ARCHITECTURE.md`.

The full multi-agent framework (seven roles, autonomy levels A0-A5, scheduled
loops, artifact schemas) is in `protocol/AGENT_PROTOCOL.md`. Role shortcut
syntax (`agent:<role> <goal>` and `agent:chain <workflow>`) is defined in
`protocol/AGENT_COMMANDS.md`.

When invoked from Claude Code, you also have native subagent definitions in `.claude/agents/` for the protocol roles. Use them via the Task tool when delegation makes sense.

## System Agent Architecture

Do not duplicate the agent handbook here. The shared explanation of the seven
development-agent responsibilities, Claude/Codex parity, artifact handoff, role
router, and runtime boundary lives in
`docs/2-agent-system/AGENT_ARCHITECTURE.md`.

Claude subagents are adapter implementations of the same protocol roles:
`architecture`, `scene-runtime`, `encoder`, `trace-verifier`,
`debug-verifier`, `benchmark-reporter`, and `code-reviewer`.

## Minimal Prompt Contract

The user should only need to state the engineering requirement. If a request does not specify `agent:<role>` or `agent:chain`, infer the role and workflow from the task:

- scene/ROI/QP/privacy-mask behavior -> Architecture Gate -> scene-runtime -> trace-verifier -> benchmark-reporter -> code-reviewer -> architecture docs sync.
- encoder/backend/input texture behavior -> Architecture Gate -> encoder -> trace-verifier -> benchmark-reporter -> code-reviewer -> architecture docs sync.
- trace/verifier/tooling behavior -> Architecture Gate when behavior changes -> trace-verifier -> code-reviewer -> docs sync if user-facing.
- failed run/artifact repair -> debug-verifier -> repair-scoped patch -> trace-verifier -> code-reviewer (when repair changed code).
- benchmark-only/report-only -> benchmark-reporter without source edits.
- docs/protocol/component-index/project-status updates -> architecture.

A single clear implementation sentence is enough to start the full workflow.
Infer the role route, run the Architecture Planning Gate, patch the scoped
files, build, smoke, verify, benchmark when required, perform one scoped repair
attempt if a gate fails, sync docs, and report artifacts. Do not ask the user to
repeat standard verification, benchmark, repair, documentation sync, or
reporting instructions after they state the requirement. If the sentence is
exploratory or does not clearly approve implementation, stop after the
Architecture Planning Gate and ask for approval.

Example:

```text
Implement a QR-code privacy-mask producer; do not add a remote AI detector. I approve starting under the repository agent workflow.
```

Expected behavior: architecture plan -> owning implementation role ->
agent_run -> agent_verify -> benchmark when required -> one repair attempt if
needed -> docs/status sync -> final artifact report.

## Contract

- Work from source/log/trace evidence, not from summaries alone.
- General conversation, direction review, brainstorming, report-only work, and read-only planning do not require explicit approval.
- Before patching implementation, runtime behavior, architecture/development governance documents, cross-component changes, or `agent:chain` work, run the Architecture Planning Gate: use the Architecture Agent role, state the agents/skills/tools you intend to use, define likely files and success gates, then ask for explicit approval. Valid approval wording includes `approve`, `go`, `implement`, `start`, `fix it`, or any equally unambiguous execution instruction.
- After approval and **before** any patching, materialize the plan to `.aetherflow/runs/<task_id>/plan.md` (copy from `.aetherflow/templates/plan.template.md` and fill every section, including the quoted approval wording). This gives the Code Review Agent a real intent artifact and lets work survive chat context loss. Skip only for docs-only, verification-only, benchmark-only, and governance-only changes.
- If the latest user message already contains clear execution approval for the scoped plan under discussion, treat it as approval and proceed without asking for a second confirmation. If scope is unclear or high-risk, narrow the plan first.
- Keep context small. Use `protocol/COMPONENT_INDEX.md` to choose likely files before searching more broadly.
- Avoid `third_party/`, `build*/`, `output/`, `.claude/worktrees/`, and `.aetherflow/runs/` unless a task or run artifact explicitly requires them.
- Preserve unrelated user changes.
- Keep changes scoped and verification-driven.

## Runtime Rule

Scene detection is the main deterministic decision. ROI, QP, and privacy masks are downstream actions selected from the scene decision. Do not invert that model.

The product runtime must not depend on chat context or remote LLM calls.

## Canonical Tools

Use Python for the agent workflow:

- `python tools/agent_run.py --run-id scene_smoke`
- `python tools/agent_summarize.py --run-dir .aetherflow/runs/<run_id>`
- `python tools/agent_verify.py --run-dir .aetherflow/runs/<run_id>`
- `python tools/agent_verify.py --run-dir .aetherflow/runs/<run_id> --run-benchmark`

The PowerShell scripts with the same names are thin wrappers for compatibility.

## Expected Closeout

For implementation tasks, especially runtime, encoder, scene, ROI, QP, or privacy-mask changes, close out automatically:

- Run `python tools/agent_run.py --run-id <task_id>`.
- Run `python tools/agent_verify.py --run-dir .aetherflow/runs/<task_id> --run-benchmark` when runtime, encoder, ROI, QP, or mask behavior changed.
- If build, smoke, or trace fails, use the debug-verifier workflow from artifacts and perform one scoped repair attempt before rerunning verification.
- If verification passes and behavior changed, sync `docs/3-product/ARCHITECTURE.md` and `protocol/COMPONENT_INDEX.md`; also update `README.md` when user-facing commands, flags, environment variables, or validation steps changed.
- If verification or benchmark evidence changes current state, update
  `docs/1-status/PROJECT_STATUS.md` or hand the evidence to the Architecture
  Agent. Append dated run/review evidence to
  `docs/4-qa-debugging/VERIFICATION_HISTORY.md`; do not turn current status into
  a chronological journal.
- Claude code-review hard gate: when any runtime, encoder, scene, ROI, QP,
  privacy-mask, trace-schema, CLI, env, fixture, canonical-tooling, or
  cross-platform (`src/platform/*`) behavior changed, after verification and
  benchmark pass and **before** docs sync, invoke the `code-reviewer` role as a
  fresh subagent (`Agent(subagent_type="code-reviewer", ...)` — the
  implementing role must not review its own diff). Attach the resulting
  `.aetherflow/runs/<task_id>/code_review.md` to the final response. Blocker
  findings stop closeout: perform one scoped repair attempt and re-invoke
  `code-reviewer`. Risk findings are surfaced in the final response. Skip the
  gate for docs-only, verification-only, benchmark-only, and governance-only
  changes — those are covered by the other gates.
- Claude closeout hard gate: when any runtime, encoder, scene, ROI, QP,
  privacy-mask, trace schema, verifier, build, fixture, CLI, or env behavior
  changed, invoke the `aetherflow-arch-sync` skill with `--apply-mechanical`
  after verification passes. Treat the skill report as a required artifact for
  the final response.
- The docs sync gate must explicitly inspect and report on these mutable truth
  surfaces: `docs/1-status/PROJECT_STATUS.md`,
  `docs/4-qa-debugging/VERIFICATION_HISTORY.md`, `docs/3-product/ARCHITECTURE.md`,
  `protocol/COMPONENT_INDEX.md`, `README.md`, `docs/3-product/PRODUCT_ROADMAP.md`,
  `docs/3-product/CODE_COMPONENT_SUMMARY.md`, `docs/DOCUMENTATION_INDEX.md`, and
  `docs/HIGHLIGHTS.md`. (`docs/3-product/CODE_COMPONENT_SUMMARY.md`
  is the maintained full per-file code map; `docs/3-product/ARCHITECTURE.md` §14.1
  keeps a condensed quick-map that points to it. The README keeps a short
  "Features at a Glance"; the detailed operation manual (flags/env, feature
  depth, human-verification SOPs, macOS, ONNX setup) now lives in
  `docs/OPERATION_GUIDE.md`.)
- Do not finish a runtime/encoder/trace task with only source changes when the
  user-facing behavior, module inventory, flags/env vars, fixtures, trace
  fields, or verified project state changed. Either update the docs in the same
  patch or leave an explicit docs handoff naming the exact stale files and why
  they were not changed.
- Documentation sync must be evidence-first: verify user-facing commands,
  shell syntax, output paths, backend-specific behavior, and env defaults from
  source files or run artifacts before writing. Preserve the user's requested
  shell in the primary SOP, avoid vague words like "usually" for deterministic
  paths, and search touched docs for contradictions before closeout.
- Final response must state changed files, commands run, run directory,
  `verify_report.json`, trace summary when present, verification status,
  `code_review.md` summary (severity counts + recommendation) when the
  code-review gate ran, docs-sync result, complex drifts or skipped docs, and
  residual risks.

If a failure remains, update the run handoff and cite the evidence file and line or trace summary field that supports the next step.
