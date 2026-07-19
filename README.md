# AetherFlow

**Author-directed, AI-assisted engineering of a native real-time media system.**

AetherFlow is a Windows-first portfolio project for GPU screen-media pipelines.
It captures a display with Windows Graphics Capture or DXGI duplication, keeps
frames on D3D11 surfaces through masking and color conversion, makes a
scene-first frame decision, and encodes H.264 through a shared NVIDIA NVENC /
Intel oneVPL abstraction. Deterministic privacy masks are applied before encode,
optional SRT output carries the already-masked stream, and a separate local AI
slow path can observe sampled frames without blocking or controlling the default
media path. Its first product wedge is **Live Share Guard**: reducing accidental
disclosure during screen sharing without adding a network or model dependency
to the media fast path.

The project demonstrates:

- native Windows capture and D3D11 texture ownership;
- GPU-resident BGRA masking and BGRA-to-NV12 conversion;
- asynchronous hardware-encoder submission, drain, and bitstream handling;
- pre-encode, fail-closed privacy composition for password fields, selected
  chat/application windows, manual regions, and panic masking;
- optional video-only SRT/MPEG-TS output from encoded H.264 access units;
- a macOS path using ScreenCaptureKit, CoreImage/Metal, VideoToolbox, and
  AVAssetWriter; and
- reproducible trace and verification artifacts for timing, decisions, masks,
  capture behavior, and encode outcomes.

![AetherFlow Studio settings for deterministic privacy masks and SRT streaming](docs/assets/aetherflow-studio-ui.png)

*AetherFlow Studio controls the same native pipeline: deterministic UI
Automation password-field and recognized messenger-window masks run before
encode; SRT receives the masked output. The ONNX classifier is optional,
off-thread, and advisory.*

## Architecture in Brief

```text
Fast path (per frame, deterministic)
WGC / DXGI -> D3D11 BGRA -> decide -> privacy mask -> NV12 -> NVENC / oneVPL
                                                           -> optional SRT
                                                           -> frame trace

Slow path (sampled, droppable, advisory)
sampled pixels -> local ONNX analyzer -> cached scene proposal -> status / trace
```

The fast path does not wait for a model, cloud service, or LLM. Deterministic
producers decide privacy regions from local state, and the compositor transforms
those pixels before the encoder sees them. On Windows, a non-blackout mask
failure falls back to blackout; if the fail-closed path cannot complete, the run
aborts instead of silently encoding the original frame. macOS currently skips a
frame after an ultimate mask failure, so cross-platform failure semantics are
not yet identical.

The optional ONNX classifier runs at roughly 1 Hz on its own worker path. Its
result is cached for policy telemetry and Studio status; it does not steer
NVENC, oneVPL, or VideoToolbox. An explicit opt-in demo can map stable classes to
a full-screen visual effect, but that demo is not product privacy behavior.

See [the product architecture](docs/3-product/ARCHITECTURE.md) for pipeline
stages, ownership boundaries, module ordering, and backend details.

## Evidence and Current Boundaries

These are point-in-time results from named runs, not universal performance or
coverage claims. Re-run the repository gates before quoting them as current.

| Evidence | Result | Boundary |
|---|---|---|
| Windows default smoke, `public_release_hardening_20260714` | **120/120 encoded**, 0 encode failures, 0 trace parse errors | The same nearly static desktop produced 1007 WGC retries for 120 delivered frames; the verifier treats this as a warning, not clean capture-reliability proof. |
| First-party tests | **CTest 4/4** | Covers selected decision, policy, Annex-B, and settings behavior; it is not exhaustive system coverage. |
| Deterministic decision time, historical 2026-05-17 interactive run | p99 **0.17 ms**, 0% at or above 10 ms | CPU-side decision timing with both deterministic producers enabled. |
| ONNX scene inference, `scene_classifier_onnx_smoke` | p95 **15.254 ms** off-thread | Proves runtime plumbing and timing, not real-screen classification accuracy. |
| SRT loopback, `srt_output_v1` | **90 frames decoded** by a local FFmpeg client | NVENC, local loopback, video-only, single viewer; Intel hardware and real LAN/loss conditions remain unverified. |
| macOS mask stage, `mac_chat_window_mosaic_masked` | mean **5.40 ms**, p99 **7.14 ms** with 11 rectangles/frame | Historical run; `mask_ms` is CPU-side dispatch timing, not GPU-completion timing, and the current tree was not rerun on this Windows host. |

The current release boundary is deliberately narrow:

- This is a pre-release source snapshot and local pre-encode masking prototype,
  not a certified DLP product. It does not claim to detect every sensitive
  surface, application, notification, browser field, or custom control.
