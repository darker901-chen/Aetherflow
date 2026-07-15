# Agent and Pipeline Change Log

This file records notable changes to the development-agent protocol and a small
set of early pipeline changes that established the evidence model. Current
product status belongs in `docs/1-status/PROJECT_STATUS.md`.

## 2026-07-14: English Public Documentation Boundary

### Files changed

- Public root, product, QA, protocol, and agent-adapter documentation
- `docs/1-status/PROJECT_STATUS.md`
- `docs/4-qa-debugging/VERIFICATION_HISTORY.md`
- `docs/DOCUMENTATION_INDEX.md`

### Summary

All tracked first-party documentation was normalized to English. The public
`handoff` category was retired: current truth now belongs in the concise status
snapshot, dated evidence belongs in verification history, and task-specific
handoff remains an ignored run artifact. The resolved judder handoff was renamed
as an investigation record. Ignored local notes remain outside Git and outside
public documentation routing. Runtime, encoder, trace, CLI, environment, and
package behavior were unchanged.

## 2026-05-21 (c): Audience Shortcut Parity

### Files changed

- Architecture adapters for Claude and GitHub
- Claude architecture-sync skill
- Then-public audience/orientation documentation

### Summary

A user found that an audience guide omitted the newly added Code Review role.
The architecture-sync checks searched for formal role names such as
`Code Review Agent`, while audience documents used shortcut syntax such as
`agent:review`. Neither search surface found the other.

The Architecture responsibility gained a documentation quality gate requiring
both formal role-name and `agent:<role>` sweeps when roles change. The
architecture-sync skill gained a role-shortcut parity check against the alias
table in `AGENT_COMMANDS.md`, including explicit verification of role-count
phrases. This was a governance/documentation-only change; runtime, encoder,
trace, CLI, and environment behavior were unchanged.

## 2026-05-21 (b): Materialized Planning Artifact

### Files changed

- `.aetherflow/templates/plan.template.md`
- `tools/agent_run.py`
- Architecture and code-review adapters
- `protocol/AGENT_PROTOCOL.md`
- `AGENTS.md`, `CLAUDE.md`
- `docs/2-agent-system/AGENT_ARCHITECTURE.md`

### Summary

The Architecture Planning Gate changed from a chat-only statement into a
durable artifact. After approval and before a governed patch, the Architecture
responsibility writes `<run_dir>/plan.md` containing:

- goal;
- owning role;
- allowed and out-of-scope files;
- approach;
- success gates;
- planning decisions;
- approval wording and timestamp.

This preserves intent after context loss, gives Code Review a real
intent-versus-diff source, and makes plan-versus-result auditing possible.
`tools/agent_run.py` copies the template only when no plan exists, so an approved
plan cannot be overwritten.

A missing plan on a triggered change is a Code Review **risk**, not an automatic
blocker. The change affected governance and template handling only; product
runtime behavior was unchanged.

## 2026-05-21: Independent Code Review Responsibility

### Files changed

- GitHub and Claude `code-reviewer` adapters
- `protocol/AGENT_PROTOCOL.md`
- `protocol/AGENT_COMMANDS.md`
- `AGENTS.md`, `CLAUDE.md`
- `docs/2-agent-system/AGENT_ARCHITECTURE.md`

### Summary

Code Review became the seventh protocol responsibility. It audits the actual
diff after verification and benchmark gates and before architecture/status
closeout. Triggering surfaces include runtime, encoder, scene, ROI, mask,
trace-schema, CLI, environment, canonical tooling, and cross-platform changes.

Findings use three severities:

- blocker;
- risk;
- nit/improvement.

Recommendations are `proceed`, `repair-then-recheck`, or
`needs-architecture-decision`. The report lives at
`.aetherflow/runs/<run_id>/code_review.md` and covers intent versus diff,
findings, portability, test coverage, and summary.

The implementing role must not review its own patch. Review adapters may write
the report but must not patch tracked implementation files.

## 2026-03-24: NVENC Observability and Log Control

### Files changed

- `include/AetherFlow/NvencH264Wrapper.h`
- `src/NvencH264Wrapper.cpp`

### Observability

Added counters for:

