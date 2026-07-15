# AetherFlow Product Roadmap

> **Versioning note (2026-07-14):** `v0.1`, `v0.2`, and `v0.3` are capability
> milestones, not published Git tags. The repository has no formal release
> version. Current verification boundaries are maintained in
> [PROJECT_STATUS.md](../1-status/PROJECT_STATUS.md).

## Product Decision

AetherFlow leads with **Live Share Guard**: local, deterministic, pre-encode
privacy masking for screen sharing. Scene detection remains the runtime
organizing model and an optional advisory slow path, but it is not the first
product claim by itself.

The product sequence is:

```text
detect a bounded sensitive surface
-> choose a deterministic protection action
-> modify pixels before encode
-> prove the result through trace and visible output
```

## Positioning

AetherFlow demonstrates three load-bearing capabilities:

1. native GPU screen-media engineering across Windows and macOS;
2. deterministic, fallback-safe privacy behavior on the media fast path;
3. a file-based development-agent workflow with reproducible verification and
   independent diff review.

AI is local, opt-in, and advisory until accuracy and product-action gates are
closed. It may fail or be absent without stopping capture, masking, or encode.

## Why Live Share Guard First

- The benefit is visible: sensitive pixels are altered before they enter the
  encoded stream.
- Deterministic operating-system signals can provide useful protection without
  waiting for model quality.
- The fast-path architecture already supports frame-level regions and a GPU
  compositor.
- Verification can combine fixture geometry, real application evidence, trace,
  and an uninterrupted source-versus-output demo.
- The claim can remain narrow and honest while optional analysis evolves.

## Scene Detection Role

Scene-first means the system models frame content before selecting downstream
actions. It does not mean every product decision must be controlled by AI.

Current responsibilities:

- baseline scene and deterministic producer merge;
- optional cursor quality region;
- optional local ONNX scene proposal;
- advisory policy and Studio status;
- an opt-in visual demo action that is explicitly not product behavior.

Future responsibility, gated on evidence:

- influence encoder strategy only after real-screen classifier quality,
  stabilization, cross-platform behavior, and rollback semantics are measured.

## v0.1: Live Share Guard

### Implemented foundation

- manual privacy-mask producer;
- startup panic mask and Studio panic latch;
- password-field producer using UI Automation `IsPassword=true` Edit controls;
- notification/window producer with process whitelist, packaged-app aliases,
  and z-order clipping;
- off-thread polling for password and notification producers;
- Windows pre-encode blackout, blur, and mosaic compositor paths;
- internal grayscale scene-demo shader;
- macOS chat-window producer and CoreImage/Metal compositor;
- trace fields for source, counts, path, fallback, panic state, and timing;
- verifier coverage for deterministic fixtures and panic path.

### Success gates

| Gate | State | Required closeout |
|---|---|---|
| Deterministic sensitive producer wired | Met for bounded password/window conditions | Preserve exact coverage boundary. |
| Mask applied before encode | Met | Keep visible path and trace evidence. |
| Panic protection | Met in startup evidence | Finalize product UX separately from the developer hotkey. |
| Mask-stage cost under 4 ms | **Partially measured** | Add GPU timestamps; current `maskMs` is CPU dispatch, not shader completion. |
| Real application coverage | Partial | Add live LINE, Slack, Discord, Telegram, and WhatsApp artifacts; broaden password-control matrix. |
| Visual proof | Partial | Record uninterrupted source-versus-output blackout/blur/mosaic coverage and internal grayscale demo evidence. |
| Current release package | Open | Rebuild, stage-test, checksum, and decide signing/version scope. |

### v0.1 completion boundary

Do not call the milestone complete as a formal release until the P0 publication,
capture-reliability, real-application, visual-proof, and current-package gates
in project status are addressed or explicitly excluded from a source-only
pre-release.

## v0.2 / Phase 4: On-Device Scene Classifier and Policy

### Goal

Demonstrate a local scene classifier feeding a stable advisory policy on
Windows and macOS, with evidence strong enough to decide whether product action
or encoder steering is justified.

### P0.1 — Windows ONNX classifier: implemented

- `SceneClassifierOnnx` implements `IAIFrameAnalyzer`.
- ONNX Runtime uses DirectML when available, with CPU fallback.
- Real captured BGRA pixels enter the model through a staging readback.
- The worker is asynchronous; the bridge drops work when busy.
- The current model is CLIP zero-shot Stage A with five labels.
- Trace and verifier blocks are additive and report `not_applicable` when the
  feature is inactive.

Open P0.1 gate: real-screen accuracy is unmeasured. A labeled sweep must decide
whether Stage A is acceptable or Stage B fine-tuning is required.

