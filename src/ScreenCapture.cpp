#include "AetherFlow/ScreenCapture.h"

#include <algorithm>
#include <iostream>
#include <sstream>

#include <winrt/Windows.Foundation.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

using namespace winrt;
namespace WGC = winrt::Windows::Graphics::Capture;
namespace WDX = winrt::Windows::Graphics::DirectX;

extern "C" {
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) IDirect3DDxgiInterfaceAccess : ::IUnknown {
    virtual HRESULT __stdcall GetInterface(const GUID& id, void** object) = 0;
};
}

static std::string HrToHex(HRESULT hr) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return oss.str();
}

// High-resolution monotonic QPC stamp converted to 100ns units (the same
// unit as WGC TimeSpan / SystemRelativeTime). Used as the PD1 fallback when
// WGC SystemRelativeTime is unavailable or zero so a monotonic timestamp
// always exists.
int64_t ScreenCapture::QpcNow100ns() const {
    LARGE_INTEGER freq{};
    LARGE_INTEGER counter{};
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0 ||
        !QueryPerformanceCounter(&counter)) {
        return 0;
    }
    // ticks -> 100ns: counter / freq seconds * 10,000,000. Done in 128-bit-ish
    // order (multiply before divide, split to avoid overflow on long runs).
    const long long whole = counter.QuadPart / freq.QuadPart;
    const long long rem = counter.QuadPart % freq.QuadPart;
    return whole * 10000000LL + (rem * 10000000LL) / freq.QuadPart;
}

static winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice CreateDirect3DDevice(IDXGIDevice* dxgiDevice) {
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice d3dDevice = nullptr;
    HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, reinterpret_cast<IInspectable**>(put_abi(d3dDevice)));
    if (FAILED(hr)) {
        throw hresult_error(hr);
    }
    return d3dDevice;
}

ScreenCapture::ScreenCapture() {
    try {
        init_apartment(apartment_type::multi_threaded);
    } catch (...) {
        // Already initialized with another mode; keep going.
    }
}

ScreenCapture::~ScreenCapture() {
    Close();
}

void ScreenCapture::Close() {
    // Unsubscribe FrameArrived before closing the pool to avoid a callback firing
    // on a partially-destroyed object.
    if (m_frameArrivedToken.value != 0 && framePool) {
        framePool.FrameArrived(m_frameArrivedToken);
        m_frameArrivedToken = {};
    }
    if (m_frameEvent) {
        CloseHandle(m_frameEvent);
        m_frameEvent = nullptr;
    }

    if (session) {
        session.Close();
        session = nullptr;
    }
    if (framePool) {
        framePool.Close();
        framePool = nullptr;
    }
    item = nullptr;

    if (dxgiDupTexture) {
        dxgiDupTexture->Release();
        dxgiDupTexture = nullptr;
    }
    if (dxgiDuplication) {
        dxgiDuplication->Release();
        dxgiDuplication = nullptr;
    }
    useDxgiDuplication = false;
    captureWidth = 0;
    captureHeight = 0;
    captureLeft = 0;
    captureTop = 0;

    if (d3dContext) {
        d3dContext->Release();
        d3dContext = nullptr;
    }
    if (d3dDevice) {
        d3dDevice->Release();
        d3dDevice = nullptr;
    }
}

namespace {

struct MonitorEnumEntry {
    HMONITOR handle = nullptr;
    RECT rect{};
    bool primary = false;
};

BOOL CALLBACK CollectMonitorsProc(HMONITOR hmon, HDC, LPRECT, LPARAM lparam) {
    auto* out = reinterpret_cast<std::vector<MonitorEnumEntry>*>(lparam);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(hmon, &mi)) {
        out->push_back({hmon, mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
    }
    return TRUE;
}

// EnumDisplayMonitors order is unspecified; sort primary-first then by
// desktop position so a saved monitorIndex means the same screen across runs.
std::vector<MonitorEnumEntry> CollectMonitorsSorted() {
    std::vector<MonitorEnumEntry> entries;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitorsProc, reinterpret_cast<LPARAM>(&entries));
    std::sort(entries.begin(), entries.end(), [](const MonitorEnumEntry& a, const MonitorEnumEntry& b) {
        if (a.primary != b.primary) return a.primary;
        if (a.rect.left != b.rect.left) return a.rect.left < b.rect.left;
        return a.rect.top < b.rect.top;
    });
    return entries;
}

}  // namespace

std::vector<ScreenCapture::MonitorInfo> ScreenCapture::EnumerateMonitors() {
    std::vector<MonitorInfo> out;
    const auto entries = CollectMonitorsSorted();
    out.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        MonitorInfo info;
        info.index = static_cast<int>(i);
        info.rect = entries[i].rect;
        info.primary = entries[i].primary;
        out.push_back(info);
    }
    return out;
}