- `dropCount`
- `encoderBusyCount`
- `lockBusyCount`
- `mapFailures`
- `unmapFailures`
- `maxInflightSlotsObserved`
- `encodedFrameCount`

Submit attempts drain once before and once after submission. Per-frame telemetry
records capture, conversion completion, encode submission/output, and frame age
at submit/output. The full-GPU `CopyResource` path remains; registering converter
output directly is a deferred optimization.

### Noise control

Per-frame telemetry became disabled by default. Optional output is sampled or
emitted when latency crosses a threshold. Controls:

- `AETHERFLOW_NVENC_FRAME_TELEMETRY`
- `AETHERFLOW_NVENC_FRAME_TELEMETRY_VERBOSE`
- `AETHERFLOW_NVENC_FRAME_TELEMETRY_INTERVAL`
- `AETHERFLOW_NVENC_FRAME_TELEMETRY_ALERT_MS`

### Verification snapshot

Historical quiet run: `output/run_2026-03-24_nvenc_quiet.log`.

- Average FPS: 29.77
- Remaining AI budget: 20.75 ms
- `encodedFrameCount=896`
- zero drops, busy counts, and map/unmap failures
- `maxInflightSlotsObserved=1`

No new CPU-side multi-thread pipeline was introduced in this change. With
NVENC async mode available, the per-frame `Enc` number measures submit/drain
work in that loop, not complete encode latency.

### Log field meanings

- `Cap`: capture-call time
- `Conv`: BGRA-to-NV12 GPU video-processor time
- `ROI`: ROI-map generation/update time
- `Enc`: current-loop encode submit/drain cost
- `Total`: sum of the reported stage costs
- `AIBudget(remaining)`: `33.33 ms - Total` at a 30 fps target

## 2026-03-25: Intel Synchronization, Pipeline, and Rate Control

### Change 1: oneVPL cross-engine GPU fence

File: `src/VplH264Wrapper.cpp`,
`VplH264Wrapper::MergeYUVtoNV12_GPU()`.

A `D3D11_QUERY_EVENT` completion poll was added after `Flush()`. D3D11 copy/3D
work and the Intel hardware video-encode engine are separate execution domains;
`Flush()` submits commands but does not wait. The query provides an explicit
cross-engine fence before encode consumes the NV12 surface.

### Change 2: producer-consumer pipeline

File: `src/main.cpp`.

The serial loop was replaced by producer and consumer responsibilities:

- Producer: WGC pacing, capture, `VideoProcessorBlt`, pool copy, cursor sample,
  bounded-queue push.
- Consumer: queue pop, `EncodeFromYUVWithROI`, pool release, completed-frame
  recording.

The pipeline uses a four-slot `NV12TexturePool` and a four-entry
`BoundedQueue<PipelineFrame>`. Capture failures became atomic. Benchmark end
time moved after consumer join, and repeated encode failure closes the queue.

### Change 3: Intel CBR bitrate diagnosis

The tested Intel UHD driver enforced an approximately 1074 kbps minimum for
1080p30 CBR. A 600 kbps request was silently negotiated upward:

```text
Original TargetKbps=600, Queried TargetKbps=1074
```

The wrapper now records requested and negotiated parameters, adopts Query
adjustments, reads effective bitrate through `GetVideoParam`, applies
`BRCParamMultiplier`, and warns on mismatch. This established the rule that
requested configuration and effective configuration must both be observable.

### Failed experiments retained as compatibility evidence

#### CQP with video-memory input

On the tested Intel UHD path,
`MFX_RATECONTROL_CQP + MFX_IOPATTERN_IN_VIDEO_MEMORY` failed on the first
`EncodeFrameAsync` call with `MFX_ERR_DEVICE_FAILED`. The zero-copy path returned
to CBR. This is hardware/driver-specific evidence, not a universal API claim.

#### VBR for stronger ROI contrast

VBR increased both ROI and background budget, reducing visible contrast. CBR
preserves the zero-sum competition needed by that experiment.

## Update Rule

After every agent-authored code modification, append a dated (`YYYY-MM-DD`)
entry. Documentation-only work may also add an entry when it materially changes
protocol, governance, or public evidence routing.
