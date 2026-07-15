#!/usr/bin/env bash
# AetherFlow demo runner — cross-platform.
#
# Windows (Git Bash): runs build/Release/AetherFlow.exe with Live Share Guard
# defaults (blur + panic hotkey + password-field + notification whitelist),
# muxes to mp4 if ffmpeg is on PATH, opens the output folder.
#
# macOS: runs the agent harness `python3 tools/agent_run.py --platform macos`,
# which builds the macOS binary, runs the ScreenCaptureKit + VideoToolbox
# pipeline through `MacosPlatformShim`, and writes `output.mp4` straight from
# AVAssetWriter (no ffmpeg needed). Output is also copied to
# `<repo>/output/demo.mp4` for convenience.
#
# Pass-through args after `--` go to the exe (Windows) or the macOS binary
# via the harness `-- <args>` form.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo_root"

# --- Detect platform -----------------------------------------------------
case "$(uname -s)" in
    Darwin) platform="macos" ;;
    Linux)  platform="linux" ;;
    *)      platform="windows" ;;  # Git Bash / MSYS / Cygwin all report MINGW/MSYS
esac

# Collect args after `--`
forward=()
dash_dash_seen=0
for a in "$@"; do
    if [[ $dash_dash_seen -eq 0 && "$a" == "--" ]]; then
        dash_dash_seen=1
        continue
    fi
    forward+=("$a")
done

output_dir="$repo_root/output"
mkdir -p "$output_dir"

# =========================================================================
# macOS branch
# =========================================================================
if [[ "$platform" == "macos" ]]; then
    if ! command -v python3 >/dev/null 2>&1; then
        echo "[demo] python3 required for macOS demo runner" >&2
        exit 1
    fi
    if ! command -v cmake >/dev/null 2>&1; then
        echo "[demo] cmake required (install via: brew install cmake)" >&2
        exit 1
    fi

    run_id="${AETHERFLOW_DEMO_RUN_ID:-demo}"
    run_dir="$repo_root/.aetherflow/runs/$run_id"

    echo "[demo] macOS phase 1: ScreenCaptureKit + VideoToolbox -> AVAssetWriter MP4"
    echo "[demo] Run id: $run_id  (artifacts under .aetherflow/runs/$run_id/)"
    echo "[demo] Prereq: System Settings -> Privacy & Security -> Screen Recording"
    echo "[demo]         must include this terminal/app, or the run will be 'unsupported'."

    # Run the agent harness. It builds, captures, encodes, writes trace + smoke
    # + verify-ready artifacts. macOS phase 1 has no panic / mask / notification
    # / privacy-mode flags — those are Windows-only for now.
    python3 tools/agent_run.py --platform macos --run-id "$run_id" ${forward[@]+"${forward[@]}"}

    smoke="$run_dir/macos_smoke.json"
    mp4_src="$run_dir/artifacts/output/output.mp4"
    mp4_dst="$output_dir/demo.mp4"

    if [[ -f "$mp4_src" && -s "$mp4_src" ]]; then
        cp -f "$mp4_src" "$mp4_dst"
        echo "[demo] MP4 ready: $mp4_dst (also at $mp4_src)"
        if [[ -f "$smoke" ]]; then
            python3 -c "import json,sys; s=json.load(open('$smoke')); print(f\"[demo] captured={s.get('captured_frames')} encoded={s.get('encoded_frames')} failures={s.get('encode_failure_frames')} duration={s.get('duration_seconds')}s permission={s.get('screen_capture_permission')}\")"
        fi
    else
        echo "[demo] No MP4 produced. Check $run_dir/console.log and $smoke."
        if [[ -f "$smoke" ]]; then
            python3 -c "import json; s=json.load(open('$smoke')); print('[demo] smoke errors:', s.get('errors'))"
        fi
    fi

    # Convenience: open the demo output folder
    if command -v open >/dev/null 2>&1; then
        open "$output_dir" || true
    fi
    exit 0
fi

# =========================================================================
# Windows (Git Bash) branch — unchanged from v0.2 demo
# =========================================================================
exe="$repo_root/build/Release/AetherFlow.exe"
if [[ ! -x "$exe" && ! -f "$exe" ]]; then
    echo "[demo] AetherFlow.exe not found at $exe"
    echo "[demo] Building Release first..."
    cmake --build build --config Release --target AetherFlow
fi

export AETHERFLOW_OUTPUT_DIR="$output_dir"

echo "[demo] Live Share Guard defaults: blur + panic hotkey + password-field + notification (LINE/Slack/Discord/Teams/Telegram/WhatsApp)"
echo "[demo] Running: $exe ${forward[*]:-}"
"$exe" ${forward[@]+"${forward[@]}"}

h264="$output_dir/output_encoded.h264"
mp4="$output_dir/demo.mp4"

if [[ -f "$h264" ]]; then
    echo "[demo] Bitstream: $h264"
    if command -v ffmpeg >/dev/null 2>&1; then
        rm -f "$mp4"
        if ffmpeg -hide_banner -loglevel error -r 30 -i "$h264" -c copy "$mp4"; then
            echo "[demo] Muxed to mp4: $mp4"
        else
            echo "[demo] ffmpeg mux failed; raw .h264 still available"
        fi
    else
        echo "[demo] ffmpeg not on PATH; skipped mp4 mux. To convert manually:"
        echo "       ffmpeg -r 30 -i \"$h264\" -c copy \"$mp4\""
    fi
else
    echo "[demo] No bitstream produced. Check console output above."
fi

if command -v explorer.exe >/dev/null 2>&1; then
    explorer.exe "$(cygpath -w "$output_dir")" || true
fi
