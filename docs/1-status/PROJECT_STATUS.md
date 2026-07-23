# AetherFlow Project Status

Last checked: 2026-07-23 +08:00. This page is the concise source of truth for
the current release boundary. Dated implementation and verification records
live in [VERIFICATION_HISTORY.md](../4-qa-debugging/VERIFICATION_HISTORY.md).

## Release Identity

- **Release stage:** pre-release source snapshot on `master`.
- **Formal version:** none. No Git tag or GitHub Release is claimed.
- **Roadmap labels:** `v0.1`, `v0.2`, and `v0.3` are capability milestones,
  not published semantic versions.
- **Binary status:** `output/AetherFlow-portable-20260723.zip` (44.8 MiB) was
  rebuilt from current HEAD (`0536d42`) on 2026-07-23 and passed its staged
  self-tests (staged 300-frame capture/encode run, SRT probe decoded 30 frames,
  Studio `--ui-smoke` exit 0). SHA256
  `5506490343ED731215E0F3AC57AC2197D611ED45172BB147C651DA54248E3C11`. It is
  unsigned, not attached to any tag or GitHub Release, and `--with-model` was
  not exercised. No installer or code-signing gate has been completed.
- **Product boundary:** AetherFlow is a local, pre-encode masking prototype. It
  is not a certified DLP product and does not claim to cover every sensitive
  surface or application.

Status terms on this page:

- **Implemented:** code exists and is wired into the product.
- **Verified:** a named test or artifact exercised the stated path.
- **Partially verified:** the implementation exists, but at least one platform,
  backend, mode, or real-world condition lacks qualifying evidence.
- **Unverified / not implemented:** no qualifying artifact exists, or the
  feature is intentionally absent or stubbed. Source inspection is not runtime
  proof.

## Current Capability Matrix

| Surface | Implementation | Verification | Exact boundary |
|---|---|---|---|
| Windows capture -> decision -> encode -> trace | Implemented | **Verified locally** | `evidence_refresh_20260723`: 900/900 encoded, 0 encode failures, 0 trace parse errors, ROI benchmark exit 0. The near-static desktop reproduced the warn-only WGC retry ratio (8081/900 = 8.979), while the same-day moving-content run `capture_motion_20260723` recorded 0/300 retries (ratio 0.000) — the ratio tracks content dynamics, not a capture defect. |
| Deterministic password-field masking | Implemented and enabled by default | **Fixture-verified / real-application coverage partial** | Masks foreground UI Automation `IsPassword=true` Edit controls. The producer scans on a background poll thread and the frame path reads a cached snapshot. Browser and custom-control coverage is not comprehensive. |
| Deterministic notification/window masking | Implemented and enabled by default | **Fixture + Teams alias + live LINE verified / startup gap found** | The default whitelist covers LINE, Slack, Discord, Teams, Telegram, and WhatsApp executable identities. `line_window_mask_live_20260723` masked a real installed LINE window on 199/200 frames — but frame 0 was encoded unmasked because the window predated the first background poll snapshot (worst-case exposure = the 5-frame poll cadence; on SRT those frames leave the machine). A fail-closed startup/appearance gate is an open design decision. Canonical live artifacts are still missing for Slack, Discord, Telegram, and WhatsApp. |
| Windows mask visuals | Product modes: `blackout`, `blur`, `mosaic`; Windows default `blur`, fail-closed fallback `blackout` | **Trace-verified per mode / human visual confirmation pending** | `mask_sweep_{blackout,blur,mosaic}_20260723` masked 120/120 frames per mode through distinct GPU paths (`d3d11-bgra-clearview` / `-blur-shader` / `-mosaic-shader`), with muxed artifacts in `output/mask_sweep_<mode>.mp4`. An uninterrupted human source-versus-output confirmation and the internal `grayscale` demo effect (not a CLI/env product mode) remain unswept. |
| Panic mask | Implemented | **Verified** | Startup panic evidence applied a full-frame mask on every frame. The Right Ctrl hotkey is a developer test affordance, not the product trigger contract. |
| NVIDIA NVENC backend | Implemented and default when available | **Verified locally on NVIDIA** | Current and historical Windows runs encoded without failures. Long-duration soak and open encoder serialization observations remain. |
| Intel oneVPL backend | Implemented and build-covered | **Current hardware evidence missing** | A real Intel-machine run must verify timestamps, frame types, and the SRT access-unit path before backend parity is claimed. |
| SRT live output | Implemented, opt-in, video-only, single viewer | **NVENC loopback verified on current HEAD** | `srt_loopback_refresh_20260723`: a local FFmpeg client joined mid-stream and decoded 90 frames (sender: connections=1, sent=160, 0 queue-full drops, 13 AUs dropped awaiting keyframe by design). Intel hardware, phone/VLC over LAN, firewall onboarding, reconnect under real loss, and non-local latency are unverified. |
| Studio settings UI | Implemented | **Self-test verified / interaction coverage partial** | `--ui-smoke` and `--ui-selftest` passed on 2026-07-03. Panic during a live session, resize, exit while running, settings lockout, and corrupt-model error UX are not covered by deterministic gates. |
| Portable package builder | Implemented | **Current-HEAD package verified via staged self-tests** | `AetherFlow-portable-20260723.zip` (44.8 MiB, from sha `0536d42`) passed the staged capture/encode run, SRT probe, and Studio UI smoke. `--with-model`, signing, installer delivery, and a clean Intel-only machine remain unverified. |
| Windows ONNX scene classifier | Implemented, opt-in, off-thread, advisory | **Runtime plumbing verified on current HEAD / accuracy unverified** | `scene_classifier_onnx_20260723`: DirectML provider, p95 inference 13.235 ms, 0 failures, all four classifier gates passed. Real-screen accuracy is unmeasured. `PolicyDecision.encode_hint` does not steer the encoder. |
| Scene demo action | Implemented, opt-in, not product behavior | **Two of five classes evidenced** | Mapping is `sensitive_surface -> blackout`, `video -> mosaic`, `code_text -> blur`, `slides -> grayscale`, `mixed_ui/unknown -> passthrough`. The complete five-class visual sweep is missing. |
| macOS capture, encode, MP4, and chat-window masking | Implemented for phase 1/2 | **Historically verified; not rerun on the current Windows host** | ScreenCaptureKit, VideoToolbox, AVAssetWriter, and chat-window mosaic have dated artifacts. CoreML classification is absent, secure-text detection remains a stub, and ROI/QP is unsupported. |
| Android path | Planned only | **Not implemented** | MediaProjection, MediaCodec, LiteRT, JNI, and Gradle integration are reserved architecture paths, not files on disk. |
| Agent development system | Seven protocol responsibilities; seven GitHub, seven Claude, and seven Codex adapters | **Static parity and historical workflow value verified** | Adapter filenames match and Codex TOML parses. Native execution in every host and installation of the protocol's scheduled loops as an external scheduler are unverified. |
| Hosted GitHub controls | Workflow and policy files exist | **Post-push hosted behavior unverified** | Local YAML/config checks pass. Hosted CI, public link rendering, private vulnerability reporting, branch settings, and final repository visibility require external confirmation. |