- The NVIDIA path is locally verified. The Intel oneVPL backend is implemented
  and build-covered, but a current supported Intel-hardware artifact is missing.
- The macOS ScreenCaptureKit / VideoToolbox path and chat-window masking have
  dated verification, but were not rerun on the current Windows host;
  secure-text detection remains a stub and ROI/QP is unsupported.
- The classifier is optional and advisory; real-screen accuracy is unmeasured.
- Blackout, blur, and mosaic are implemented product modes, but a current full
  visual sweep is still missing.
- No formal version, Git tag, or GitHub Release is claimed. The last portable
  zip predates the current hardened tree, is unsigned, and is not a current
  release artifact.
- A successful build does not prove encoder hardware/driver availability, live
  application coverage, long-duration reliability, or field-ready SRT behavior.

Raw `.aetherflow/runs/` bundles are intentionally gitignored because traces and
recordings may contain captured-screen-sensitive data. Public claim boundaries
and dated evidence remain in [Project Status](docs/1-status/PROJECT_STATUS.md)
and [Verification History](docs/4-qa-debugging/VERIFICATION_HISTORY.md).

## Quick Start on Windows

Requirements: Windows 10 1903+ or Windows 11; Visual Studio or Build Tools
2019/2022 with Desktop development with C++ and a Windows SDK; CMake 3.20+;
Python 3; and a supported Intel Gen 6 Skylake+ or NVIDIA Maxwell+ GPU.

From the repository root in PowerShell:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\tools\bootstrap_windows.ps1
.\build\Release\AetherFlowStudio.exe
```

The bootstrap builds the Studio UI, SRT support, first-party tests, and the
deterministic privacy-mask pipeline while leaving the large optional ONNX
classifier disabled. It fetches the pinned ImGui and FFmpeg/SRT packages used by
the showcased Studio path; oneVPL is vendored. NVENC still requires one NVIDIA
SDK header supplied by the user, while ONNX Runtime and the model are manual and
optional.

For prerequisites, alternate build profiles, NVENC setup, exact success
criteria, and common failures, use
[Build AetherFlow from a Fresh Windows Clone](docs/BUILD_WINDOWS.md). For flags,
environment variables, output paths, SRT setup, verification commands, portable
packaging, and macOS operation, use the
[Operation Guide](docs/OPERATION_GUIDE.md).

## Development Provenance

This project is best described as **author-directed, AI-assisted engineering**.
The author defined and directed the product problem, architecture, constraints,
acceptance criteria, verification gates, and review boundaries. AI coding agents
produced much of the implementation and documentation. The repository therefore
does not imply that the author manually wrote every line of code; the authorship
claim is technical direction, system design, evidence standards, and accountable
review of the resulting work.

The seven-role agent workflow remains part of the repository as the development
method, not the primary product achievement. It organizes planning,
implementation, artifact-based verification, independent diff review, scoped
repair, and documentation sync. See
[Agent Architecture](docs/2-agent-system/AGENT_ARCHITECTURE.md) and the cited
[Effectiveness Log](docs/2-agent-system/AGENT_EFFECTIVENESS_LOG.md).

## Documentation

- [Highlights](docs/HIGHLIGHTS.md) — 60-second technical skim.
- [Project Status](docs/1-status/PROJECT_STATUS.md) — current capability and
  verification boundaries.
- [Product Architecture](docs/3-product/ARCHITECTURE.md) — durable runtime and
  platform design.
- [Verification History](docs/4-qa-debugging/VERIFICATION_HISTORY.md) — dated
  runs and measurements.
- [QA and Troubleshooting](docs/4-qa-debugging/TROUBLESHOOTING_QA.md) — tested
  and unverified paths.
- [Documentation Index](docs/DOCUMENTATION_INDEX.md) — canonical owner for each
  topic.

Contributor and repository policy: [CONTRIBUTING.md](CONTRIBUTING.md) ·
[SECURITY.md](SECURITY.md). Agent entry contracts:
[AGENTS.md](AGENTS.md) · [CLAUDE.md](CLAUDE.md) ·
[COMPONENT_INDEX.md](protocol/COMPONENT_INDEX.md).

## License

AetherFlow's own source is released under the [MIT License](LICENSE). Bundled or
fetched dependencies retain their own terms: Intel oneVPL/libvpl is MIT; the
NVIDIA Video Codec SDK header is fetched but not redistributed under the NVIDIA
SDK License ([source policy](external/VideoCodecSDK/SOURCE.md)); and the fetched
FFmpeg shared SDK used for SRT is dynamically linked under LGPL 2.1+
([source policy](third_party/ffmpeg/SOURCE.md)).
