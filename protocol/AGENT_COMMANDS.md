# AetherFlow Agent Command Shortcuts

Date: 2026-05-02

This file defines short commands for invoking the AetherFlow agent protocol without repeating the full prompt every time.

These commands are meant for Codex, Claude Code, GitHub Copilot coding agent, or any future file-based coding agent that follows `AGENTS.md`.

---

## 1. Basic Form

Use this form:

```text
agent:<role> <goal>
```

Examples:

```text
agent:architecture design scene inference MVP
agent:runtime implement scene inference MVP
agent:trace improve scene inference trace fields
agent:encoder fix NVENC input pool issue
agent:benchmark run ROI benchmark and report
agent:repair fix latest failed run
```

The agent must automatically load:

- `AGENTS.md`
- `protocol/AGENT_PROTOCOL.md`
- `docs/3-product/ARCHITECTURE.md`
- `protocol/COMPONENT_INDEX.md`

The user should not need to repeat these files in every prompt.

## 1.1 Minimal User Prompt Contract

The user may also provide a plain-language requirement without `agent:<role>`.

Example:

```text
Implement the Live Share Guard v0.1.1 Right Ctrl panic-mask fast path; do not add an AI detector.
```

The main agent must infer the role, run the standard implementation workflow, verify, repair once if needed, sync docs, and report artifacts. The user should not have to restate:

- run `agent_run`
- run `agent_verify`
- run benchmark for runtime/encoder/ROI/QP/mask changes
- use debug-verifier on failed build/smoke/trace
- sync architecture/component docs
- include run dir, verify report, trace summary, changed files, and residual risks

---

## 2. Architecture Planning Gate

The Architecture Planning Gate controls patching, not ordinary discussion. General conversation, direction review, brainstorming, report-only work, and read-only planning can proceed without explicit approval.

For implementation requests, runtime behavior changes, architecture/development governance changes, cross-component changes, or `agent:chain` requests, start with the Architecture Agent before implementation.

The first response must be a plan, not a patch. It must state:

- interpreted goal
- agents, subagents, skills, and tools that will be used
- likely files or components to inspect or edit
- autonomy level and approval boundary
- success gates and expected artifacts
- direct approval question

Do not patch source files or governance documents until the user explicitly approves with wording such as `approve`, `go`, `implement`, `start`, `fix it`, or any equally unambiguous execution instruction.

If the latest user message already contains clear execution approval for the scoped plan under discussion, treat it as approval and proceed without asking for a second confirmation. If scope is unclear or high-risk, narrow the plan first.

Exceptions:

- report-only requests such as `agent:benchmark`
- docs-only cleanup requests such as `agent:docs`, unless they change architecture/development governance rules
- direct verification commands where the user only asks to run or summarize artifacts
- emergency narrow repair when the task card already contains approved paths and autonomy

---

## 3. Role Aliases

| Command | Role |
|---|---|
| `agent:architecture` | Architecture Agent |
| `agent:runtime` | Runtime Scene Agent |
| `agent:scene` | Runtime Scene Agent |
| `agent:encoder` | Encoder Agent |
| `agent:trace` | Trace and Verification Agent |
| `agent:verify` | Trace and Verification Agent |
| `agent:benchmark` | Benchmark Agent |
| `agent:review` | Code Review Agent |
| `agent:code-review` | Code Review Agent |
| `agent:repair` | Repair Agent |
| `agent:docs` | Architecture Agent, docs-only, including current status and verification history |

---

## 3.1 Role Inference Router

When no role shortcut is provided, route by task content:

| Request signal | Default route |
|---|---|
| scene labels, policy engine, ROI producer, privacy mask producer, frame decision trace fields | Architecture Gate -> Runtime Scene Agent -> Trace and Verification Agent -> Benchmark Agent -> Code Review Agent -> Architecture Agent docs sync |
| NVENC, oneVPL, encoder interface, input texture pool, encode latency/drop behavior | Architecture Gate -> Encoder Agent -> Trace and Verification Agent -> Benchmark Agent -> Code Review Agent -> Architecture Agent docs sync |
| trace schema, summarizer, verifier, artifact quality, agent tools | Architecture Gate if behavior changes -> Trace and Verification Agent -> Code Review Agent -> Architecture Agent docs sync when user-facing |
| failed run, build failure, smoke failure, trace failure, existing `.aetherflow/runs/<id>` issue | Trace and Verification Agent -> Repair Agent -> Trace and Verification Agent -> Code Review Agent (when repair changed code) |
| benchmark-only, latency/quality report-only | Benchmark Agent only, no source patch unless explicitly requested |
| architecture docs, protocol docs, component index, project status/progress updates | Architecture Agent |

The role router is a responsibility map, not a requirement to use separate models. One executor may perform multiple roles, but must preserve the role boundaries and artifacts.

---

## 4. Chained Commands

Use this form when one role should hand work to the next role automatically:

```text
agent:chain <workflow> <goal>
```

Examples:

```text
agent:chain scene-inference design and implement scene inference MVP
agent:chain failed-run repair latest failed run
agent:chain architecture-sync update architecture docs after current changes
```

