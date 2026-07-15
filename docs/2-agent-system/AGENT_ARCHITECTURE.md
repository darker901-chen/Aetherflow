# AetherFlow Agent Architecture

This document explains how file-based development agents cooperate around the
AetherFlow repository. It describes development-time intelligence only. The
product runtime remains local, deterministic, and free of chat or LLM calls.

Canonical protocol details:

- [AGENT_OPERABLE_ARCHITECTURE.md](../../protocol/AGENT_OPERABLE_ARCHITECTURE.md)
- [AGENT_COMMANDS.md](../../protocol/AGENT_COMMANDS.md)
- [AGENT_PROTOCOL.md](../../protocol/AGENT_PROTOCOL.md)
- [COMPONENT_INDEX.md](../../protocol/COMPONENT_INDEX.md)
- [PROJECT_STATUS.md](../1-status/PROJECT_STATUS.md)

## 1. Current Implementation State

| Surface | Current state | Boundary |
|---|---|---|
| Protocol responsibilities | Seven: Architecture, Runtime Scene, Encoder, Trace and Verification, Benchmark, Code Review, Repair | Responsibilities are canonical; adapter filenames are implementation details. |
| GitHub adapters | 7 files in `.github/agents/` | Static file parity verified; native host execution not comprehensively exercised. |
| Claude adapters | 7 files in `.claude/agents/` | Static file parity verified. |
| Codex adapters | 7 files in `.codex/agents/` | Static parity verified; all TOML files parse. |
| Architecture-sync skill | Claude and Codex repository-local adapters | This is a skill, not an eighth protocol role. |
| Planning artifact | `.aetherflow/runs/<run_id>/plan.md` | Required after approval and before a governed patch. |
| Run and verification harness | `tools/agent_run.py`, `agent_summarize.py`, `agent_verify.py` | Canonical entry points; PowerShell files are compatibility wrappers. |
| Independent review | `code_review.md` written after implementation | Reviewer reports findings by severity and does not patch. |
| Scoped repair | Repair responsibility, represented by the `debug-verifier` adapter | At most one scoped repair attempt after a classified failed gate or review blocker. |
| Longitudinal evidence | `.aetherflow/audit/ledger.jsonl`, `evidence_log.md`, public effectiveness log | Run bundles remain local; public summaries contain attributable evidence only. |
| Scheduled loops | Defined in the protocol | Installation as an external scheduler has not been verified. |

## 2. System Layers

```text
User intent and approval
        |
        v
Architecture planning and role routing
        |
        v
Owning implementation responsibility
        |
        v
Build / smoke / trace / benchmark evidence
        |
        v
Independent review -> optional scoped repair -> final judgment
        |
        v
Architecture/status/QA synchronization and report
```

There are three distinct layers:

1. **Product runtime** — C++ capture, decision, masking, encoding, streaming,
   and trace code. It must remain deterministic and locally testable.
2. **Development-agent system** — file-based roles that read source and
   artifacts, plan changes, patch files, run gates, review diffs, and report.
3. **Evidence layer** — immutable or append-only run artifacts that let a later
   agent verify what actually happened instead of relying on chat memory.

The protocol is the source of truth. `.github/agents`, `.claude/agents`, and
`.codex/agents` adapt that protocol to host-specific formats and must not create
new rules.

## 3. Collaboration Medium

Agents cooperate through repository files rather than hidden conversational
state.

### Intent artifacts

- `plan.md` — approved scope, likely files, approach, success gates, decisions,
  and the user's approval wording.
- Source and documentation diff — the actual implementation under review.

### Runtime and verification artifacts

- `run_manifest.json` — environment, command, outputs, and run identity.
- `console.log` — build/runtime console evidence.
- `frame_trace.jsonl` — frame-level runtime trace.
- `trace.summary.json` — compact trace summary.
- `verify_report.json` — gate results and final verification status.
- `benchmark_report.*` — benchmark evidence when requested.

### Failure and review artifacts

