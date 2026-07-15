#pragma once

#include <windows.h>
#include <vector>
#include <cstdint>

// ROI Generator: 生成基於滑鼠位置的品質熱力圖
class ROIGenerator {
public:
    ROIGenerator(int width, int height);
    ~ROIGenerator();

    // 生成 QP Map（基於滑鼠位置）
    // 返回: QP 值陣列 (16x16 macroblock grid)
    // 滑鼠周圍 = 低 QP (高品質), 其他 = 高 QP (低品質)
    std::vector<uint8_t> GenerateQPMap();

    // 獲取 QP Map 尺寸（macroblock 數量）
    int GetMBWidth() const { return m_mbWidth; }
    int GetMBHeight() const { return m_mbHeight; }
    int GetTotalMBs() const { return m_mbWidth * m_mbHeight; }

    // 配置參數
    void SetROIRadius(int radius);
    void SetCenterQP(uint8_t qp);
    void SetBackgroundQP(uint8_t qp);
    
    // 輸出當前配置（在設置完參數後調用）
    void PrintConfig() const;

private:
    int m_width;          // 畫面寬度 (pixels)
    int m_height;         // 畫面高度 (pixels)
    int m_mbWidth;        // Macroblock 寬度數量
    int m_mbHeight;       // Macroblock 高度數量
    
    // ROI 參數
    int m_roiRadius;      // ROI 半徑 (pixels)
    uint8_t m_centerQP;   // ROI 中心 QP (低=高品質)
    uint8_t m_backgroundQP; // 背景 QP (高=低品質)

    // 獲取當前滑鼠位置 (螢幕座標)
    POINT GetMousePosition();
};
