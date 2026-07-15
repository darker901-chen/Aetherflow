#!/bin/bash

ENGINE_PATH="./build/Release/AetherFlow_Engine.exe"
BASELINE_FILE="baseline.mp4"
AETHER_FILE="aetherflow_test.mp4"

if [ ! -f "$ENGINE_PATH" ]; then
    echo "[錯誤] 找不到程式！"
    exit 1
fi

echo "========================================================"
echo "   AetherFlow 產品原型驗證 (聚光燈模式)"
echo "========================================================"

# --- 1. 錄製對照組 ---
echo ""
echo "[1/2] 錄製【對照組】(15秒)..."
echo ">>> 請播放影片或大幅移動視窗，製造高流量場景 <<<"
sleep 2

ffmpeg -hide_banner -loglevel error -f gdigrab -framerate 30 -t 15 -i desktop \
    -vf "scale=1920:1080,pad=1920:1088:0:0,format=nv12" \
    -c:v h264_qsv -global_quality 25 -look_ahead 0 -y $BASELINE_FILE
echo "      -> 完成 (Baseline)"

# --- 2. 錄製實驗組 ---
echo ""
echo "[2/2] 錄製【AetherFlow 聚光燈】(15秒)..."
echo ">>> 請保持同樣的影片播放，並移動滑鼠展示聚光燈效果 <<<"
sleep 2

ffmpeg -hide_banner -loglevel error -f gdigrab -framerate 30 -t 15 -i desktop \
    -vf "scale=1920:1080,pad=1920:1088:0:0,format=nv12" -f rawvideo - | \
    $ENGINE_PATH | \
    ffmpeg -hide_banner -loglevel error -f rawvideo -pixel_format nv12 -video_size 1920x1088 -framerate 30 \
    -i - -c:v h264_qsv -global_quality 25 -look_ahead 0 -y $AETHER_FILE
echo "      -> 完成 (AetherFlow)"

# --- 3. 結果計算 ---
if [ -s "$BASELINE_FILE" ] && [ -s "$AETHER_FILE" ]; then
    SIZE_BASE=$(stat -c%s "$BASELINE_FILE")
    SIZE_AETHER=$(stat -c%s "$AETHER_FILE")
    DIFF=$((SIZE_BASE - SIZE_AETHER))
    
    if [ $SIZE_BASE -eq 0 ]; then SIZE_BASE=1; fi
    SAVED=$(( (DIFF * 100) / SIZE_BASE ))
    
    echo ""
    echo "========= 產品驗證戰報 ========="
    echo "原始流量 : $(($SIZE_BASE/1024)) KB"
    echo "優化流量 : $(($SIZE_AETHER/1024)) KB"
    echo "頻寬節省 : $SAVED %"
    echo "================================"
    
    if [ $SAVED -gt 20 ]; then
        echo "🎉 成功！這證明了 C++ 引擎具備強大的頻寬控制能力。"
    else
        echo "⚠️ 數據異常，請確認畫面背景是否成功變黑。"
    fi
else
    echo "[錯誤] 錄製失敗"
fi