HMONITOR ScreenCapture::ResolveMonitorHandle(int monitorIndex) {
    const auto entries = CollectMonitorsSorted();
    if (entries.empty()) {
        return MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    }
    if (monitorIndex < 0 || monitorIndex >= static_cast<int>(entries.size())) {
        std::cerr << "[ScreenCapture] monitorIndex " << monitorIndex
                  << " out of range (0-" << entries.size() - 1 << "); using primary.\n";
        monitorIndex = 0;
    }
    return entries[static_cast<size_t>(monitorIndex)].handle;
}

bool ScreenCapture::InitDxgiDuplication() {
    if (!d3dDevice || !d3dContext) {
        return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    HRESULT hr = d3dDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr) || !dxgiDevice) {
        std::cerr << "[ScreenCapture][DXGI] QueryInterface(IDXGIDevice) failed: " << HrToHex(hr) << "\n";
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr) || !adapter) {
        std::cerr << "[ScreenCapture][DXGI] GetAdapter failed: " << HrToHex(hr) << "\n";
        return false;
    }

    bool ok = false;
    // Pass 0 accepts only the output backing the monitor chosen at Init
    // (m_targetMonitor); pass 1 accepts any working output as the fallback,
    // preserving the historical "first output that duplicates" behavior.
    for (int pass = 0; pass < 2 && !ok; ++pass) {
    for (UINT i = 0;; ++i) {
        IDXGIOutput* output = nullptr;
        hr = adapter->EnumOutputs(i, &output);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr) || !output) {
            continue;
        }

        DXGI_OUTPUT_DESC outDesc = {};
        output->GetDesc(&outDesc);
        if (pass == 0 && m_targetMonitor && outDesc.Monitor != m_targetMonitor) {
            output->Release();
            continue;
        }
        if (pass == 1 && m_targetMonitor && outDesc.Monitor != m_targetMonitor) {
            // Fallback pass accepted a DIFFERENT screen than the one selected
            // — never do that silently in a privacy product.
            std::cerr << "[ScreenCapture][DXGI] WARNING: requested monitor is not "
                         "duplicable; falling back to another output — the captured "
                         "screen is NOT the selected one.\n";
        }

        IDXGIOutput1* output1 = nullptr;
        hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
        if (FAILED(hr) || !output1) {
            output->Release();
            continue;
        }

        IDXGIOutputDuplication* duplication = nullptr;
        hr = output1->DuplicateOutput(d3dDevice, &duplication);
        output1->Release();
        if (FAILED(hr) || !duplication) {
            output->Release();
            continue;
        }

        UINT outW = static_cast<UINT>(outDesc.DesktopCoordinates.right - outDesc.DesktopCoordinates.left);
        UINT outH = static_cast<UINT>(outDesc.DesktopCoordinates.bottom - outDesc.DesktopCoordinates.top);
        if (outW == 0 || outH == 0) {
            duplication->Release();
            output->Release();
            continue;
        }

        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = outW;
        texDesc.Height = outH;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        ID3D11Texture2D* copyTex = nullptr;
        hr = d3dDevice->CreateTexture2D(&texDesc, nullptr, &copyTex);
        if (FAILED(hr) || !copyTex) {
            duplication->Release();
            output->Release();
            continue;
        }

        dxgiDuplication = duplication;
        dxgiDupTexture = copyTex;
        useDxgiDuplication = true;
        captureWidth = static_cast<int>(outW);
        captureHeight = static_cast<int>(outH);
        captureLeft = outDesc.DesktopCoordinates.left;
        captureTop = outDesc.DesktopCoordinates.top;
        ok = true;

        std::cout << "[ScreenCapture][DXGI] Duplication fallback enabled: "
                  << outW << "x" << outH
                  << " at (" << captureLeft << "," << captureTop << ")\n";

        output->Release();
        break;
    }
    }

    adapter->Release();
    return ok;
}

