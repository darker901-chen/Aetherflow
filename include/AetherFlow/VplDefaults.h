#pragma once

#include "AetherFlow/Config.h"

namespace AetherFlow {

// ============================================================
//  Intel VPL (Quick Sync) ROI 預設值
//  對應 NvencRoiDefaults.h 的結構，集中管理 VPL 相關常數
// ============================================================

// ROI 半徑 (像素) — 滑鼠周圍高品質區域
// 預設值來自 Config.h，統一由 AETHERFLOW_ROI_* 控制
constexpr int kVplRoiDefaultRadius  = AETHERFLOW_ROI_RADIUS;
constexpr int kVplRoiMinRadius      = 16;
constexpr int kVplRoiMaxRadius      = 2000;

// ROI 區塊品質提升 (相對 QP 偏移，負值 = 更高品質)
// VPL 使用 DeltaQP per-ROI block，範圍 -51 ~ +51
// -5 微小 / -10 輕微 / -15 推薦 / -20 顯著 / -30 預設 / -51 極端(會閃)
constexpr int kVplRoiDefaultDeltaQp = AETHERFLOW_ROI_DELTA_QP;
constexpr int kVplRoiMinDeltaQp     = -51;
constexpr int kVplRoiMaxDeltaQp     = 0;

// 背景區域（非 ROI）品質降低偏移，0 = 不降低
constexpr int kVplBgDefaultDeltaQp  = AETHERFLOW_BG_DELTA_QP;
constexpr int kVplBgMinDeltaQp      = 0;
constexpr int kVplBgMaxDeltaQp      = 51;

} // namespace AetherFlow
