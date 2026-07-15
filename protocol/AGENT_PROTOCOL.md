# AetherFlow Universal Agent Design

Date: 2026-05-02

This document defines the project-level agent design for AetherFlow. It is not a design for putting an LLM agent inside `AetherFlow.exe`. It is a design for how external development agents should work on this repository in a consistent way.

Target agent executors:

- Codex
- Claude Code
- GitHub Copilot coding agent
- Future file-based coding agents

Core idea:

```text
The agent executor can change.
The AetherFlow Agent Protocol should stay the same.
```

---

## 1. Purpose

AetherFlow should be developed through a shared agent protocol rather than through tool-specific habits.

The goal is to let different agents use the same development logic:

```text
Codex local session
Claude Code local session
GitHub Copilot issue/PR agent
Future hosted coding agent
```

All of them should understand:

- what to read first
- how to reproduce a problem
- how to classify a failure
- how to decide what files they own
- how to patch safely
- how to verify
- how to report and hand off work

This makes the repository agent-operable instead of agent-dependent.

---

## 2. Non-Goals

This design does not mean:

- adding remote LLM calls to `AetherFlow.exe`
- letting runtime behavior depend on chat history
- letting agents patch from summaries alone
- letting agents scan the entire repository by default
- letting agents freely rewrite architecture without approval
- making GitHub Copilot, Codex, or Claude Code the source of truth

The source of truth remains:

```text
source code
logs
frame traces
verification reports
benchmark reports
human-approved architecture documents
```

---

## 3. The Three-Layer Agent Model

The agent design has three layers.

```text
Layer 1: Project Agent Contract
Layer 2: Universal Agent Workflow
Layer 3: Platform Adapter
```

### 3.1 Layer 1: Project Agent Contract

This is the AetherFlow-specific rule set.

It defines:

- runtime boundaries
- development boundaries
- repository navigation rules
- verification expectations
- artifact expectations

Current files:

- `AGENTS.md`
- `CLAUDE.md`
- `protocol/AGENT_OPERABLE_ARCHITECTURE.md`
- `protocol/COMPONENT_INDEX.md`
- `protocol/AGENT_COMMANDS.md`
- this document

Contract summary:

```text
AetherFlow.exe must stay deterministic.
Development agents operate outside the product.
Scene is the primary runtime decision.
ROI, QP, and privacy masks are downstream actions.
Agents work from source/log/trace evidence.
Verification artifacts decide pass/fail.
```

### 3.2 Layer 2: Universal Agent Workflow

This is the workflow every agent should follow, regardless of platform.

```text
Intake
  -> Architecture Planning Gate
  -> Context Load
  -> Reproduce or Inspect
  -> Classify
  -> Patch Plan
  -> Patch
  -> Verify
  -> Report and Handoff
```

This workflow is the core of the system.

### 3.3 Layer 3: Platform Adapter

This is where each agent tool differs.

Examples:

```text
Codex:
  local workspace, shell access, chat report

Claude Code:
  local workspace, file edits, chat report

GitHub Copilot coding agent:
  GitHub issue intake, branch, PR, GitHub Actions verification
```

The adapter changes how the agent is invoked. It should not change the project protocol.

---

## 4. Runtime Boundary

The runtime boundary is strict.

`AetherFlow.exe` may include:

- deterministic scene policy
- local model inference in the future
- local OCR in the future
- local sensitive-surface detection in the future
- GPU processing
- trace output

`AetherFlow.exe` must not include:

- remote LLM calls
- chat-agent dependencies
- agent planning loops
- behavior that requires conversation memory

Runtime intelligence must behave like normal product code:

- local
- reproducible
- testable
- traceable
- benchmarkable

Development intelligence belongs outside the executable.

---

## 5. Universal Workflow Stages

### 5.1 Intake

Input can come from:

- user request
- GitHub issue
- task card
- failed run artifact
- benchmark regression
- architecture drift report

Output should be:

- clear goal
- allowed files or components
- blocked areas
- success gates
- required evidence

Preferred artifact:

```text
task_card.json
```

The intake stage should answer:

```text
What is the agent allowed to change?
How will success be judged?
What evidence already exists?
```

### 5.2 Architecture Planning Gate

The Architecture Planning Gate controls patching, not ordinary discussion. General conversation, direction review, brainstorming, report-only work, and read-only planning can proceed without explicit approval.