bool ScreenCapture::Init(int width, int height, ID3D11Device* sharedDevice, int monitorIndex) {
    Close();
    w = width;
    h = height;
    m_targetMonitor = ResolveMonitorHandle(monitorIndex);

    if (!sharedDevice) {
        std::cerr << "[ScreenCapture] Init failed: sharedDevice is null.\n";
        return false;
    }

    d3dDevice = sharedDevice;
    d3dDevice->AddRef();
    d3dDevice->GetImmediateContext(&d3dContext);
    if (!d3dContext) {
        std::cerr << "[ScreenCapture] GetImmediateContext failed.\n";
        return false;
    }

    // Try WGC first.
    bool wgcOk = false;
    IDXGIDevice* dxgiDevice = nullptr;
    HRESULT hr = d3dDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (SUCCEEDED(hr) && dxgiDevice) {
        try {
            auto winrtDevice = CreateDirect3DDevice(dxgiDevice);

            auto activationFactory = get_activation_factory<WGC::GraphicsCaptureItem>();
            auto interopFactory = activationFactory.as<IGraphicsCaptureItemInterop>();

            HWND hwnd = GetDesktopWindow();
            HMONITOR hmon = m_targetMonitor
                ? m_targetMonitor
                : MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
            void* itemPtr = nullptr;

            hr = interopFactory->CreateForMonitor(
                hmon,
                guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                &itemPtr);

            bool createdForMonitor = SUCCEEDED(hr) && itemPtr;

            if (FAILED(hr) || !itemPtr) {
                std::cerr << "[ScreenCapture] CreateForMonitor failed: " << HrToHex(hr)
                          << " (fallback to CreateForWindow)\n";
                itemPtr = nullptr;
                hr = interopFactory->CreateForWindow(
                    hwnd,
                    guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                    &itemPtr);
                createdForMonitor = false;
            }

            if (SUCCEEDED(hr) && itemPtr) {
                item = nullptr;
                winrt::attach_abi(item, itemPtr);

                auto captureSize = item.Size();
                if (captureSize.Width <= 0 || captureSize.Height <= 0) {
                    captureSize.Width = w;
                    captureSize.Height = h;
                }
                captureWidth = captureSize.Width;
                captureHeight = captureSize.Height;
                captureLeft = 0;
                captureTop = 0;

                if (createdForMonitor) {
                    MONITORINFO mi = {};
                    mi.cbSize = sizeof(mi);
                    if (GetMonitorInfo(hmon, &mi)) {
                        captureLeft = mi.rcMonitor.left;
                        captureTop = mi.rcMonitor.top;
                    }
                } else {
                    captureLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
                    captureTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
                }

                framePool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(
                    winrtDevice,
                    WDX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    4,
                    captureSize);

                session = framePool.CreateCaptureSession(item);
                session.StartCapture();

                // Event-driven capture: FrameArrived signals m_frameEvent.
                // CaptureTexture waits on this instead of spin-polling with Sleep.
                if (!m_frameEvent)
                    m_frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                m_frameArrivedToken = framePool.FrameArrived(
                    [this](WGC::Direct3D11CaptureFramePool const&, winrt::Windows::Foundation::IInspectable const&) {
                        if (m_frameEvent) SetEvent(m_frameEvent);
                    });

                wgcOk = true;
            } else {
                std::cerr << "[ScreenCapture] CreateForWindow failed: " << HrToHex(hr) << "\n";
            }
        } catch (const winrt::hresult_error& e) {
            std::cerr << "[ScreenCapture] WGC init exception: " << HrToHex(e.code().value) << "\n";
        }
        dxgiDevice->Release();
    } else {
        std::cerr << "[ScreenCapture] QueryInterface(IDXGIDevice) failed: " << HrToHex(hr) << "\n";
    }

    if (wgcOk) {
        std::cout << "[ScreenCapture] Init OK (WGC): "
                  << captureWidth << "x" << captureHeight
                  << " at (" << captureLeft << "," << captureTop << ")\n";
        return true;
    }

    std::cout << "[ScreenCapture] WGC unavailable; trying DXGI duplication fallback...\n";
    if (InitDxgiDuplication()) {
        return true;
    }

    std::cerr << "[ScreenCapture] Init failed: both WGC and DXGI duplication are unavailable.\n";
    return false;
}

