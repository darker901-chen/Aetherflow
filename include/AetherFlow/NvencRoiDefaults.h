#pragma once

#include "AetherFlow/Config.h"

namespace AetherFlow {

// 預設值來自 Config.h，統一由 AETHERFLOW_ROI_* 控制
constexpr int kNvencRoiDefaultRadius  = AETHERFLOW_ROI_RADIUS;
constexpr int kNvencRoiMinRadius      = 16;
constexpr int kNvencRoiMaxRadius      = 2000;

constexpr int kNvencRoiDefaultDeltaQp = AETHERFLOW_ROI_DELTA_QP;
constexpr int kNvencRoiMinDeltaQp     = -51;
constexpr int kNvencRoiMaxDeltaQp     = 0;

constexpr int kNvencBgDefaultDeltaQp  = AETHERFLOW_BG_DELTA_QP;
constexpr int kNvencBgMinDeltaQp      = 0;
constexpr int kNvencBgMaxDeltaQp      = 51;

} // namespace AetherFlow
