# AetherFlow Agent-Operable Architecture

This project separates runtime intelligence from development intelligence.

## Runtime Deterministic Layer

The runtime layer runs inside `AetherFlow.exe`. It does not call an LLM and does not depend on chat context. It consumes frame context and emits deterministic frame decisions:

- scene classification for the current screen-share workload
- quality ROI regions for encoder QP/ROI control
- privacy mask regions for future blur/black-box processing
- source labels and confidence for traceability

Scene detection is the primary decision. ROI, QP, and privacy masks are possible actions selected after the scene decision. The runtime path must stay fast, reproducible, and testable. Any future ONNX, OCR, or NER component is still a local module in this layer, not a chat agent.

## Development Agent Layer

The development layer runs outside the product. Coding agents debug and patch through structured artifacts instead of long conversation history.

Required implementation cycle:

```text
Observe -> Architecture Planning Gate -> Reproduce -> Classify -> Patch -> Build -> Smoke Test -> Benchmark -> Judge -> Report
```

The user should only need to state the requirement. The main agent infers the role route from the request and automatically performs the fixed completion work: `agent_run`, `agent_verify`, benchmark when runtime/encoder/ROI/QP/mask behavior changed, one scoped debug-verifier repair attempt on build/smoke/trace failure, docs sync after passing verification, and artifact-based reporting.

General conversation, direction review, brainstorming, report-only work, and read-only planning do not require explicit approval.

Before patching implementation, runtime behavior, architecture/development governance documents, cross-component changes, or chained development work, the Architecture Planning Gate comes before implementation. The agent must state the plan, intended agents/skills/tools, likely files, success gates, and approval boundary, then wait for explicit human approval before patching. Valid approval wording includes `approve`, `go`, `implement`, `start`, `fix it`, or any equally unambiguous execution instruction.

If the latest user message already contains clear execution approval for the scoped plan under discussion, treat it as approval and proceed without asking for a second confirmation. If scope is unclear or high-risk, narrow the plan first.

Agents communicate through files:

- `task_card.json`
- `diagnosis.json`
- `patch_report.md`
- `verify_report.json`
- `handoff.md`

Summaries are navigation aids only. Source files, logs, traces, and verification results are the truth source.

Canonical tool entrypoints are Python:

- `tools/agent_run.py`
- `tools/agent_summarize.py`
- `tools/agent_verify.py`

The matching PowerShell scripts are compatibility wrappers. They should not contain the long-term workflow logic.

## Token Control Rules

- Do not scan the whole repository by default.
- Do not read `third_party/`, `build/`, or old worktrees unless a task card explicitly allows it.
- Do not patch code from a summary alone.
- Every diagnosis must cite source/log/trace evidence.
- Patch ownership must be explicit.
- A compact `handoff.md` is produced after every failed iteration.
- Success is decided by verification scripts, not by an LLM claim.

## Run Artifact Layout

```text
.aetherflow/runs/<timestamp>/
  task_card.json
  run_manifest.json
  console.log
  console.summary.json
  frame_trace.jsonl
  trace.summary.json
  diagnosis.json
  patch_report.md
  verify_report.json
  handoff.md
```