- `diagnosis.json` — classified failure and first relevant evidence.
- `patch_report.md` — scoped repair description.
- `code_review.md` — independent diff audit and final recommendation.
- `.aetherflow/audit/ledger.jsonl` — one quantitative row per verified run.
- `.aetherflow/audit/evidence_log.md` — warnings/failures that demonstrate the
  governance layer caught something.

Raw run directories are ignored because traces or recordings may contain
captured-screen-sensitive data. Public status and effectiveness documents cite
only the minimum evidence needed to support a claim.

## 4. Seven Protocol Responsibilities

| Responsibility | Owns | Must not do | Primary output |
|---|---|---|---|
| Architecture | Scope, planning gate, role routing, architecture/status/protocol/component synchronization | Patch governed behavior before approval; infer current state from summaries alone | Approved `plan.md`, synchronized public docs |
| Runtime Scene | Scene-first frame decisions, ROI/mask producers, policy engine, runtime trace fields | Add LLM calls to the product; block the frame path with slow analysis | Runtime patch and run artifacts |
| Encoder | Encoder interface, NVENC, oneVPL, VideoToolbox boundary, input texture ownership, encode latency | Change scene/product policy without Architecture routing | Backend patch and hardware-specific evidence |
| Trace and Verification | Trace schema, summarizer, verifier, evidence quality, failure classification | Hide warnings or promote source inspection to runtime proof | `trace.summary.json`, `verify_report.json`, diagnosis |
| Benchmark | Benchmark execution and interpretation from artifacts | Change product code during report-only work | Benchmark report and bounded conclusion |
| Code Review | Independent audit of intent versus diff, correctness, edge/race behavior, portability, and scope | Patch the files it reviews | Severity-ranked findings and `proceed` / `repair-then-recheck` / `needs-architecture-decision` recommendation |
| Repair | One narrow fix for a classified failure or review blocker | Redesign unrelated components or perform repeated unbounded repairs | Scoped patch and repair report |

Adapter names intentionally compress some responsibilities:

| Protocol responsibility | Adapter filename |
|---|---|
| Architecture | `architecture` |
| Runtime Scene | `scene-runtime` |
| Encoder | `encoder` |
| Trace and Verification | `trace-verifier` |
| Benchmark | `benchmark-reporter` |
| Code Review | `code-reviewer` |
| Repair | `debug-verifier` |

## 5. Standard Implementation Lifecycle

```text
Observe
  -> Architecture Planning Gate
  -> explicit approval
  -> materialize plan.md
  -> reproduce
  -> classify
  -> patch
  -> build
  -> smoke test
  -> benchmark when required
  -> verify
  -> independent code review
  -> one scoped repair when required
  -> re-verify / delta review
  -> architecture and status sync
  -> report
```

### Planning gate

Before changing runtime behavior, architecture/governance documents,
cross-component code, or an `agent:chain` workflow, the Architecture
responsibility states:

- goal and scope;
- owning role;
- agents, skills, and tools;
- likely files;
- success gates;
- explicit non-goals and approval boundary.

After the user approves, copy `.aetherflow/templates/plan.template.md` to the
run directory and fill every section before patching.

### Verification depth

- Documentation/governance-only changes: language, links, configuration
  parsing, architecture/component parity, and diff checks. Independent Code
  Review is skipped by default and runs only when an approved plan explicitly
  adds it, as in the 2026-07-14 public-documentation audit.
- Runtime, encoder, ROI, QP, or mask behavior: run `agent_run.py`, then
  `agent_verify.py --run-benchmark` unless the task card explicitly narrows the
  gate with a justified reason.
- Failed build/smoke/trace: classify from artifacts, perform one scoped repair,
  then rerun the failed gate and review the repair delta.

## 6. Role Routing

