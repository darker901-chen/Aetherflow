# AetherFlow Copilot Instructions

Use the repository protocol as the source of truth:

- `AGENTS.md`
- `protocol/AGENT_OPERABLE_ARCHITECTURE.md`
- `protocol/COMPONENT_INDEX.md`
- `protocol/AGENT_PROTOCOL.md`
- `protocol/AGENT_COMMANDS.md`

## Architecture Planning Gate

The Architecture Planning Gate controls patching, not ordinary discussion. General conversation, direction review, brainstorming, report-only work, and read-only planning can proceed without maintainer approval.

For implementation requests, runtime behavior changes, architecture/development governance changes, cross-component changes, or `agent:chain` requests:

1. Start with the Architecture Agent role.
2. Post an implementation plan before code changes.
3. List the agents/subagents, skills, and tools that will be used.
4. Identify likely files or components, approval boundary, success gates, and expected artifacts.
5. Wait for maintainer approval before patching source files or governance documents, unless the issue explicitly states that implementation is pre-approved or the latest maintainer instruction already contains clear execution approval for the scoped plan.

Valid approval wording includes `approve`, `go`, `implement`, `start`, `fix it`, or any equally unambiguous execution instruction.

## Runtime Rule

`AetherFlow.exe` must stay deterministic, local, reproducible, and testable. Do not add chat-agent or remote LLM dependencies to runtime code.

Scene detection is the primary runtime decision. ROI, QP, and privacy masks are downstream actions selected after a scene decision.

## Role Routing

If the issue does not specify `agent:<role>`, infer the route:

- scene/ROI/QP/privacy-mask behavior: Architecture -> scene-runtime -> trace-verifier -> benchmark-reporter -> code-reviewer -> docs sync
- encoder/backend/input texture behavior: Architecture -> encoder -> trace-verifier -> benchmark-reporter -> code-reviewer -> docs sync
- trace/verifier/tooling behavior: Architecture when behavior changes -> trace-verifier -> code-reviewer -> docs sync
- failed run/build/smoke/trace artifact: trace-verifier -> debug-verifier -> trace-verifier -> code-reviewer when the repair changed code
- benchmark-only: benchmark-reporter only
- docs/protocol/component index: architecture

The user should only need to state the requirement. Do not require repeated instructions for verification, benchmark, repair, docs sync, or reporting.

## Verification

Use the Python agent tools as canonical entrypoints:

- `python tools/agent_run.py --run-id <run_id>`
- `python tools/agent_summarize.py --run-dir .aetherflow/runs/<run_id>`
- `python tools/agent_verify.py --run-dir .aetherflow/runs/<run_id>`
- `python tools/agent_verify.py --run-dir .aetherflow/runs/<run_id> --run-benchmark`

For implementation tasks, automatically run `agent_run`, `agent_verify`, and `--run-benchmark` when runtime/encoder/ROI/QP/mask behavior changed. If build, smoke, or trace fails, use debug-verifier from artifacts for one scoped repair attempt, then rerun verification. After passing verification, run an independent code-reviewer for source changes, then sync `docs/3-product/ARCHITECTURE.md`, `protocol/COMPONENT_INDEX.md`, and `README.md` when user-facing commands or validation steps changed.

Report changed files, commands run, artifact directory, `verify_report.json`, trace summary when present, verification result, and remaining risk.
