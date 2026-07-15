#!/usr/bin/env bash
# run_full_test.sh — one command: ALL shippable features ON, then verify.
#
# Runs build/Release/AetherFlow.exe with every REAL product feature enabled,
# then runs the verifier so you also get the audit trail (capture-failure
# warning + ledger.jsonl + evidence_log.md) — without typing python yourself.
#
# Features turned on (read from src/main.cpp, not guessed):
#   --password-field-mask                deterministic password-field mask
#   --notification-mask                  deterministic LINE/Teams/etc window mask
#   --privacy-mask-mode=blur             mask visual = blur (default is blackout)
#   --scene-classifier-onnx-model=...    the ONNX AI scene classifier
#   --scene-classifier-provider=directml AI on the GPU (use --ai-cpu for CPU)
#   --scene-classifier-demo-action       make the AI verdict visible on screen
#   (the right-Ctrl panic hotkey is on by default; press it to test panic mask)
#
# Deliberately NOT turned on, and why:
#   --panic-mask        startup full-screen blackout — would hide everything else
#   --mock-analyzer     fake analyzer; mutually exclusive with the real classifier
#                       (src/main.cpp: if both given, the real classifier wins)
#
# Usage:
#   ./run_full_test.sh                 # all features (AI on GPU): build + run + verify
#   ./run_full_test.sh --skip-build    # skip the cmake build
#   ./run_full_test.sh --ai-cpu        # run the AI classifier on CPU, not GPU
#   ./run_full_test.sh --seconds 60    # 跑 60 秒(預設 ~30 秒)
#   ./run_full_test.sh --minutes 3     # 跑 3 分鐘
#   ./run_full_test.sh --frames 1800   # 或直接給幀數(幀 = 秒 × 30)
#   ./run_full_test.sh --run-id myrun  # custom run id (default: full_feature_test)
#
# How this differs from the other runners:
#   demo.sh           = product only, deterministic masks, NO AI classifier.
#   run_scene_test.sh = AI classifier eyeball report, NO verify audit trail.
#   run_full_test.sh  = ALL features + the verify gates / ledger / evidence_log.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RUN_ID="full_feature_test"
MODEL="models/scene_classifier_v1.onnx"
PROVIDER="directml"
FRAMES=0
SKIP_BUILD=0

while [ $# -gt 0 ]; do
  case "$1" in
    --run-id)     RUN_ID="$2"; shift 2 ;;
    --frames)     FRAMES="$2"; shift 2 ;;
    --seconds)    FRAMES=$(( $2 * 30 )); shift 2 ;;   # 用秒指定長度(×30fps)
    --minutes)    FRAMES=$(( $2 * 60 * 30 )); shift 2 ;;
    --ai-cpu)     PROVIDER="cpu"; shift ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    -h|--help)    sed -n '2,45p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1 (try --help)" >&2; exit 2 ;;
  esac
done

RUN_DIR=".aetherflow/runs/$RUN_ID"

# All real product features. Deterministic masks default to ON, but we pass
# them explicitly so a lingering AETHERFLOW_*=0 env override can't silently
# disable them.
FEATURES=( --password-field-mask --notification-mask --privacy-mask-mode=blur )
if [ -f "$MODEL" ]; then
  FEATURES+=( "--scene-classifier-onnx-model=$MODEL" \
              "--scene-classifier-provider=$PROVIDER" \
              --scene-classifier-demo-action )
else
  echo "[full-test] $MODEL missing — AI classifier + demo skipped; deterministic masks still on."
  echo "[full-test] (recreate the model via: python tools/export_clip_zeroshot.py)"
fi

[ "$FRAMES" -gt 0 ] && export AETHERFLOW_MAX_FRAMES="$FRAMES"

echo "[full-test] features: ${FEATURES[*]}"
echo "[full-test] arrange your screen NOW: code editor / a video / slides /"
echo "[full-test] a LINE or Teams window / a password field — so every feature has something to act on."

# Run the product through the harness (it builds, runs the exe, captures
# console.log, copies frame_trace, summarizes). You don't type python; this does.
RUN_ARGS=( tools/agent_run.py --run-id "$RUN_ID" )
[ "$SKIP_BUILD" -eq 1 ] && RUN_ARGS+=( --skip-build )
python "${RUN_ARGS[@]}" -- "${FEATURES[@]}"

# Verify: runs the gates, writes ledger.jsonl + evidence_log.md.
python tools/agent_verify.py --run-dir "$RUN_DIR"

# Put the playable video where you expect it: output/demo.mp4 (not buried deep
# under the run dir). Mirrors demo.sh.
OUT_DIR="$SCRIPT_DIR/output"
mkdir -p "$OUT_DIR"
H264="$RUN_DIR/artifacts/output/output_encoded.h264"
MP4="$OUT_DIR/demo.mp4"
VIDEO_MSG="這次沒產生影片(看 $RUN_DIR/console.log)"
if [ -f "$H264" ]; then
  if command -v ffmpeg >/dev/null 2>&1; then
    rm -f "$MP4"
    if ffmpeg -hide_banner -loglevel error -fflags +genpts -r 30 -i "$H264" -c copy -movflags +faststart "$MP4"; then
      VIDEO_MSG="影片: $MP4"
    else
      VIDEO_MSG="ffmpeg mux 失敗;原始 bitstream: $H264"
    fi
  else
    cp -f "$H264" "$OUT_DIR/demo.h264"
    VIDEO_MSG="ffmpeg 不在 PATH;原始檔複製到 $OUT_DIR/demo.h264(裝 ffmpeg 才能轉 mp4)"
  fi
