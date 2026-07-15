# Vendored FFmpeg shared SDK (SRT/MPEG-TS live output stage)

The `include/`, `lib/`, and `bin/` subdirectories here are **gitignored** —
they hold a prebuilt FFmpeg shared SDK that AetherFlow's optional SRT output
stage (`src/streaming/SrtStreamOutput.cpp`) links against. Same vendoring
model as `third_party/onnxruntime/`: binaries never enter git history;
recreate them with the SOP below.

## SOP — recreate this directory

```bash
python tools/fetch_ffmpeg.py
```

That script downloads the **pinned** BtbN autobuild asset, verifies its
SHA256, extracts only the needed subset, and confirms the `srt` protocol is
compiled in. Provenance of the currently extracted copy is recorded in
`VERSION.json` (gitignored, regenerated on every fetch).

Current pin (also hardcoded in `tools/fetch_ffmpeg.py`):

| Field | Value |
|---|---|
| Release tag | `autobuild-2026-07-01-13-54` |
| Asset | `ffmpeg-n8.1.2-21-gce3c09c101-win64-lgpl-shared-8.1.zip` |
| SHA256 | `c45ddc95e9c8c7df9a4a79dced901de7eac503e446334937252988c8e5d5e99b` |
| FFmpeg version | n8.1.2 (release/8.1 branch) |
| License of the binaries | **LGPL 2.1+** (BtbN "lgpl-shared" variant) |

## What gets extracted

- `include/` — `libavformat/`, `libavcodec/`, `libavutil/`, `libswresample/`
  headers (what `SrtStreamOutput.cpp` compiles against).
- `lib/` — `avformat.lib`, `avcodec.lib`, `avutil.lib`, `swresample.lib`
  import libraries (what `AetherFlow.exe` links).
- `bin/` — all runtime DLLs from the build, plus `ffmpeg.exe` / `ffprobe.exe`.
  `AetherFlow.exe` itself loads only `avformat-*.dll`, `avcodec-*.dll`,
  `avutil-*.dll`, `swresample-*.dll` (CMake copies exactly those next to the
  exe post-build). The remaining DLLs (`avfilter`, `avdevice`, `swscale`)
  exist only so the bundled `ffmpeg.exe`/`ffprobe.exe` — the pinned SRT test
  caller used by the verification SOP — can start.

## Why the LGPL variant

- It **does** enable the `srt` protocol (verified 2026-07-02 by running
  `bin/ffmpeg.exe -protocols` on this exact asset — libsrt is MPL-2.0, so
  BtbN includes it in LGPL builds).
- Dynamic linking against LGPL libavformat leaves AetherFlow's own license
  unaffected. The GPL variant also carries srt but would GPL-encumber any
  distributed bundle for no benefit.

## License obligations when redistributing (Delta C packaging)

If a portable zip / installer ships these DLLs, LGPL 2.1 requires: keep the
DLLs as separate dynamically-linked files (already the case), include the
FFmpeg license text, and state where the corresponding source can be obtained
(https://github.com/BtbN/FFmpeg-Builds — each autobuild links the exact
source snapshot).

## CMake integration

`CMakeLists.txt` auto-detects `third_party/ffmpeg/include/libavformat/avformat.h`
+ `lib/avformat.lib`. Present → compiles `src/streaming/SrtStreamOutput.cpp`
with `AETHERFLOW_ENABLE_SRT_OUTPUT=1` and copies the four runtime DLLs next to
`AetherFlow.exe`. Absent (e.g. CI) → the SRT stage is compiled out with a
STATUS message and `--srt-output` reports "not built with SRT support" at
runtime. Reconfigure CMake after fetching.