Implementation requests, runtime behavior changes, architecture/development governance changes, cross-component changes, and chained development requests must start with the Architecture Agent before implementation.

The gate output is a short plan, not a patch. It must include:

- interpreted goal
- agents, subagents, skills, and tools that will be used
- likely files or components to inspect or edit
- autonomy level and approval boundary
- success gates and expected artifacts
- direct approval question

No source patch or governance-document patch should happen until the user explicitly approves the plan with wording such as:

```text
approve
go
implement
start
fix it
```

If the latest user message already contains clear execution approval for the scoped plan under discussion, treat it as approval and proceed without asking for a second confirmation. If scope is unclear or high-risk, narrow the plan first.

This gate can be skipped for conversation-only work, report-only work, docs-only cleanup that does not change architecture/development governance rules, direct verification/summarization commands, or repair tasks whose task card already contains approved paths and autonomy.

For GitHub Copilot coding agent, where interactive chat approval may not be available, the equivalent gate is an issue or PR plan comment. The agent should wait for maintainer approval, or for the issue text to explicitly state that the plan is pre-approved, before implementation commits.

### 5.2.1 Main Agent Role Router

The user should be able to state only the requirement. If the request does not explicitly name `agent:<role>` or `agent:chain`, the main agent must infer the route and execute the standard workflow.

Default routing:

| Request signal | Route |
|---|---|
| scene labels, frame policy, ROI producer, privacy mask producer, decision trace fields | Architecture Agent -> Runtime Scene Agent -> Trace and Verification Agent -> Benchmark Agent -> Code Review Agent -> Architecture Agent docs sync |
| NVENC, oneVPL, encoder interface, input texture pool, encode latency/drop behavior | Architecture Agent -> Encoder Agent -> Trace and Verification Agent -> Benchmark Agent -> Code Review Agent -> Architecture Agent docs sync |
| trace schema, summarizer, verifier, artifact quality, agent tooling | Architecture Agent if behavior changes -> Trace and Verification Agent -> Code Review Agent -> docs sync when user-facing |
| failed run, build failure, smoke failure, trace failure, existing `.aetherflow/runs/<id>` issue | Trace and Verification Agent -> Repair Agent -> Trace and Verification Agent -> Code Review Agent (when repair changed code) |
| benchmark-only, latency/quality report-only | Benchmark Agent only |
| architecture docs, protocol docs, component index | Architecture Agent |

Roles are responsibilities, not necessarily separate models. One executor may act as multiple roles, but must keep boundaries, artifacts, and verification duties explicit.

The user should not have to restate standard closeout steps such as build, smoke, verify, benchmark, debug-verifier repair, docs sync, or final artifact reporting.

### 5.3 Context Load

Every agent should first read:

- `AGENTS.md`
- `protocol/AGENT_OPERABLE_ARCHITECTURE.md`
- `protocol/COMPONENT_INDEX.md`

Then read only the source files relevant to the component.

Default excluded areas:

- `third_party/`
- `build*/`
- `output/`
- `.claude/worktrees/`
- `.aetherflow/runs/`

Exception:

- read `.aetherflow/runs/<run_id>/` when the task is about that run artifact.

The purpose is context control. Agents should not spend tokens scanning unrelated code.

### 5.4 Reproduce or Inspect

If the problem is runtime behavior, the agent should reproduce it or inspect an existing artifact.

Canonical command:

```text
python tools/agent_run.py --run-id <run_id>
```

If a run already exists:

```text
python tools/agent_summarize.py --run-dir .aetherflow/runs/<run_id>
```

The goal is to create evidence before patching.

### 5.5 Classify

The agent should classify the issue by stage.

Common stages:

- capture
- color conversion
- runtime decision layer
- encoder interface
- NVENC backend
- oneVPL backend
- trace output
- agent tooling
- benchmark tooling
- documentation drift

Preferred artifact:

```text
diagnosis.json
```

A good diagnosis includes:

- failure stage
- suspected component
- evidence references
- confidence
- files that must be read before patching

Summaries can guide this stage, but they are not enough to patch.

### 5.6 Patch Plan

Before editing, the agent should define patch ownership.

Examples:

```text
Runtime scene policy:
  include/AetherFlow/IAIFrameAnalyzer.h
  src/main.cpp

NVENC input pool:
  include/AetherFlow/NvencH264Wrapper.h
  src/NvencH264Wrapper.cpp

Agent summarizer:
  tools/agent_summarize.py
```