ID3D11Texture2D* ScreenCapture::CaptureTexture() {
    if (framePool) {
        auto frame = framePool.TryGetNextFrame();
        if (!frame) {
            // Wait up to 40ms for FrameArrived signal, then retry once.
            // This replaces the old Sleep(1) spin loop which burned ~15ms per
            // iteration (Windows default timer resolution) and could stall 600ms.
            if (m_frameEvent) {
                WaitForSingleObject(m_frameEvent, 40);
            } else {
                Sleep(5);
            }
            frame = framePool.TryGetNextFrame();
        }

        if (!frame) {
            return nullptr;
        }

        // PD1: read the real per-frame capture timestamp WHILE the
        // Direct3D11CaptureFrame is still alive (it is destroyed when `frame`
        // goes out of scope at the end of this block — the historical bug was
        // that this stamp was never read). SystemRelativeTime() is a TimeSpan
        // in 100ns units (QPC-based true presentation time of the content).
        // Fall back to a capture-time QPC stamp if WGC returns zero (can
        // happen on the very first frame before the compositor timeline is
        // established) so a monotonic timestamp always exists.
        {
            const int64_t wgcStamp = frame.SystemRelativeTime().count();
            if (wgcStamp > 0) {
                m_lastFrameSystemRelativeTime100ns = wgcStamp;
                m_lastFrameTimestampFromWgc = true;
            } else {
                m_lastFrameSystemRelativeTime100ns = QpcNow100ns();
                m_lastFrameTimestampFromWgc = false;
            }
        }

        auto surface = frame.Surface();
        auto interopSurface = surface.as<IDirect3DDxgiInterfaceAccess>();

        ID3D11Texture2D* gpuTex = nullptr;
        HRESULT hr = interopSurface->GetInterface(IID_PPV_ARGS(&gpuTex));
        if (FAILED(hr)) {
            std::cerr << "[ScreenCapture] GetInterface(ID3D11Texture2D) failed: " << HrToHex(hr) << "\n";
            return nullptr;
        }

        return gpuTex;
    }

    if (useDxgiDuplication && dxgiDuplication && dxgiDupTexture) {
        DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
        IDXGIResource* desktopResource = nullptr;

        HRESULT hr = dxgiDuplication->AcquireNextFrame(33, &frameInfo, &desktopResource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            return nullptr;
        }
        if (FAILED(hr) || !desktopResource) {
            if (hr == DXGI_ERROR_ACCESS_LOST) {
                std::cerr << "[ScreenCapture][DXGI] AcquireNextFrame lost access.\n";
            }
            return nullptr;
        }

        // PD1 fallback: the DXGI desktop-duplication path has no equivalent of
        // WGC SystemRelativeTime. Stamp with a high-resolution QPC read taken
        // at capture time so a monotonic timestamp still exists; the
        // diagnostic flags this as a non-WGC source.
        m_lastFrameSystemRelativeTime100ns = QpcNow100ns();
        m_lastFrameTimestampFromWgc = false;

        ID3D11Texture2D* srcTex = nullptr;
        hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&srcTex));
        desktopResource->Release();

        if (FAILED(hr) || !srcTex) {
            dxgiDuplication->ReleaseFrame();
            return nullptr;
        }

        d3dContext->CopyResource(dxgiDupTexture, srcTex);
        srcTex->Release();
        dxgiDuplication->ReleaseFrame();

        dxgiDupTexture->AddRef();
        return dxgiDupTexture;
    }

    return nullptr;
}

// Non-blocking version: returns a frame only if one is already waiting in the
// WGC pool.  Never calls WaitForSingleObject.  Use to drain stale frames from
// the pool immediately after encoding so that by the time the pacing sleep
// ends, only fresh frames remain in the pool.
ID3D11Texture2D* ScreenCapture::TryCaptureTexture() {
    if (framePool) {
        auto frame = framePool.TryGetNextFrame();
        if (!frame) return nullptr;

        // PD1: same real-timestamp read as CaptureTexture(), done while the
        // Direct3D11CaptureFrame is still alive.
        {
            const int64_t wgcStamp = frame.SystemRelativeTime().count();
            if (wgcStamp > 0) {
                m_lastFrameSystemRelativeTime100ns = wgcStamp;
                m_lastFrameTimestampFromWgc = true;
            } else {
                m_lastFrameSystemRelativeTime100ns = QpcNow100ns();
                m_lastFrameTimestampFromWgc = false;
            }
        }

        auto surface = frame.Surface();
        auto interopSurface = surface.as<IDirect3DDxgiInterfaceAccess>();

        ID3D11Texture2D* gpuTex = nullptr;
        HRESULT hr = interopSurface->GetInterface(IID_PPV_ARGS(&gpuTex));
        if (FAILED(hr)) return nullptr;

        return gpuTex;
    }

    // DXGI duplication path: use AcquireNextFrame(0) for non-blocking poll.
    if (useDxgiDuplication && dxgiDuplication && dxgiDupTexture) {
        DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
        IDXGIResource* desktopResource = nullptr;

        HRESULT hr = dxgiDuplication->AcquireNextFrame(0, &frameInfo, &desktopResource);
        if (FAILED(hr) || !desktopResource) {
            return nullptr;
        }

        // PD1 fallback: DXGI path has no presentation time; QPC stamp.
        m_lastFrameSystemRelativeTime100ns = QpcNow100ns();
        m_lastFrameTimestampFromWgc = false;

        ID3D11Texture2D* srcTex = nullptr;
        hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&srcTex));
        desktopResource->Release();

        if (FAILED(hr) || !srcTex) {
            dxgiDuplication->ReleaseFrame();
            return nullptr;
        }

        d3dContext->CopyResource(dxgiDupTexture, srcTex);
        srcTex->Release();
        dxgiDuplication->ReleaseFrame();

        dxgiDupTexture->AddRef();
        return dxgiDupTexture;
    }

    return nullptr;
}
