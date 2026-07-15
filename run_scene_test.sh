#!/usr/bin/env bash
# run_scene_test.sh — one-button scene-classifier eyeball test (bash).
#
# Build (unless --skip-build) -> run AetherFlow.exe with the ONNX scene
# classifier enabled -> print, in plain language, what the AI guessed,
# who won the scene merge, and what actually got masked on screen.
#
# Usage:
#   ./run_scene_test.sh                    # AI on GPU (DirectML), report only
#   ./run_scene_test.sh --no-ai            # NO AI at all (pure pipeline baseline)
#   ./run_scene_test.sh --ai-cpu           # AI on CPU (off the GPU) — less GPU contention
#   ./run_scene_test.sh --demo             # AI on GPU + visible per-class effect
#   ./run_scene_test.sh --frames 1800      # ~60s of content
#   ./run_scene_test.sh --skip-build       # skip cmake build
#   ./run_scene_test.sh --out-dir /d/mytest
#
# Smoothness diagnosis — run all three and compare scene_test_out/demo.mp4:
#   --no-ai   : baseline, no classifier at all (how smooth is the bare pipeline?)
#   --ai-cpu  : classifier runs on CPU, OFF the render GPU (1Hz slow path, OK to be slow)
#   (default) : classifier on GPU/DirectML — competes with capture+NVENC for the GPU
# If --no-ai and --ai-cpu are smooth but the default is janky => the jank is
# GPU contention from the CLIP model, not the readback (which rev2 fixed).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/scene_test_out"
MODEL="models/scene_classifier_v1.onnx"
FRAMES=0
SKIP_BUILD=0
DEMO=0
NO_AI=0
AI_CPU=0

while [ $# -gt 0 ]; do
  case "$1" in
    --out-dir)    OUT_DIR="$2"; shift 2 ;;
    --model)      MODEL="$2";   shift 2 ;;
    --frames)     FRAMES="$2";  shift 2 ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --demo)       DEMO=1;       shift ;;
    --no-ai)      NO_AI=1;      shift ;;
    --ai-cpu)     AI_CPU=1;     shift ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

EXE="$SCRIPT_DIR/build/Release/AetherFlow.exe"

if [ "$SKIP_BUILD" -eq 0 ]; then
  echo "[1/3] building (cmake --build build --config Release)..."
  cmake --build "$SCRIPT_DIR/build" --config Release --target AetherFlow
else
  echo "[1/3] skip build"
fi

[ -f "$EXE" ] || { echo "AetherFlow.exe not found at $EXE (build first)" >&2; exit 1; }
if [ "$NO_AI" -eq 0 ]; then
  [ -f "$SCRIPT_DIR/$MODEL" ] || { echo "model not found: $MODEL — run 'python tools/export_clip_zeroshot.py' first" >&2; exit 1; }
fi

mkdir -p "$OUT_DIR"
export AETHERFLOW_OUTPUT_DIR="$OUT_DIR"
[ "$FRAMES" -gt 0 ] && export AETHERFLOW_MAX_FRAMES="$FRAMES"

# Demo path: record real per-frame capture timestamps so the mux below can
# honor true capture timing instead of forcing a fake uniform 30fps. This is
# the eyeball/demo script only; the canonical verify harness (tools/agent_*)
# does NOT set this, so its bitstream stays byte-stable.
export AETHERFLOW_TIMED_RECORDING=1

# This script drives EVERYTHING via explicit CLI args. Wipe any inherited
# AETHERFLOW_SCENE_CLASSIFIER_* env vars so a lingering shell export can NOT
# secretly turn the classifier on during --no-ai (or override the provider).
unset AETHERFLOW_SCENE_CLASSIFIER_ONNX_MODEL
unset AETHERFLOW_SCENE_CLASSIFIER_DEMO_ACTION
unset AETHERFLOW_SCENE_CLASSIFIER_PROVIDER
echo "      (cleared inherited AETHERFLOW_SCENE_CLASSIFIER_* env so flags are authoritative)"

# clean stale outputs so a failed/aborted run can't leave an old mp4 behind
rm -f "$OUT_DIR/output_encoded.h264" "$OUT_DIR/demo.mp4"