The patch plan should avoid unrelated cleanup.

### 5.7 Patch

The patch should be:

- scoped
- evidence-driven
- compatible with existing architecture
- respectful of existing user changes
- traceable in the final report

Agents should not rewrite architecture while fixing a narrow bug.

### 5.8 Verify

Verification depends on the task.

Docs-only:

```text
git diff --check -- <changed-docs>
```

Build-affecting:

```text
cmake --build build --config Release --target AetherFlow
```

Runtime-affecting:

```text
python tools/agent_run.py --run-id <run_id>
python tools/agent_verify.py --run-dir .aetherflow/runs/<run_id>
```

ROI/QP/privacy-mask affecting:

```text
python tools/agent_verify.py --run-dir .aetherflow/runs/<run_id> --run-benchmark
```

Preferred artifact:

```text
verify_report.json
```

### 5.8.1 Default Implementation Completion

For implementation tasks, especially runtime, encoder, scene, ROI, QP, privacy-mask, trace, or agent-tooling changes, completion is automatic after patching:

1. Run:
   ```text
   python tools/agent_run.py --run-id <run_id>
   ```
2. Run:
   ```text
   python tools/agent_verify.py --run-dir .aetherflow/runs/<run_id>
   ```
3. If runtime, encoder, ROI, QP, or privacy-mask behavior changed, run:
   ```text
   python tools/agent_verify.py --run-dir .aetherflow/runs/<run_id> --run-benchmark
   ```
4. If build, smoke, or trace fails, invoke the debug-verifier/repair workflow from the run artifacts and perform one scoped repair attempt before rerunning verification.
5. If verification passes and runtime/architecture behavior changed, sync:
   - `docs/3-product/ARCHITECTURE.md`
   - `protocol/COMPONENT_INDEX.md`
   - `README.md` when user-facing commands, flags, environment variables, or validation steps changed
   - `docs/1-status/PROJECT_STATUS.md` when the observed state or verification evidence changed
6. Documentation sync must be evidence-first:
   - verify user-facing commands, shell syntax, output paths, backend-specific behavior, and environment defaults from source files, run manifests, console logs, or actual artifacts
   - preserve the user's requested shell/platform in the primary SOP
   - avoid vague wording such as "usually" for deterministic paths; state exact conditions when behavior is backend-specific or env-dependent
   - include what the user should open, what correct behavior looks like, and common wrong outcomes for human validation SOPs
   - search touched docs for stale contradictions before closeout, then run `git diff --check`

The completion contract is part of the agent system. The user should only need to describe the desired change.

### 5.9 Report and Handoff

Every completed agent task should report:

- what changed
- commands run
- artifact directory
- verification result
- `verify_report.json`
- trace summary when present
- remaining risks

If the task fails, the agent should update:

```text
handoff.md
```

The handoff should include:

- first relevant evidence
- next suspected component
- files the next agent must read

---

## 6. Agent Roles

Roles are operating modes. They do not require different models. The same executor can act as different roles depending on the task.

Each role has a practical job in the development loop:

- Architecture Agent turns an ambiguous requirement into approved scope and keeps docs truthful.
- Runtime Scene Agent changes deterministic scene/ROI/mask behavior.
- Encoder Agent changes backend encode behavior and latency-sensitive texture ownership.
- Trace and Verification Agent makes the behavior observable and gates pass/fail from artifacts.
- Benchmark Agent measures latency/quality impact without inventing results.
- Code Review Agent provides independent post-implementation audit of the diff itself (intent vs change, correctness, race/edge, cross-platform consistency, scope drift); reports findings by severity, never patches.
- Repair Agent performs one narrow fix from classified evidence when a gate fails.

The main agent is responsible for routing between these roles automatically from the user's requirement.

### 6.1 Architecture Agent

Owns:

- Architecture Planning Gate for implementation/runtime/governance/cross-component work
- architecture documentation
- agent protocol documentation
- component index accuracy
- current project status and verification history
- architecture drift reports

Should run:

- docs checks
- source-vs-doc consistency review
- progress snapshot updates from verification artifacts

Should not:

- change runtime behavior unless explicitly assigned

Typical output:

