#pragma once

// ============================================
//  AetherFlow Configuration
// ============================================
// 修改這個檔案來調整編碼參數，無需修改多個地方

// ============================================
//  編碼參數 Encoding Parameters
// ============================================
#define AETHERFLOW_WIDTH       1920      // 寬度 (Width)
#define AETHERFLOW_HEIGHT      1080      // 高度 (Height)
#define AETHERFLOW_FPS         30        // 幀率 (Frame Rate) - 試試 24, 30, 60
#define AETHERFLOW_BITRATE     1500      // 位元率 kbps (Bitrate)

// ============================================
//  進階參數 Advanced Parameters
// ============================================
#define AETHERFLOW_GOP_SECONDS 2         // GOP 長度（秒）GOP = FPS * 這個值
#define AETHERFLOW_MAX_FRAMES  900       // 最大幀數 (0 = 無限制)

// ============================================
//  ROI 參數 ROI Parameters
// ============================================
// 適用 Intel VPL 與 NVENC 兩個 backend
#define AETHERFLOW_ROI_RADIUS    200     // ROI 半徑 px（滑鼠周圍高品質區域）
#define AETHERFLOW_ROI_DELTA_QP  -30  // ROI 品質提升（負值 = 更清晰，建議 -10 ~ -40）
#define AETHERFLOW_BG_DELTA_QP   0      // 背景品質降低（正值 = 更模糊，0 = 不降低）

// 靜態 ROI 模式（用於 benchmark 重現性）
// 設成 1 後 ROI 固定在 (X,Y)，不追蹤滑鼠
// 由 roi_benchmark.ps1 -Static 自動控制，通常不需要手動修改
#define AETHERFLOW_STATIC_ROI_ENABLED  0      // 0=追蹤滑鼠, 1=固定座標
#define AETHERFLOW_STATIC_ROI_X        1280   // 固定 ROI 中心 X（僅 ENABLED=1 時有效）
#define AETHERFLOW_STATIC_ROI_Y        720    // 固定 ROI 中心 Y

// ============================================
//  性能調整 Performance Tuning
// ============================================
// 幀間延遲計算：1000ms / FPS
// 例如：30fps → 33ms, 60fps → 16ms, 24fps → 41ms
#define AETHERFLOW_FRAME_DELAY_MS  (1000 / AETHERFLOW_FPS)

// ============================================
//  測試預設 Quick Presets
// ============================================
// 取消註解想要的預設（只能選一個）

// 高幀率預設 (High FPS)
// #define PRESET_HIGH_FPS
// #ifdef PRESET_HIGH_FPS
//     #undef AETHERFLOW_FPS
//     #undef AETHERFLOW_BITRATE
//     #define AETHERFLOW_FPS 60
//     #define AETHERFLOW_BITRATE 12000
// #endif

// 電影預設 (Cinema)
// #define PRESET_CINEMA
// #ifdef PRESET_CINEMA
//     #undef AETHERFLOW_FPS
//     #undef AETHERFLOW_BITRATE
//     #define AETHERFLOW_FPS 24
//     #define AETHERFLOW_BITRATE 6000
// #endif

// 高品質預設 (High Quality)
// #define PRESET_HIGH_QUALITY
// #ifdef PRESET_HIGH_QUALITY
//     #undef AETHERFLOW_WIDTH
//     #undef AETHERFLOW_HEIGHT
//     #undef AETHERFLOW_BITRATE
//     #define AETHERFLOW_WIDTH 2560
//     #define AETHERFLOW_HEIGHT 1440
//     #define AETHERFLOW_BITRATE 16000
// #endif

// ============================================
//  除錯資訊 Debug Info
// ============================================
#define AETHERFLOW_SHOW_FPS_INFO 1       // 顯示 FPS 資訊

// ============================================
//  自動計算參數（不要修改）
// ============================================
#define AETHERFLOW_GOP_SIZE    (AETHERFLOW_FPS * AETHERFLOW_GOP_SECONDS)
