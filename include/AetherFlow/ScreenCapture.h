#pragma once

#include <windows.h>
#include <cstdint>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <vector>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();

    // Deterministic monitor list for capture-source selection (UI dropdown +
    // Init's monitorIndex). Ordered primary-first, then by desktop position,
    // so index 0 is always the primary monitor regardless of the OS
    // enumeration order.
    struct MonitorInfo {
        int index = 0;
        RECT rect{};
        bool primary = false;
    };
    static std::vector<MonitorInfo> EnumerateMonitors();

    // monitorIndex selects the capture source per EnumerateMonitors() order;
    // out-of-range values clamp to 0 (primary) with a console note. Default 0
    // preserves the historical primary-monitor behavior for all existing
    // callers.
    bool Init(int width, int height, ID3D11Device* sharedDevice, int monitorIndex = 0);
    void Close();

    // Blocking: waits up to 40ms for the next WGC frame.
    // Caller owns one reference and must Release() when done.
    ID3D11Texture2D* CaptureTexture();

    // Non-blocking: returns a frame immediately if one is available, else nullptr.
    // Use to drain stale frames from the pool before a pacing sleep.
    // Caller owns one reference and must Release() when done.
    ID3D11Texture2D* TryCaptureTexture();

    // WGC FrameArrived event (auto-reset).  Signalled each time WGC delivers a
    // new frame into the pool.  Use with WaitForSingleObject() in the pacing
    // sleep to wake up the moment a fresh frame is ready rather than polling.
    HANDLE GetFrameEvent() const { return m_frameEvent; }

    // Real per-frame capture timestamp of the frame returned by the most
    // recent CaptureTexture()/TryCaptureTexture() call, in 100ns units
    // (TimeSpan / QPC-based, monotonic).
    //
    // Capture-timing root-fix PD1: primary source is WGC
    // Direct3D11CaptureFrame.SystemRelativeTime() (true presentation time of
    // the captured content). Fallback (DXGI desktop-duplication path, or when
    // SystemRelativeTime is zero/unavailable e.g. first frame) is a
    // high-resolution QPC stamp taken at the moment of capture so a monotonic
    // timestamp ALWAYS exists.
    //
    // Accessor pattern (not an out-param / not a signature change) was chosen
    // as the lowest-blast-radius option: CaptureTexture()/TryCaptureTexture()
    // are called from several call sites and changing their return contract
    // would churn all of them. It is populated synchronously inside the
    // capture call before it returns and read immediately afterward by the
    // single producer thread, mirroring the existing GetFrameEvent() accessor
    // convention in this header.
    int64_t LastFrameSystemRelativeTime() const { return m_lastFrameSystemRelativeTime100ns; }

    // True if the last capture timestamp came from WGC SystemRelativeTime
    // (real presentation time); false if it was the QPC fallback. Makes the
    // PD4 capture diagnostic unambiguous on the DXGI-fallback path.
    bool LastFrameTimestampFromWgc() const { return m_lastFrameTimestampFromWgc; }

    int GetCaptureWidth() const { return captureWidth; }
    int GetCaptureHeight() const { return captureHeight; }
    int GetCaptureLeft() const { return captureLeft; }
    int GetCaptureTop() const { return captureTop; }
    bool IsDxgiFallback() const { return useDxgiDuplication; }

private:
    bool InitDxgiDuplication();
    static HMONITOR ResolveMonitorHandle(int monitorIndex);

    int w = 0;
    int h = 0;

    // Capture source chosen at Init; the DXGI-duplication fallback matches its
    // output against this handle so both paths capture the same monitor.
    HMONITOR m_targetMonitor = nullptr;

    // WGC path
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{ nullptr };

    // FrameArrived event for signal-driven (non-polling) capture.
    HANDLE m_frameEvent = nullptr;
    winrt::event_token m_frameArrivedToken{};

    // PD1 real capture timestamp of the last returned frame (100ns units).
    // Populated by CaptureTexture()/TryCaptureTexture(); see accessor doc.
    int64_t m_lastFrameSystemRelativeTime100ns = 0;
    bool m_lastFrameTimestampFromWgc = false;

    // QPC-fallback helper: a high-resolution monotonic stamp in 100ns units
    // taken at capture time, used when WGC SystemRelativeTime is unavailable
    // or zero (DXGI path / first frame) so a timestamp always exists.
    int64_t QpcNow100ns() const;

    // Shared D3D device from encoder backend
    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;

    // DXGI desktop duplication fallback
    IDXGIOutputDuplication* dxgiDuplication = nullptr;
    ID3D11Texture2D* dxgiDupTexture = nullptr;
    bool useDxgiDuplication = false;

    // Capture source geometry in desktop coordinates.
    int captureWidth = 0;
    int captureHeight = 0;
    int captureLeft = 0;
    int captureTop = 0;
};