## Latest Local Evidence

The latest default-path artifact is
`.aetherflow/runs/evidence_refresh_20260723/`, one of seven verification runs
recorded on 2026-07-23 (all on sha `0536d42`, no source changes).

| Gate | Result | Evidence and limitation |
|---|---|---|
| Build | Passed | Build tree reconfigured at the new repository path (VS 2022 x64; tests, SRT, NVENC, scene classifier ON) after the stale-cache failure documented in VERIFICATION_HISTORY. |
| Unit tests | Passed | CTest 4/4 on current HEAD. |
| Default smoke and trace | Passed with warning | 900/900 encoded, 0 encode failures, 0 parse errors; near-static WGC retry ratio 8.979 is warn-only. Companion run `capture_motion_20260723` with moving content: 0/300 retries. |
| Benchmark | Passed | ROI benchmark exit 0 via `agent_verify.py --run-benchmark`. |
| Masks, SRT, classifier, package | Passed | `mask_sweep_*_20260723` (three modes, 120/120 each), `line_window_mask_live_20260723` (LINE 199/200), `srt_loopback_refresh_20260723` (90 frames decoded), `scene_classifier_onnx_20260723` (p95 13.235 ms, 4/4 gates), and the `AetherFlow-portable-20260723.zip` staged self-tests. |
| Independent review | Not required | Verification-only refresh; no source diff to review. |

Canonical files:

- Run manifest: `.aetherflow/runs/evidence_refresh_20260723/run_manifest.json`
- Verification: `.aetherflow/runs/evidence_refresh_20260723/verify_report.json`
- Trace summary: `.aetherflow/runs/evidence_refresh_20260723/trace.summary.json`
- Companion runs: `.aetherflow/runs/{capture_motion,mask_sweep_blackout,mask_sweep_blur,mask_sweep_mosaic,line_window_mask_live,srt_loopback_refresh,scene_classifier_onnx}_20260723/`