fi

# Scene-detection log NEXT TO the video so you can watch + compare side by side.
# Reads the run's frame_trace.jsonl (no runtime change); times use 30fps so they
# line up with demo.mp4 (also muxed at 30fps).
TRACE="$RUN_DIR/frame_trace.jsonl"
[ -f "$TRACE" ] || TRACE="$RUN_DIR/artifacts/output/traces/frame_trace.jsonl"
SCENE_LOG="$OUT_DIR/scene_log.txt"
SCENE_MSG="(找不到 trace,沒產生場景紀錄)"
if [ -f "$TRACE" ]; then
  python - "$TRACE" "$SCENE_LOG" <<'PY'
import json, sys, collections
trace, out = sys.argv[1], sys.argv[2]
PLAIN = {
  "code_text": "寫程式/看文字", "slides": "簡報", "video": "影片",
  "mixed_ui": "一般桌面/網頁", "sensitive_surface": "敏感畫面(通知/聊天)",
}
# 偵測到該場景時,--scene-classifier-demo-action 會對畫面做的處理
# (對應 SceneDemoActionModule + main.cpp Stage 3 的 mapping)。
EFFECT = {
  "sensitive_surface": "塗黑", "video": "馬賽克", "code_text": "模糊",
  "slides": "灰階", "mixed_ui": "不處理(桌面保持乾淨)",
}
rows = [json.loads(l) for l in open(trace, encoding="utf-8") if l.strip()]
n = len(rows)
ai = collections.Counter()
for d in rows:
    sc = d.get("sceneClass")
    if sc:
        ai[sc] += 1
out_lines = []
out_lines.append("AetherFlow 場景偵測紀錄  (對著同資料夾的 demo.mp4 看;時間用 30fps 換算,對得上)")
out_lines.append(f"總幀數 {n} (約 {round(n/30)} 秒)")
out_lines.append("")
out_lines.append("== 場景變化時間軸 (偵測到新場景才印一行;→ 後面是 demo 會做的處理) ==")
prev = None
for d in rows:
    sc = d.get("sceneClass")
    if sc is None:
        continue
    if sc != prev:
        t = d.get("frameIndex", 0) / 30.0
        c = d.get("sceneClassConfidence")
        cs = f"{c:.0%}" if isinstance(c, (int, float)) else "?"
        eff = EFFECT.get(sc, "不處理")
        out_lines.append(f"  {t:6.1f}s  {PLAIN.get(sc, sc):<18} 把握 {cs:>4}  → {eff}")
        prev = sc
out_lines.append("")
out_lines.append("== 整段分佈 (→ 後面是 demo 會做的處理) ==")
for cls, cnt in ai.most_common():
    out_lines.append(f"  {round(100*cnt/n):3d}%  {PLAIN.get(cls, cls):<18} → {EFFECT.get(cls, '不處理')}")
out_lines.append("")
out_lines.append("註:加 --scene-classifier-demo-action(run_full_test 預設有加)時,上面的「→ 處理」")
out_lines.append("    會真的套到畫面上;不加就只是觀察、不動畫面。要判斷 AI 準不準,回想那段時間")
out_lines.append("    螢幕實際在幹嘛,跟它猜的場景對照。")
open(out, "w", encoding="utf-8").write("\n".join(out_lines) + "\n")
PY
  SCENE_MSG="場景紀錄: $SCENE_LOG"
else
  echo "[full-test] 找不到 trace,略過場景紀錄: $TRACE"
fi

# Plain-language summary of THIS run only (NOT the whole history — dumping the
# accumulated log was confusing because it showed unrelated older runs).
echo
echo "========== 這次跑的結果 =========="
python - "$RUN_DIR/verify_report.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1], encoding="utf-8"))
print("整體:", "通過" if d.get("status") == "passed" else "失敗")
sc = d.get("scene_classifier", {})
st = sc.get("status")
if st == "passed":
    n = (sc.get("metrics") or {}).get("scene_inference_count", "?")
    print(f"AI 場景分類器: 有在跑(推論 {n} 次)。只證明 AI 看到畫面、輸出有變化 —— 不證明猜得準。")
elif st == "not_applicable":
    print("AI 場景分類器: 這次沒開。")
else:
    print("AI 場景分類器: 失敗 —", sc.get("summary", ""))
w = d.get("warnings") or []
if not w:
    print("警告: 沒有 —— 擷取正常,這次沒抓到問題。")
else:
    print(f"警告: {len(w)} 個 —")
    for x in w:
        print("  -", x.get("summary"))
PY

echo
echo "$VIDEO_MSG"
echo "$SCENE_MSG"
echo
echo "（output/ 同一個資料夾:demo.mp4 是影片、scene_log.txt 是 AI 偵測到的場景時間軸,時間對得上,可邊看邊比。"
echo "  歷史警告在 .aetherflow/audit/evidence_log.md;想看 AI 白話報告:./run_scene_test.sh --demo）"