EXE_ARGS=()
if [ "$NO_AI" -eq 1 ]; then
  echo "[2/3] running AetherFlow.exe (NO AI — pure pipeline baseline)... -> $OUT_DIR"
  [ "$DEMO" -eq 1 ] && echo "      (note: --demo ignored, needs the classifier)"
else
  EXE_ARGS+=( "--scene-classifier-onnx-model=$MODEL" )
  if [ "$AI_CPU" -eq 1 ]; then
    EXE_ARGS+=( "--scene-classifier-provider=cpu" )
    PROV="CPU (off the render GPU)"
  else
    PROV="GPU/DirectML (shares the GPU with capture+NVENC)"
  fi
  if [ "$DEMO" -eq 1 ]; then
    EXE_ARGS+=( "--scene-classifier-demo-action" )
    echo "[2/3] running AetherFlow.exe (classifier ON [$PROV] + VISIBLE DEMO effect)... -> $OUT_DIR"
  else
    echo "[2/3] running AetherFlow.exe (classifier ON [$PROV], report only)... -> $OUT_DIR"
  fi
fi
echo "      arrange your screen NOW (code editor / video / slides / LINE window / etc.)"
if [ "${#EXE_ARGS[@]}" -gt 0 ]; then
  "$EXE" "${EXE_ARGS[@]}" || echo "exe exit $? (continuing to report)"
else
  "$EXE" || echo "exe exit $? (continuing to report)"
fi

TRACE="$OUT_DIR/traces/frame_trace.jsonl"
[ -f "$TRACE" ] || { echo "no trace at $TRACE" >&2; exit 1; }

echo "[3/3] AI scene report"
python - "$TRACE" <<'PY'
import json, sys, collections
path = sys.argv[1]
PLAIN = {
    "code_text":         "在寫程式 / 看文字",
    "slides":            "簡報投影片",
    "video":             "影片",
    "mixed_ui":          "一般網頁 / 桌面",
    "sensitive_surface": "像通知 / 聊天的敏感畫面",
}
ai = collections.Counter(); won = collections.Counter(); masked = collections.Counter()
confs = []; n = 0
for line in open(path, encoding="utf-8"):
    line = line.strip()
    if not line: continue
    d = json.loads(line); n += 1
    sc = d.get("sceneClass")
    if sc:
        ai[sc] += 1
        c = d.get("sceneClassConfidence")
        if isinstance(c, (int, float)): confs.append(c)
    won[d.get("sceneSource", "-")] += 1
    masked[d.get("privacyMaskSource", "none")] += 1

secs = round(n / 30)
ai_on = bool(ai) or won.get("scene-classifier-onnx", 0) > 0
print()
print("=" * 60)
if ai_on:
    print(f"  >>> AI 這次【確實有在跑】(classifier 貢獻了 {won.get('scene-classifier-onnx',0)} 幀)")
else:
    print(f"  >>> AI 這次【完全沒跑】(真正的無-AI 基準 — sceneSource 全是 baseline/deterministic)")