- architecture plan with intended agents/skills/tools and approval question
- materialized plan artifact at `.aetherflow/runs/<task_id>/plan.md` (copied from `.aetherflow/templates/plan.template.md` and filled in) — written after approval and before any implementing role patches; gives the Code Review Agent a real intent doc to audit against and survives chat context loss
- updated architecture docs
- updated `docs/1-status/PROJECT_STATUS.md` when project state changed
- `ARCH_DRIFT_REPORT.md`
- proposed follow-up tasks

### 6.2 Runtime Scene Agent

Owns:

- scene-first decision layer
- deterministic frame policy
- scene labels
- decision trace fields
- future analyzer bridge behavior

Must preserve:

```text
Scene first.
ROI/QP/masks second.
No remote LLM in runtime.
```

Typical files:

- `include/AetherFlow/IAIFrameAnalyzer.h`
- `src/main.cpp`

### 6.3 Encoder Agent

Owns:

- encoder interface behavior
- NVENC backend
- oneVPL backend
- ROI translation into backend controls
- input texture ownership
- encoder latency and drop behavior

Typical files:

- `include/AetherFlow/IH264Encoder.h`
- `src/NvencH264Wrapper.cpp`
- `src/VplH264Wrapper.cpp`

Should run benchmark when:

- ROI/QP behavior changes
- latency behavior changes
- input texture pipeline changes

### 6.4 Trace and Verification Agent

Owns:

- frame trace schema
- console summaries
- trace summaries
- verification reports
- failure evidence quality

Typical files:

- `tools/agent_summarize.py`
- `tools/agent_verify.py`
- trace-writing parts of `src/main.cpp`

Goal:

```text
Make failures easy for the next agent to understand.
```

### 6.5 Benchmark Agent

Owns:

- ROI benchmark workflow
- latency benchmark interpretation
- quality comparison reports
- benchmark artifacts

Typical files:

- `tools/roi_benchmark.ps1`
- future benchmark tools
- benchmark reports under run artifacts

Should usually report only, not patch runtime.

### 6.6 Repair Agent

Owns:

- narrow fixes from a classified failure

Input:

- failed run artifact
- diagnosis
- allowed paths
- success gates

The repair agent should not broaden the task. If the failure is architectural, it should stop and request an architecture task.

### 6.7 Code Review Agent

Owns:

- independent post-implementation audit of the diff itself
- intent-vs-diff check against the Architecture Planning Gate scope and the user's requirement
- cross-platform consistency check when a change mirrors another platform
- scope-drift detection (diff touching files outside the declared scope)
- test-coverage gap notes (what smoke + trace did not exercise)

Input:

- `git diff <base>..HEAD` (base = commit before the implementing role started)
- user's original requirement sentence
- Architecture Planning Gate plan at `<run_dir>/plan.md` (required when the trigger list applies; missing plan.md is a risk finding, not a blocker)
- run artifacts under `.aetherflow/runs/<run_id>/` (verify_report.json, trace.summary.json)

Typical files (read-only):

- the diff target files
- `protocol/COMPONENT_INDEX.md` to confirm ownership boundaries
- the reference platform file when cross-platform mirroring is claimed

Output:

- `.aetherflow/runs/<run_id>/code_review.md` with sections: Intent vs Diff, Findings by severity (blocker / risk / nit), Cross-Platform Check, Test Coverage, Summary with recommendation (`proceed` | `repair-then-recheck` | `needs-architecture-decision`)
- one-paragraph summary to the invoking agent

When to run:

- after Trace and Verification Agent + Benchmark Agent gates pass, before Architecture Agent docs sync
- skip docs-only, verification-only, benchmark-only, and governance-only changes — the Architecture Agent / Trace and Verification Agent / Benchmark Agent / `aetherflow-arch-sync` already cover those surfaces

Independence requirement:

- the implementing role must not review its own diff; self-review is not review
- invoke as a fresh subagent (no prior chat context) or hand the diff to the parity adapter on the other platform (Codex reviewing a Claude patch, or vice versa)

Closeout effect:

- blocker stops closeout; the implementing role gets one scoped repair attempt then this role is re-invoked
- risk is surfaced in the final closeout report; closeout proceeds; residual risk recorded
- nit stays in the report file

Should not:

- patch source, docs, or any file other than `<run_dir>/code_review.md`
- rerun smoke / verify / benchmark; those already passed when this role was invoked
- approve based on smoke passing alone — smoke is necessary, not sufficient

---

## 7. Autonomy Levels

Agents should not all have the same autonomy. AetherFlow should use explicit autonomy levels.

