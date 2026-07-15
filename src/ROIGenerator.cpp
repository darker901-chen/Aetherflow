#include "AetherFlow/ROIGenerator.h"
#include "AetherFlow/Config.h"
#include <iostream>
#include <cmath>

ROIGenerator::ROIGenerator(int width, int height)
    : m_width(width)
    , m_height(height)
    , m_roiRadius(AETHERFLOW_ROI_RADIUS)
    , m_centerQP(10)          // ROI 中心: QP=10 (高品質)
    , m_backgroundQP(40)      // 背景: QP=40 (低品質)
{
    // 計算 macroblock 數量 (H.264 使用 16x16 MB)
    m_mbWidth = (width + 15) / 16;
    m_mbHeight = (height + 15) / 16;
    
    // 輸出移到 SetCenterQP/SetBackgroundQP 之後，確保顯示正確的值
}

ROIGenerator::~ROIGenerator() {
}

void ROIGenerator::SetROIRadius(int radius) {
    m_roiRadius = radius;
    std::cout << "[ROI] ✓ Radius set to " << radius << " pixels\n";
}

void ROIGenerator::SetCenterQP(uint8_t qp) {
    m_centerQP = qp;
    std::cout << "[ROI] ✓ Center QP set to " << (int)qp << "\n";
}

void ROIGenerator::SetBackgroundQP(uint8_t qp) {
    m_backgroundQP = qp;
    std::cout << "[ROI] ✓ Background QP set to " << (int)qp << "\n";
}

void ROIGenerator::PrintConfig() const {
    std::cout << "[ROI] ============ Configuration ============\n";
    std::cout << "[ROI] Resolution: " << m_width << "x" << m_height << "\n";
    std::cout << "[ROI] Macroblock Grid: " << m_mbWidth << "x" << m_mbHeight 
              << " (Total: " << GetTotalMBs() << ")\n";
    std::cout << "[ROI] ROI Radius: " << m_roiRadius << " pixels\n";
    std::cout << "[ROI] Center QP: " << (int)m_centerQP << " (quality)\n";
    std::cout << "[ROI] Background QP: " << (int)m_backgroundQP << " (quality)\n";
    
    // 計算預期覆蓋率
    double radiusSquared = m_roiRadius * m_roiRadius;
    double screenArea = m_width * m_height;
    double roiArea = 3.14159 * radiusSquared;  // π * r²
    double coverage = (roiArea / screenArea) * 100.0;
    std::cout << "[ROI] Expected Coverage: " << coverage << "% high quality\n";
    std::cout << "[ROI] ========================================\n";
}

POINT ROIGenerator::GetMousePosition() {
    POINT pt;
    GetCursorPos(&pt);
    return pt;
}

std::vector<uint8_t> ROIGenerator::GenerateQPMap() {
    std::vector<uint8_t> qpMap(GetTotalMBs());

    // Note: main.cpp handles static vs mouse-tracking at the call site.
    // GenerateQPMap always reads the live mouse position here; static mode
    // is achieved by the caller (main.cpp) calling EncodeFromYUVWithROI
    // with fixed coords instead of live GetCursorPos().
    POINT mouse = GetMousePosition();
    int mouseX = (mouse.x < 0) ? 0 : ((mouse.x >= m_width) ? m_width - 1 : mouse.x);
    int mouseY = (mouse.y < 0) ? 0 : ((mouse.y >= m_height) ? m_height - 1 : mouse.y);
    
    // 🔍 調試用：統計各質量區域的宏塊數量
    int highQualityCount = 0;   // QP < 20 的宏塊
    int lowQualityCount = 0;    // QP > 40 的宏塊
    
    // 遍歷每個 macroblock
    for (int mbY = 0; mbY < m_mbHeight; mbY++) {
        for (int mbX = 0; mbX < m_mbWidth; mbX++) {
            // 計算 MB 中心點座標（pixel）
            int mbCenterX = mbX * 16 + 8;
            int mbCenterY = mbY * 16 + 8;
            
            // 計算與滑鼠的距離
            int dx = mbCenterX - mouseX;
            int dy = mbCenterY - mouseY;
            double distance = std::sqrt(dx * dx + dy * dy);
            
            // 根據距離設定 QP（距離越近，QP 越低，品質越高）
            uint8_t qp;
            if (distance < m_roiRadius) {
                // 線性插值: 中心=centerQP, 邊緣=backgroundQP
                double ratio = distance / m_roiRadius;
                qp = (uint8_t)(m_centerQP + ratio * (m_backgroundQP - m_centerQP));
            } else {
                qp = m_backgroundQP;
            }
            
            // 限制 QP 範圍 (H.264: 0-51)
            qp = (qp > 51) ? 51 : qp;
            
            int mbIndex = mbY * m_mbWidth + mbX;
            qpMap[mbIndex] = qp;
            
            // 統計質量分布
            if (qp < 20) highQualityCount++;
            if (qp > 40) lowQualityCount++;
        }
    }
    
    // 🔍 每 120 幀輸出一次統計 (避免刷屏)
    static int debugFrameCount = 0;
    if (debugFrameCount++ % 120 == 0) {
        std::cout << "[ROI Debug] Mouse=(" << mouseX << "," << mouseY << ") "
                  << "HighQ=" << highQualityCount << "/" << GetTotalMBs() << " ("
                  << (100.0*highQualityCount/GetTotalMBs()) << "%) "
                  << "LowQ=" << lowQualityCount << "/" << GetTotalMBs() << "\n";
    }
    
    return qpMap;
}