| Request | Route |
|---|---|
| Scene, ROI, QP, privacy-mask behavior | Architecture -> Runtime Scene -> Trace and Verification -> Benchmark -> Code Review -> docs sync |
| Encoder, backend, input texture, encode latency | Architecture -> Encoder -> Trace and Verification -> Benchmark -> Code Review -> docs sync |
| Trace, verifier, or tooling behavior | Architecture when behavior changes -> Trace and Verification -> Code Review -> docs sync if user-facing |
| Failed run or artifact repair | Trace and Verification -> Repair -> Trace and Verification -> Code Review when code changed |
| Benchmark-only report | Benchmark; no source patch unless explicitly requested |
| Architecture, protocol, component index, status | Architecture -> static verification; Code Review only when an approved plan explicitly requires it |

Plain-language requests use the same router. Users do not need to know adapter
names or repeat the standard verification and closeout sequence.

## 7. Handoff Without Public Noise

Handoff is an evidence contract, not a public chronological journal.

| Information | Durable owner |
|---|---|
| Current release identity and open gates | `docs/1-status/PROJECT_STATUS.md` |
| Dated verification history | `docs/4-qa-debugging/VERIFICATION_HISTORY.md` |
| Product architecture | `docs/3-product/ARCHITECTURE.md` |
| Component ownership | `protocol/COMPONENT_INDEX.md` |
| Per-run intent and results | `.aetherflow/runs/<run_id>/` |
| Longitudinal machine-readable evidence | `.aetherflow/audit/ledger.jsonl` |
| Public agent-value examples | `docs/2-agent-system/AGENT_EFFECTIVENESS_LOG.md` |

The public status page stays concise. Detailed run chronology belongs in
verification history, while task-specific handoff remains in ignored run
artifacts.

## 8. Approval and Autonomy

The protocol defines autonomy levels A0 through A5. The practical boundary is:

- Read-only conversation, planning, reporting, and benchmark interpretation do
  not require implementation approval.
- A clear implementation instruction authorizes the standard scoped workflow.
- Governed patching still requires the planning gate and a materialized plan.
- A materially different scope, destructive external action, repository
  visibility change, release publication, or missing product decision requires
  explicit direction.
- Standard build, smoke, verification, one repair attempt, documentation sync,
  and reporting do not require the user to repeat approval after implementation
  has been authorized.

## 9. Canonical Commands

```powershell
# Build and run the default smoke path.
python tools/agent_run.py --run-id <run_id>

# Summarize an existing run.
python tools/agent_summarize.py --run-dir .aetherflow/runs/<run_id>

# Verify artifacts.
python tools/agent_verify.py --run-dir .aetherflow/runs/<run_id>

# Include the benchmark gate for runtime/encoder/ROI/QP/mask changes.
python tools/agent_verify.py --run-dir .aetherflow/runs/<run_id> --run-benchmark
```

Role shortcuts are defined in
[AGENT_COMMANDS.md](../../protocol/AGENT_COMMANDS.md), for example:

```text
agent:architecture design scene inference MVP
agent:runtime implement scene inference MVP
agent:encoder fix an NVENC input-pool issue
agent:trace improve trace evidence
agent:benchmark report the current ROI benchmark
agent:review audit the current diff
agent:repair fix a classified failed run
```

## 10. Runtime Boundary

Development agents may use local source, logs, traces, and run artifacts. They
must not introduce chat-model calls into `AetherFlow.exe`.

```text
Allowed runtime slow path:
sampled frame -> local on-device analyzer -> cached advisory result

Forbidden runtime dependency:
captured frame -> network/LLM request -> block capture or encode
```

Slow analyzers must be sampled, droppable, asynchronous, and fallback-safe.
Deterministic producers always outrank advisory analysis when protecting
sensitive content.

## 11. Evidence Interpretation Example

Notification masking illustrates why evidence must stay bounded:

- A local Win32 popup fixture proves producer geometry, process filtering, and
  mask application for the fixture condition.
- `teams_notification_alias_fix` proves Teams alias/config/window identity for
  the observed live window.
- Neither artifact proves every Teams notification form or the other default
  whitelist applications.
- Missing live LINE, Slack, Discord, Telegram, and WhatsApp evidence remains an
  explicit QA gate.

Agents must preserve these distinctions when updating status or release claims.
