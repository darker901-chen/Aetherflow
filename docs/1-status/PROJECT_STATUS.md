# AetherFlow Project Status

Last checked: 2026-07-14 +08:00. This page is the concise source of truth for
the current release boundary. Dated implementation and verification records
live in [VERIFICATION_HISTORY.md](../4-qa-debugging/VERIFICATION_HISTORY.md).

## Release Identity

- **Release stage:** pre-release source snapshot on `master`.
- **Formal version:** none. No Git tag or GitHub Release is claimed.
- **Roadmap labels:** `v0.1`, `v0.2`, and `v0.3` are capability milestones,
  not published semantic versions.
- **Binary status:** the last portable zip was staged and tested on 2026-07-03.
  It predates the current documentation and repository-hardening commits, has
  not been rebuilt from current HEAD, is unsigned, and is not a current release
  artifact. No installer or code-signing gate has been completed.
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
| Windows capture -> decision -> encode -> trace | Implemented | **Verified locally** | `public_release_hardening_20260714`: 120/120 encoded, 0 encode failures, 0 trace parse errors. A nearly static desktop also produced 1007 WGC retries for 120 delivered frames (ratio 8.392); the current verifier treats this as a warning, so this is not clean capture-reliability proof. |
| Deterministic password-field masking | Implemented and enabled by default | **Fixture-verified / real-application coverage partial** | Masks foreground UI Automation `IsPassword=true` Edit controls. The producer scans on a background poll thread and the frame path reads a cached snapshot. Browser and custom-control coverage is not comprehensive. |
| Deterministic notification/window masking | Implemented and enabled by default | **Fixture + Teams alias verified** | The default whitelist covers LINE, Slack, Discord, Teams, Telegram, and WhatsApp executable identities. `teams_notification_alias_fix` proves Teams alias/config/window identity, not every Teams notification form. Canonical live artifacts are missing for the other listed applications. |
| Windows mask visuals | Product modes: `blackout`, `blur`, `mosaic`; Windows default `blur`, fail-closed fallback `blackout` | **Partially visually verified** | Internal scene-demo logic can also choose `grayscale` for slides, but `grayscale` is not a CLI/env product-mode value. A current full visual sweep is missing. |
| Panic mask | Implemented | **Verified** | Startup panic evidence applied a full-frame mask on every frame. The Right Ctrl hotkey is a developer test affordance, not the product trigger contract. |
| NVIDIA NVENC backend | Implemented and default when available | **Verified locally on NVIDIA** | Current and historical Windows runs encoded without failures. Long-duration soak and open encoder serialization observations remain. |
| Intel oneVPL backend | Implemented and build-covered | **Current hardware evidence missing** | A real Intel-machine run must verify timestamps, frame types, and the SRT access-unit path before backend parity is claimed. |
| SRT live output | Implemented, opt-in, video-only, single viewer | **NVENC loopback verified** | A local FFmpeg client decoded 90 live frames, including mid-GOP join handling. Intel hardware, phone/VLC over LAN, firewall onboarding, reconnect behavior under real loss, and non-local latency are unverified. |
| Studio settings UI | Implemented | **Self-test verified / interaction coverage partial** | `--ui-smoke` and `--ui-selftest` passed on 2026-07-03. Panic during a live session, resize, exit while running, settings lockout, and corrupt-model error UX are not covered by deterministic gates. |
| Portable package builder | Implemented | **Historical engine-only package verified; current package absent** | The 2026-07-03 package passed staged application, UI, and SRT checks. `--with-model`, a current-HEAD rebuild, signing, installer delivery, and a clean Intel-only machine remain unverified. |
| Windows ONNX scene classifier | Implemented, opt-in, off-thread, advisory | **Runtime plumbing verified / accuracy unverified** | Real captured pixels reached ONNX Runtime through DirectML or CPU fallback and outputs varied. Real-screen accuracy is unmeasured. `PolicyDecision.encode_hint` does not steer the encoder. |
| Scene demo action | Implemented, opt-in, not product behavior | **Two of five classes evidenced** | Mapping is `sensitive_surface -> blackout`, `video -> mosaic`, `code_text -> blur`, `slides -> grayscale`, `mixed_ui/unknown -> passthrough`. The complete five-class visual sweep is missing. |
| macOS capture, encode, MP4, and chat-window masking | Implemented for phase 1/2 | **Historically verified; not rerun on the current Windows host** | ScreenCaptureKit, VideoToolbox, AVAssetWriter, and chat-window mosaic have dated artifacts. CoreML classification is absent, secure-text detection remains a stub, and ROI/QP is unsupported. |
| Android path | Planned only | **Not implemented** | MediaProjection, MediaCodec, LiteRT, JNI, and Gradle integration are reserved architecture paths, not files on disk. |
| Agent development system | Seven protocol responsibilities; seven GitHub, seven Claude, and seven Codex adapters | **Static parity and historical workflow value verified** | Adapter filenames match and Codex TOML parses. Native execution in every host and installation of the protocol's scheduled loops as an external scheduler are unverified. |
| Hosted GitHub controls | Workflow and policy files exist | **Post-push hosted behavior unverified** | Local YAML/config checks pass. Hosted CI, public link rendering, private vulnerability reporting, branch settings, and final repository visibility require external confirmation. |

## Latest Local Evidence

The latest default-path artifact is
`.aetherflow/runs/public_release_hardening_20260714/`.

| Gate | Result | Evidence and limitation |
|---|---|---|
| Build | Passed | `AetherFlow`, `AetherFlowStudio`, and four first-party test executables built with MSVC `/utf-8`. |
| Unit tests | Passed | CTest 4/4. |
| Default smoke and trace | Passed with warning | 120/120 encoded, 0 encode failures, 0 parse errors; WGC retries 1007/120 (ratio 8.392) are warn-only. |
| Analyzer, masks, SRT, Intel, macOS | Not exercised by this run | These paths are covered only by their separately dated artifacts and the limitations in the capability matrix. |
| Benchmark | Not requested | The public-hardening and subsequent documentation changes did not alter runtime, encoder, ROI, QP, or mask behavior. |
| Independent review | Passed after one repair | The public-hardening review closed 1 blocker and 2 risks; delta review ended at 0 blockers / 0 risks / 0 nits. |

Canonical files:

- Run manifest: `.aetherflow/runs/public_release_hardening_20260714/run_manifest.json`
- Verification: `.aetherflow/runs/public_release_hardening_20260714/verify_report.json`
- Trace summary: `.aetherflow/runs/public_release_hardening_20260714/trace.summary.json`
- Review: `.aetherflow/runs/public_release_hardening_20260714/code_review.md`

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
| P0 | Current package | Rebuild from current HEAD; run staged application/UI/SRT checks; exercise `--with-model`; document SmartScreen/signing behavior. |
| P0 | Capture reliability | Run live moving content for a justified duration and decide whether capture-retry ratio should become a hard verifier gate. |
| P0 | Privacy claim coverage | Record real application artifacts for LINE/Slack/Discord/Telegram/WhatsApp and a broader password-control matrix. Do not generalize fixtures into universal coverage. |
| P0 | Visual proof | Record an uninterrupted real-time source-versus-output sweep for blackout, blur, mosaic, and the internal grayscale demo effect. |
| P1 | Intel parity | Run build, capture, encode, trace, and SRT on a supported Intel GPU/driver. |
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
2. Make the release decision: source-only pre-release versus rebuilt binary
   release with version, checksum, and signing plan.
3. Close the P0 publication and capture/privacy evidence gates before announcing
   a formal release.

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
