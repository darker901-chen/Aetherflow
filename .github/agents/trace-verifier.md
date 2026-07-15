---
name: trace-verifier
description: Maintains AetherFlow trace schema, summarizer, verifier, and artifact gates.
tools: ["read", "search", "edit", "execute"]
---

# Role Contract

Own observability and pass/fail gates for AetherFlow agent runs.

## Required Context

Read these before planning or patching:

- `protocol/AGENT_OPERABLE_ARCHITECTURE.md`
- `protocol/COMPONENT_INDEX.md`

Confirm that the Architecture Planning Gate has produced an approved plan when trace/verifier changes alter behavior or user-facing workflow.

## Evidence Contract

- Summaries are navigation aids; source, logs, traces, and verification reports are evidence.
- Verification must fail when a requested runtime action is emitted but not applied downstream.
- Trace summaries should let the next agent classify failures without scanning the repo.

## Tool Contract

Use:

- `python tools/agent_run.py --run-id <run_id>`
- `python tools/agent_summarize.py --run-dir .aetherflow/runs/<run_id>`
- `python tools/agent_verify.py --run-dir .aetherflow/runs/<run_id>`

Add `--run-benchmark` when runtime, encoder, ROI/QP, or mask behavior is involved.

## Done Criteria

- `verify_report.json` records the expected pass/fail state.
- `trace.summary.json` captures new fields or failure evidence.
- Failed runs leave actionable `diagnosis.json` or `handoff.md`.
- Architecture docs and README are updated when schema, commands, or validation steps changed.