| Level | Name | Allowed behavior |
|---|---|---|
| A0 | Report only | Read artifacts, summarize, classify, no edits |
| A1 | Docs only | Edit documentation and reports |
| A2 | Tools/tests | Edit agent tools, tests, scripts, non-runtime automation |
| A3 | Scoped runtime patch | Edit explicitly allowed runtime files |
| A4 | Architecture change | Requires human approval before patching |
| A5 | Release decision | Human-only |

Recommended defaults:

- scheduled verification agents: A0
- architecture sync agent: A1
- tool repair agent: A2
- runtime repair agent: A3 with allowed paths
- scene policy redesign: A4
- privacy behavior guarantees: A4 or A5

This lets the owner rely heavily on agents without giving them unlimited control.

---

## 8. Human Approval Boundaries

The Architecture Planning Gate is mandatory before implementation for runtime behavior changes, architecture/development governance changes, cross-component changes, and chained development requests. It is not required for ordinary conversation, direction review, brainstorming, report-only work, or read-only planning.

After approval, the gate must materialize the plan to `.aetherflow/runs/<task_id>/plan.md` (using `.aetherflow/templates/plan.template.md` as the skeleton) **before** any implementing role starts patching. The plan artifact carries the quoted approval wording, owning role, declared file scope, success gates, and non-goals so the Code Review Agent can audit intent-vs-diff from disk rather than chat history.

Agents can do most execution work, but humans should approve:

- product direction changes
- architecture/development governance changes
- scene policy semantics
- privacy mask behavior
- benchmark threshold changes
- encoder quality tradeoffs
- large refactors
- release readiness
- changes that alter the runtime boundary

Agents can usually proceed without approval for:

- ordinary conversation and direction review
- read-only planning or brainstorming
- docs cleanup
- artifact schema documentation
- trace parser fixes
- narrowly scoped build fixes
- typo fixes
- failed-run triage
- benchmark report generation

The practical model:

```text
Human sets direction.
Agent executes the loop.
Verification decides pass/fail.
Human approves high-risk changes.
```

---

## 9. Platform Adapters

### 9.1 Codex Adapter

Codex usually works in a local workspace.

Expected behavior:

- read `AGENTS.md`
- read component index
- run the Architecture Planning Gate before implementation/runtime/governance/cross-component patching
- use shell commands
- patch files directly
- run verification commands
- report in chat

Codex should still follow the universal workflow and produce the same artifacts when runtime behavior changes.

### 9.2 Claude Code Adapter

Claude Code also works in a local workspace.

Expected behavior:

- read `CLAUDE.md`
- follow `AGENTS.md` if present
- use the native architecture subagent for the Architecture Planning Gate when available
- use the same Python tools
- keep patches scoped
- report changed files and verification

`CLAUDE.md` should remain a thin adapter. It should point back to the universal protocol instead of defining a separate process.

### 9.3 GitHub Copilot Coding Agent Adapter

GitHub Copilot coding agent works through issues and pull requests.

Expected behavior:

- issue provides task intake
- repository instructions point to the universal protocol
- issue or PR starts with an Architecture Planning Gate plan unless the issue explicitly pre-approves implementation
- Copilot opens a branch and PR
- GitHub Actions run verification
- PR body reports artifact paths and verification result

GitHub-specific files:

- `.github/copilot-instructions.md`
- `.github/agents/*.md`

GitHub-specific files that can be added later:

- `.github/ISSUE_TEMPLATE/agent_task.yml`
- `.github/workflows/health.yml`
- `.github/workflows/benchmark.yml`
- PR checklist

These files should adapt GitHub to the protocol, not replace the protocol.

---

## 10. Scheduled Agent Loops

The project can use scheduled agents, but the schedule should create evidence first and patch only under controlled conditions.

### 10.1 Daily Health Loop

Autonomy:

```text
A0 report only
```

Purpose:

- detect breakage early

Actions:

- build
- smoke run
- summarize trace
- verify existing run

Output:

- run artifact
- health report
- issue or handoff if failed

### 10.2 Nightly or Weekly Benchmark Loop

Autonomy:

```text
A0 report only
```

Purpose:

- detect quality or latency regressions

Actions:

- smoke run
- ROI benchmark
- latency review
- benchmark report

Output:

- benchmark artifact
- regression issue if failed

### 10.3 Failure Triage Loop

Autonomy:

```text
A0 or A1
```