### P0.2 — macOS CoreML classifier: pending

Planned work:

- `SceneClassifierCoreML` adapter;
- CoreML model converted from the same classifier head;
- Neural Engine preferred, GPU/CPU fallback;
- reuse the shared `IAIFrameAnalyzer`, `AsyncAnalyzerBridgeModule`,
  `PolicyEngine`, and trace schema;
- current-tree macOS verification artifact.

No CoreML adapter or `.mlmodel` exists on disk today.

### P0.3 — Shared policy engine: implemented, advisory

`PolicyEngine` maps scene state into:

```text
PolicyDecision { mask_mode, encode_hint, mode_label, reason }
```

It includes a 150-frame (5-second at 30 fps) switch floor,
three-consecutive confirmation, low-confidence
fallback, and panic override reset. Product consumers are trace and Studio
status; the opt-in `SceneDemoActionModule` also reads the stable class to drive
a non-product compositor preview. Policy does not gate the merged scene or
change encoder parameters.

### P0.4/P0.5 — Evidence schema: implemented

Trace and verification include classifier state, inference count/latency,
failure count, class distribution, policy switches, fallback frames, and
low-confidence frames. Gates cover inference latency, switch cadence,
fast-path non-blocking behavior, and low-confidence fallback.

### P0.6 — Dual-platform demonstration: pending

Required evidence:

- Windows and macOS running the same five-class contract;
- local inference provider visible;
- deterministic masking unaffected when analysis fails or is absent;
- policy remains advisory unless a separately approved product-action gate is
  implemented.

### P1.0 — Android adapter: gated

Android work begins only after suitable NNAPI/NPU hardware is available and the
Architecture Planning Gate approves the platform scope. Reserved components:

- MediaProjection capture;
- HardwareBuffer/ImageReader bridge;
- MediaCodec H.264 encoder;
- LiteRT classifier with NNAPI/GPU delegate policy;
- JNI and Gradle integration.

An emulator CPU path is not NPU evidence and must not be used to claim mobile
hardware parity.

### P1.1 — Policy-to-product action: pending decision

Potential work:

- define which stable scene states may influence bitrate, GOP, or quality
  regions;
- specify deterministic override and rollback behavior;
- add merge-layer scene stabilization;
- test NVENC, oneVPL, and VideoToolbox separately;
- measure benefit against built-in encoder adaptive-quality features.

Do not wire `encode_hint` into production merely because the field exists.

### Explicitly out of Phase 4

- cloud or remote LLM inference;
- OCR/QR sub-region detection as a classifier prerequisite;
- custom backbone training before the Stage A checkpoint;
- universal sensitive-content detection claims;
- unmeasured automatic encoder steering;
- Android implementation without hardware and scope approval.

## Phase Checkpoints

| Checkpoint | Question | Required artifact |
|---|---|---|
| Windows quality | Is Stage A accurate enough on labeled real screens? | Dataset/sweep summary and go/fine-tune decision |
| macOS parity | Does CoreML preserve the same class/policy/trace contract? | macOS run and verification report |
| Product action | Does a stable policy improve an operator-visible outcome without weakening deterministic safety? | A/B trace, quality/latency report, rollback test |
| Android gate | Is suitable hardware available and is the platform scope approved? | Architecture plan and device record |
| Release | Are current binaries, version, checksums, signing, and support scope defined? | Release checklist and staged-package evidence |

Pivot or narrow scope when:

- classifier quality remains inadequate after a bounded fine-tuning attempt;
- the slow path creates measurable fast-path regression;
- cross-platform contracts require platform-specific product behavior that
  cannot be explained cleanly;
- encoder steering does not beat simpler deterministic or built-in adaptive
  behavior by a meaningful margin;
- packaging/support cost exceeds the value of a binary release.

## v0.3: Product Packaging and Integration

The existing portable zip builder is a delivery utility, not proof that v0.3
is complete.

Possible v0.3 surfaces:

- signed Windows portable package or installer;
- documented SDK boundary for frame decision and mask producers;
- plugin registration for deterministic producer modules;
- stable configuration schema and migration policy;
- host integration sample;
- release checksums, supported-platform matrix, and security support policy.

Completion requires a maintained public API boundary and support commitment,
not only a working archive.

## Final Direction

1. Complete the English public documentation and private local study split.
2. Produce real-time visual proof of deterministic pre-encode masking.
3. Close the source-only versus binary-release decision.
4. Finish current hardware/platform evidence before widening claims.
5. Measure classifier quality before allowing advisory policy to control product
   or encoder behavior.