Chained commands still start with the Architecture Planning Gate unless the chain is report-only or the user explicitly says the plan is already approved.

---

## 5. Built-In Chains

### 5.1 Scene Inference Chain

Command:

```text
agent:chain scene-inference <goal>
```

Stages:

0. Architecture Planning Gate
   - resolve goal and scope
   - list intended agents/skills/tools
   - ask for approval before code edits

1. Architecture Agent
   - write or update scene inference plan
   - define acceptance criteria
   - define likely files

2. Runtime Scene Agent
   - implement the scoped runtime change
   - preserve scene-first design
   - avoid encoder changes unless required

3. Trace and Verification Agent
   - verify scene trace fields
   - summarize run artifacts

4. Architecture Agent
   - update architecture docs if code changed

### 5.2 Failed Run Repair Chain

Command:

```text
agent:chain failed-run <run_id or goal>
```

Stages:

0. Architecture Planning Gate, unless the run artifact already contains an approved task card
   - state intended debug/repair/verify roles
   - ask before patching if allowed paths are unclear

1. Trace and Verification Agent
   - inspect run artifacts
   - classify failure

2. Repair Agent
   - patch only the owning files

3. Trace and Verification Agent
   - verify build, smoke, and trace

4. Architecture Agent
   - update handoff or docs if behavior changed

### 5.3 Architecture Sync Chain

Command:

```text
agent:chain architecture-sync <goal>
```

Stages:

1. Architecture Agent
   - compare code entrypoints and docs
   - update docs or write drift report

2. Trace and Verification Agent
   - run docs-only checks

---

## 6. Default Autonomy

If the user does not specify autonomy, use these defaults:

| Role | Default autonomy |
|---|---|
| Architecture Agent | A1 docs-only |
| Runtime Scene Agent | A3 scoped runtime patch |
| Encoder Agent | A3 scoped runtime patch |
| Trace and Verification Agent | A2 tools/tests or report-only, depending on goal |
| Benchmark Agent | A0 report-only |
| Repair Agent | A3 only when allowed paths are clear; otherwise A0 classify first |

If a task requires A4 architecture change, stop and ask for approval before patching runtime code.

---

## 6.1 Default Implementation Completion Contract

For any implementation task that patches runtime, encoder, scene, ROI, QP, privacy-mask behavior, trace behavior, or agent tooling:

1. Choose a stable run id based on the task name.
2. Run:
   ```text
   python tools/agent_run.py --run-id <run_id>
   ```
3. Run verification:
   ```text
   python tools/agent_verify.py --run-dir .aetherflow/runs/<run_id>
   ```
4. If runtime, encoder, ROI, QP, or privacy-mask behavior changed, run the benchmark gate through the verifier:
   ```text
   python tools/agent_verify.py --run-dir .aetherflow/runs/<run_id> --run-benchmark
   ```
5. If build, smoke, or trace fails, use the debug-verifier flow from the run artifacts and perform one scoped repair attempt before rerunning verification.
6. If verification passes and behavior changed, sync:
   - `docs/3-product/ARCHITECTURE.md`
   - `protocol/COMPONENT_INDEX.md`
   - `README.md` when user-facing commands, flags, environment variables, or validation steps changed
   - `docs/1-status/PROJECT_STATUS.md` when a current capability, verification boundary, release gate, or next action changed
   - `docs/4-qa-debugging/VERIFICATION_HISTORY.md` when dated run/review evidence should be preserved
7. Documentation sync must be evidence-first:
   - Do not document output filenames, backend behavior, defaults, or validation commands from memory.
   - Verify user-facing paths from source code, run manifests, console logs, or actual artifacts.
   - Preserve the user's requested shell/platform in the primary SOP. If the user asks for bash, do not present PowerShell as the main path.
   - Avoid vague wording such as "usually" for deterministic paths. If behavior is conditional, state the exact condition.
   - For human validation SOPs, include exact commands, the exact file to open, expected correct behavior, and common wrong outcomes.
   - Search touched docs for contradictions after editing, then run `git diff --check`.
8. Final report must include:
   - run directory
   - `verify_report.json`
   - trace summary when present
   - changed files
   - residual risks

This contract is automatic. The user should only need to state the requirement.

---

## 7. Examples for This Project

Design scene inference:

```text
agent:architecture design scene inference MVP
```

Design and implement scene inference in one flow:

```text
agent:chain scene-inference design and implement scene inference MVP
```

Fix latest failed run:

```text
agent:chain failed-run latest
```

Run benchmark only:

```text
agent:benchmark run current ROI benchmark and report only
```

Update architecture after code changes:

```text
agent:chain architecture-sync update docs after latest runtime changes
```

---

## 8. Handoff Rule

When one role hands work to another role, it should use files, not memory.

Preferred handoff files:

- `.aetherflow/runs/<run_id>/architecture_plan.md`
- `.aetherflow/runs/<run_id>/task_card.json`
- `.aetherflow/runs/<run_id>/diagnosis.json`
- `.aetherflow/runs/<run_id>/handoff.md`

This lets Codex, Claude Code, and GitHub Copilot continue the same workflow even when they are different executors.