Purpose:

- classify failed health or benchmark runs

Actions:

- read latest failed artifact
- summarize evidence
- classify stage
- write handoff
- create task card for repair

Output:

- `diagnosis.json`
- `handoff.md`
- repair task

### 10.4 Scoped Repair Loop

Autonomy:

```text
A2 or A3
```

Purpose:

- patch a classified failure

Actions:

- read task card
- inspect source evidence
- patch allowed files
- run gates
- report result

Output:

- patch
- verify report
- handoff if failed

### 10.5 Architecture Sync Loop

Autonomy:

```text
A1 by default
A4 if architecture behavior changes
```

Purpose:

- keep architecture docs aligned with code
- keep `docs/1-status/PROJECT_STATUS.md` aligned with the latest verified progress

Actions:

- inspect component index
- inspect key interfaces and runtime entrypoints
- compare with architecture docs
- update docs/status snapshots or write drift report

Output:

- updated architecture docs
- updated project status/progress snapshot when evidence changed
- architecture drift report
- proposed tasks

This is the loop the owner should run regularly after major changes.

---

## 11. Artifact Schemas

The project already has basic templates under `.aetherflow/templates/`.

The long-term protocol should standardize these artifacts.

### 11.1 `task_card.json`

Purpose:

- define the task and the agent's operating bounds

Should include:

- goal
- failure command
- allowed paths
- blocked paths
- must-read files
- evidence
- success gates
- max fix iterations
- autonomy level
- architecture plan required/approved state
- approval evidence
- planned agents, skills, and tools

### 11.2 `diagnosis.json`

Purpose:

- record classification before patching

Should include:

- status
- failure stage
- suspected component
- evidence references
- confidence
- files read
- next action

### 11.3 `patch_report.md`

Purpose:

- explain what changed and why

Should include:

- changed files
- reason for change
- evidence that justified the change
- risks
- verification commands

### 11.4 `verify_report.json`

Purpose:

- machine-readable pass/fail result

Should include:

- build status
- smoke status
- trace status
- benchmark status
- command summaries
- artifact paths

### 11.5 `handoff.md`

Purpose:

- let another agent continue without chat memory

Should include:

- goal
- tried
- result
- first relevant evidence
- next suspected component
- must-read files

---

## 12. Minimum Viable Agent System

The minimum useful version is not complicated.

Add or maintain:

```text
AGENTS.md
protocol/AGENT_OPERABLE_ARCHITECTURE.md
protocol/COMPONENT_INDEX.md
protocol/AGENT_PROTOCOL.md
protocol/AGENT_COMMANDS.md
protocol/AGENT_CHANGELOG.md
tools/agent_run.py
tools/agent_summarize.py
tools/agent_verify.py
.aetherflow/templates/*
```

Then enforce this rule:

```text
No runtime patch is complete without an artifact path and verification result.
```

That alone makes Codex, Claude Code, and GitHub Copilot much more reliable.

---

## 13. Recommended Next Steps

### Step 1: Make this protocol the source of truth

Update:

- `AGENTS.md`
- `CLAUDE.md`
- `.github/copilot-instructions.md`

They should all point to:

```text
protocol/AGENT_PROTOCOL.md
protocol/COMPONENT_INDEX.md
```

### Step 2: Add autonomy level to task cards

Extend `task_card.json` with:

```text
autonomy_level
```

This lets the same protocol support report-only agents and patching agents.

### Step 3: Add architecture sync output

Create a standard output:

```text
docs/ARCH_DRIFT_REPORT.md
```

or store it under:

```text
.aetherflow/runs/<run_id>/architecture_drift.md
```

### Step 4: Extend the GitHub adapter

The baseline GitHub adapter should include `.github/copilot-instructions.md` and `.github/agents/*.md`. After the protocol is stable, add:

- issue templates
- GitHub Actions
- PR checklist

The GitHub layer should adapt to the protocol rather than invent a second process.

---

## 14. Final Principle

AetherFlow should be developed mostly by agents, but not governed blindly by agents.

The intended model is:

```text
Human:
  owns goals, architecture direction, privacy rules, release decisions

Agent:
  owns reproduction, diagnosis, scoped implementation, verification, handoff

Tools:
  provide deterministic build, smoke, benchmark, and report artifacts

Artifacts:
  preserve truth across agents and across time
```

This makes agent-based development practical without making the project dependent on one specific agent product.
