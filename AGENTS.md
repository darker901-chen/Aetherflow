# AetherFlow Agent Contract

This repository is optimized for file-based development agents. Codex, Claude
Code, GitHub Copilot, and future file-based agents should use the same mental
model and the same source-of-truth documents before planning or patching:

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

Codex role adapters live in `.codex/agents/*.toml`; repository-local Codex
skills live in `.agents/skills/*/SKILL.md`. They mirror the shared protocol and
must not become a separate source of truth.

## Operating Rules

- Follow the implementation cycle: Observe -> Architecture Planning Gate -> Reproduce -> Classify -> Patch -> Build -> Smoke Test -> Benchmark -> Judge -> Report.
- A single clear implementation sentence from the user is enough to start the
  full workflow. Infer the role route, run the Architecture Planning Gate, patch
  the scoped files, build, smoke, verify, benchmark when required, perform one
  scoped repair attempt if a gate fails, sync docs, and report artifacts. Do not
  ask the user to repeat standard verification or closeout steps. If the
  sentence is exploratory or does not clearly approve implementation, stop after
  the Architecture Planning Gate and ask for approval.
- General conversation, direction review, brainstorming, report-only work, and read-only planning do not require explicit approval.
- Before patching implementation, runtime behavior, architecture/development governance documents, cross-component changes, or `agent:chain` work, run the Architecture Planning Gate: produce a short Architecture Agent plan, list the agents/skills/tools to use, define likely files and success gates, then ask for explicit approval. Valid approval wording includes `approve`, `go`, `implement`, `start`, `fix it`, or any equally unambiguous execution instruction.
- After approval and **before** any patching, materialize the plan to `.aetherflow/runs/<task_id>/plan.md` (copy `.aetherflow/templates/plan.template.md` and fill every section, including the quoted approval wording). Gives the Code Review Agent a real intent artifact; missing `plan.md` is a risk finding (not a blocker) on a triggered change.
- If the latest user message already contains clear execution approval for the scoped plan under discussion, treat it as approval and proceed without asking for a second confirmation. If scope is unclear or high-risk, narrow the plan first.
- Do not scan the whole repository by default.
- Do not read `third_party/`, `build*/`, `output/`, `.claude/worktrees/`, or `.aetherflow/runs/` unless the task card or current run artifact requires it.
- Do not patch from summaries alone. Summaries are navigation aids; source files, logs, traces, and verification reports are the evidence.
- Keep patch ownership explicit and scoped to the files needed for the task.
- Preserve user work already present in the tree.

## Role Shortcut Protocol

When the user starts a request with `agent:<role>` or `agent:chain`, use `protocol/AGENT_COMMANDS.md` to resolve the role, default autonomy, required context, and chain sequence. The user should not need to repeat the same required-read list in every request. The full multi-agent framework (seven roles, autonomy levels A0-A5, scheduled loops, artifact schemas) is in `protocol/AGENT_PROTOCOL.md`.

When the user does not specify a role, infer the role from the request and use the same protocol. The user should only need to state the product or engineering requirement; do not require them to repeat standard verification, repair, documentation sync, or reporting steps.

Example one-sentence implementation request:

```text
Implement a QR-code privacy-mask producer; do not add a remote AI detector. I approve starting under the repository agent workflow.
```

Expected behavior: Architecture Agent plan -> owning implementation role ->
agent_run -> agent_verify -> benchmark when required -> one repair attempt if
needed -> docs/status sync -> final artifact report.

Examples:

- `agent:architecture design scene inference MVP`
- `agent:runtime implement scene inference MVP`
- `agent:chain scene-inference design and implement scene inference MVP`
- `agent:chain failed-run latest`

## Role Router

Use the seven protocol roles as execution responsibilities, even when a single model is doing the work:

- Architecture Agent: planning gate, architecture/governance docs, component index, drift review.
- Runtime Scene Agent: scene-first frame decisions, ROI/mask producers, policy engine, runtime trace fields.
- Encoder Agent: encoder interface, NVENC, oneVPL, input texture ownership, encode latency.
- Trace and Verification Agent: trace schema, summarizer, verifier, evidence quality.
- Benchmark Agent: benchmark execution and interpretation from artifacts.
- Code Review Agent: independent post-implementation audit of the diff itself (intent vs change, correctness, race/edge, cross-platform consistency, scope drift); reports findings by severity, never patches.
- Repair Agent: one scoped fix from a classified failed run or a code-review blocker.

Route plain-language requests automatically:

- scene/ROI/QP/privacy-mask behavior -> Architecture Gate -> Runtime Scene Agent -> Trace/Verification -> Benchmark -> Code Review -> Architecture docs sync.
- encoder/backend/input texture behavior -> Architecture Gate -> Encoder Agent -> Trace/Verification -> Benchmark -> Code Review -> Architecture docs sync.
- trace/verifier/tooling behavior -> Architecture Gate when behavior changes -> Trace and Verification Agent -> Code Review -> docs sync if user-facing.
- failed run/artifact repair -> Trace and Verification Agent -> Repair Agent -> Trace and Verification Agent -> Code Review (when repair changed code).
- benchmark-only/report-only -> Benchmark Agent, no source patch unless explicitly requested.
- docs/protocol/component-index updates -> Architecture Agent.

## Default Completion Contract

For implementation tasks, especially runtime, encoder, scene, ROI, QP, or privacy-mask changes, the agent must complete the standard loop automatically after patching. The user should not have to ask for these steps:

- Run `python tools/agent_run.py --run-id <task_id>`.
- Run `python tools/agent_verify.py --run-dir .aetherflow/runs/<task_id> --run-benchmark` when runtime, encoder, ROI, QP, or mask behavior changed.
- If build, smoke, or trace fails, use the debug-verifier/repair workflow from the run artifacts and perform one scoped repair attempt before rerunning verification.
- After verification passes, sync `docs/3-product/ARCHITECTURE.md` and `protocol/COMPONENT_INDEX.md` for architecture/component changes.
- Also update `README.md` when user-facing commands, flags, environment variables, or manual validation steps changed.
- Update `docs/1-status/PROJECT_STATUS.md` only when a current capability,
  verification boundary, release gate, or next action changed. Append dated
  run/review evidence to `docs/4-qa-debugging/VERIFICATION_HISTORY.md`; do not
  turn current status into a chronological journal.
- Documentation sync must be evidence-first: verify user-facing commands,
  shell syntax, output paths, backend-specific behavior, and env defaults from
  source files or run artifacts before writing. Preserve the user's requested
  shell in the primary SOP, avoid vague words like "usually" for deterministic
  paths, and search touched docs for contradictions before closeout.
- Final report must include run directory, `verify_report.json`, trace summary when present, files changed, and residual risks.

## Runtime Direction

Scene detection is the primary runtime decision path. ROI, QP, and privacy mask behavior are actions chosen after a scene decision.

The deterministic runtime layer lives in product code and must stay local, reproducible, and testable. Do not introduce LLM calls into `AetherFlow.exe`.

## Agent Tools

Use the Python tools as canonical entrypoints:

- Run build/runtime smoke: `python tools/agent_run.py --run-id scene_smoke`
- Summarize an existing run: `python tools/agent_summarize.py --run-dir .aetherflow/runs/<run_id>`
- Verify build and run artifacts: `python tools/agent_verify.py --run-dir .aetherflow/runs/<run_id>`
- Run the benchmark gate through the verifier: `python tools/agent_verify.py --run-dir .aetherflow/runs/<run_id> --run-benchmark`

The PowerShell scripts in `tools/agent_*.ps1` are compatibility wrappers only.

## Reporting

Report the command run, the artifact directory, the verification result, and the specific files changed. If verification fails, leave a compact handoff with the first relevant evidence and the next suspected component from `protocol/COMPONENT_INDEX.md`.