Raw run bundles are intentionally ignored because traces and recordings can
contain captured-screen-sensitive data. Public documentation records only the
claim boundary and the minimum evidence needed to reproduce it locally.

## Open Verification and Release Gates

The complete QA owner is
[TROUBLESHOOTING_QA.md](../4-qa-debugging/TROUBLESHOOTING_QA.md). The table below
is the release-level summary.

| Priority | Open gate | Required evidence or decision |
|---|---|---|
| P0 | Repository publication | Confirm public visibility, hosted CI, rendered links, branch/release settings, and private vulnerability reporting after the final push. |
| P0 | Release identity | Choose a version, create a tag only after regenerating current binaries, record checksums, and decide signing and support scope. |
| P0 | Current package | Rebuilt from HEAD 2026-07-23 with staged checks passed and SHA256 recorded. Remaining: exercise `--with-model`, document SmartScreen/signing behavior, and attach the zip to a tagged release. |
| P0 | Capture reliability | Moving-content evidence recorded 2026-07-23 (0/300 retries vs 8081/900 on a static desktop). Remaining: a longer soak and the decision whether a content-aware retry ratio becomes a hard verifier gate. |
| P0 | Privacy claim coverage | LINE live artifact recorded 2026-07-23. Still required: real Slack/Discord/Telegram/WhatsApp artifacts and a broader password-control matrix. Do not generalize fixtures into universal coverage. |
| P0 | Visual proof | Per-mode trace + mp4 artifacts recorded 2026-07-23 (`output/mask_sweep_<mode>.mp4`). Remaining: uninterrupted human source-versus-output confirmation and the internal grayscale demo effect. |
| P1 | Intel parity | Run build, capture, encode, trace, and SRT on a supported Intel GPU/driver. The current verification host (GTX 1660 + AMD iGPU) has no Intel GPU, so this requires external hardware. |
| P1 | SRT field conditions | Test phone/VLC over LAN, firewall onboarding, viewer reconnect, packet loss, and non-local latency. |
| P1 | Studio interaction | Exercise panic while running, resize, stop/exit races, persisted-setting lockout, and invalid/corrupt model UX. |
| P1 | Scene-classifier quality | Run the planned real-screen sweep, label outcomes, measure class quality, and make the Stage A-to-B decision. |
| P1 | Product policy wiring | Decide whether and how advisory scene policy may influence encoder settings. The current demo action is not product behavior. |
| P1 | macOS current-tree parity | Build and run on macOS; close capture-timestamp diagnostic parity and mask hard-failure semantic parity. |
| P1 | ROI benchmark governance | Decide whether adverse quality-direction markers are hard failures or report-only. Current pass/fail follows process exit. |
| P1 | Mask GPU cost | Add GPU timestamping (`gpuMaskMs`) before claiming the asynchronous shader stage meets the complete under-4-ms cost gate. |
| P2 | Encoder concurrency | Investigate `maxInflightSlotsObserved=1`, high lock-busy counts, periodic submit/output age spikes, and the historical encoded-frame off-by-one observation. |
| P2 | Agent native-host proof | Exercise the same delegated task in GitHub, Claude, and Codex; separately install and observe scheduled loops before calling them automated. |

## Next Work

1. Record the real-time masking demo without broadening product claims.
2. Make the release decision: the 2026-07-23 portable zip and its SHA256 are
   release-candidate ready; choose a version/tag and a signing stance.
3. Close the remaining P0 gates (human visual confirmation, remaining messenger
   artifacts, publication checks) before announcing a formal release.

## Operational Entry Points

- Setup and quick start: [README.md](../../README.md)
- Full operation guide: [OPERATION_GUIDE.md](../OPERATION_GUIDE.md)
- Product architecture: [ARCHITECTURE.md](../3-product/ARCHITECTURE.md)
- Component ownership: [COMPONENT_INDEX.md](../../protocol/COMPONENT_INDEX.md)
- Agent workflow: [AGENT_ARCHITECTURE.md](../2-agent-system/AGENT_ARCHITECTURE.md)
- QA and unverified coverage:
  [TROUBLESHOOTING_QA.md](../4-qa-debugging/TROUBLESHOOTING_QA.md)
- Agent-discovered issues and review evidence:
  [AGENT_EFFECTIVENESS_LOG.md](../2-agent-system/AGENT_EFFECTIVENESS_LOG.md)
- Dated evidence: [VERIFICATION_HISTORY.md](../4-qa-debugging/VERIFICATION_HISTORY.md)