print("=" * 60)
print(f"  AI 看了你螢幕約 {secs} 秒（{n} 幀），它覺得你在做：")
print("=" * 60)
if ai:
    for cls, cnt in ai.most_common():
        pct = round(100 * cnt / n)
        bar = "#" * (pct // 3)
        print(f"   {pct:3d}%  {PLAIN.get(cls, cls):<22} {bar}")
else:
    print("   (AI 完全沒貢獻 — 是不是沒加 --scene-classifier-onnx-model 旗標?)")
if confs:
    confs.sort()
    med = confs[len(confs)//2]
    sure = "通常蠻有把握" if med >= 0.75 else ("普通" if med >= 0.6 else "常常不太確定")
    print(f"\n  把握程度：{sure}（最低 {confs[0]:.0%}、中間 {med:.0%}、最高 {confs[-1]:.0%}）")
mask_real = sum(v for k, v in masked.items() if k and k != "none")
if mask_real == 0:
    print(f"  畫面遮罩：這次完全沒有東西被模糊（整段沒抓到 LINE/Teams 等視窗）")
else:
    print(f"  畫面遮罩：{mask_real} 幀有東西被模糊（既有遮罩，與 AI 無關）")
print()
print("  >>> 判斷 AI 準不準：想一下剛剛那 %d 秒你螢幕『實際』是什麼，" % secs)
print("      跟上面百分比對得起來就算準。AI 是觀察模式，不會改畫面。")
print()
print(f"  [raw] guessed={dict(ai)} | won={dict(won)} | masked={dict(masked)}")
PY

# auto-mux raw bitstream -> playable mp4.
#   PTS-honoring path: if the timed-recording sidecar exists AND mkvmerge is
#   available, mux with the REAL captured timestamps (no fake 30fps). This is
#   the structural fix for the recorded-video judder.
#   Fallback: old `-r 30 -c copy` (only correct if capture was perfectly
#   uniform — kept so environments without mkvmerge still get a file).
H264="$OUT_DIR/output_encoded.h264"
SIDECAR="$OUT_DIR/output_encoded.timestamps.txt"
MP4="$OUT_DIR/demo.mp4"
MKV="$OUT_DIR/demo.mkv"
if [ -f "$H264" ] && [ -f "$SIDECAR" ] && command -v mkvmerge >/dev/null 2>&1; then
  rm -f "$MP4" "$MKV"
  if mkvmerge -q -o "$MKV" --timestamps "0:$SIDECAR" "$H264" >/dev/null 2>&1; then
    echo ""
    echo "video (REAL per-frame PTS, true capture timing): $MKV"
    if command -v ffmpeg >/dev/null 2>&1 && \
       ffmpeg -hide_banner -loglevel error -y -i "$MKV" -c copy "$MP4"; then
      echo "                              also remuxed to: $MP4"
    fi
    DUR=$(ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "$MKV" 2>/dev/null || echo "?")
    echo "  (mkv duration ${DUR}s should ≈ real wall-clock capture span, NOT frames/30)"
  else
    echo "[mux] mkvmerge failed; falling back to fixed-30fps mux" >&2
    ffmpeg -hide_banner -loglevel error -y -r 30 -i "$H264" -c copy "$MP4" && \
      echo "video (FALLBACK fixed 30fps — timing NOT honored): $MP4"
  fi
elif [ -f "$H264" ] && command -v ffmpeg >/dev/null 2>&1; then
  rm -f "$MP4"
  MUX_RATE=30
  RATE_NOTE="fixed 30fps (no sidecar — timing NOT honored)"
  if [ -f "$SIDECAR" ]; then
    # ffmpeg alone cannot inject per-frame PTS into a raw H.264 stream, but we
    # can recover the REAL average capture fps from the sidecar (true
    # timestamps) and mux at that rate. This removes the dominant "assumed 30
    # but really X fps" judder; install mkvmerge for perfect per-frame timing.
    AVG=$(awk '!/^#/ && NF { if (n==0) first=$1; last=$1; n++ }
               END { if (n>1 && last>first) printf "%.4f", (n-1)*1000.0/(last-first) }' "$SIDECAR")
    if [ -n "$AVG" ]; then
      MUX_RATE="$AVG"
      RATE_NOTE="real average ${AVG}fps from capture timestamps (per-frame jitter still uniformized — install mkvmerge for exact PTS)"
    else
      echo "[mux] sidecar present but unreadable; using fixed 30fps" >&2
    fi
  fi
  if ffmpeg -hide_banner -loglevel error -y -r "$MUX_RATE" -i "$H264" -c copy "$MP4"; then
    echo ""
    if [ "$DEMO" -eq 1 ]; then
      echo "WATCH THIS (per-class effect baked in): $MP4"
      echo "  [$RATE_NOTE]"
    else
      echo "video ($RATE_NOTE): $MP4"
    fi
  fi
elif [ -f "$H264" ]; then
  echo ""
  echo "raw bitstream: $H264 (ffmpeg not on PATH, no mp4)"
fi
echo "raw trace + bitstream under: $OUT_DIR"
