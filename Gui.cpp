// Gui.cpp - Direct2D / DirectWrite front-end.
//
// The benchmark runs on a worker thread and streams into SharedState (a
// gui::Feed). The UI thread copies a Snapshot of that state once per frame
// and renders it at the monitor's refresh rate: the live sweep curve, an
// oscilloscope of individual Sleep(1) wake-ups, the wake-up distribution
// against the clock ticks, animated statistics, the verification bars and
// finally the result / install flow. The Synchronus mark and the Lucius And
// Wayne Systems LLC producer logo are embedded as RCDATA and drawn through
// WIC. No other dependencies.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "Gui.h"
#ifndef SHTDN_REASON_MAJOR_APPLICATION
#define SHTDN_REASON_MAJOR_APPLICATION 0x00040000
#define SHTDN_REASON_MINOR_MAINTENANCE 0x00000001
#define SHTDN_REASON_FLAG_PLANNED 0x80000000
#endif

#include <d2d1_1.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <dwrite_3.h>
#include <dxgi1_2.h>
#include <objbase.h>
#include <shellapi.h>
#include <winreg.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <deque>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <thread>
#include <unordered_map>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

namespace gui {
namespace {

using Microsoft::WRL::ComPtr;

constexpr float kPi = 3.14159265f;
constexpr int kHistBins = 100;
constexpr float kHistMin = -0.05f, kHistMax = 1.20f;
constexpr size_t kRingSize = 4096;
constexpr size_t kTraceCount = 360;
constexpr float kTraceMin = -0.05f, kTraceMax = 1.25f;

// ------------------------------------------------------------- utilities
std::wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

double Ms(unsigned units) { return units / 10000.0; }

std::wstring Upper(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towupper(c));
    return s;
}

std::wstring Duration(double seconds) {
    const int total = static_cast<int>(std::max(0.0, seconds) + 0.5);
    return Fmt(L"%d:%02d", total / 60, total % 60);
}

float Clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }
float Lerp(float a, float b, float t) { return a + (b - a) * t; }
float Approach(float cur, float target, float dt, float speed) {
    return target + (cur - target) * std::exp(-speed * dt);
}
float EaseOutCubic(float t) { t = Clamp01(t); const float u = 1 - t; return 1 - u * u * u; }
float EaseOutBack(float t) {
    t = Clamp01(t);
    const float c1 = 1.70158f, c3 = c1 + 1;
    const float u = t - 1;
    return 1 + c3 * u * u * u + c1 * u * u;
}

D2D1_COLOR_F Rgb(UINT32 hex, float a = 1.0f) { return D2D1::ColorF(hex, a); }
D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { c.a = a; return c; }
D2D1_COLOR_F MixC(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t), Lerp(a.a, b.a, t));
}

namespace col {
const D2D1_COLOR_F bg = Rgb(0x071638);        // Midnight Navy
const D2D1_COLOR_F panel = Rgb(0x0A1E4C);
const D2D1_COLOR_F panel2 = Rgb(0x0C2458);
const D2D1_COLOR_F border = Rgb(0x123A91);
const D2D1_COLOR_F grid = Rgb(0x0C2A68);
const D2D1_COLOR_F text = Rgb(0xF4F7FF);      // Cool White
const D2D1_COLOR_F muted = Rgb(0x8DB8FF);     // Icy Blue
const D2D1_COLOR_F dim = Rgb(0x4D72B8);
const D2D1_COLOR_F cyan = Rgb(0x347BFF);      // Electric Blue (primary accent)
const D2D1_COLOR_F teal = Rgb(0x8DB8FF);
const D2D1_COLOR_F violet = Rgb(0x123A91);    // Blueprint
const D2D1_COLOR_F magenta = Rgb(0x347BFF);
const D2D1_COLOR_F amber = Rgb(0x8DB8FF);
const D2D1_COLOR_F green = Rgb(0x8DB8FF);
const D2D1_COLOR_F red = Rgb(0xFF8A9A);
const D2D1_COLOR_F white = Rgb(0xF4F7FF);
}  // namespace col

D2D1_RECT_F Inset(const D2D1_RECT_F& r, float l, float t, float rr, float b) {
    return D2D1::RectF(r.left + l, r.top + t, r.right - rr, r.bottom - b);
}
float W(const D2D1_RECT_F& r) { return r.right - r.left; }
float H(const D2D1_RECT_F& r) { return r.bottom - r.top; }
bool PtIn(D2D1_POINT_2F p, const D2D1_RECT_F& r) {
    return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom;
}

// A sample's "missed a tick" threshold for live colouring: the typical wake
// is at the first clock tick >= 1 ms, i.e. 2R - 1 ms late; half a tick
// beyond that means it slept one tick longer.
float LateThreshold(unsigned requested) {
    const float r = static_cast<float>(Ms(requested));
    const float base = std::max(0.0f, 2 * r - 1);
    return base + r / 2;
}

// 128³ measurement lattice.
//   X = late percent (0–100), the running share of missed ticks for that resolution
//   Y = measured Sleep(1) latency, in milliseconds of overshoot
//   Z = how many samples landed in that cell
// Column membranes sit at the candidate's final late percent and mean latency.
constexpr int kLat = 128;
constexpr int kLatRT = 640;

struct LatCell {
    uint16_t n = 0, loop = 0, idle = 0, late = 0;
};
struct ColStat {
    float loopU = 0, idleU = 0, p99U = 0, stdevU = 0;
    float late01 = 0, score = 0, present = 0, pad = 0;
};

float LatYU(float ms) {
    if (ms <= 2.0f) return Clamp01((ms - kHistMin) / (2.0f - kHistMin) * 0.80f);
    return Clamp01(0.80f + Clamp01((ms - 2.0f) / 18.0f) * 0.20f);
}
int MapLatY(float ms) {
    return std::min(kLat - 1, std::max(0, static_cast<int>(LatYU(ms) * (kLat - 1) + 0.5f)));
}
int MapLateX(float latePct) {
    const float u = Clamp01(latePct / 100.0f);
    return std::min(kLat - 1, std::max(0, static_cast<int>(std::lround(u * static_cast<float>(kLat - 1)))));
}

// ---------------------------------------------------------- shared state
struct Sample {
    float v = 0;
    unsigned requested = 0;
    bool idle = false;
    double t = 0;   // seconds since GUI start
};

struct Snapshot {
    std::wstring phase, detail;
    double progress = 0, elapsed = 0, remaining = 0;
    std::vector<std::wstring> log;
    bool hasBaseline = false;
    StatsView baseline;
    unsigned baselineUnits = 0;
    SweepView sweep;
    int activeIdx = -1;
    std::vector<CandidateView> finalists;
    int activeFinalist = -1;
    unsigned target = 0;
    SystemView system;
    std::vector<Sample> trace;
    uint64_t sampleSeq = 0;
    unsigned histRequested = 0;
    std::vector<unsigned> hist;
    unsigned histTotal = 0, histLate = 0;
    std::vector<LatCell> lat;
    std::vector<ColStat> col;
    double now = 0;
    std::wstring narrateTitle, narrateBody;
    std::vector<PpmStepView> ppm;
};

class SharedState final : public Feed {
public:
    SharedState() {
        QueryPerformanceFrequency(&freq_);
        QueryPerformanceCounter(&t0_);
        ring_.resize(kRingSize);
        hist_.assign(kHistBins, 0);
        lat_.assign(static_cast<size_t>(kLat) * kLat, {});
        col_.assign(kLat, {});
    }

    double Now() const {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return static_cast<double>(t.QuadPart - t0_.QuadPart) / static_cast<double>(freq_.QuadPart);
    }

    void System(const SystemView& s) override {
        std::lock_guard<std::mutex> g(m_);
        system_ = s;
    }
    void Phase(const std::wstring& title, const std::wstring& detail) override {
        std::lock_guard<std::mutex> g(m_);
        phase_ = title;
        detail_ = detail;
    }
    void Progress(double fraction, double elapsedS, double remainingS) override {
        std::lock_guard<std::mutex> g(m_);
        progress_ = fraction;
        elapsed_ = elapsedS;
        remaining_ = remainingS;
    }
    void Log(const std::wstring& line) override {
        std::lock_guard<std::mutex> g(m_);
        size_t start = 0;
        while (start < line.size()) {
            size_t end = line.find(L'\n', start);
            if (end == std::wstring::npos) end = line.size();
            if (end > start) {
                log_.push_back(line.substr(start, end - start));
                if (log_.size() > 40) log_.pop_front();
            }
            start = end + 1;
        }
    }
    void Baseline(const StatsView& s, unsigned reportedUnits) override {
        std::lock_guard<std::mutex> g(m_);
        baseline_ = s;
        baselineUnits_ = reportedUnits;
        hasBaseline_ = true;
    }
    void Sample(unsigned requested, double overshootMs, bool idlePattern) override {
        std::lock_guard<std::mutex> g(m_);
        ::gui::Sample& s = ring_[head_ % kRingSize];
        s.v = static_cast<float>(overshootMs);
        s.requested = requested;
        s.idle = idlePattern;
        s.t = Now();
        ++head_;
        histRequested_ = requested;
        int bin = static_cast<int>((overshootMs - kHistMin) / (kHistMax - kHistMin) * kHistBins);
        bin = std::min(kHistBins - 1, std::max(0, bin));
        ++hist_[bin];
        ++histTotal_;
        const bool late = overshootMs > LateThreshold(requested);
        if (late) ++histLate_;
        RunLate& run = runLate_[requested];
        if (run.n < 1000000u) ++run.n;
        if (late && run.late < 1000000u) ++run.late;
        const float latePct = run.n ? (100.0f * static_cast<float>(run.late) / static_cast<float>(run.n)) : 0.0f;
        const int ix = MapLateX(latePct);
        const int iy = MapLatY(static_cast<float>(overshootMs));
        LatCell& cell = lat_[static_cast<size_t>(iy) * kLat + ix];
        if (cell.n < 65535) ++cell.n;
        if (idlePattern) {
            if (cell.idle < 65535) ++cell.idle;
        } else if (cell.loop < 65535) {
            ++cell.loop;
        }
        if (overshootMs > LateThreshold(requested) && cell.late < 65535) ++cell.late;
    }
    void Sweep(const SweepView& view, int activeIdx) override {
        std::lock_guard<std::mutex> g(m_);
        if (view.pass > 0 && view.pass != sweepPass_) {
            sweepPass_ = view.pass;
            verifyLap_ = false;
            ClearHist();
        }
        sweep_ = view;
        activeIdx_ = activeIdx;
        WriteCols(view.items);
    }
    void Verify(const std::vector<CandidateView>& finalists, int activeIdx, unsigned target) override {
        std::lock_guard<std::mutex> g(m_);
        if (!verifyLap_) {
            verifyLap_ = true;
            ClearHist();
        }
        finalists_ = finalists;
        activeFinalist_ = activeIdx;
        target_ = target;
        WriteCols(finalists);
    }
    void Narrate(const std::wstring& title, const std::wstring& body) override {
        std::lock_guard<std::mutex> g(m_);
        narrateTitle_ = title;
        narrateBody_ = body;
    }
    void Ppm(const std::vector<PpmStepView>& steps) override {
        std::lock_guard<std::mutex> g(m_);
        ppm_ = steps;
    }
    bool Cancelled() const override { return cancelled_.load(); }

    void Cancel() { cancelled_ = true; }

    // Fresh run: keep the system info, drop everything measured.
    void Reset() {
        std::lock_guard<std::mutex> g(m_);
        phase_.clear();
        detail_.clear();
        progress_ = elapsed_ = remaining_ = 0;
        log_.clear();
        hasBaseline_ = false;
        sweep_ = SweepView{};
        activeIdx_ = -1;
        finalists_.clear();
        activeFinalist_ = -1;
        target_ = 0;
        head_ = 0;
        histRequested_ = 0;
        sweepPass_ = 0;
        verifyLap_ = false;
        ClearHist();
        cancelled_ = false;
        narrateTitle_.clear();
        narrateBody_.clear();
        ppm_.clear();
    }

    void Snap(Snapshot& out) {
        std::lock_guard<std::mutex> g(m_);
        out.phase = phase_;
        out.detail = detail_;
        out.progress = progress_;
        out.elapsed = elapsed_;
        out.remaining = remaining_;
        out.log.assign(log_.begin(), log_.end());
        out.hasBaseline = hasBaseline_;
        out.baseline = baseline_;
        out.baselineUnits = baselineUnits_;
        out.sweep = sweep_;
        out.activeIdx = activeIdx_;
        out.finalists = finalists_;
        out.activeFinalist = activeFinalist_;
        out.target = target_;
        out.system = system_;
        const size_t n = static_cast<size_t>(std::min<uint64_t>(head_, kTraceCount));
        out.trace.resize(n);
        for (size_t i = 0; i < n; ++i) out.trace[i] = ring_[(head_ - n + i) % kRingSize];
        out.sampleSeq = head_;
        out.histRequested = histRequested_;
        out.hist = hist_;
        out.histTotal = histTotal_;
        out.histLate = histLate_;
        out.now = Now();
        out.narrateTitle = narrateTitle_;
        out.narrateBody = narrateBody_;
        out.ppm = ppm_;
        out.lat = lat_;
        out.col = col_;
    }

    void WriteCols(const std::vector<CandidateView>& items) {
        std::fill(col_.begin(), col_.end(), ColStat{});
        for (const auto& c : items) {
            if (c.loop.n == 0 && c.idle.n == 0) continue;
            const bool loop = c.loop.n != 0;
            const float latePct = static_cast<float>(loop ? c.loop.latePct : c.idle.latePct);
            const int ix = MapLateX(latePct);
            ColStat s;
            s.loopU = LatYU(static_cast<float>(loop ? c.loop.mean : c.idle.mean));
            s.idleU = LatYU(static_cast<float>(c.idle.n ? c.idle.mean : c.loop.mean));
            s.p99U = LatYU(static_cast<float>(loop ? c.loop.p99 : c.idle.p99));
            s.stdevU = static_cast<float>(loop ? c.loop.stdev : c.idle.stdev) / (2.0f - kHistMin) * 0.80f;
            s.late01 = static_cast<float>(loop ? c.loop.latePct : c.idle.latePct) / 100.0f;
            s.score = static_cast<float>(c.score);
            s.present = 1.0f;
            col_[static_cast<size_t>(ix)] = s;
        }
    }

    void ClearHist() {
        std::fill(hist_.begin(), hist_.end(), 0u);
        histTotal_ = histLate_ = 0;
        std::fill(lat_.begin(), lat_.end(), LatCell{});
        std::fill(col_.begin(), col_.end(), ColStat{});
        runLate_.clear();
    }

private:
    std::mutex m_;
    LARGE_INTEGER freq_{}, t0_{};
    std::wstring phase_, detail_;
    double progress_ = 0, elapsed_ = 0, remaining_ = 0;
    std::deque<std::wstring> log_;
    bool hasBaseline_ = false;
    StatsView baseline_;
    unsigned baselineUnits_ = 0;
    SweepView sweep_;
    int activeIdx_ = -1;
    std::vector<CandidateView> finalists_;
    int activeFinalist_ = -1;
    unsigned target_ = 0;
    SystemView system_;
    std::vector<::gui::Sample> ring_;
    uint64_t head_ = 0;
    unsigned histRequested_ = 0;
    int sweepPass_ = 0;
    bool verifyLap_ = false;
    std::vector<unsigned> hist_;
    unsigned histTotal_ = 0, histLate_ = 0;
    std::vector<LatCell> lat_;
    std::vector<ColStat> col_;
    struct RunLate {
        unsigned n = 0, late = 0;
    };
    std::unordered_map<unsigned, RunLate> runLate_;
    std::wstring narrateTitle_, narrateBody_;
    std::vector<PpmStepView> ppm_;
    std::atomic<bool> cancelled_{false};
};

// -------------------------------------------------------------- renderer
class Renderer {
public:
    bool Init(HWND hwnd, float dpi) {
        hwnd_ = hwnd;
        dpi_ = dpi;
        Release();

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                            D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_1};
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_9_1;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, _countof(levels),
                                       D3D11_SDK_VERSION, &device, &got, &context);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, _countof(levels),
                                   D3D11_SDK_VERSION, &device, &got, &context);
        }
        if (FAILED(hr)) return false;
        device_ = device;
        context_ = context;
        feature_ = got;

        ComPtr<IDXGIDevice1> dxgiDevice;
        if (FAILED(device.As(&dxgiDevice))) return false;
        dxgiDevice->SetMaximumFrameLatency(1);
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

        RECT rc{};
        GetClientRect(hwnd, &rc);
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = std::max<UINT>(1, rc.right - rc.left);
        sd.Height = std::max<UINT>(1, rc.bottom - rc.top);
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.Scaling = DXGI_SCALING_NONE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &sd, nullptr, nullptr, &swapChain_))) {
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            sd.Scaling = DXGI_SCALING_STRETCH;
            if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &sd, nullptr, nullptr, &swapChain_))) {
                return false;
            }
        }
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        D2D1_FACTORY_OPTIONS fo{};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo,
                                     reinterpret_cast<void**>(d2dFactory_.ReleaseAndGetAddressOf())))) {
            return false;
        }
        ComPtr<ID2D1Device> d2dDevice;
        if (FAILED(d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice))) return false;
        if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return false;
        dc_->SetDpi(dpi_, dpi_);
        dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        if (!CreateTarget()) return false;

        if (!dwrite_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                   reinterpret_cast<IUnknown**>(dwrite_.ReleaseAndGetAddressOf())))) {
            return false;
        }
        if (!fonts_) LoadEmbeddedFonts();
        if (!fTitle) CreateFormats();

        dc_->CreateSolidColorBrush(col::white, &brush_);
        d2dFactory_->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                        D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f),
            nullptr, 0, &round_);
        d2dFactory_->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                        D2D1_LINE_JOIN_MITER, 10.0f, D2D1_DASH_STYLE_DASH, 0.0f),
            nullptr, 0, &dash_);
        d2dFactory_->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                        D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_DOT, 0.0f),
            nullptr, 0, &dot_);
        glowCyan_ = MakeRadial(col::cyan);
        glowMagenta_ = MakeRadial(col::magenta);
        glowViolet_ = MakeRadial(col::violet);
        glowAmber_ = MakeRadial(col::amber);
        glowGreen_ = MakeRadial(col::green);
        glowWhite_ = MakeRadial(col::white);
        glowTeal_ = MakeRadial(col::teal);
        glowRed_ = MakeRadial(col::red);
        titleGrad_ = MakeLinear(col::cyan, col::muted);
        progressGrad_ = MakeLinear(col::border, col::cyan);
        greenGrad_ = MakeLinear(col::cyan, col::muted);
        LoadLogos();
        InitLattice();
        return true;
    }

    void Release() {
        latticeBmp_.Reset();
        latticeRTV_.Reset();
        latticeTex_.Reset();
        histSRV_.Reset();
        histTex_.Reset();
        statSRV_.Reset();
        statTex_.Reset();
        latVS_.Reset();
        latPS_.Reset();
        latCB_.Reset();
        latSamp_.Reset();
        latRS_.Reset();
        latBS_.Reset();
        latDS_.Reset();
        latticeReady_ = false;
        target_.Reset();
        dc_.Reset();
        swapChain_.Reset();
        d2dFactory_.Reset();
        brush_.Reset();
        round_.Reset();
        dash_.Reset();
        dot_.Reset();
        glowCyan_.Reset();
        glowMagenta_.Reset();
        glowViolet_.Reset();
        glowAmber_.Reset();
        glowGreen_.Reset();
        glowWhite_.Reset();
        glowTeal_.Reset();
        glowRed_.Reset();
        titleGrad_.Reset();
        progressGrad_.Reset();
        greenGrad_.Reset();
        logo_.Reset();
        mark_.Reset();
        brand_.Reset();
        context_.Reset();
        device_.Reset();
    }

    bool Valid() const { return dc_ && target_; }

    void Resize(UINT w, UINT h) {
        if (!swapChain_ || !dc_) return;
        dc_->SetTarget(nullptr);
        target_.Reset();
        swapChain_->ResizeBuffers(0, std::max<UINT>(1, w), std::max<UINT>(1, h), DXGI_FORMAT_UNKNOWN, 0);
        CreateTarget();
    }

    void SetDpi(float dpi) {
        dpi_ = dpi;
        if (dc_) dc_->SetDpi(dpi, dpi);
    }

    D2D1_SIZE_F Size() const { return dc_ ? dc_->GetSize() : D2D1::SizeF(1, 1); }

    void Begin() {
        dc_->BeginDraw();
        dc_->Clear(col::bg);
    }

    // Returns false when the device was lost and resources must be rebuilt.
    bool End() {
        HRESULT hr = dc_->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) return false;
        hr = swapChain_->Present(1, 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) return false;
        return true;
    }

    ID2D1DeviceContext* Dc() { return dc_.Get(); }

    ID2D1SolidColorBrush* Brush(D2D1_COLOR_F c) {
        brush_->SetColor(c);
        return brush_.Get();
    }

    // ---- primitives ----
    void Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c) { dc_->FillRectangle(r, Brush(c)); }
    void FillRound(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c) {
        dc_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(c));
    }
    void StrokeRound(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c, float w = 1) {
        dc_->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(c), w);
    }
    void Line(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_COLOR_F c, float w = 1, ID2D1StrokeStyle* style = nullptr) {
        dc_->DrawLine(a, b, Brush(c), w, style);
    }
    void Dashed(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_COLOR_F c, float w = 1) { Line(a, b, c, w, dash_.Get()); }
    void Dotted(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_COLOR_F c, float w = 1) { Line(a, b, c, w, dot_.Get()); }
    void Dot(D2D1_POINT_2F p, float r, D2D1_COLOR_F c) { dc_->FillEllipse(D2D1::Ellipse(p, r, r), Brush(c)); }
    void Ring(D2D1_POINT_2F p, float r, D2D1_COLOR_F c, float w = 1) {
        dc_->DrawEllipse(D2D1::Ellipse(p, r, r), Brush(c), w);
    }

    ID2D1RadialGradientBrush* GlowBrush(D2D1_COLOR_F c) {
        if (c.r == col::cyan.r && c.g == col::cyan.g && c.b == col::cyan.b) return glowCyan_.Get();
        if (c.r == col::magenta.r && c.g == col::magenta.g && c.b == col::magenta.b) return glowMagenta_.Get();
        if (c.r == col::violet.r && c.g == col::violet.g && c.b == col::violet.b) return glowViolet_.Get();
        if (c.r == col::amber.r && c.g == col::amber.g && c.b == col::amber.b) return glowAmber_.Get();
        if (c.r == col::green.r && c.g == col::green.g && c.b == col::green.b) return glowGreen_.Get();
        if (c.r == col::teal.r && c.g == col::teal.g && c.b == col::teal.b) return glowTeal_.Get();
        if (c.r == col::red.r && c.g == col::red.g && c.b == col::red.b) return glowRed_.Get();
        return glowWhite_.Get();
    }

    // Soft radial glow of `radius` around p.
    void Glow(D2D1_POINT_2F p, float radius, D2D1_COLOR_F c, float opacity) {
        ID2D1RadialGradientBrush* br = GlowBrush(c);
        br->SetCenter(p);
        br->SetGradientOriginOffset(D2D1::Point2F(0, 0));
        br->SetRadiusX(radius);
        br->SetRadiusY(radius);
        br->SetOpacity(Clamp01(opacity));
        dc_->FillEllipse(D2D1::Ellipse(p, radius, radius), br);
    }
    void GlowDot(D2D1_POINT_2F p, float r, D2D1_COLOR_F c, float intensity = 1) {
        Glow(p, r * 3.2f, c, 0.55f * intensity);
        Dot(p, r, Alpha(c, c.a * Clamp01(0.4f + 0.6f * intensity)));
    }

    ComPtr<ID2D1PathGeometry> Poly(const D2D1_POINT_2F* pts, size_t n, bool filled = false) {
        ComPtr<ID2D1PathGeometry> g;
        if (n < 2 || FAILED(d2dFactory_->CreatePathGeometry(&g))) return nullptr;
        ComPtr<ID2D1GeometrySink> sink;
        g->Open(&sink);
        sink->BeginFigure(pts[0], filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLines(pts + 1, static_cast<UINT32>(n - 1));
        sink->EndFigure(filled ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        sink->Close();
        return g;
    }

    void FillQuad(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c, D2D1_POINT_2F d, D2D1_COLOR_F fill) {
        const D2D1_POINT_2F pts[4] = {a, b, c, d};
        auto g = Poly(pts, 4, true);
        if (g) dc_->FillGeometry(g.Get(), Brush(fill));
    }
    void StrokeQuad(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c, D2D1_POINT_2F d, D2D1_COLOR_F stroke, float w) {
        const D2D1_POINT_2F pts[4] = {a, b, c, d};
        auto g = Poly(pts, 4, true);
        if (g) dc_->DrawGeometry(g.Get(), Brush(stroke), w, round_.Get());
    }

    void Polyline(const std::vector<D2D1_POINT_2F>& pts, D2D1_COLOR_F c, float w, ID2D1StrokeStyle* style = nullptr) {
        auto g = Poly(pts.data(), pts.size());
        if (g) dc_->DrawGeometry(g.Get(), Brush(c), w, style ? style : round_.Get());
    }

    void GlowLine(const std::vector<D2D1_POINT_2F>& pts, D2D1_COLOR_F c, float w, float glow = 1) {
        auto g = Poly(pts.data(), pts.size());
        if (!g) return;
        dc_->DrawGeometry(g.Get(), Brush(Alpha(c, 0.08f * glow)), w * 7, round_.Get());
        dc_->DrawGeometry(g.Get(), Brush(Alpha(c, 0.22f * glow)), w * 3, round_.Get());
        dc_->DrawGeometry(g.Get(), Brush(c), w, round_.Get());
    }

    // Filled area between a polyline and a baseline y.
    void Area(const std::vector<D2D1_POINT_2F>& pts, float baseY, D2D1_COLOR_F c) {
        if (pts.size() < 2) return;
        ComPtr<ID2D1PathGeometry> g;
        if (FAILED(d2dFactory_->CreatePathGeometry(&g))) return;
        ComPtr<ID2D1GeometrySink> sink;
        g->Open(&sink);
        sink->BeginFigure(D2D1::Point2F(pts.front().x, baseY), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLines(pts.data(), static_cast<UINT32>(pts.size()));
        sink->AddLine(D2D1::Point2F(pts.back().x, baseY));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        dc_->FillGeometry(g.Get(), Brush(c));
    }

    // Arc from angle a0 to a1 (radians, clockwise, 0 = +x).
    void Arc(D2D1_POINT_2F c, float r, float a0, float a1, D2D1_COLOR_F color, float w) {
        float sweep = a1 - a0;
        if (sweep < 0.002f) return;
        sweep = std::min(sweep, 2 * kPi - 0.01f);
        a1 = a0 + sweep;
        ComPtr<ID2D1PathGeometry> g;
        if (FAILED(d2dFactory_->CreatePathGeometry(&g))) return;
        ComPtr<ID2D1GeometrySink> sink;
        g->Open(&sink);
        sink->BeginFigure(D2D1::Point2F(c.x + r * std::cos(a0), c.y + r * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(c.x + r * std::cos(a1), c.y + r * std::sin(a1)),
                                      D2D1::SizeF(r, r), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      sweep > kPi ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        dc_->DrawGeometry(g.Get(), Brush(color), w, round_.Get());
    }

    void Clip(const D2D1_RECT_F& r) { dc_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE); }
    void Unclip() { dc_->PopAxisAlignedClip(); }

    // ---- text ----
    void Text(const std::wstring& s, const D2D1_RECT_F& r, IDWriteTextFormat* f, D2D1_COLOR_F c,
              DWRITE_TEXT_ALIGNMENT ha = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT va = DWRITE_PARAGRAPH_ALIGNMENT_NEAR) {
        if (s.empty()) return;
        f->SetTextAlignment(ha);
        f->SetParagraphAlignment(va);
        dc_->DrawText(s.c_str(), static_cast<UINT32>(s.size()), f, r, Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void TextBrush(const std::wstring& s, const D2D1_RECT_F& r, IDWriteTextFormat* f, ID2D1Brush* b,
                   DWRITE_TEXT_ALIGNMENT ha = DWRITE_TEXT_ALIGNMENT_LEADING,
                   DWRITE_PARAGRAPH_ALIGNMENT va = DWRITE_PARAGRAPH_ALIGNMENT_NEAR) {
        f->SetTextAlignment(ha);
        f->SetParagraphAlignment(va);
        dc_->DrawText(s.c_str(), static_cast<UINT32>(s.size()), f, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    float TextWidth(const std::wstring& s, IDWriteTextFormat* f) {
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, 10000.0f, 100.0f, &layout))) {
            return 0;
        }
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }

    ID2D1LinearGradientBrush* TitleGrad() { return titleGrad_.Get(); }
    ID2D1LinearGradientBrush* ProgressGrad() { return progressGrad_.Get(); }
    ID2D1LinearGradientBrush* GreenGrad() { return greenGrad_.Get(); }
    ID2D1StrokeStyle* Round() { return round_.Get(); }
    ID2D1Bitmap* Logo() { return logo_.Get(); }
    ID2D1Bitmap* Mark() { return mark_.Get(); }
    ID2D1Bitmap* Brand() { return brand_.Get(); }

    void DrawBitmap(ID2D1Bitmap* bmp, const D2D1_RECT_F& dest, float opacity = 1.0f) {
        if (!bmp || opacity <= 0) return;
        dc_->DrawBitmap(bmp, dest, Clamp01(opacity), D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    }
    void DrawLogo(const D2D1_RECT_F& dest, float opacity = 1.0f) { DrawBitmap(logo_.Get(), dest, opacity); }
    void DrawMark(const D2D1_RECT_F& dest, float opacity = 1.0f) { DrawBitmap(mark_.Get(), dest, opacity); }
    void DrawBrand(const D2D1_RECT_F& dest, float opacity = 1.0f) { DrawBitmap(brand_.Get(), dest, opacity); }

    bool LatticeReady() const { return latticeReady_; }
    ID2D1Bitmap* Lattice() const { return latticeBmp_.Get(); }

    void RenderLattice(float time, const LatCell* cells, const ColStat* cols) {
        if (!latticeReady_ || !cells || !cols || !context_) return;
        float* hp = histUpload_.data();
        for (int i = 0; i < kLat * kLat; ++i) {
            const LatCell& c = cells[i];
            const float n = static_cast<float>(c.n);
            hp[0] = n;
            hp[1] = n > 0 ? static_cast<float>(c.idle) / n : 0.0f;
            hp[2] = n > 0 ? static_cast<float>(c.late) / n : 0.0f;
            hp[3] = n > 0 ? static_cast<float>(c.loop) / n : 0.0f;
            hp += 4;
        }
        float* sp = statUpload_.data();
        for (int x = 0; x < kLat; ++x) {
            const ColStat& c = cols[x];
            sp[0] = c.loopU;
            sp[1] = c.idleU;
            sp[2] = c.p99U;
            sp[3] = c.stdevU;
            sp += 4;
        }
        for (int x = 0; x < kLat; ++x) {
            const ColStat& c = cols[x];
            sp[0] = c.late01;
            sp[1] = c.score;
            sp[2] = c.present;
            sp[3] = 0.0f;
            sp += 4;
        }
        const UINT row = kLat * 4 * sizeof(float);
        context_->UpdateSubresource(histTex_.Get(), 0, nullptr, histUpload_.data(), row, 0);
        context_->UpdateSubresource(statTex_.Get(), 0, nullptr, statUpload_.data(), row, 0);
        struct {
            float time, res, pad0, pad1;
        } cb{time, static_cast<float>(kLatRT), 0, 0};
        context_->UpdateSubresource(latCB_.Get(), 0, nullptr, &cb, 0, 0);

        const float clear[4] = {0, 0, 0, 0};
        context_->OMSetRenderTargets(1, latticeRTV_.GetAddressOf(), nullptr);
        context_->ClearRenderTargetView(latticeRTV_.Get(), clear);
        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(kLatRT);
        vp.Height = static_cast<float>(kLatRT);
        vp.MaxDepth = 1;
        context_->RSSetViewports(1, &vp);
        context_->RSSetState(latRS_.Get());
        context_->OMSetBlendState(latBS_.Get(), nullptr, 0xFFFFFFFFu);
        context_->OMSetDepthStencilState(latDS_.Get(), 0);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(latVS_.Get(), nullptr, 0);
        context_->PSSetShader(latPS_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, latCB_.GetAddressOf());
        context_->PSSetShaderResources(0, 1, histSRV_.GetAddressOf());
        context_->PSSetShaderResources(1, 1, statSRV_.GetAddressOf());
        context_->PSSetSamplers(0, 1, latSamp_.GetAddressOf());
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* nullSRV[2] = {};
        context_->PSSetShaderResources(0, 2, nullSRV);
        ID3D11RenderTargetView* nullRTV = nullptr;
        context_->OMSetRenderTargets(1, &nullRTV, nullptr);
        context_->Flush();
    }

    // formats
    ComPtr<IDWriteTextFormat> fTitle, fPhase, fLabel, fSmall, fBody, fMono, fMonoSmall, fMonoBig, fHuge, fButton,
        fSub, fTileLabel;

private:
    bool InitLattice() {
        latticeReady_ = false;
        if (!device_ || !context_ || !dc_ || feature_ < D3D_FEATURE_LEVEL_10_0) return false;
        static const char* kSrc = R"lat(
cbuffer Frame : register(b0) { float time; float res; float2 _pad; };
Texture2D histTex : register(t0);
Texture2D statTex : register(t1);
SamplerState samp0 : register(s0);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID) {
    // Oversized triangle. uv must be 0 and 2 at the far vertices so the
    // visible [-1,1] screen interpolates to 0..1. Halving here crops the view.
    float2 p = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(p * 2.0 - 1.0, 0, 1);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}

float3 rotY(float3 p, float a) {
    float c = cos(a), s = sin(a);
    return float3(c * p.x + s * p.z, p.y, -s * p.x + c * p.z);
}
float3 rotX(float3 p, float a) {
    float c = cos(a), s = sin(a);
    return float3(p.x, c * p.y - s * p.z, s * p.y + c * p.z);
}
float3 rotZ(float3 p, float a) {
    float c = cos(a), s = sin(a);
    return float3(c * p.x - s * p.y, s * p.x + c * p.y, p.z);
}

float edgeDist(float3 p) {
    float3 d = 1.0 - abs(p);
    float a = min(d.x, min(d.y, d.z));
    float c = max(d.x, max(d.y, d.z));
    float b = d.x + d.y + d.z - a - c;
    return length(float2(a, b));
}

float4 ps(VSOut i) : SV_TARGET {
    float2 ndc;
    ndc.x = i.uv.x * 2.0 - 1.0;
    ndc.y = (1.0 - i.uv.y) * 2.0 - 1.0;
    float yaw = time * 0.32;
    float pitch = 0.40 + 0.16 * sin(time * 0.21);
    float roll = 0.08 * sin(time * 0.17);
    float3 ro = float3(0, 0, -3.65);
    float3 rd = normalize(float3(ndc * 0.70, 1.45));
    ro = rotZ(rotX(rotY(ro, yaw), pitch), roll);
    rd = rotZ(rotX(rotY(rd, yaw), pitch), roll);

    float3 bg = float3(0.012, 0.028, 0.07);
    bg *= 1.0 - 0.35 * dot(ndc, ndc);

    float3 inv = 1.0 / rd;
    float3 t0 = (-1.02 - ro) * inv;
    float3 t1 = ( 1.02 - ro) * inv;
    float3 tsm = min(t0, t1);
    float3 tbg = max(t0, t1);
    float tN = max(tsm.x, max(tsm.y, tsm.z));
    float tF = min(tbg.x, min(tbg.y, tbg.z));
    if (tF < max(tN, 0.0)) return float4(bg, 1);

    const float N = 128.0;
    const float cellW = 2.0 / N;
    float t = max(tN, 0.0);
    float3 pos = ro + rd * (t + 1e-4);
    int ix = clamp((int)floor((pos.x + 1.0) / cellW), 0, 127);
    int iy = clamp((int)floor((pos.y + 1.0) / cellW), 0, 127);
    int iz = clamp((int)floor((pos.z + 1.0) / cellW), 0, 127);
    int sx = rd.x < 0 ? -1 : 1;
    int sy = rd.y < 0 ? -1 : 1;
    int sz = rd.z < 0 ? -1 : 1;
    float rdx = rd.x >= 0 ? max(rd.x, 1e-6) : min(rd.x, -1e-6);
    float rdy = rd.y >= 0 ? max(rd.y, 1e-6) : min(rd.y, -1e-6);
    float rdz = rd.z >= 0 ? max(rd.z, 1e-6) : min(rd.z, -1e-6);
    float cellMinX = -1.0 + ix * cellW;
    float cellMinY = -1.0 + iy * cellW;
    float cellMinZ = -1.0 + iz * cellW;
    float tMaxX = ((rd.x >= 0 ? cellMinX + cellW : cellMinX) - ro.x) / rdx;
    float tMaxY = ((rd.y >= 0 ? cellMinY + cellW : cellMinY) - ro.y) / rdy;
    float tMaxZ = ((rd.z >= 0 ? cellMinZ + cellW : cellMinZ) - ro.z) / rdz;
    float tDeltaX = cellW / abs(rdx);
    float tDeltaY = cellW / abs(rdy);
    float tDeltaZ = cellW / abs(rdz);
    if (tMaxX < t) tMaxX += tDeltaX * (floor((t - tMaxX) / tDeltaX) + 1.0);
    if (tMaxY < t) tMaxY += tDeltaY * (floor((t - tMaxY) / tDeltaY) + 1.0);
    if (tMaxZ < t) tMaxZ += tDeltaZ * (floor((t - tMaxZ) / tDeltaZ) + 1.0);

    // Grid and measurements are accumulated separately so the mesh cannot
    // paint itself opaque and hide the heatmap behind it.
    float3 heat = 0;
    float heatA = 0;
    float3 wire = 0;
    float wireA = 0;
    // lattice-grey-04: ghost mesh, app blues on the measurements.
    const float3 grey = float3(0.55, 0.57, 0.60);
    float3 rimP = ro + rd * max(tN, 0.0);
    float rim = exp(-edgeDist(rimP) * edgeDist(rimP) * 140.0);
    float rimA = rim * 0.012;
    wire += grey * rimA;
    wireA += rimA;

    [loop]
    for (int s = 0; s < 260; ++s) {
        if (ix < 0 || iy < 0 || iz < 0 || ix > 127 || iy > 127 || iz > 127) break;
        float tExit = min(tMaxX, min(tMaxY, tMaxZ));
        float tHit = clamp(tExit, t, tF);
        float3 ph = ro + rd * tHit;
        float3 g = (ph + 1.0) * (0.5 * N);
        float3 fr = min(frac(g), 1.0 - frac(g));

        float d = 1.0;
        int plane = 0;
        if (tMaxX <= tExit + 1e-5) {
            d = min(fr.y, fr.z);
            plane = sx > 0 ? ix + 1 : ix;
        } else if (tMaxY <= tExit + 1e-5) {
            d = min(fr.x, fr.z);
            plane = sy > 0 ? iy + 1 : iy;
        } else {
            d = min(fr.x, fr.y);
            plane = sz > 0 ? iz + 1 : iz;
        }
        bool major = (plane & 7) == 0;
        float y01 = (iy + 0.5) / N;
        float z01 = (iz + 0.5) / N;
        // A few percent of grey at most. The mesh must not frost the cube.
        float mark = major ? exp(-d * d * 160.0) : exp(-d * d * 520.0);
        float wa = mark * (major ? 0.007 : 0.0018);
        if (wireA < 0.045 && wa > 0.0004) {
            float a = min(wa, 0.045 - wireA) * (1.0 - heatA);
            wire += grey * a;
            wireA += a;
        }

        float2 uv = (float2(ix, iy) + 0.5) / N;
        float4 h = histTex.SampleLevel(samp0, uv, 0);
        float4 st0 = statTex.SampleLevel(samp0, float2(uv.x, 0.25), 0);
        float4 st1 = statTex.SampleLevel(samp0, float2(uv.x, 0.75), 0);
        float n = h.r;

        // App palette: Blueprint #123A91, Electric #347BFF, Icy #8DB8FF, Cool White #F4F7FF.
        const float3 blueprint = float3(0.071, 0.227, 0.569);
        const float3 electric = float3(0.204, 0.482, 1.0);
        const float3 icy = float3(0.553, 0.722, 1.0);
        const float3 cool = float3(0.957, 0.969, 1.0);

        if (st1.z > 0.5) {
            float dLoop = abs(y01 - st0.x);
            float dIdle = abs(y01 - st0.y);
            float dP99 = abs(y01 - st0.z);
            float sig = max(st0.w, 1.6 / N);
            float band = exp(-pow(dLoop / sig, 2.0));
            float3 membrane = lerp(electric, icy, saturate(st1.x));
            float ba = band * 0.42;
            heat += membrane * ba * (1.0 - heatA);
            heatA += ba * (1.0 - heatA);
            if (abs(st0.y - st0.x) > 1.5 / N && dIdle < 0.55 / N) {
                float ia = 0.50;
                heat += icy * ia * (1.0 - heatA);
                heatA += ia * (1.0 - heatA);
            }
            if (dP99 < 0.55 / N) {
                float pa = 0.55;
                heat += lerp(icy, cool, 0.35) * pa * (1.0 - heatA);
                heatA += pa * (1.0 - heatA);
            }
        }

        // Z is the sample count in this (late %, latency) cell.
        float height = n >= 1.0 ? lerp(4.0 / N, 1.0, saturate(log(n) / log(64.0))) : 0.0;
        if (n >= 1.0 && z01 <= height) {
            float hot = saturate(log(n) / log(48.0));
            // Exact app blues: Electric #347BFF, Icy #8DB8FF. No extra gain,
            // or the voxels wash out to the same cyan as the old grid.
            float3 col = lerp(blueprint, electric, 0.50 + 0.50 * hot);
            col = lerp(col, icy, saturate(h.g));
            col = lerp(col, lerp(icy, cool, 0.4), saturate(h.b));
            float a = 0.94;
            heat += col * a * (1.0 - heatA);
            heatA += a * (1.0 - heatA);
        }

        if (heatA > 0.97 || tExit >= tF) break;
        if (tMaxX <= tExit + 1e-5) { ix += sx; tMaxX += tDeltaX; }
        if (tMaxY <= tExit + 1e-5) { iy += sy; tMaxY += tDeltaY; }
        if (tMaxZ <= tExit + 1e-5) { iz += sz; tMaxZ += tDeltaZ; }
    }

    float3 rgb = bg * (1.0 - heatA) + heat;
    rgb = rgb * (1.0 - wireA) + wire;
    return float4(saturate(rgb), 1);
}
)lat";

        auto compile = [&](const char* entry, const char* profile, ComPtr<ID3DBlob>& out) {
            ComPtr<ID3DBlob> err;
            const HRESULT hr = D3DCompile(kSrc, std::strlen(kSrc), "lattice.hlsl", nullptr, nullptr, entry, profile,
                                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err);
            if (FAILED(hr)) {
                if (err) OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
                return false;
            }
            return true;
        };
        ComPtr<ID3DBlob> vsb, psb;
        if (!compile("vs", "vs_4_0", vsb) || !compile("ps", "ps_4_0", psb)) return false;
        if (FAILED(device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &latVS_))) return false;
        if (FAILED(device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &latPS_))) return false;

        auto makeTex = [&](UINT w, UINT h, DXGI_FORMAT fmt, UINT bind, ComPtr<ID3D11Texture2D>& tex,
                           ComPtr<ID3D11ShaderResourceView>* srv) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = fmt;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = bind;
            if (FAILED(device_->CreateTexture2D(&td, nullptr, &tex))) return false;
            if (srv) {
                D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
                sv.Format = fmt;
                sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sv.Texture2D.MipLevels = 1;
                if (FAILED(device_->CreateShaderResourceView(tex.Get(), &sv, srv->ReleaseAndGetAddressOf()))) return false;
            }
            return true;
        };
        if (!makeTex(kLat, kLat, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, histTex_, &histSRV_)) return false;
        if (!makeTex(kLat, 2, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, statTex_, &statSRV_)) return false;
        if (!makeTex(kLatRT, kLatRT, DXGI_FORMAT_B8G8R8A8_UNORM,
                     D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, latticeTex_, nullptr))
            return false;
        if (FAILED(device_->CreateRenderTargetView(latticeTex_.Get(), nullptr, &latticeRTV_))) return false;

        D3D11_SAMPLER_DESC samp{};
        samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        if (FAILED(device_->CreateSamplerState(&samp, &latSamp_))) return false;

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device_->CreateBuffer(&bd, nullptr, &latCB_))) return false;

        // Direct2D leaves the shared context with scissor enabled. That rect is in
        // the swap-chain's pixels and misses this smaller target, so the draw
        // produces nothing unless scissor, blend, and depth are reset here.
        D3D11_RASTERIZER_DESC rsd{};
        rsd.FillMode = D3D11_FILL_SOLID;
        rsd.CullMode = D3D11_CULL_NONE;
        rsd.DepthClipEnable = TRUE;
        if (FAILED(device_->CreateRasterizerState(&rsd, &latRS_))) return false;
        D3D11_BLEND_DESC bld{};
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device_->CreateBlendState(&bld, &latBS_))) return false;
        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (FAILED(device_->CreateDepthStencilState(&dsd, &latDS_))) return false;

        ComPtr<IDXGISurface> surface;
        if (FAILED(latticeTex_.As(&surface))) return false;
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi_,
            dpi_);
        if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &latticeBmp_))) return false;

        histUpload_.assign(static_cast<size_t>(kLat) * kLat * 4, 0.0f);
        statUpload_.assign(static_cast<size_t>(kLat) * 2 * 4, 0.0f);
        latticeReady_ = true;
        return true;
    }

    bool CreateTarget() {
        ComPtr<IDXGISurface> surface;
        if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi_, dpi_);
        if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target_))) return false;
        dc_->SetTarget(target_.Get());
        return true;
    }

    ComPtr<IDWriteTextFormat> Format(const wchar_t* family, DWRITE_FONT_WEIGHT weight, float size, bool wrap = false,
                                     bool trim = true) {
        ComPtr<IDWriteTextFormat> f;
        dwrite_->CreateTextFormat(family, fonts_.Get(), weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                  size, L"en-us", &f);
        if (!f) {
            dwrite_->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
        }
        if (!f) return nullptr;
        f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (trim && !wrap) {
            ComPtr<IDWriteInlineObject> ellipsis;
            dwrite_->CreateEllipsisTrimmingSign(f.Get(), &ellipsis);
            DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            f->SetTrimming(&t, ellipsis.Get());
        }
        return f;
    }

    void LoadEmbeddedFonts() {
        fonts_.Reset();
        ComPtr<IDWriteFactory5> f5;
        if (FAILED(dwrite_.As(&f5))) return;
        if (!fontLoader_) {
            if (FAILED(f5->CreateInMemoryFontFileLoader(&fontLoader_))) return;
            if (FAILED(f5->RegisterFontFileLoader(fontLoader_.Get()))) {
                fontLoader_.Reset();
                return;
            }
        }
        ComPtr<IDWriteFontSetBuilder1> builder;
        if (FAILED(f5->CreateFontSetBuilder(&builder))) return;
        const wchar_t* names[] = {L"FONT_PHILOSOPHER", L"FONT_PHILOSOPHER_BOLD", L"FONT_BRUNO", L"FONT_GRUPPO"};
        for (const wchar_t* name : names) {
            HRSRC src = FindResourceW(nullptr, name, RT_RCDATA);
            if (!src) continue;
            HGLOBAL mem = LoadResource(nullptr, src);
            if (!mem) continue;
            void* data = LockResource(mem);
            const DWORD size = SizeofResource(nullptr, src);
            if (!data || !size) continue;
            ComPtr<IDWriteFontFile> file;
            if (SUCCEEDED(fontLoader_->CreateInMemoryFontFileReference(dwrite_.Get(), data, size, nullptr, &file)) &&
                file) {
                builder->AddFontFile(file.Get());
            }
        }
        ComPtr<IDWriteFontSet> set;
        if (FAILED(builder->CreateFontSet(&set)) || !set) return;
        ComPtr<IDWriteFontCollection1> coll;
        if (FAILED(f5->CreateFontCollectionFromFontSet(set.Get(), &coll))) return;
        fonts_ = coll;
    }

    void CreateFormats() {
        // luciuswayne.com: Bruno Ace SC wordmark, Philosopher body/headings, Gruppo labels.
        fTitle = Format(L"Bruno Ace SC", DWRITE_FONT_WEIGHT_NORMAL, 26);
        fPhase = Format(L"Philosopher", DWRITE_FONT_WEIGHT_BOLD, 16);
        fLabel = Format(L"Gruppo", DWRITE_FONT_WEIGHT_NORMAL, 12);
        fSmall = Format(L"Philosopher", DWRITE_FONT_WEIGHT_NORMAL, 12);
        // Gruppo 12 is wider than the default candidate tiles. Philosopher 8 fits
        // "MISSED AFTER IDLE" on one line inside a ~92px tile.
        fTileLabel = Format(L"Philosopher", DWRITE_FONT_WEIGHT_NORMAL, 8);
        fSub = Format(L"Gruppo", DWRITE_FONT_WEIGHT_NORMAL, 13);
        fBody = Format(L"Philosopher", DWRITE_FONT_WEIGHT_NORMAL, 13.5f, true, false);
        fMono = Format(L"Consolas", DWRITE_FONT_WEIGHT_NORMAL, 12.5f);
        fMonoSmall = Format(L"Consolas", DWRITE_FONT_WEIGHT_NORMAL, 10.5f);
        // 15pt fits the score arc in the default 1340×880 pane. Tile values use fMono.
        fMonoBig = Format(L"Consolas", DWRITE_FONT_WEIGHT_BOLD, 15);
        fHuge = Format(L"Bruno Ace SC", DWRITE_FONT_WEIGHT_NORMAL, 40);
        fButton = Format(L"Philosopher", DWRITE_FONT_WEIGHT_BOLD, 13.5f);
    }

    ComPtr<ID2D1LinearGradientBrush> MakeLinear(D2D1_COLOR_F a, D2D1_COLOR_F b) {
        D2D1_GRADIENT_STOP stops[2] = {{0.0f, a}, {1.0f, b}};
        ComPtr<ID2D1GradientStopCollection> coll;
        dc_->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &coll);
        ComPtr<ID2D1LinearGradientBrush> br;
        dc_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(1, 0)), coll.Get(), &br);
        return br;
    }

    ComPtr<ID2D1Bitmap> LoadPngResource(const wchar_t* name) {
        ComPtr<ID2D1Bitmap> out;
        HRSRC src = FindResourceW(nullptr, name, RT_RCDATA);
        if (!src) return out;
        HGLOBAL mem = LoadResource(nullptr, src);
        if (!mem) return out;
        void* data = LockResource(mem);
        const DWORD size = SizeofResource(nullptr, src);
        if (!data || !size) return out;
        ComPtr<IWICImagingFactory> wic;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return out;
        ComPtr<IWICStream> stream;
        if (FAILED(wic->CreateStream(&stream))) return out;
        if (FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(data), size))) return out;
        ComPtr<IWICBitmapDecoder> dec;
        if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &dec))) return out;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(dec->GetFrame(0, &frame))) return out;
        ComPtr<IWICFormatConverter> conv;
        if (FAILED(wic->CreateFormatConverter(&conv))) return out;
        if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeMedianCut)))
            return out;
        dc_->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &out);
        return out;
    }

    void LoadLogos() {
        logo_ = LoadPngResource(L"LOGO_PNG");
        mark_ = LoadPngResource(L"MARK_PNG");
        brand_ = LoadPngResource(L"BRAND_PNG");
        if (!mark_) mark_ = brand_ ? brand_ : logo_;
        if (!brand_) brand_ = mark_;
    }

    ComPtr<ID2D1RadialGradientBrush> MakeRadial(D2D1_COLOR_F c) {
        D2D1_GRADIENT_STOP stops[3] = {{0.0f, c}, {0.35f, Alpha(c, 0.35f)}, {1.0f, Alpha(c, 0.0f)}};
        ComPtr<ID2D1GradientStopCollection> coll;
        dc_->CreateGradientStopCollection(stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &coll);
        ComPtr<ID2D1RadialGradientBrush> br;
        dc_->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 0), 1, 1), coll.Get(), &br);
        return br;
    }

    HWND hwnd_ = nullptr;
    float dpi_ = 96;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IDWriteInMemoryFontFileLoader> fontLoader_;
    ComPtr<IDWriteFontCollection> fonts_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> round_, dash_, dot_;
    ComPtr<ID2D1RadialGradientBrush> glowCyan_, glowMagenta_, glowViolet_, glowAmber_, glowGreen_, glowWhite_,
        glowTeal_, glowRed_;
    ComPtr<ID2D1LinearGradientBrush> titleGrad_, progressGrad_, greenGrad_;
    ComPtr<ID2D1Bitmap> logo_;   // Lucius And Wayne producer mark (welcome only)
    ComPtr<ID2D1Bitmap> mark_;   // Synchronus icon (header, window)
    ComPtr<ID2D1Bitmap> brand_;  // Synchronus logo (welcome masthead)

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    D3D_FEATURE_LEVEL feature_ = D3D_FEATURE_LEVEL_9_1;
    ComPtr<ID3D11VertexShader> latVS_;
    ComPtr<ID3D11PixelShader> latPS_;
    ComPtr<ID3D11Buffer> latCB_;
    ComPtr<ID3D11SamplerState> latSamp_;
    ComPtr<ID3D11RasterizerState> latRS_;
    ComPtr<ID3D11BlendState> latBS_;
    ComPtr<ID3D11DepthStencilState> latDS_;
    ComPtr<ID3D11Texture2D> histTex_, statTex_, latticeTex_;
    ComPtr<ID3D11ShaderResourceView> histSRV_, statSRV_;
    ComPtr<ID3D11RenderTargetView> latticeRTV_;
    ComPtr<ID2D1Bitmap1> latticeBmp_;
    std::vector<float> histUpload_, statUpload_;
    bool latticeReady_ = false;
};

// ------------------------------------------------------------------- app
enum class Stage {
    Welcome,
    PreparePrompt, PrepareApplying, PrepareDone, PrepareFailed,
    Bios,
    Running, Result, Failed, Cancelled, Installing, Installed, InstallFailed,
    PpmPrompt, PpmApplying, PpmDone, PpmFailed,
    GpuClean,
    GpuPrompt, GpuApplying, GpuDone, GpuFailed,
    MemoryPrompt, MemoryApplying, MemoryDone, MemoryFailed,
    StoragePrompt, StorageApplying, StorageDone, StorageFailed,
    Review
};

constexpr int kStepCount = 10;

struct StepMeta {
    int number;
    const wchar_t* name;
    const wchar_t* title;
};

StepMeta Meta(Stage s) {
    switch (s) {
        case Stage::Welcome:
            return {0, L"Welcome", L"Thank you for using Synchronus"};
        case Stage::PreparePrompt:
        case Stage::PrepareApplying:
        case Stage::PrepareDone:
        case Stage::PrepareFailed:
            return {1, L"Prepare", L"Backup / restore point"};
        case Stage::Bios:
            return {2, L"BIOS", L"Lock the CPU and enable XMP"};
        case Stage::Running:
        case Stage::Failed:
        case Stage::Cancelled:
            return {3, L"Measure", L"Find this PC's fastest Sleep(1)"};
        case Stage::Result:
        case Stage::Installing:
        case Stage::Installed:
        case Stage::InstallFailed:
            return {4, L"Hold", L"Install the boot service"};
        case Stage::PpmPrompt:
        case Stage::PpmApplying:
        case Stage::PpmDone:
        case Stage::PpmFailed:
            return {5, L"CPU Tune", L"Ultimate Performance and PPM"};
        case Stage::GpuClean:
            return {6, L"GPU Clean", L"Latest driver, graphics only"};
        case Stage::GpuPrompt:
        case Stage::GpuApplying:
        case Stage::GpuDone:
        case Stage::GpuFailed:
            return {7, L"GPU Config", L"Power, latency, overlays"};
        case Stage::MemoryPrompt:
        case Stage::MemoryApplying:
        case Stage::MemoryDone:
        case Stage::MemoryFailed:
            return {8, L"Memory", L"Pagefile and compression"};
        case Stage::StoragePrompt:
        case Stage::StorageApplying:
        case Stage::StorageDone:
        case Stage::StorageFailed:
            return {9, L"Storage", L"SSD, PCIe, USB, TRIM"};
        case Stage::Review:
        default:
            return {10, L"Review", L"Validate, then restart"};
    }
}

bool IsTuneStage(Stage s) {
    switch (s) {
        case Stage::PreparePrompt:
        case Stage::PrepareApplying:
        case Stage::PrepareDone:
        case Stage::PrepareFailed:
        case Stage::Bios:
        case Stage::PpmPrompt:
        case Stage::PpmApplying:
        case Stage::PpmDone:
        case Stage::PpmFailed:
        case Stage::GpuClean:
        case Stage::GpuPrompt:
        case Stage::GpuApplying:
        case Stage::GpuDone:
        case Stage::GpuFailed:
        case Stage::MemoryPrompt:
        case Stage::MemoryApplying:
        case Stage::MemoryDone:
        case Stage::MemoryFailed:
        case Stage::StoragePrompt:
        case Stage::StorageApplying:
        case Stage::StorageDone:
        case Stage::StorageFailed:
        case Stage::Review:
            return true;
        default:
            return false;
    }
}

enum {
    kWantPrepare = 0,
    kWantBios,
    kWantMeasure,
    kWantHold,
    kWantCpu,
    kWantGpuClean,
    kWantGpu,
    kWantMemory,
    kWantStorage,
    kWantCount
};

struct Particle {
    float x, y, vx, vy, life, maxLife, size;
    D2D1_COLOR_F c;
};

class App {
public:
    explicit App(const Host& host) : host_(host) {
        fidelity_ = host_.fidelity;
        for (int i = 0; i < kWantCount; ++i) want_[i] = true;
        if (host_.prepareSkip) want_[kWantPrepare] = false;
        if (host_.ppmSkip) want_[kWantCpu] = false;
        if (host_.gpuSkip) want_[kWantGpu] = false;
        if (host_.memorySkip) want_[kWantMemory] = false;
        if (host_.storageSkip) want_[kWantStorage] = false;
    }

    int Run() {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const HINSTANCE hinst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &App::WndProcThunk;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hinst, MAKEINTRESOURCEW(101));
        if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = reinterpret_cast<HICON>(LoadImageW(hinst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        wc.lpszClassName = L"Synchronus.Gui";
        RegisterClassExW(&wc);

        const UINT dpi = GetDpiForSystem();
        dpi_ = static_cast<float>(dpi);
        RECT rc{0, 0, MulDiv(1340, dpi, 96), MulDiv(880, dpi, 96)};
        AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
        const int x = work.left + std::max(0L, (work.right - work.left - ww) / 2);
        const int y = work.top + std::max(0L, (work.bottom - work.top - wh) / 2);
        hwnd_ = CreateWindowExW(0, wc.lpszClassName,
                                L"Synchronus  \u00B7  Lucius And Wayne Systems LLC",
                                WS_OVERLAPPEDWINDOW, x, y, ww, wh, nullptr, nullptr, hinst, this);
        if (!hwnd_) return 1;

        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        COLORREF caption = RGB(7, 22, 56);
        DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
        DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &caption, sizeof(caption));

        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (!rd_.Init(hwnd_, dpi_)) {
            MessageBoxW(nullptr, L"Direct2D initialisation failed. Use the command line: Synchronus.exe --cli",
                        L"Synchronus", MB_ICONERROR);
            CoUninitialize();
            return 1;
        }
        ShowWindow(hwnd_, SW_SHOW);
        // The measurement thread takes realtime priority; keep the UI thread
        // well below it so rendering never delays a Sleep(1) wake-up.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        state_.System(host_.system());
        lastFrame_ = state_.Now();
        if (host_.autoInstall && host_.pinnedResolution) {
            {
                std::lock_guard<std::mutex> g(resultM_);
                result_.winner = host_.pinnedResolution;
                result_.optimum = host_.pinnedResolution;
            }
            stage_ = Stage::Result;
            StartInstall();
        } else if (host_.autoInstall) {
            StartBenchmark();
        } else {
            stage_ = Stage::Welcome;
            state_.Phase(L"WELCOME", L"thank you for using Synchronus");
        }
        StartUpdateCheck();

        MSG msg{};
        while (running_) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) running_ = false;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (!running_) break;
            if (IsIconic(hwnd_)) {
                WaitMessage();
                continue;
            }
            Frame();
        }

        gpuStop_ = true;
        updateStop_.store(true);
        state_.Cancel();
        if (updateThread_.joinable()) updateThread_.join();
        JoinWorker();
        if (serviceStopped_ && !installedThisSession_ && !relaunch_.load()) host_.restartService();
        CoUninitialize();
        return relaunch_.load() ? 2 : 0;
    }

private:
    enum class GpuDl { Idle, Looking, Offer, Downloading, Ready, Installing, Done, Failed, Manual };

    // ---- window plumbing ----
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        App* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        } else {
            self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self ? self->WndProc(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_SIZE:
                if (wp != SIZE_MINIMIZED) rd_.Resize(LOWORD(lp), HIWORD(lp));
                return 0;
            case WM_DPICHANGED: {
                dpi_ = static_cast<float>(HIWORD(wp));
                rd_.SetDpi(dpi_);
                const RECT* r = reinterpret_cast<const RECT*>(lp);
                SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case WM_GETMINMAXINFO: {
                MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lp);
                mmi->ptMinTrackSize.x = static_cast<LONG>(1040 * dpi_ / 96);
                mmi->ptMinTrackSize.y = static_cast<LONG>(700 * dpi_ / 96);
                return 0;
            }
            case WM_MOUSEMOVE:
                mouse_ = D2D1::Point2F(static_cast<short>(LOWORD(lp)) * 96.0f / dpi_,
                                       static_cast<short>(HIWORD(lp)) * 96.0f / dpi_);
                return 0;
            case WM_LBUTTONDOWN:
                mouseDown_ = true;
                SetCapture(hwnd_);
                return 0;
            case WM_LBUTTONUP:
                if (mouseDown_) {
                    clickPending_ = true;
                    clickPos_ = mouse_;
                }
                mouseDown_ = false;
                ReleaseCapture();
                return 0;
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT) {
                    SetCursor(LoadCursorW(nullptr, hoverButton_ ? IDC_HAND : IDC_ARROW));
                    return TRUE;
                }
                break;
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE) {
                    if (restartConfirm_) restartConfirm_ = false;
                    else PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                }
                if (wp == VK_RETURN) enterPending_ = true;
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
                ValidateRect(hwnd_, nullptr);
                return 0;
            case WM_CLOSE:
                DestroyWindow(hwnd_);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    // ---- worker control ----
    void JoinWorker() {
        if (worker_.joinable()) worker_.join();
    }

    void StartUpdateCheck() {
        if (!host_.checkUpdate) return;
        updateThread_ = std::thread([this] {
            AppUpdate offer;
            std::wstring err;
            const bool newer = host_.checkUpdate(offer, updateStop_, err);
            std::lock_guard<std::mutex> g(updateM_);
            updateOffer_ = std::move(offer);
            updateNewer_ = newer;
            updateReady_ = true;
        });
    }

    void StartApply(AppUpdate offer) {
        if (!host_.applyUpdate) return;
        if (updateThread_.joinable()) updateThread_.join();
        updateStop_.store(false);
        updateBusy_.store(true);
        {
            std::lock_guard<std::mutex> g(updateM_);
            updateFailed_ = false;
            updateLine_ = L"Downloading " + offer.version + L"\u2026";
        }
        updateThread_ = std::thread([this, offer = std::move(offer)] {
            std::wstring err;
            const int rc = host_.applyUpdate(
                offer,
                [this](const std::wstring& line) {
                    std::lock_guard<std::mutex> g(updateM_);
                    updateLine_ = line;
                },
                updateStop_, err);
            if (updateStop_.load()) return;
            if (rc == 0) {
                relaunch_.store(true);
                PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                return;
            }
            {
                std::lock_guard<std::mutex> g(updateM_);
                updateErr_ = err.empty() ? L"The update could not be installed." : err;
                updateLine_.clear();
                updateFailed_ = true;
            }
            updateBusy_.store(false);
        });
    }

    std::wstring UpdateStatusLine() {
        std::lock_guard<std::mutex> g(updateM_);
        return updateLine_;
    }

    // The version check starts with the window. Ask once, as soon as the
    // result is in, and only download after the user agrees.
    void PollUpdate() {
        std::wstring fail;
        {
            std::lock_guard<std::mutex> g(updateM_);
            if (updateFailed_) {
                updateFailed_ = false;
                fail = updateErr_;
            }
        }
        if (!fail.empty()) {
            MessageBoxW(hwnd_, fail.c_str(), L"Synchronus", MB_ICONWARNING | MB_OK);
            clickPending_ = false;
            enterPending_ = false;
        }
        if (updatePrompted_ || updateBusy_.load()) return;

        AppUpdate offer;
        bool ready = false;
        bool newer = false;
        {
            std::lock_guard<std::mutex> g(updateM_);
            ready = updateReady_;
            newer = updateNewer_;
            if (ready) offer = updateOffer_;
        }
        if (!ready) return;
        updatePrompted_ = true;
        if (!newer) return;

        std::wstring notes = offer.notes;
        if (notes.size() > 500) {
            notes.resize(500);
            notes += L"\u2026";
        }
        std::wstring text = L"Synchronus " + offer.version + L" is available.\nThis copy is " + host_.version + L".";
        if (!notes.empty()) text += L"\n\n" + notes;
        const bool installed = host_.system && host_.system().serviceState != 0;
        text += installed ? L"\n\nDownload it and replace this copy, including the installed service? Synchronus will close and open the new version."
                          : L"\n\nDownload it and replace this copy? Synchronus will close and open the new version.";
        const int answer = MessageBoxW(hwnd_, text.c_str(), L"Synchronus",
                                       MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2 | MB_SETFOREGROUND);
        clickPending_ = false;
        enterPending_ = false;
        if (answer == IDYES) StartApply(std::move(offer));
    }

    void StartBenchmark() {
        JoinWorker();
        state_.Reset();
        {
            std::lock_guard<std::mutex> g(resultM_);
            result_ = ResultView{};
        }
        {
            std::lock_guard<std::mutex> g(probeM_);
            probe_ = CpuProbeView{};
            probeReady_ = false;
        }
        {
            std::lock_guard<std::mutex> g(gpuM_);
            gpu_ = GpuProbeView{};
            gpuReady_ = false;
        }
        gpuSkipped_ = false;
        stage_ = Stage::Running;
        revealT_ = 0;
        installT_ = 0;
        particles_.clear();
        worker_ = std::thread([this] {
            ResultView r = host_.benchmark(state_, fidelity_);
            {
                std::lock_guard<std::mutex> g(resultM_);
                result_ = r;
            }
            if (r.stoppedService) serviceStopped_ = true;
            if (state_.Cancelled()) {
                stage_ = Stage::Cancelled;
                return;
            }
            if (r.winner == 0) {
                state_.Phase(L"FAILED", r.failure);
                stage_ = Stage::Failed;
                return;
            }
            state_.Progress(1.0, 0, 0);
            state_.Phase(L"RESULT", Fmt(L"%.4f ms selected after verification", Ms(r.winner)));
            stage_ = Stage::Result;
            if (host_.autoInstall) InstallNow();
        });
    }

    void InstallNow() {
        unsigned units = 0;
        {
            std::lock_guard<std::mutex> g(resultM_);
            units = result_.winner;
        }
        stage_ = Stage::Installing;
        state_.Phase(L"INSTALLING", L"copying the executable, writing the registry, starting the service");
        const int rc = host_.install(units, state_);
        if (rc == 2) {
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            return;
        }
        state_.System(host_.system());
        if (rc == 0) {
            installedThisSession_ = true;
            state_.Phase(L"INSTALLED", Fmt(L"the service holds %.4f ms now and at every boot", Ms(units)));
            if (host_.probeCpu) {
                CpuProbeView p = host_.probeCpu();
                std::lock_guard<std::mutex> g(probeM_);
                probe_ = p;
                probeReady_ = true;
            }
            stage_ = Stage::Installed;
        } else {
            state_.Phase(L"INSTALL FAILED", L"see the log for details");
            stage_ = Stage::InstallFailed;
        }
    }

    void StartInstall() {
        JoinWorker();
        worker_ = std::thread([this] { InstallNow(); });
    }

    CpuProbeView ProbeCopy() {
        std::lock_guard<std::mutex> g(probeM_);
        if (!probeReady_ && host_.probeCpu) {
            probe_ = host_.probeCpu();
            probeReady_ = true;
        }
        return probe_;
    }

    GpuProbeView GpuCopy() {
        std::lock_guard<std::mutex> g(gpuM_);
        if (!gpuReady_ && host_.probeGpu) {
            gpu_ = host_.probeGpu();
            gpuReady_ = true;
        }
        return gpu_;
    }

    bool Want(int i) const { return i >= 0 && i < kWantCount && want_[i]; }
    bool AnyWanted() const {
        for (int i = 0; i < kWantCount; ++i) {
            if (want_[i]) return true;
        }
        return false;
    }

    unsigned Winner() {
        std::lock_guard<std::mutex> g(resultM_);
        return result_.winner;
    }

    void StartAtOrAfter(int index) {
        for (int i = index; i < kWantCount; ++i) {
            if (!want_[i]) continue;
            if (host_.autoInstall && (i == kWantBios || i == kWantGpuClean)) continue;
            switch (i) {
                case kWantPrepare:
                    if (!host_.applyPrepare) continue;
                    if (host_.autoInstall) {
                        StartPrepare();
                        return;
                    }
                    stage_ = Stage::PreparePrompt;
                    state_.Phase(L"PREPARE", L"create a System Restore point");
                    return;
                case kWantBios:
                    stage_ = Stage::Bios;
                    state_.Phase(L"BIOS", L"lock the multiplier and enable XMP/DOCP");
                    return;
                case kWantMeasure:
                    StartBenchmark();
                    return;
                case kWantHold:
                    if (Winner() == 0 && host_.pinnedResolution == 0) continue;
                    if (Winner() == 0 && host_.pinnedResolution) {
                        std::lock_guard<std::mutex> g(resultM_);
                        result_.winner = host_.pinnedResolution;
                        result_.optimum = host_.pinnedResolution;
                    }
                    StartInstall();
                    return;
                case kWantCpu:
                    if (!host_.applyPpm) continue;
                    {
                        const CpuProbeView p = ProbeCopy();
                        if (host_.autoInstall) {
                            if (host_.ppmForce || p.mode == CpuProbeView::Mode::Locked) {
                                StartPpm();
                                return;
                            }
                            continue;
                        }
                        stage_ = Stage::PpmPrompt;
                        state_.Phase(L"CPU TUNE", p.mode == CpuProbeView::Mode::Locked
                                                     ? L"static frequency detected — PPM checks are just noise"
                                                     : p.mode == CpuProbeView::Mode::Dynamic
                                                           ? L"dynamic boost detected — 5000 ms interval would delay turbo"
                                                           : L"could not classify the multiplier");
                    }
                    return;
                case kWantGpuClean:
                    stage_ = Stage::GpuClean;
                    state_.Phase(L"GPU CLEAN", L"looking up the current driver");
                    BeginGpuLookup();
                    return;
                case kWantGpu:
                    if (!host_.applyGpu) continue;
                    {
                        const GpuProbeView g = GpuCopy();
                        if (host_.autoInstall) {
                            StartGpu();
                            return;
                        }
                        stage_ = Stage::GpuPrompt;
                        state_.Phase(L"GPU CONFIG", g.detail.empty() ? L"driver / Windows graphics settings" : g.detail);
                    }
                    return;
                case kWantMemory:
                    if (!host_.applyMemory) continue;
                    if (host_.autoInstall) {
                        StartMemory();
                        return;
                    }
                    stage_ = Stage::MemoryPrompt;
                    state_.Phase(L"MEMORY", L"pagefile and memory compression");
                    return;
                case kWantStorage:
                    if (!host_.applyStorage) continue;
                    if (host_.autoInstall) {
                        StartStorage();
                        return;
                    }
                    stage_ = Stage::StoragePrompt;
                    state_.Phase(L"STORAGE", L"SSD, PCIe, USB, TRIM");
                    return;
                default:
                    break;
            }
        }
        stage_ = Stage::Review;
        state_.Phase(L"REVIEW", L"validate, then restart");
    }

    void MaybeStartPpm() { StartAtOrAfter(kWantCpu); }
    void MaybeStartGpu() { StartAtOrAfter(kWantGpu); }
    void MaybeStartMemory() { StartAtOrAfter(kWantMemory); }
    void MaybeStartStorage() { StartAtOrAfter(kWantStorage); }

    void BeginWorkflow() { StartAtOrAfter(kWantPrepare); }

    void StartPpm() {
        JoinWorker();
        stage_ = Stage::PpmApplying;
        worker_ = std::thread([this] {
            state_.Phase(L"PPM", L"writing power-plan settings");
            const int rc = host_.applyPpm(state_);
            if (rc == 0) {
                state_.Phase(L"PPM APPLIED", L"every processor power setting written");
                stage_ = Stage::PpmDone;
            } else {
                state_.Phase(L"PPM FAILED", L"see the log for details");
                stage_ = Stage::PpmFailed;
            }
        });
    }

    void StartGpu() {
        JoinWorker();
        gpuSkipped_ = false;
        stage_ = Stage::GpuApplying;
        worker_ = std::thread([this] {
            state_.Phase(L"GPU CONFIG", L"writing driver / Windows graphics settings");
            const int rc = host_.applyGpu(state_);
            if (rc == 0) {
                state_.Phase(L"GPU CONFIG", L"HAGS, overlays, NVIDIA/AMD profile");
                stage_ = Stage::GpuDone;
            } else {
                state_.Phase(L"GPU CONFIG", L"see the log for details");
                stage_ = Stage::GpuFailed;
            }
        });
    }

    void StartPrepare() {
        JoinWorker();
        stage_ = Stage::PrepareApplying;
        worker_ = std::thread([this] {
            const int rc = host_.applyPrepare ? host_.applyPrepare(state_) : 1;
            stage_ = (rc == 0) ? Stage::PrepareDone : Stage::PrepareFailed;
        });
    }

    void StartMemory() {
        JoinWorker();
        stage_ = Stage::MemoryApplying;
        worker_ = std::thread([this] {
            const int rc = host_.applyMemory ? host_.applyMemory(state_) : 1;
            stage_ = (rc == 0) ? Stage::MemoryDone : Stage::MemoryFailed;
        });
    }

    void StartStorage() {
        JoinWorker();
        stage_ = Stage::StorageApplying;
        worker_ = std::thread([this] {
            const int rc = host_.applyStorage ? host_.applyStorage(state_) : 1;
            stage_ = (rc == 0) ? Stage::StorageDone : Stage::StorageFailed;
        });
    }

    // ---- frame ----
    void Frame() {
        if (!rd_.Valid() && !rd_.Init(hwnd_, dpi_)) {
            Sleep(100);
            return;
        }
        PollUpdate();
        const double now = state_.Now();
        const float dt = static_cast<float>(std::min(0.05, std::max(0.0, now - lastFrame_)));
        lastFrame_ = now;
        time_ += dt;

        state_.Snap(snap_);
        if (stage_.load() != lastStage_) {
            const Stage entered = stage_.load();
            if (entered == Stage::Result) revealT_ = 0.0001f;
            if (entered == Stage::Installed) {
                installT_ = 0.0001f;
                StartAtOrAfter(kWantCpu);
            }
            if (entered == Stage::PrepareDone) StartAtOrAfter(kWantBios);
            if (entered == Stage::PpmDone) StartAtOrAfter(kWantGpuClean);
            if (entered == Stage::GpuDone) StartAtOrAfter(kWantMemory);
            if (entered == Stage::MemoryDone) StartAtOrAfter(kWantStorage);
            if (entered == Stage::StorageDone) {
                stage_ = Stage::Review;
                state_.Phase(L"REVIEW", L"validate, then restart");
            }
            lastStage_ = stage_.load();
        }
        const Stage stage = stage_.load();
        if (revealT_ > 0) revealT_ = std::min(1.0f, revealT_ + dt * 0.8f);
        if (installT_ > 0) installT_ = std::min(1.0f, installT_ + dt * 1.2f);
        introT_ = std::min(1.0f, introT_ + dt * 1.4f);
        spinner_ += dt * 4.0f;

        buttons_.clear();
        hoverButton_ = false;

        const D2D1_SIZE_F size = rd_.Size();
        const float w = size.width, h = size.height;
        const float pad = 16;
        const D2D1_RECT_F header = D2D1::RectF(pad, pad, w - pad, pad + 78);
        const D2D1_RECT_F bar = D2D1::RectF(pad, header.bottom + 8, w - pad, header.bottom + 14);
        const D2D1_RECT_F footer = D2D1::RectF(pad, h - pad - 28, w - pad, h - pad);
        const D2D1_RECT_F body = D2D1::RectF(pad, bar.bottom + 14, w - pad, footer.top - 10);

        if (stage != Stage::Welcome && snap_.lat.size() == static_cast<size_t>(kLat) * kLat &&
            snap_.col.size() == static_cast<size_t>(kLat)) {
            rd_.RenderLattice(time_, snap_.lat.data(), snap_.col.data());
        }

        rd_.Begin();
        DrawBackground(w, h);
        if (stage == Stage::Welcome) {
            DrawWelcome(D2D1::RectF(pad, pad, w - pad, footer.top - 10), dt);
        } else {
            DrawHeader(header, bar, stage);
            const float gap = 12;
            const float leftW = std::floor((W(body) - gap) * 0.62f);
            const float finalH = IsTuneStage(stage) ? 360.0f : 236.0f;
            const D2D1_RECT_F chart = D2D1::RectF(body.left, body.top, body.left + leftW, body.bottom - finalH - gap);
            const D2D1_RECT_F finals = D2D1::RectF(body.left, chart.bottom + gap, body.left + leftW, body.bottom);
            const float rx = chart.right + gap;
            const float rh = H(body);
            const D2D1_RECT_F trace = D2D1::RectF(rx, body.top, body.right, body.top + std::floor(rh * 0.22f));
            const D2D1_RECT_F hist =
                D2D1::RectF(rx, trace.bottom + gap, body.right, trace.bottom + gap + std::floor(rh * 0.52f));
            const D2D1_RECT_F stats = D2D1::RectF(rx, hist.bottom + gap, body.right, body.bottom);
            DrawChart(chart, dt, stage);
            DrawTrace(trace, dt);
            DrawHistogram(hist, dt);
            DrawStats(stats, dt);
            DrawFinalists(finals, dt, stage);
        }
        DrawFooter(footer, dt);
        UpdateParticles(dt);
        DrawParticles();
        if (introT_ < 1) rd_.Fill(D2D1::RectF(0, 0, w, h), Alpha(col::bg, 1 - EaseOutCubic(introT_)));
        if (!rd_.End()) rd_.Release();

        clickPending_ = false;
        enterPending_ = false;
        lastSeq_ = snap_.sampleSeq;
    }

    // ---- background ----
    void DrawBackground(float w, float h) {
        const float t = time_;
        rd_.Glow(D2D1::Point2F(w * 0.22f + 70 * std::sin(t * 0.13f), h * 0.30f + 50 * std::cos(t * 0.11f)),
                 h * 0.75f, col::cyan, 0.07f);
        rd_.Glow(D2D1::Point2F(w * 0.78f + 80 * std::cos(t * 0.09f), h * 0.72f + 60 * std::sin(t * 0.12f)),
                 h * 0.80f, col::violet, 0.07f);
        rd_.Glow(D2D1::Point2F(w * 0.55f + 90 * std::sin(t * 0.07f), h * 0.05f), h * 0.5f, col::magenta, 0.035f);
        if (rd_.Mark()) {
            const float lh = h * 0.55f;
            const D2D1_SIZE_F sz = rd_.Mark()->GetSize();
            const float lw = lh * sz.width / std::max(1.0f, sz.height);
            rd_.DrawMark(D2D1::RectF(w - lw * 0.55f, (h - lh) * 0.42f, w + lw * 0.45f, (h - lh) * 0.42f + lh), 0.10f);
        }
    }

    // ---- header + progress ----
    void DrawHeader(const D2D1_RECT_F& r, const D2D1_RECT_F& bar, Stage stage) {
        float textLeft = r.left;
        if (rd_.Mark()) {
            const D2D1_SIZE_F sz = rd_.Mark()->GetSize();
            const float lh = H(r) - 4;
            const float lw = lh * sz.width / std::max(1.0f, sz.height);
            rd_.Glow(D2D1::Point2F(r.left + lw / 2, (r.top + r.bottom) / 2), lh * 0.9f, col::cyan, 0.25f);
            rd_.DrawMark(D2D1::RectF(r.left, r.top + 2, r.left + lw, r.top + 2 + lh), 1.0f);
            textLeft = r.left + lw + 14;
        }
        ID2D1LinearGradientBrush* grad = rd_.TitleGrad();
        grad->SetStartPoint(D2D1::Point2F(textLeft, r.top));
        grad->SetEndPoint(D2D1::Point2F(textLeft + 420, r.top));
        grad->SetOpacity(1);
        rd_.TextBrush(L"SYNCHRONUS", D2D1::RectF(textLeft, r.top, textLeft + 560, r.top + 32), rd_.fTitle.Get(),
                      grad);
        rd_.Text(L"Lucius And Wayne Systems LLC  \u00B7  luciuswayne.com",
                 D2D1::RectF(textLeft, r.top + 32, textLeft + 560, r.top + 50), rd_.fSmall.Get(), col::cyan);
        const StepMeta meta = Meta(stage);
        rd_.Text(meta.number ? Fmt(L"%s  \u2014  %s  \u00B7  v%s", meta.name, meta.title, host_.version.c_str())
                             : Fmt(L"latency tuner  \u00B7  v%s", host_.version.c_str()),
                 D2D1::RectF(textLeft, r.top + 50, textLeft + 560, r.top + 68), rd_.fSub.Get(), col::muted);

        // phase, right aligned
        const std::wstring phase = snap_.phase.empty() ? Upper(std::wstring(meta.name)) : snap_.phase;
        D2D1_COLOR_F phaseColor = col::cyan;
        if (stage == Stage::Result || stage == Stage::Installed || stage == Stage::PpmDone || stage == Stage::GpuDone ||
            stage == Stage::PrepareDone || stage == Stage::MemoryDone || stage == Stage::StorageDone ||
            stage == Stage::Review)
            phaseColor = col::green;
        if (stage == Stage::Failed || stage == Stage::InstallFailed || stage == Stage::Cancelled ||
            stage == Stage::PpmFailed || stage == Stage::GpuFailed || stage == Stage::PrepareFailed ||
            stage == Stage::MemoryFailed || stage == Stage::StorageFailed)
            phaseColor = col::red;
        if (stage == Stage::Installing || stage == Stage::PpmApplying || stage == Stage::GpuApplying ||
            stage == Stage::PrepareApplying || stage == Stage::MemoryApplying || stage == Stage::StorageApplying)
            phaseColor = col::amber;
        if (stage == Stage::PpmPrompt || stage == Stage::GpuPrompt || stage == Stage::Welcome ||
            stage == Stage::PreparePrompt || stage == Stage::Bios || stage == Stage::GpuClean ||
            stage == Stage::MemoryPrompt || stage == Stage::StoragePrompt)
            phaseColor = col::violet;
        const float pulse = 0.75f + 0.25f * std::sin(time_ * 3.0f);
        const D2D1_RECT_F pr = D2D1::RectF(r.left + 520, r.top + 2, r.right, r.top + 26);
        rd_.Text(meta.number ? Fmt(L"STEP %d OF %d  \u00B7  %s", meta.number, kStepCount, Upper(phase).c_str())
                             : Upper(phase),
                 pr, rd_.fPhase.Get(), Alpha(phaseColor, stage == Stage::Running ? pulse : 1.0f),
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
        std::wstring detail = snap_.detail;
        if (stage == Stage::Running && snap_.elapsed > 0) {
            detail += Fmt(L"   elapsed %s", Duration(snap_.elapsed).c_str());
            if (snap_.remaining > 0) detail += Fmt(L"  \u00B7  remaining ~%s", Duration(snap_.remaining).c_str());
        }
        rd_.Text(detail, D2D1::RectF(r.left + 520, r.top + 28, r.right, r.top + 48), rd_.fSmall.Get(), col::muted,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);

        // progress bar
        const float stepFrac = meta.number > 0 ? (static_cast<float>(meta.number - 1) +
                                                  (stage == Stage::Running ? static_cast<float>(snap_.progress) : 1.0f)) /
                                                     static_cast<float>(kStepCount)
                                               : 0.0f;
        progress_ = Approach(progress_, stage == Stage::Running ? static_cast<float>(snap_.progress) : stepFrac, 0.016f, 6.0f);
        rd_.FillRound(bar, 3, col::panel2);
        const float fx = bar.left + W(bar) * Clamp01(progress_);
        if (fx > bar.left + 2) {
            ID2D1LinearGradientBrush* pg = rd_.ProgressGrad();
            pg->SetStartPoint(D2D1::Point2F(bar.left, 0));
            pg->SetEndPoint(D2D1::Point2F(bar.right, 0));
            pg->SetOpacity(1);
            rd_.Dc()->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bar.left, bar.top, fx, bar.bottom), 3, 3), pg);
            // shimmer sliding along the filled part
            const float sh = std::fmod(time_ * 0.45f, 1.3f) - 0.15f;
            const float sx = bar.left + (fx - bar.left) * sh;
            rd_.Clip(D2D1::RectF(bar.left, bar.top, fx, bar.bottom));
            rd_.Glow(D2D1::Point2F(sx, (bar.top + bar.bottom) / 2), 60, col::white, 0.35f);
            rd_.Unclip();
            // comet head
            if (stage == Stage::Running) {
                rd_.Glow(D2D1::Point2F(fx, (bar.top + bar.bottom) / 2), 22, col::cyan, 0.9f);
                rd_.Dot(D2D1::Point2F(fx, (bar.top + bar.bottom) / 2), 3.5f, col::white);
            }
        }
    }

    // ---- shared panel chrome ----
    void Panel(const D2D1_RECT_F& r, const std::wstring& title, const std::wstring& sub) {
        rd_.FillRound(r, 10, col::panel);
        rd_.StrokeRound(r, 10, col::border, 1);
        rd_.Line(D2D1::Point2F(r.left + 12, r.top + 1), D2D1::Point2F(r.right - 12, r.top + 1), Alpha(col::white, 0.05f));
        const std::wstring t = Upper(title);
        const float tw = rd_.TextWidth(t, rd_.fLabel.Get());
        rd_.Text(t, D2D1::RectF(r.left + 16, r.top + 11, r.left + 16 + tw + 4, r.top + 28), rd_.fLabel.Get(), col::cyan);
        rd_.Text(sub, D2D1::RectF(r.left + 16 + tw + 12, r.top + 10, r.right - 16, r.top + 28), rd_.fSmall.Get(), col::dim);
    }

    struct ButtonRec {
        int id;
        D2D1_RECT_F r;
    };

    bool Button(int id, const D2D1_RECT_F& r, const std::wstring& label, D2D1_COLOR_F accent, bool primary,
                bool enabled, float dt, bool pulse = false, bool acceptEnter = false) {
        if (hover_.size() <= static_cast<size_t>(id)) hover_.resize(static_cast<size_t>(id) + 1, 0.0f);
        const bool over = enabled && PtIn(mouse_, r);
        if (over) hoverButton_ = true;
        hover_[id] = Approach(hover_[id], over ? 1.0f : 0.0f, dt, 12.0f);
        const float hv = hover_[id];
        const bool pressed = over && mouseDown_;
        const float breathe = pulse && enabled ? 0.5f + 0.5f * std::sin(time_ * 3.1f) : 0.0f;
        const D2D1_POINT_2F c = D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2);
        if (primary) {
            const float glowA = enabled ? (pulse ? 0.28f + 0.55f * breathe : 0.22f + 0.35f * hv) : 0.0f;
            rd_.Glow(c, W(r) * (0.55f + 0.35f * breathe), accent, glowA);
            if (pulse && enabled) rd_.Glow(c, W(r) * 1.15f, accent, 0.10f + 0.16f * breathe);
            D2D1_COLOR_F fill = enabled ? MixC(accent, col::white, 0.10f * hv + 0.08f * breathe) : col::panel2;
            if (pressed) fill = MixC(accent, col::bg, 0.25f);
            rd_.FillRound(r, 8, fill);
            rd_.StrokeRound(r, 8, Alpha(col::white, (pulse ? 0.28f : 0.18f) + 0.25f * hv + 0.25f * breathe),
                            pulse ? 1.6f : 1.0f);
            rd_.Text(label, r, rd_.fButton.Get(), enabled ? col::text : col::dim, DWRITE_TEXT_ALIGNMENT_CENTER,
                     DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        } else {
            rd_.FillRound(r, 8, Alpha(accent, enabled ? 0.08f + 0.12f * hv : 0.03f));
            rd_.StrokeRound(r, 8, Alpha(accent, enabled ? 0.45f + 0.4f * hv : 0.15f));
            rd_.Text(label, r, rd_.fButton.Get(), enabled ? MixC(accent, col::white, 0.3f * hv) : col::dim,
                     DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        buttons_.push_back({id, r});
        return enabled && ((clickPending_ && PtIn(clickPos_, r)) || (acceptEnter && enterPending_));
    }

    bool CheckRow(int id, const D2D1_RECT_F& cell, bool checked, bool locked, const wchar_t* label, float dt) {
        if (hover_.size() <= static_cast<size_t>(id)) hover_.resize(static_cast<size_t>(id) + 1, 0.0f);
        const bool over = !locked && PtIn(mouse_, cell);
        if (over) hoverButton_ = true;
        hover_[id] = Approach(hover_[id], over ? 1.0f : 0.0f, dt, 14.0f);
        const float hv = hover_[id];
        rd_.FillRound(cell, 8, checked ? Alpha(col::cyan, 0.16f + 0.10f * hv) : Alpha(col::panel2, 0.92f + 0.08f * hv));
        rd_.StrokeRound(cell, 8, checked ? Alpha(col::cyan, 0.55f + 0.30f * hv) : Alpha(col::border, 0.85f), 1.2f);
        const float bs = 16.0f;
        const float by = cell.top + (H(cell) - bs) * 0.5f;
        const D2D1_RECT_F box = D2D1::RectF(cell.left + 10, by, cell.left + 10 + bs, by + bs);
        rd_.FillRound(box, 3.5f, checked ? MixC(col::cyan, col::white, 0.08f) : Alpha(col::bg, 0.85f));
        rd_.StrokeRound(box, 3.5f, checked ? col::text : (locked ? col::dim : col::muted), 1.3f);
        if (checked) {
            const D2D1_POINT_2F a = D2D1::Point2F(box.left + 3.2f, box.top + bs * 0.52f);
            const D2D1_POINT_2F m = D2D1::Point2F(box.left + bs * 0.40f, box.bottom - 3.4f);
            const D2D1_POINT_2F b = D2D1::Point2F(box.right - 3.0f, box.top + 3.6f);
            rd_.Line(a, m, col::white, 1.9f, rd_.Round());
            rd_.Line(m, b, col::white, 1.9f, rd_.Round());
        }
        rd_.Text(label, D2D1::RectF(box.right + 8, cell.top, cell.right - 8, cell.bottom), rd_.fSmall.Get(),
                 locked ? col::dim : col::text, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        return over && clickPending_ && PtIn(clickPos_, cell);
    }

    // ---- sweep chart ----
    void DrawChart(const D2D1_RECT_F& R, float dt, Stage stage) {
        Panel(R, L"Sweep", L"overshoot vs requested resolution");
        const D2D1_RECT_F P = Inset(R, 60, 44, 18, 52);
        const auto& items = snap_.sweep.items;
        const size_t n = items.size();
        if (n == 0) {
            rd_.Text(L"Measuring the untuned Sleep(1) baseline\u2026", P, rd_.fSub.Get(), col::dim,
                     DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            return;
        }
        if (anScore_.size() != n) {
            anScore_.assign(n, 0);
            anMean_.assign(n, 0);
            anMedian_.assign(n, 0);
            anLate_.assign(n, 0);
            anIdle_.assign(n, 0);
            anAppear_.assign(n, 0);
        }
        float maxV = 0.3f;
        for (size_t i = 0; i < n; ++i) {
            const CandidateView& c = items[i];
            if (!c.hasData) {
                anAppear_[i] = Approach(anAppear_[i], 0.0f, dt, 8.0f);
                continue;
            }
            const bool first = anAppear_[i] <= 0.0f;
            anAppear_[i] = std::min(1.0f, anAppear_[i] + dt * 3.0f);
            const float sp = first ? 1e9f : 9.0f;
            anScore_[i] = Approach(anScore_[i], static_cast<float>(c.score), dt, sp);
            anMean_[i] = Approach(anMean_[i], static_cast<float>(c.loop.mean), dt, sp);
            anMedian_[i] = Approach(anMedian_[i], static_cast<float>(c.loop.median), dt, sp);
            anLate_[i] = Approach(anLate_[i], static_cast<float>(c.loop.latePct), dt, sp);
            anIdle_[i] = Approach(anIdle_[i], static_cast<float>(c.idle.mean), dt, sp);
            maxV = std::max({maxV, anScore_[i], anMean_[i], anMedian_[i], anIdle_[i]});
        }
        yMax_ = Approach(yMax_, maxV * 1.12f, dt, 4.0f);
        const float yMin = -0.04f;
        auto X = [&](size_t i) { return P.left + (static_cast<float>(i) + 0.5f) * W(P) / static_cast<float>(n); };
        auto Y = [&](float v) {
            return P.bottom - (std::min(std::max(v, yMin), yMax_) - yMin) / (yMax_ - yMin) * H(P);
        };
        const float colW = W(P) / static_cast<float>(n);

        // grid
        const float step = yMax_ > 1.0f ? 0.25f : yMax_ > 0.5f ? 0.1f : 0.05f;
        for (float v = 0; v < yMax_; v += step) {
            const float y = Y(v);
            rd_.Line(D2D1::Point2F(P.left, y), D2D1::Point2F(P.right, y), v == 0 ? Alpha(col::muted, 0.5f) : col::grid);
            rd_.Text(Fmt(L"%+.2f", v), D2D1::RectF(R.left + 10, y - 8, P.left - 8, y + 8), rd_.fMonoSmall.Get(),
                     col::muted, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        const size_t lab = std::max<size_t>(1, n / 10);
        for (size_t i = 0; i < n; i += lab) {
            const float x = X(i);
            rd_.Line(D2D1::Point2F(x, P.bottom), D2D1::Point2F(x, P.bottom + 4), col::muted);
            rd_.Text(Fmt(L"%.3f", Ms(items[i].requested)), D2D1::RectF(x - 30, P.bottom + 18, x + 30, P.bottom + 32),
                     rd_.fMonoSmall.Get(), col::muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        rd_.Text(L"ms", D2D1::RectF(P.right - 30, P.bottom + 18, P.right + 10, P.bottom + 32), rd_.fMonoSmall.Get(),
                 col::dim, DWRITE_TEXT_ALIGNMENT_TRAILING);

        // late% heat strip just below the axis
        for (size_t i = 0; i < n; ++i) {
            if (anAppear_[i] <= 0) continue;
            const float a = Clamp01(anLate_[i] / 35.0f) * anAppear_[i];
            rd_.Fill(D2D1::RectF(X(i) - colW / 2, P.bottom + 7, X(i) + colW / 2, P.bottom + 15),
                     MixC(Alpha(col::cyan, 0.15f), col::magenta, a));
        }
        rd_.Text(L"late %", D2D1::RectF(R.left + 10, P.bottom + 3, P.left - 8, P.bottom + 19), rd_.fMonoSmall.Get(),
                 col::dim, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        // live smoothing over +-2 neighbours (what the selection uses)
        std::vector<float> sm(n, 0);
        std::vector<bool> has(n, false);
        int leadIdx = -1;
        for (size_t i = 0; i < n; ++i) {
            if (anAppear_[i] <= 0) continue;
            float sum = 0;
            int cnt = 0;
            for (size_t j = (i >= 2 ? i - 2 : 0); j <= i + 2 && j < n; ++j) {
                if (anAppear_[j] <= 0) continue;
                sum += anScore_[j];
                ++cnt;
            }
            sm[i] = sum / static_cast<float>(cnt);
            has[i] = true;
            if (leadIdx < 0 || sm[i] <= sm[leadIdx]) leadIdx = static_cast<int>(i);
        }

        rd_.Clip(D2D1::RectF(P.left - 6, R.top + 30, P.right + 6, P.bottom + 1));
        // series: build segments broken at gaps
        auto Series = [&](const std::vector<float>& vals, D2D1_COLOR_F c, float w, bool glow, bool area) {
            std::vector<D2D1_POINT_2F> pts;
            auto flush = [&] {
                if (pts.size() >= 2) {
                    if (area) rd_.Area(pts, P.bottom, Alpha(c, 0.08f));
                    if (glow) rd_.GlowLine(pts, c, w);
                    else rd_.Polyline(pts, c, w);
                } else if (pts.size() == 1) {
                    rd_.Dot(pts[0], w, c);
                }
                pts.clear();
            };
            for (size_t i = 0; i < n; ++i) {
                if (!has[i]) {
                    flush();
                    continue;
                }
                pts.push_back(D2D1::Point2F(X(i), Y(vals[i])));
            }
            flush();
        };
        Series(anMedian_, Alpha(col::teal, 0.55f), 1.0f, false, false);
        Series(anIdle_, Alpha(col::violet, 0.55f), 1.0f, false, false);
        Series(anMean_, Alpha(col::cyan, 0.9f), 1.2f, false, true);
        Series(sm, col::amber, 1.8f, true, false);
        for (size_t i = 0; i < n; ++i) {
            if (!has[i]) continue;
            const float pop = EaseOutBack(anAppear_[i]);
            rd_.Dot(D2D1::Point2F(X(i), Y(anScore_[i])), 2.6f * pop, Alpha(col::violet, 0.9f));
            rd_.Dot(D2D1::Point2F(X(i), Y(anMean_[i])), 2.0f * pop, col::cyan);
            if (anAppear_[i] < 1) {
                rd_.Glow(D2D1::Point2F(X(i), Y(anMean_[i])), 26 * (1 - anAppear_[i]) + 6, col::cyan, 1 - anAppear_[i]);
            }
        }

        // active candidate band
        const int active = snap_.activeIdx;
        if (active >= 0 && static_cast<size_t>(active) < n && stage == Stage::Running) {
            const float tx = X(static_cast<size_t>(active));
            activeX_ = activeX_ < 0 ? tx : Approach(activeX_, tx, dt, 14.0f);
            const float pulse = 0.5f + 0.5f * std::sin(time_ * 5.0f);
            rd_.Fill(D2D1::RectF(activeX_ - colW, P.top, activeX_ + colW, P.bottom), Alpha(col::cyan, 0.05f + 0.05f * pulse));
            rd_.Line(D2D1::Point2F(activeX_, P.top), D2D1::Point2F(activeX_, P.bottom), Alpha(col::cyan, 0.5f + 0.4f * pulse));
            rd_.Glow(D2D1::Point2F(activeX_, P.top + 6), 18, col::cyan, 0.8f);
            rd_.Text(Fmt(L"%.4f", Ms(items[static_cast<size_t>(active)].requested)),
                     D2D1::RectF(activeX_ - 40, P.top - 16, activeX_ + 40, P.top), rd_.fMonoSmall.Get(), col::cyan,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            activeX_ = -1;
        }

        // markers
        const int best = snap_.sweep.bestIdx >= 0 ? snap_.sweep.bestIdx : leadIdx;
        if (best >= 0 && has[static_cast<size_t>(best)]) {
            const D2D1_POINT_2F p = D2D1::Point2F(X(static_cast<size_t>(best)), Y(sm[static_cast<size_t>(best)]));
            const bool final = snap_.sweep.bestIdx >= 0;
            rd_.Glow(p, 16, col::amber, final ? 0.9f : 0.5f);
            Diamond(p, 5.5f, final ? col::amber : Alpha(col::amber, 0.7f), final);
            rd_.Text(Fmt(L"%s %.4f", final ? L"best" : L"leading", Ms(items[static_cast<size_t>(best)].requested)),
                     D2D1::RectF(p.x - 60, p.y - 30, p.x + 60, p.y - 14), rd_.fMonoSmall.Get(), col::amber,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (snap_.sweep.targetIdx >= 0 && static_cast<size_t>(snap_.sweep.targetIdx) < n) {
            const float x = X(static_cast<size_t>(snap_.sweep.targetIdx));
            rd_.Dashed(D2D1::Point2F(x, P.top + 14), D2D1::Point2F(x, P.bottom), Alpha(col::green, 0.6f));
            rd_.Text(Fmt(L"target %.4f (+%.4f margin)", Ms(items[static_cast<size_t>(snap_.sweep.targetIdx)].requested),
                         Ms(snap_.sweep.margin)),
                     D2D1::RectF(x + 6, P.top + 4, x + 220, P.top + 18), rd_.fMonoSmall.Get(), Alpha(col::green, 0.9f));
        }
        if (snap_.sweep.boundaryIdx >= 0 && static_cast<size_t>(snap_.sweep.boundaryIdx) < n) {
            const float x = X(static_cast<size_t>(snap_.sweep.boundaryIdx));
            rd_.Dotted(D2D1::Point2F(x, P.top + 30), D2D1::Point2F(x, P.bottom), Alpha(col::teal, 0.6f));
            rd_.Text(L"no missed ticks from here", D2D1::RectF(x + 6, P.top + 22, x + 200, P.top + 36),
                     rd_.fMonoSmall.Get(), Alpha(col::teal, 0.8f));
        }
        // selected (after verification)
        unsigned winner = 0;
        {
            std::lock_guard<std::mutex> g(resultM_);
            winner = result_.winner;
        }
        if (winner && revealT_ > 0) {
            for (size_t i = 0; i < n; ++i) {
                if (items[i].requested != winner || !has[i]) continue;
                const D2D1_POINT_2F p = D2D1::Point2F(X(i), Y(anScore_[i]));
                const float e = EaseOutCubic(revealT_);
                rd_.Ring(p, 10 + 140 * e, Alpha(col::green, (1 - e) * 0.8f), 2);
                rd_.Ring(p, 10 + 70 * e, Alpha(col::green, (1 - e) * 0.5f), 1.5f);
                const float pulse = 0.5f + 0.5f * std::sin(time_ * 3.0f);
                rd_.Glow(p, 30 + 6 * pulse, col::green, 0.9f);
                rd_.Ring(p, 8 + 2 * pulse, col::green, 2);
                rd_.Dot(p, 3.5f, col::white);
                rd_.Text(Fmt(L"SELECTED %.4f ms", Ms(winner)), D2D1::RectF(p.x - 80, p.y + 12, p.x + 80, p.y + 28),
                         rd_.fLabel.Get(), col::green, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
        rd_.Unclip();

        // legend, right-aligned in the panel header so it never sits on the plot
        const float ly = R.top + 19;
        const wchar_t* legs[] = {L"mean (loop)", L"median", L"mean (idle)", L"score"};
        const D2D1_COLOR_F lcol[] = {col::cyan, col::teal, col::violet, col::amber};
        float legendW = 0;
        for (const wchar_t* s : legs) legendW += 12.0f + rd_.TextWidth(s, rd_.fSmall.Get()) + 18.0f;
        float lx = R.right - 16 - legendW;
        if (lx < R.left + 220) lx = R.left + 220;
        for (int i = 0; i < 4; ++i) lx = Legend(lx, ly, legs[i], lcol[i]);
        (void)lx;
    }

    float Legend(float x, float cy, const wchar_t* label, D2D1_COLOR_F c) {
        rd_.Dot(D2D1::Point2F(x + 4, cy), 3.5f, c);
        const std::wstring s = label;
        const float tw = rd_.TextWidth(s, rd_.fSmall.Get());
        rd_.Text(s, D2D1::RectF(x + 12, cy - 9, x + 12 + tw + 4, cy + 9), rd_.fSmall.Get(), col::muted,
                 DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        return x + 12 + tw + 18;
    }

    void Diamond(D2D1_POINT_2F p, float r, D2D1_COLOR_F c, bool filled) {
        const D2D1_POINT_2F pts[5] = {{p.x, p.y - r}, {p.x + r, p.y}, {p.x, p.y + r}, {p.x - r, p.y}, {p.x, p.y - r}};
        if (filled) {
            ComPtr<ID2D1PathGeometry> g = rd_.Poly(pts, 5, true);
            if (g) rd_.Dc()->FillGeometry(g.Get(), rd_.Brush(c));
            rd_.Ring(p, r + 3, Alpha(c, 0.5f), 1);
        } else {
            std::vector<D2D1_POINT_2F> v(pts, pts + 5);
            rd_.Polyline(v, c, 1.5f);
        }
    }

    // ---- live trace ----
    void DrawTrace(const D2D1_RECT_F& R, float dt) {
        (void)dt;
        Panel(R, L"Live", L"Sleep(1) overshoot, held until the next wake");
        const D2D1_RECT_F P = Inset(R, 48, 40, 64, 22);
        auto Y = [&](float v) {
            return P.bottom - (std::min(std::max(v, kTraceMin), kTraceMax) - kTraceMin) / (kTraceMax - kTraceMin) * H(P);
        };
        for (float v = 0; v <= 1.0f; v += 0.25f) {
            const float y = Y(v);
            rd_.Line(D2D1::Point2F(P.left, y), D2D1::Point2F(P.right, y), v == 0 ? Alpha(col::muted, 0.5f) : col::grid);
            rd_.Text(Fmt(L"%+.2f", v), D2D1::RectF(R.left + 8, y - 8, P.left - 6, y + 8), rd_.fMonoSmall.Get(), col::muted,
                     DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        const auto& tr = snap_.trace;
        const unsigned req = tr.empty() ? snap_.histRequested : tr.back().requested;
        if (req) {
            // clock ticks for the current candidate: the first tick >= 1 ms
            // (2R - 1 late), then one tick later each
            const float r = static_cast<float>(Ms(req));
            const float base = std::max(0.0f, 2 * r - 1);
            const wchar_t* names[] = {L"2nd tick", L"3rd tick", L"4th tick"};
            for (int k = 0; k < 3; ++k) {
                const float v = base + k * r;
                if (v > kTraceMax) break;
                const float y = Y(v);
                const D2D1_COLOR_F c = k == 0 ? col::amber : col::magenta;
                rd_.Dotted(D2D1::Point2F(P.left, y), D2D1::Point2F(P.right, y), Alpha(c, 0.55f));
                rd_.Text(names[k], D2D1::RectF(P.right + 6, y - 8, R.right - 6, y + 8), rd_.fMonoSmall.Get(),
                         Alpha(c, 0.9f), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
        if (tr.empty()) {
            rd_.Text(L"waiting for samples\u2026", P, rd_.fSub.Get(), col::dim, DWRITE_TEXT_ALIGNMENT_CENTER,
                     DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            return;
        }
        rd_.Clip(D2D1::RectF(P.left, R.top + 30, P.right + 2, P.bottom + 4));
        const size_t n = tr.size();
        auto X = [&](size_t i) {
            return P.left + (static_cast<float>(i + (kTraceCount - n))) / static_cast<float>(kTraceCount - 1) * W(P);
        };
        std::vector<D2D1_POINT_2F> pts;
        pts.reserve(n);
        for (size_t i = 0; i < n; ++i) pts.push_back(D2D1::Point2F(X(i), Y(tr[i].v)));

        // Sample-and-hold heartbeat: each Sleep(1) holds its overshoot as a
        // horizontal until the next wake. No spike stems, no sparkle.
        for (size_t i = 0; i < n; ++i) {
            const ::gui::Sample& s = tr[i];
            const bool late = s.v > LateThreshold(s.requested);
            const D2D1_COLOR_F c = late ? col::magenta : (s.idle ? col::teal : col::cyan);
            const float x1 = (i + 1 < n) ? pts[i + 1].x : P.right;
            rd_.Line(pts[i], D2D1::Point2F(x1, pts[i].y), Alpha(c, 0.92f), 1.85f, rd_.Round());
        }
        for (size_t i = 0; i < n; ++i) {
            const ::gui::Sample& s = tr[i];
            const bool late = s.v > LateThreshold(s.requested);
            const D2D1_COLOR_F c = late ? col::magenta : (s.idle ? col::teal : col::cyan);
            if (late) rd_.Glow(pts[i], 6.0f, col::magenta, 0.28f);
            if (s.idle) rd_.Ring(pts[i], 2.0f, c, 1.15f);
            else rd_.Dot(pts[i], 2.0f, c);
        }
        rd_.Unclip();
        float lx = P.left;
        lx = Legend(lx, R.bottom - 11, L"back-to-back", col::cyan);
        lx = Legend(lx, R.bottom - 11, L"after idle", col::teal);
        lx = Legend(lx, R.bottom - 11, L"missed a tick", col::magenta);
        (void)lx;
    }

    // ---- measurement lattice ----
    void DrawHistogram(const D2D1_RECT_F& R, float dt) {
        (void)dt;
        const int pass = snap_.sweep.pass;
        const int passes = snap_.sweep.passes;
        const bool verify = !snap_.finalists.empty();
        Panel(R, L"Lattice",
              verify ? Fmt(L"verification  \u00B7  128\u00B3  \u00B7  n = %u  \u00B7  missed %u", snap_.histTotal,
                           snap_.histLate)
                     : pass ? Fmt(L"pass %d of %d  \u00B7  late %% \u00D7 latency \u00D7 n  \u00B7  n = %u",
                                  pass, std::max(1, passes), snap_.histTotal)
                            : L"late %  \u00D7  measured latency  \u00D7  sample n");
        const D2D1_RECT_F P = Inset(R, 8, 36, 8, 8);
        if (!rd_.LatticeReady()) {
            rd_.Text(L"Lattice renderer unavailable on this device.", P, rd_.fBody.Get(), col::dim,
                     DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            return;
        }
        const float side = std::min(W(P), H(P));
        const float cx = (P.left + P.right) * 0.5f;
        const float cy = (P.top + P.bottom) * 0.5f;
        rd_.DrawBitmap(rd_.Lattice(),
                       D2D1::RectF(cx - side * 0.5f, cy - side * 0.5f, cx + side * 0.5f, cy + side * 0.5f), 1.0f);
    }

    // ---- statistics ----
    void DrawStats(const D2D1_RECT_F& R, float dt) {
        const CandidateView* c = nullptr;
        std::wstring who;
        const auto& items = snap_.sweep.items;
        if (snap_.activeFinalist >= 0 && static_cast<size_t>(snap_.activeFinalist) < snap_.finalists.size() &&
            snap_.finalists[static_cast<size_t>(snap_.activeFinalist)].hasData) {
            c = &snap_.finalists[static_cast<size_t>(snap_.activeFinalist)];
            who = L"verification";
        } else if (snap_.activeIdx >= 0 && static_cast<size_t>(snap_.activeIdx) < items.size() &&
                   items[static_cast<size_t>(snap_.activeIdx)].hasData) {
            c = &items[static_cast<size_t>(snap_.activeIdx)];
            who = L"sweep";
        } else if (snap_.activeIdx >= 0 && static_cast<size_t>(snap_.activeIdx) < items.size()) {
            who = L"first pass";
        }
        unsigned winner = 0;
        {
            std::lock_guard<std::mutex> g(resultM_);
            winner = result_.winner;
        }
        if (winner) {
            for (const auto& f : snap_.finalists) {
                if (f.requested == winner) {
                    c = &f;
                    who = L"selected";
                }
            }
        }
        const unsigned req = c ? c->requested : (snap_.activeIdx >= 0 && static_cast<size_t>(snap_.activeIdx) < items.size()
                                                     ? items[static_cast<size_t>(snap_.activeIdx)].requested
                                                     : 0);
        Panel(R, L"Candidate", req ? Fmt(L"%.4f ms  \u00B7  %s", Ms(req), who.c_str()) : L"statistics of the candidate being measured");

        const float targets[7] = {
            c ? static_cast<float>(c->loop.median) : 0, c ? static_cast<float>(c->loop.mean) : 0,
            c ? static_cast<float>(c->loop.stdev) : 0,  c ? static_cast<float>(c->loop.latePct) : 0,
            c ? static_cast<float>(c->idle.mean) : 0,   c ? static_cast<float>(c->idle.latePct) : 0,
            c ? static_cast<float>(c->score) : 0};
        for (int i = 0; i < 7; ++i) statV_[i] = Approach(statV_[i], targets[i], dt, 8.0f);

        const D2D1_RECT_F P = Inset(R, 16, 40, 16, 14);
        const float gaugeW = std::min(170.0f, W(P) * 0.34f);
        const float tilesW = W(P) - gaugeW - 12;
        const float tw = (tilesW - 16) / 3, th = (H(P) - 8) / 2;
        const wchar_t* labels[6] = {L"median", L"mean", L"stdev", L"missed ticks", L"mean after idle", L"missed after idle"};
        const D2D1_COLOR_F accents[6] = {col::teal, col::cyan, col::cyan, col::magenta, col::violet, col::magenta};
        for (int i = 0; i < 6; ++i) {
            const int cx = i % 3, cy = i / 3;
            const D2D1_RECT_F t = D2D1::RectF(P.left + cx * (tw + 8), P.top + cy * (th + 8), P.left + cx * (tw + 8) + tw,
                                              P.top + cy * (th + 8) + th);
            std::wstring value = L"\u2014";
            if (c) {
                if (i == 3 || i == 5) value = Fmt(L"%.1f%%", statV_[i]);
                else if (i == 2) value = Fmt(L"%.4f", statV_[i]);
                else value = Fmt(L"%+.4f", statV_[i]);
            }
            Tile(t, labels[i], value, (i == 3 || i == 5) ? L"" : L"ms", accents[i], c != nullptr);
        }
        // score gauge
        const D2D1_RECT_F G = D2D1::RectF(P.right - gaugeW, P.top, P.right, P.bottom);
        rd_.FillRound(G, 8, col::panel2);
        const D2D1_POINT_2F gc = D2D1::Point2F((G.left + G.right) / 2, (G.top + G.bottom) / 2 + 4);
        const float gr = std::min(W(G), H(G)) / 2 - 16;
        const float a0 = 0.75f * kPi, a1 = 2.25f * kPi;
        rd_.Arc(gc, gr, a0, a1, col::border, 7);
        const float frac = c ? Clamp01(statV_[6] / 0.8f) : 0.0f;
        const D2D1_COLOR_F gcol = MixC(col::green, col::magenta, Clamp01((frac - 0.3f) / 0.5f));
        if (frac > 0) {
            rd_.Arc(gc, gr, a0, a0 + (a1 - a0) * frac, Alpha(gcol, 0.25f), 14);
            rd_.Arc(gc, gr, a0, a0 + (a1 - a0) * frac, gcol, 7);
            const float ea = a0 + (a1 - a0) * frac;
            rd_.GlowDot(D2D1::Point2F(gc.x + gr * std::cos(ea), gc.y + gr * std::sin(ea)), 4, gcol, 1);
        }
        rd_.Text(c ? Fmt(L"%.3f", statV_[6]) : L"\u2014", D2D1::RectF(G.left, gc.y - 22, G.right, gc.y + 6),
                 rd_.fMonoBig.Get(), col::text, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        rd_.Text(L"SCORE", D2D1::RectF(G.left, gc.y + 8, G.right, gc.y + 24), rd_.fLabel.Get(), col::muted,
                 DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    void Tile(const D2D1_RECT_F& r, const wchar_t* label, const std::wstring& value, const wchar_t* unit,
              D2D1_COLOR_F accent, bool live) {
        rd_.FillRound(r, 8, col::panel2);
        rd_.Fill(D2D1::RectF(r.left, r.top + 10, r.left + 3, r.bottom - 10), Alpha(accent, live ? 0.9f : 0.3f));
        rd_.Text(Upper(label), D2D1::RectF(r.left + 8, r.top + 6, r.right - 4, r.top + 20), rd_.fTileLabel.Get(),
                 col::muted);
        const float vw = rd_.TextWidth(value, rd_.fMono.Get());
        rd_.Text(value, D2D1::RectF(r.left + 12, r.top + 22, r.right - 8, r.bottom - 2), rd_.fMono.Get(),
                 live ? col::text : col::dim, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        rd_.Text(unit, D2D1::RectF(r.left + 16 + vw, r.top + 30, r.right - 4, r.bottom - 4), rd_.fSmall.Get(), col::dim,
                 DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // ---- finalists + result ----
    void DrawFinalists(const D2D1_RECT_F& R, float dt, Stage stage) {
        const StepMeta meta = Meta(stage);
        const wchar_t* sub = (stage == Stage::Running || stage == Stage::Result)
                                 ? L"interleaved re-measure of the leading candidates"
                                 : meta.title;
        Panel(R, meta.name, sub);
        const float split = std::floor(W(R) * 0.56f);
        const D2D1_RECT_F L = D2D1::RectF(R.left + 16, R.top + 40, R.left + split, R.bottom - 12);
        const D2D1_RECT_F B = D2D1::RectF(R.left + split + 12, R.top + 36, R.right - 16, R.bottom - 12);
        rd_.Line(D2D1::Point2F(R.left + split, R.top + 44), D2D1::Point2F(R.left + split, R.bottom - 16), col::border);

        const auto& fin = snap_.finalists;
        if (fin.empty()) {
            rd_.Text(L"Top candidates are re-measured in interleaved rounds under the same load. The lowest "
                     L"verified score at or above the safety margin is selected.",
                     L, rd_.fBody.Get(), col::dim);
        } else {
            if (finBar_.size() != fin.size()) finBar_.assign(fin.size(), 0.0f);
            double maxScore = 0.01;
            for (const auto& f : fin) if (f.hasData) maxScore = std::max(maxScore, f.score);
            const float rowH = std::min(28.0f, H(L) / static_cast<float>(fin.size()));
            const float barX0 = L.left + 92, barX1 = L.right - 150;
            unsigned winner = 0;
            {
                std::lock_guard<std::mutex> g(resultM_);
                winner = result_.winner;
            }
            for (size_t i = 0; i < fin.size(); ++i) {
                const CandidateView& f = fin[i];
                const float y = L.top + i * rowH;
                const D2D1_RECT_F row = D2D1::RectF(L.left, y, L.right, y + rowH - 3);
                const bool active = static_cast<int>(i) == snap_.activeFinalist && stage == Stage::Running;
                const bool selected = winner && f.requested == winner;
                const bool eligible = f.requested >= snap_.target;
                if (active) rd_.FillRound(row, 6, Alpha(col::cyan, 0.06f + 0.04f * std::sin(time_ * 5)));
                if (selected) rd_.FillRound(row, 6, Alpha(col::green, 0.10f));
                rd_.Text(Fmt(L"%.4f ms", Ms(f.requested)), D2D1::RectF(L.left + 8, y, barX0, y + rowH - 3), rd_.fMono.Get(),
                         selected ? col::green : (eligible ? col::text : col::muted), DWRITE_TEXT_ALIGNMENT_LEADING,
                         DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                const float target = f.hasData ? static_cast<float>(f.score / (maxScore * 1.1)) : 0.0f;
                finBar_[i] = Approach(finBar_[i], target, dt, 8.0f);
                const float cy = y + (rowH - 3) / 2;
                rd_.FillRound(D2D1::RectF(barX0, cy - 5, barX1, cy + 5), 5, col::panel2);
                const float bx = barX0 + (barX1 - barX0) * finBar_[i];
                if (bx > barX0 + 2) {
                    D2D1_COLOR_F bc = selected ? col::green : (eligible ? col::cyan : col::dim);
                    rd_.FillRound(D2D1::RectF(barX0, cy - 5, bx, cy + 5), 5, Alpha(bc, 0.85f));
                    if (selected || active) rd_.Glow(D2D1::Point2F(bx, cy), 16, bc, 0.7f);
                }
                if (f.hasData) {
                    rd_.Text(Fmt(L"%.4f", f.score), D2D1::RectF(barX1 + 8, y, barX1 + 60, y + rowH - 3), rd_.fMono.Get(),
                             col::text, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    rd_.Text(Fmt(L"%.1f%%  n=%u", f.loop.latePct, f.loop.n + f.idle.n),
                             D2D1::RectF(barX1 + 62, y, L.right, y + rowH - 3), rd_.fMonoSmall.Get(), col::muted,
                             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                }
                if (!eligible) {
                    rd_.Text(L"below target", D2D1::RectF(barX1 + 62, y, L.right, y + rowH - 3), rd_.fMonoSmall.Get(),
                             Alpha(col::dim, f.hasData ? 0.0f : 1.0f), DWRITE_TEXT_ALIGNMENT_LEADING,
                             DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                }
            }
            rd_.Text(L"score (mean + stdev)", D2D1::RectF(barX0, R.top + 24, barX1, R.top + 38), rd_.fMonoSmall.Get(), col::dim);
        }

        DrawResultBanner(B, dt, stage);
    }

    void RestartComputer() {
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
            TOKEN_PRIVILEGES tp{};
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
            AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
            CloseHandle(token);
        }
        InitiateSystemShutdownExW(
            nullptr,
            const_cast<LPWSTR>(L"Synchronus: restarting so kernel, HAGS, and Resizable BAR settings take effect."),
            0, TRUE, TRUE,
            SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_MAINTENANCE | SHTDN_REASON_FLAG_PLANNED);
    }

    bool DrawFinishButtons(const D2D1_RECT_F& B, float dt) {
        const float cx = (B.left + B.right) / 2;
        const float bw = std::min(250.0f, (W(B) - 12) / 2);
        const D2D1_RECT_F btnA = D2D1::RectF(cx - bw - 6, B.bottom - 38, cx - 6, B.bottom - 4);
        const D2D1_RECT_F btnB = D2D1::RectF(cx + 6, B.bottom - 38, cx + 6 + bw, B.bottom - 4);
        bool acted = false;
        if (restartConfirm_) {
            rd_.Text(L"Windows will restart immediately. Save anything you still need, then confirm.",
                     D2D1::RectF(B.left + 8, B.bottom - 62, B.right - 8, B.bottom - 42), rd_.fSmall.Get(), col::amber,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            if (Button(12, btnA, L"Restart now", col::green, true, true, dt, true, true)) {
                RestartComputer();
                acted = true;
            }
            if (Button(13, btnB, L"Not yet", col::muted, false, true, dt)) {
                restartConfirm_ = false;
                acted = true;
            }
            return acted;
        }
        if (Button(11, btnA, L"Restart computer", col::green, true, true, dt, true, true)) {
            restartConfirm_ = true;
            acted = true;
        }
        if (Button(4, btnB, L"Close", col::muted, false, true, dt)) {
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            acted = true;
        }
        return acted;
    }

    void DrawWelcome(const D2D1_RECT_F& R, float dt) {
        const float cx = (R.left + R.right) / 2;
        const float cardW = std::min(780.0f, W(R) - 40);
        const D2D1_RECT_F card = D2D1::RectF(cx - cardW / 2, R.top + 8, cx + cardW / 2, R.bottom);
        rd_.FillRound(card, 16, Alpha(col::panel, 0.92f));
        rd_.StrokeRound(card, 16, Alpha(col::cyan, 0.22f), 1.4f);
        rd_.Glow(D2D1::Point2F(cx, card.top + 72), 200, col::cyan, 0.18f + 0.08f * (0.5f + 0.5f * std::sin(time_ * 1.4f)));
        rd_.Glow(D2D1::Point2F(cx, card.top + 72), 260, col::violet, 0.10f);

        float y = card.top + 10;
        if (rd_.Mark()) {
            const D2D1_SIZE_F sz = rd_.Mark()->GetSize();
            const float lh = 88.0f;
            const float lw = lh * sz.width / std::max(1.0f, sz.height);
            rd_.Glow(D2D1::Point2F(cx, y + lh / 2), 90, col::cyan, 0.40f);
            rd_.DrawMark(D2D1::RectF(cx - lw / 2, y, cx + lw / 2, y + lh), 1.0f);
            y += lh + 2;
        }
        ID2D1LinearGradientBrush* grad = rd_.TitleGrad();
        grad->SetStartPoint(D2D1::Point2F(card.left, y));
        grad->SetEndPoint(D2D1::Point2F(card.right, y));
        grad->SetOpacity(1);
        rd_.TextBrush(L"SYNCHRONUS", D2D1::RectF(card.left, y, card.right, y + 32), rd_.fTitle.Get(), grad,
                      DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 32;

        const std::wstring company = L"Lucius And Wayne Systems LLC";
        const float nameW = rd_.TextWidth(company, rd_.fSub.Get());
        float oldH = 24.0f, oldW = 0.0f;
        if (rd_.Logo()) {
            const D2D1_SIZE_F sz = rd_.Logo()->GetSize();
            oldW = oldH * sz.width / std::max(1.0f, sz.height);
        }
        const float cluster = oldW + (oldW > 0 ? 10.0f : 0.0f) + nameW;
        float x0 = cx - cluster / 2;
        if (rd_.Logo() && oldW > 0) {
            rd_.DrawLogo(D2D1::RectF(x0, y + (22 - oldH) / 2, x0 + oldW, y + (22 - oldH) / 2 + oldH), 1.0f);
            x0 += oldW + 10;
        }
        rd_.Text(company, D2D1::RectF(x0, y, x0 + nameW + 8, y + 22), rd_.fSub.Get(), col::cyan,
                 DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        y += 36;
        rd_.Text(L"Timer resolution, CPU power, GPU, memory, and storage. Low-level settings are tuned to harden the machine for a more responsive, lower-latency configuration, without unneeded versatility or fluff. Perfect for gamers and HPC. Uncheck any step you want to skip.",
                 D2D1::RectF(card.left + 48, y, card.right - 48, y + 78), rd_.fBody.Get(), col::muted,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 86;

        rd_.Text(L"Select the steps to run  \u2014  everything is on by default",
                 D2D1::RectF(card.left + 36, y, card.right - 220, y + 22), rd_.fSmall.Get(), col::muted,
                 DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        const D2D1_RECT_F allR = D2D1::RectF(card.right - 210, y, card.right - 128, y + 22);
        const D2D1_RECT_F noneR = D2D1::RectF(card.right - 120, y, card.right - 36, y + 22);
        if (Button(40, allR, L"Select all", col::cyan, false, true, dt)) {
            for (int i = 0; i < kWantCount; ++i) want_[i] = true;
        }
        if (Button(41, noneR, L"Clear", col::muted, false, true, dt)) {
            for (int i = 0; i < kWantCount; ++i) want_[i] = false;
        }
        y += 28;

        static const wchar_t* kLabels[kWantCount + 1] = {
            L"Prepare \u2014 restore point",
            L"BIOS \u2014 lock clocks, XMP/DOCP",
            L"Measure \u2014 Sleep(1) sweep",
            L"Hold \u2014 boot service",
            L"CPU Tune \u2014 Ultimate + PPM",
            L"GPU Clean \u2014 latest driver, no extras",
            L"GPU Config \u2014 power & latency",
            L"Memory \u2014 pagefile, compression",
            L"Storage \u2014 ASPM, TRIM, USB",
            L"Review \u2014 always last",
        };
        const float colW = (W(card) - 88) / 2;
        for (int i = 0; i < kWantCount + 1; ++i) {
            const float colN = (i < 5) ? 0.0f : 1.0f;
            const float row = static_cast<float>(i % 5);
            const float x = card.left + 36 + colN * (colW + 16);
            const float yy = y + row * 34;
            const D2D1_RECT_F cell = D2D1::RectF(x, yy, x + colW, yy + 30);
            const bool locked = (i == kWantCount);
            const bool on = locked ? true : want_[i];
            if (CheckRow(20 + i, cell, on, locked, kLabels[i], dt) && !locked) {
                want_[i] = !want_[i];
            }
        }

        if (Want(kWantMeasure)) {
            auto about = [](double seconds) {
                const int s = std::max(1, static_cast<int>(std::lround(seconds)));
                if (s < 90) {
                    const int r = std::max(5, ((s + 2) / 5) * 5);
                    return Fmt(L"~%d s", r);
                }
                const int min = s / 60;
                int sec = ((s % 60 + 5) / 10) * 10;
                if (sec >= 60) return Fmt(L"~%d min", min + 1);
                if (sec == 0) return Fmt(L"~%d min", min);
                return Fmt(L"~%d min %d s", min, sec);
            };
            const MeasurePlan plans[3] = {MeasurePlanFor(0), MeasurePlanFor(1), MeasurePlanFor(2)};
            const wchar_t* names[3] = {L"Low", L"Medium", L"High"};
            rd_.Text(L"How thoroughly should each interval be measured?",
                     D2D1::RectF(card.left + 36, card.bottom - 168, card.right - 36, card.bottom - 148), rd_.fSmall.Get(),
                     col::muted, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const float gap = 8;
            const float bw = (W(card) - 72 - gap * 2) / 3;
            const float by = card.bottom - 144;
            for (int i = 0; i < 3; ++i) {
                const float x = card.left + 36 + i * (bw + gap);
                const bool on = fidelity_ == i;
                const std::wstring label = Fmt(L"%s   %s", names[i], about(plans[i].seconds).c_str());
                if (Button(50 + i, D2D1::RectF(x, by, x + bw, by + 32), label, col::cyan, on, true, dt)) fidelity_ = i;
            }
            const MeasurePlan& pick = plans[std::max(0, std::min(2, fidelity_))];
            const int idle = std::max(pick.samples / 2, 5);
            rd_.Text(Fmt(L"%s%d back-to-back and %d after idle, %d pass%s", fidelity_ == 1 ? L"Default. " : L"",
                         pick.samples, idle, pick.passes, pick.passes == 1 ? L"" : L"es"),
                     D2D1::RectF(card.left + 36, card.bottom - 108, card.right - 36, card.bottom - 90), rd_.fMonoSmall.Get(),
                     col::dim, DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        const bool canBegin = AnyWanted() && !updateBusy_.load();
        std::wstring hint = canBegin ? L"Restart at the end so HAGS and Resizable BAR take effect. Press Enter to begin."
                                     : L"Select at least one step to tune";
        D2D1_COLOR_F hintCol = canBegin ? col::dim : col::red;
        if (updateBusy_.load()) {
            hint = UpdateStatusLine();
            if (hint.empty()) hint = L"Downloading the update\u2026";
            hintCol = col::cyan;
        }
        rd_.Text(hint, D2D1::RectF(card.left + 36, card.bottom - 86, card.right - 36, card.bottom - 52), rd_.fSmall.Get(),
                 hintCol, DWRITE_TEXT_ALIGNMENT_CENTER);
        const float bw = std::min(240.0f, (W(card) - 80) / 2);
        const D2D1_RECT_F btnA = D2D1::RectF(cx - bw - 8, card.bottom - 46, cx - 8, card.bottom - 12);
        const D2D1_RECT_F btnB = D2D1::RectF(cx + 8, card.bottom - 46, cx + 8 + bw, card.bottom - 12);
        if (Button(2, btnA, L"Begin", col::cyan, true, canBegin, dt, canBegin, canBegin)) BeginWorkflow();
        if (Button(4, btnB, L"Close", col::muted, false, true, dt)) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }

    void DrawResultBanner(const D2D1_RECT_F& B, float dt, Stage stage) {
        ResultView r;
        {
            std::lock_guard<std::mutex> g(resultM_);
            r = result_;
        }
        const float cx = (B.left + B.right) / 2;
        const float bw = std::min(270.0f, (W(B) - 16) / 2);
        const D2D1_RECT_F btnA = D2D1::RectF(cx - bw - 6, B.bottom - 38, cx - 6, B.bottom - 4);
        const D2D1_RECT_F btnB = D2D1::RectF(cx + 6, B.bottom - 38, cx + 6 + bw, B.bottom - 4);

        switch (stage) {
            case Stage::Running: {
                const D2D1_POINT_2F sc = D2D1::Point2F(B.left + 22, B.top + 22);
                rd_.Arc(sc, 11, 0, 2 * kPi, col::border, 2.4f);
                rd_.Arc(sc, 11, spinner_, spinner_ + 1.6f, col::cyan, 2.4f);
                rd_.Text(snap_.narrateTitle.empty() ? (snap_.phase.empty() ? L"starting" : snap_.phase)
                                                    : snap_.narrateTitle,
                         D2D1::RectF(B.left + 42, B.top + 8, B.right - 8, B.top + 32), rd_.fPhase.Get(), col::text);
                rd_.Text(snap_.narrateBody.empty() ? snap_.detail : snap_.narrateBody,
                         D2D1::RectF(B.left + 8, B.top + 36, B.right - 8, B.bottom - (snap_.hasBaseline ? 64 : 44)),
                         rd_.fBody.Get(), col::muted);
                if (snap_.hasBaseline) {
                    rd_.Text(Fmt(L"untuned Sleep(1) overshoots by %+.2f ms  \u2014  default ~15.6 ms clock",
                                 snap_.baseline.median),
                             D2D1::RectF(B.left + 8, B.bottom - 62, B.right - 8, B.bottom - 44), rd_.fMonoSmall.Get(),
                             col::dim);
                }
                if (Button(1, D2D1::RectF(cx - 70, B.bottom - 36, cx + 70, B.bottom - 4), L"Stop", col::red, false, true, dt)) {
                    state_.Cancel();
                }
                break;
            }
            case Stage::Result:
            case Stage::Installing:
            case Stage::Installed:
            case Stage::InstallFailed: {
                const float e = EaseOutBack(std::min(1.0f, revealT_ * 1.3f));
                const D2D1_POINT_2F center = D2D1::Point2F(cx, B.top + 34);
                const float pulse = 0.5f + 0.5f * std::sin(time_ * 2.5f);
                rd_.Glow(center, 120 * e, col::green, 0.35f + 0.15f * pulse);
                const std::wstring big = Fmt(L"%.4f ms", Ms(r.winner));
                const float scale = 0.6f + 0.4f * e;
                D2D1_MATRIX_3X2_F old{};
                rd_.Dc()->GetTransform(&old);
                rd_.Dc()->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, center) * old);
                rd_.Text(big, D2D1::RectF(B.left - 200, center.y - 28, B.right + 200, center.y + 28), rd_.fHuge.Get(),
                         col::green, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                rd_.Dc()->SetTransform(old);
                rd_.Text(Fmt(L"best measured %.4f ms  +  %.4f ms safety margin", Ms(r.optimum), Ms(r.margin)),
                         D2D1::RectF(B.left, B.top + 66, B.right, B.top + 84), rd_.fSmall.Get(), col::muted,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
                rd_.Text(Fmt(L"verified over %u samples: median %+.4f, mean %+.4f, %.1f%% missed a tick", r.loop.n + r.idle.n,
                             r.loop.median, r.loop.mean, r.loop.latePct),
                         D2D1::RectF(B.left, B.top + 84, B.right, B.top + 100), rd_.fMonoSmall.Get(), col::dim,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
                if (stage == Stage::Result) {
                    if (Want(kWantHold)) {
                        rd_.Text(L"Install a boot service that holds this period at every startup.",
                                 D2D1::RectF(B.left, B.top + 108, B.right, B.top + 132), rd_.fSmall.Get(), col::muted,
                                 DWRITE_TEXT_ALIGNMENT_CENTER);
                        if (Button(2, btnA, L"Continue  \u2014  install service", col::green, true, snap_.system.admin, dt, true,
                                   true)) {
                            StartInstall();
                        }
                        if (Button(4, btnB, L"Close", col::muted, false, true, dt)) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                        if (!snap_.system.admin) {
                            rd_.Text(L"installing needs administrator rights",
                                     D2D1::RectF(B.left, B.bottom - 56, B.right, B.bottom - 42), rd_.fMonoSmall.Get(), col::red,
                                     DWRITE_TEXT_ALIGNMENT_CENTER);
                        }
                    } else {
                        rd_.Text(L"The boot service will not be installed. Continue to the next selected step.",
                                 D2D1::RectF(B.left, B.top + 108, B.right, B.top + 132), rd_.fSmall.Get(), col::muted,
                                 DWRITE_TEXT_ALIGNMENT_CENTER);
                        if (Button(2, btnA, L"Continue", col::cyan, true, true, dt, true, true)) StartAtOrAfter(kWantCpu);
                        if (Button(4, btnB, L"Close", col::muted, false, true, dt)) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                    }
                } else if (stage == Stage::Installing) {
                    const D2D1_POINT_2F sc = D2D1::Point2F(cx - 90, B.top + 132);
                    rd_.Arc(sc, 9, spinner_, spinner_ + 1.8f, col::amber, 2.5f);
                    rd_.Text(snap_.narrateBody.empty() ? L"Installing the auto-start service\u2026" : snap_.narrateTitle,
                             D2D1::RectF(cx - 74, B.top + 118, B.right, B.top + 138), rd_.fSub.Get(), col::amber);
                    rd_.Text(snap_.narrateBody,
                             D2D1::RectF(B.left + 8, B.top + 140, B.right - 8, B.bottom - 8), rd_.fBody.Get(), col::muted);
                } else if (stage == Stage::Installed) {
                    const float t = EaseOutCubic(installT_);
                    const D2D1_POINT_2F sc = D2D1::Point2F(cx - 96, B.top + 132);
                    rd_.Glow(sc, 26, col::green, 0.6f);
                    rd_.Arc(sc, 11, -kPi / 2, -kPi / 2 + 2 * kPi * std::min(1.0f, t * 1.4f), col::green, 2);
                    const float ct = Clamp01((t - 0.4f) / 0.6f);
                    if (ct > 0) {
                        const D2D1_POINT_2F a = D2D1::Point2F(sc.x - 5, sc.y + 0.5f), m = D2D1::Point2F(sc.x - 1.5f, sc.y + 4),
                                            b = D2D1::Point2F(sc.x + 6, sc.y - 4);
                        if (ct < 0.5f) {
                            const float u = ct / 0.5f;
                            rd_.Line(a, D2D1::Point2F(Lerp(a.x, m.x, u), Lerp(a.y, m.y, u)), col::white, 2.2f, rd_.Round());
                        } else {
                            const float u = (ct - 0.5f) / 0.5f;
                            rd_.Line(a, m, col::white, 2.2f, rd_.Round());
                            rd_.Line(m, D2D1::Point2F(Lerp(m.x, b.x, u), Lerp(m.y, b.y, u)), col::white, 2.2f, rd_.Round());
                        }
                    }
                    rd_.Text(Fmt(L"Service running: holding %.4f ms now and at every boot", Ms(r.winner)),
                             D2D1::RectF(cx - 78, B.top + 122, B.right, B.top + 142), rd_.fSub.Get(), col::green);
                } else {
                    rd_.Text(L"Install failed - see the log for details.", D2D1::RectF(B.left, B.top + 122, B.right, B.top + 142),
                             rd_.fSub.Get(), col::red, DWRITE_TEXT_ALIGNMENT_CENTER);
                    if (Button(2, btnA, L"Retry install", col::green, true, snap_.system.admin, dt, true, true)) StartInstall();
                    if (Button(5, btnB, L"Open log", col::muted, false, true, dt)) OpenLog();
                }
                break;
            }
            case Stage::PreparePrompt:
                DrawPackPrompt(B, dt, L"Create a restore point first",
                               L"Writes a System Restore checkpoint named 'Synchronus pre-tune'. Use it to roll "
                               L"back if a later change is unstable. This can take a minute.",
                               L"Continue  \u2014  create restore point", col::cyan,
                               [this] { StartPrepare(); },
                               [this] { StartAtOrAfter(kWantBios); });
                break;
            case Stage::PrepareApplying:
            case Stage::PrepareDone:
                DrawTweakProgress(B, dt, stage == Stage::PrepareDone, false);
                break;
            case Stage::PrepareFailed:
                DrawPackFail(B, dt, L"Restore point could not be created. System Protection may be off.",
                             [this] { StartPrepare(); },
                             [this] { StartAtOrAfter(kWantBios); });
                break;
            case Stage::Bios:
                DrawBios(B, dt);
                break;
            case Stage::PpmPrompt:
                DrawPpmPrompt(B, dt);
                break;
            case Stage::PpmApplying:
            case Stage::PpmDone:
                DrawTweakProgress(B, dt, stage == Stage::PpmDone, false);
                break;
            case Stage::PpmFailed:
                rd_.Text(L"PPM apply failed \u2014 see the log. Timer resolution is still installed.",
                         D2D1::RectF(B.left, B.top + 16, B.right, B.top + 40), rd_.fSub.Get(), col::red,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
                rd_.Text(L"Timer resolution is still held. Retry the power-plan writes, or skip to the next step.",
                         D2D1::RectF(B.left + 8, B.top + 44, B.right - 8, B.bottom - 44), rd_.fBody.Get(), col::muted,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
                if (Button(6, btnA, L"Retry PPM", col::violet, true, true, dt, true, true)) StartPpm();
                if (Button(7, btnB, L"Skip", col::muted, false, true, dt)) StartAtOrAfter(kWantGpuClean);
                break;
            case Stage::GpuClean:
                DrawGpuClean(B, dt);
                break;
            case Stage::GpuPrompt:
                DrawGpuPrompt(B, dt);
                break;
            case Stage::GpuApplying:
            case Stage::GpuDone:
                DrawTweakProgress(B, dt, stage == Stage::GpuDone, true);
                break;
            case Stage::GpuFailed:
                rd_.Text(L"GPU apply failed \u2014 see the log. Timer resolution is still installed.",
                         D2D1::RectF(B.left, B.top + 16, B.right, B.top + 40), rd_.fSub.Get(), col::red,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
                rd_.Text(L"Timer resolution is still held. Retry the GPU writes, or skip to the next step.",
                         D2D1::RectF(B.left + 8, B.top + 44, B.right - 8, B.bottom - 44), rd_.fBody.Get(), col::muted,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
                if (Button(6, btnA, L"Retry GPU", col::violet, true, true, dt, true, true)) StartGpu();
                if (Button(7, btnB, L"Skip", col::muted, false, true, dt)) stage_ = Stage::GpuDone;
                break;
            case Stage::MemoryPrompt:
                DrawPackPrompt(B, dt, L"Pagefile and memory compression",
                               L"Keeps a system-managed pagefile so commit never hits a zero-pagefile cliff, and "
                               L"turns off memory compression so standby pages are not compressed on a CPU thread.",
                               L"Continue  \u2014  apply memory tweaks", col::violet,
                               [this] { StartMemory(); },
                               [this] { MaybeStartStorage(); });
                break;
            case Stage::MemoryApplying:
            case Stage::MemoryDone:
                DrawTweakProgress(B, dt, stage == Stage::MemoryDone, false);
                break;
            case Stage::MemoryFailed:
                DrawPackFail(B, dt, L"Memory tweaks failed \u2014 see the log. Earlier writes are still in place.",
                             [this] { StartMemory(); },
                             [this] { MaybeStartStorage(); });
                break;
            case Stage::StoragePrompt:
                DrawPackPrompt(B, dt, L"SSD, PCIe, USB, TRIM",
                               L"PCIe ASPM off, disks never spun down, USB selective suspend off, TRIM on, "
                               L"SysMain/Superfetch disabled, hibernate off, write cache on, NTFS last-access "
                               L"timestamps off. Windows Search is left running so Start-menu search still works.",
                               L"Continue  \u2014  apply storage tweaks", col::amber,
                               [this] { StartStorage(); },
                               [this] { stage_ = Stage::Review; });
                break;
            case Stage::StorageApplying:
            case Stage::StorageDone:
                DrawTweakProgress(B, dt, stage == Stage::StorageDone, true);
                break;
            case Stage::StorageFailed:
                DrawPackFail(B, dt, L"Storage tweaks failed \u2014 see the log. Retry, or skip to Review.",
                             [this] { StartStorage(); },
                             [this] { stage_ = Stage::Review; });
                break;
            case Stage::Review:
                DrawReview(B, dt);
                break;
            case Stage::Failed:
            case Stage::Cancelled: {
                rd_.Text(stage == Stage::Cancelled ? L"Benchmark stopped" : L"Benchmark failed",
                         D2D1::RectF(B.left, B.top + 20, B.right, B.top + 48), rd_.fPhase.Get(), col::red,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
                rd_.Text(r.failure.empty() ? L"Nothing was changed." : r.failure, Inset(B, 10, 52, 10, 60), rd_.fBody.Get(),
                         col::muted, DWRITE_TEXT_ALIGNMENT_CENTER);
                if (Button(3, btnA, L"Try again", col::cyan, true, true, dt, true, true)) StartBenchmark();
                if (Button(5, btnB, L"Open log", col::muted, false, true, dt)) OpenLog();
                break;
            }
        }
    }

    void DrawPackPrompt(const D2D1_RECT_F& B, float dt, const wchar_t* headline, const wchar_t* body,
                        const wchar_t* goLabel, D2D1_COLOR_F accent, const std::function<void()>& go,
                        const std::function<void()>& skip) {
        const StepMeta meta = Meta(stage_.load());
        rd_.Text(meta.name, D2D1::RectF(B.left, B.top + 8, B.right, B.top + 32), rd_.fPhase.Get(), accent,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        rd_.Text(headline, D2D1::RectF(B.left, B.top + 32, B.right, B.top + 56), rd_.fPhase.Get(), col::text,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        rd_.Text(body, D2D1::RectF(B.left + 10, B.top + 64, B.right - 10, B.bottom - 48), rd_.fBody.Get(), col::muted,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        const float cx = (B.left + B.right) / 2;
        const float bw = std::min(270.0f, (W(B) - 16) / 2);
        const D2D1_RECT_F btnA = D2D1::RectF(cx - bw - 6, B.bottom - 38, cx - 6, B.bottom - 4);
        const D2D1_RECT_F btnB = D2D1::RectF(cx + 6, B.bottom - 38, cx + 6 + bw, B.bottom - 4);
        if (Button(6, btnA, goLabel, accent, true, true, dt, true, true)) go();
        if (Button(7, btnB, L"Skip this step", col::muted, false, true, dt)) skip();
    }

    void DrawPackFail(const D2D1_RECT_F& B, float dt, const wchar_t* body, const std::function<void()>& retry,
                      const std::function<void()>& skip) {
        rd_.Text(L"This step failed", D2D1::RectF(B.left, B.top + 16, B.right, B.top + 40), rd_.fSub.Get(), col::red,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        rd_.Text(body, D2D1::RectF(B.left + 8, B.top + 44, B.right - 8, B.bottom - 44), rd_.fBody.Get(), col::muted,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        const float cx = (B.left + B.right) / 2;
        const float bw = std::min(270.0f, (W(B) - 16) / 2);
        const D2D1_RECT_F btnA = D2D1::RectF(cx - bw - 6, B.bottom - 38, cx - 6, B.bottom - 4);
        const D2D1_RECT_F btnB = D2D1::RectF(cx + 6, B.bottom - 38, cx + 6 + bw, B.bottom - 4);
        if (Button(6, btnA, L"Retry", col::violet, true, true, dt, true, true)) retry();
        if (Button(7, btnB, L"Skip", col::muted, false, true, dt)) skip();
    }

    void DrawChecklist(const D2D1_RECT_F& B, float dt, const wchar_t* name, const wchar_t* headline,
                       std::initializer_list<const wchar_t*> items, const wchar_t* goLabel,
                       const std::function<void()>& go, const std::function<void()>& skip) {
        rd_.Text(name, D2D1::RectF(B.left, B.top + 6, B.right, B.top + 28), rd_.fPhase.Get(), col::violet,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        rd_.Text(headline, D2D1::RectF(B.left + 8, B.top + 28, B.right - 8, B.top + 52), rd_.fSmall.Get(), col::muted,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        float y = B.top + 56;
        const float rowH = std::min(28.0f, (B.bottom - 52 - y) / std::max(1.0f, static_cast<float>(items.size())));
        int n = 1;
        for (const wchar_t* item : items) {
            rd_.Text(Fmt(L"%d.", n++), D2D1::RectF(B.left + 10, y, B.left + 36, y + rowH), rd_.fMono.Get(), col::cyan,
                     DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            rd_.Text(item, D2D1::RectF(B.left + 38, y, B.right - 10, y + rowH), rd_.fSmall.Get(), col::text,
                     DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            y += rowH;
        }
        const float cx = (B.left + B.right) / 2;
        const float bw = std::min(270.0f, (W(B) - 16) / 2);
        const D2D1_RECT_F btnA = D2D1::RectF(cx - bw - 6, B.bottom - 38, cx - 6, B.bottom - 4);
        const D2D1_RECT_F btnB = D2D1::RectF(cx + 6, B.bottom - 38, cx + 6 + bw, B.bottom - 4);
        if (Button(6, btnA, goLabel, col::cyan, true, true, dt, true, true)) go();
        if (Button(7, btnB, L"Skip this step", col::muted, false, true, dt)) skip();
    }

    void DrawBios(const D2D1_RECT_F& B, float dt) {
        DrawChecklist(B, dt, L"BIOS",
                      L"Reboot into BIOS/UEFI and set:",
                      {L"Lock the CPU multiplier. Disable Intel Turbo / EIST / Speed Shift, or AMD PBO / Precision Boost.",
                       L"Disable deep C-states. Leave C1 if the machine will not POST without it.",
                       L"Enable XMP (Intel) or DOCP (AMD) at the RAM's rated frequency. Set DRAM voltage if the profile needs it.",
                       L"SATA/NVMe in AHCI (not IDE). Enable Above 4G / Resizable BAR if you want ReBAR later.",
                       L"Save, boot into Windows, then Continue. If this is already done, Continue anyway."},
                      L"Continue  \u2014  BIOS is set", [this] { StartAtOrAfter(kWantMeasure); },
                      [this] { StartAtOrAfter(kWantMeasure); });
    }

    void BeginGpuLookup() {
        {
            std::lock_guard<std::mutex> g(gpuM_);
            if (gpuPhase_ == GpuDl::Looking || gpuPhase_ == GpuDl::Downloading || gpuPhase_ == GpuDl::Installing) return;
            gpuPhase_ = GpuDl::Looking;
            gpuOffer_ = {};
        }
        gpuStop_ = false;
        state_.Progress(0, 0, 0);
        JoinWorker();
        worker_ = std::thread([this] {
            const GpuDriverOffer o = LookupGpuDriver(&state_, gpuStop_);
            std::wstring phase = L"manual driver install";
            {
                std::lock_guard<std::mutex> g(gpuM_);
                if (gpuStop_ || state_.Cancelled()) return;
                gpuOffer_ = o;
                if (o.ok) {
                    gpuPhase_ = GpuDl::Offer;
                    phase = o.version.empty() ? L"driver package found" : o.version;
                } else {
                    gpuPhase_ = GpuDl::Manual;
                }
            }
            state_.Phase(L"GPU CLEAN", phase);
        });
    }

    void BeginGpuDownload() {
        {
            std::lock_guard<std::mutex> g(gpuM_);
            if (gpuPhase_ != GpuDl::Offer && gpuPhase_ != GpuDl::Failed) return;
            if (!gpuOffer_.ok || !gpuOffer_.canDownload || gpuOffer_.url.empty()) return;
            gpuPhase_ = GpuDl::Downloading;
            gpuOffer_.error.clear();
        }
        state_.Progress(0, 0, 0);
        JoinWorker();
        gpuStop_ = false;
        worker_ = std::thread([this] {
            GpuDriverOffer o;
            {
                std::lock_guard<std::mutex> g(gpuM_);
                o = gpuOffer_;
            }
            const int rc = DownloadGpuDriver(state_, o, gpuStop_);
            std::wstring detail = L"package ready to install";
            {
                std::lock_guard<std::mutex> g(gpuM_);
                if (gpuStop_ || state_.Cancelled()) return;
                if (rc == 0) {
                    gpuOffer_.file = o.file;
                    gpuOffer_.bytes = o.bytes;
                    gpuOffer_.error.clear();
                    gpuOffer_.fail = 0;
                    gpuPhase_ = GpuDl::Ready;
                } else if (rc == 2) {
                    gpuPhase_ = GpuDl::Offer;
                    detail = o.version.empty() ? L"driver package found" : o.version;
                } else {
                    gpuOffer_.error = o.error.empty() ? L"The download failed." : o.error;
                    gpuOffer_.file.clear();
                    gpuOffer_.fail = 1;
                    gpuPhase_ = GpuDl::Failed;
                    detail = L"download failed";
                }
            }
            state_.Phase(L"GPU CLEAN", detail);
        });
    }

    void BeginGpuInstall() {
        {
            std::lock_guard<std::mutex> g(gpuM_);
            const bool retryInstall = gpuPhase_ == GpuDl::Failed && gpuOffer_.fail == 2 && !gpuOffer_.file.empty();
            if (gpuPhase_ != GpuDl::Ready && !retryInstall) return;
            if (gpuOffer_.file.empty()) return;
            gpuPhase_ = GpuDl::Installing;
            gpuOffer_.error.clear();
        }
        JoinWorker();
        worker_ = std::thread([this] {
            GpuDriverOffer o;
            {
                std::lock_guard<std::mutex> g(gpuM_);
                o = gpuOffer_;
            }
            const int rc = InstallTrimmedGpuDriver(state_, o);
            std::wstring err;
            {
                std::lock_guard<std::mutex> g(gpuM_);
                if (state_.Cancelled()) return;
                if (rc == 0) {
                    gpuOffer_.error.clear();
                    gpuOffer_.fail = 0;
                    gpuPhase_ = GpuDl::Done;
                } else {
                    gpuOffer_.error = o.error.empty() ? L"The driver did not install." : o.error;
                    gpuOffer_.fail = 2;
                    gpuPhase_ = GpuDl::Failed;
                    err = gpuOffer_.error;
                }
            }
            state_.Phase(L"GPU CLEAN", err.empty() ? L"trimmed driver installed" : L"driver install failed");
            if (!err.empty()) state_.Log(err);
        });
    }

    void SkipGpuClean() {
        gpuStop_ = true;
        JoinWorker();
        {
            std::lock_guard<std::mutex> g(gpuM_);
            gpuPhase_ = GpuDl::Idle;
        }
        state_.Phase(L"GPU CLEAN", L"driver install skipped");
        StartAtOrAfter(kWantGpu);
    }

    void DrawGpuClean(const D2D1_RECT_F& B, float dt) {
        GpuDl phase;
        GpuDriverOffer offer;
        {
            std::lock_guard<std::mutex> g(gpuM_);
            phase = gpuPhase_;
            offer = gpuOffer_;
        }
        auto sizeText = [](uint64_t n) {
            wchar_t b[32];
            if (!n) return std::wstring();
            if (n >= (1ull << 30)) swprintf_s(b, L"%.1f GB", static_cast<double>(n) / 1073741824.0);
            else swprintf_s(b, L"%.0f MB", static_cast<double>(n) / 1048576.0);
            return std::wstring(b);
        };
        const float cx = (B.left + B.right) / 2;
        const float bw = std::min(270.0f, (W(B) - 16) / 2);
        const D2D1_RECT_F btnA = D2D1::RectF(cx - bw - 6, B.bottom - 38, cx - 6, B.bottom - 4);
        const D2D1_RECT_F btnB = D2D1::RectF(cx + 6, B.bottom - 38, cx + 6 + bw, B.bottom - 4);

        if (phase == GpuDl::Manual) {
            rd_.Text(L"GPU Clean", D2D1::RectF(B.left, B.top + 6, B.right, B.top + 28), rd_.fPhase.Get(), col::violet,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            rd_.Text(offer.error.empty() ? L"No vendor package can be installed from here." : offer.error,
                     D2D1::RectF(B.left + 8, B.top + 30, B.right - 8, B.top + 72), rd_.fBody.Get(), col::amber,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            rd_.Text(L"NVIDIA: graphics driver only. Skip the NVIDIA App, telemetry, ShadowPlay, and HD audio.\n"
                     L"AMD: choose Driver Only. Skip Adrenalin, the overlay, Chill, and the other extras.\n"
                     L"DDU in Safe Mode is optional if an older package left leftovers.\n"
                     L"Afterburner and RivaTuner are installed by the GPU step. BIOS Resizable BAR stays manual.",
                     D2D1::RectF(B.left + 12, B.top + 78, B.right - 12, B.bottom - 52), rd_.fBody.Get(), col::text);
            if (Button(6, btnA, L"Continue", col::cyan, true, true, dt, true, true)) StartAtOrAfter(kWantGpu);
            if (Button(7, btnB, L"Check again", col::muted, false, true, dt)) BeginGpuLookup();
            return;
        }

        const D2D1_COLOR_F accent = offer.vendor == 2 ? col::red : (offer.vendor == 1 ? col::green : col::cyan);
        const bool busy = phase == GpuDl::Looking || phase == GpuDl::Downloading || phase == GpuDl::Installing;
        if (busy) rd_.Arc(D2D1::Point2F(B.left + 18, B.top + 18), 7, spinner_, spinner_ + 1.7f, col::cyan, 2.0f);
        rd_.Text(L"GPU Clean", D2D1::RectF(B.left, B.top + 6, B.right, B.top + 28), rd_.fPhase.Get(), col::violet,
                 DWRITE_TEXT_ALIGNMENT_CENTER);

        std::wstring headline = L"Looking up the current driver";
        if (phase == GpuDl::Offer || phase == GpuDl::Ready || phase == GpuDl::Done || phase == GpuDl::Failed ||
            phase == GpuDl::Downloading || phase == GpuDl::Installing) {
            headline = offer.gpuName.empty() ? L"Display adapter" : offer.gpuName;
        }
        rd_.Text(headline, D2D1::RectF(B.left + 8, B.top + 30, B.right - 8, B.top + 52), rd_.fPhase.Get(), accent,
                 DWRITE_TEXT_ALIGNMENT_CENTER);

        std::wstring sub;
        if (phase == GpuDl::Looking || phase == GpuDl::Idle) {
            sub = L"Reading the display adapter and asking the vendor for the current package.";
        } else if (!offer.channel.empty() || !offer.version.empty()) {
            sub = offer.channel;
            if (!offer.version.empty()) sub += (sub.empty() ? L"" : L"   ") + offer.version;
        }
        rd_.Text(sub, D2D1::RectF(B.left + 8, B.top + 52, B.right - 8, B.top + 70), rd_.fMonoSmall.Get(), col::muted,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        if (!offer.installed.empty() && phase != GpuDl::Looking) {
            rd_.Text(offer.installed, D2D1::RectF(B.left + 8, B.top + 70, B.right - 8, B.top + 86), rd_.fSmall.Get(),
                     col::dim, DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        std::wstring body;
        if (phase == GpuDl::Looking || phase == GpuDl::Idle) {
            body = L"NVIDIA and AMD packages are downloaded only from the vendor. You will be asked again before anything is installed.";
        } else if (phase == GpuDl::Downloading) {
            body = offer.summary + L"\n\nDownloading. This does not install the driver yet.";
        } else if (phase == GpuDl::Ready) {
            body = offer.summary +
                   L"\n\nInstall the trimmed driver now? The display will flicker, this window may go blank until the "
                   L"installer finishes, and a restart is required afterward.";
        } else if (phase == GpuDl::Installing) {
            body = L"Installing the trimmed driver. The display may flicker, and this window may go blank until the "
                   L"installer finishes. Leave this step running.";
        } else if (phase == GpuDl::Done) {
            body = L"The trimmed driver is installed. Restart Windows before it is fully in use. GPU Config can write "
                   L"its settings now.";
        } else if (phase == GpuDl::Failed) {
            body = offer.error.empty() ? L"The driver step failed." : offer.error;
        } else {
            body = offer.summary;
            if (!offer.error.empty()) body += L"\n\n" + offer.error;
            if (!offer.note.empty()) body += L"\n\n" + offer.note;
        }
        if (phase != GpuDl::Offer && phase != GpuDl::Failed && !offer.note.empty() && phase != GpuDl::Looking &&
            phase != GpuDl::Idle) {
            body += L"\n\n" + offer.note;
        }
        const float bodyBottom = phase == GpuDl::Downloading ? B.bottom - 116.0f : B.bottom - 52.0f;
        rd_.Text(body, D2D1::RectF(B.left + 12, B.top + 90, B.right - 12, bodyBottom), rd_.fBody.Get(), col::text);

        if (phase == GpuDl::Downloading) {
            const float frac = Clamp01(static_cast<float>(snap_.progress));
            const D2D1_RECT_F track = D2D1::RectF(B.left + 16, B.bottom - 78, B.right - 16, B.bottom - 68);
            rd_.FillRound(track, 4, col::panel2);
            if (frac > 0.002f) {
                rd_.FillRound(D2D1::RectF(track.left, track.top, track.left + std::max(8.0f, W(track) * frac), track.bottom),
                              4, col::cyan);
            }
            std::wstring pct = Fmt(L"%d%%", static_cast<int>(frac * 100 + 0.5f));
            if (offer.bytes) pct = sizeText(static_cast<uint64_t>(frac * static_cast<double>(offer.bytes))) + L"  of  " + sizeText(offer.bytes);
            rd_.Text(pct, D2D1::RectF(B.left, B.bottom - 100, B.right, B.bottom - 82), rd_.fMonoSmall.Get(), col::muted,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        if (phase == GpuDl::Looking || phase == GpuDl::Idle) {
            if (Button(7, btnB, L"Skip this step", col::muted, false, true, dt)) SkipGpuClean();
        } else if (phase == GpuDl::Offer) {
            std::wstring go = offer.vendor == 2 ? L"Download AMD driver" : L"Download NVIDIA driver";
            if (offer.bytes) go = L"Download  \u00B7  " + sizeText(offer.bytes);
            if (offer.canDownload) {
                if (Button(6, btnA, go, col::cyan, true, true, dt, true, true)) BeginGpuDownload();
            } else if (Button(6, btnA, L"Check again", col::cyan, true, true, dt, true, true)) {
                BeginGpuLookup();
            }
            if (Button(7, btnB, L"Skip this step", col::muted, false, true, dt)) SkipGpuClean();
        } else if (phase == GpuDl::Downloading) {
            if (Button(7, btnB, L"Skip this step", col::muted, false, true, dt)) SkipGpuClean();
        } else if (phase == GpuDl::Ready) {
            if (Button(6, btnA, L"Install trimmed driver", col::green, true, true, dt, true, true)) BeginGpuInstall();
            if (Button(7, btnB, L"Skip this step", col::muted, false, true, dt)) SkipGpuClean();
        } else if (phase == GpuDl::Done) {
            if (Button(6, btnA, L"Continue  \u2014  configure GPU", col::green, true, true, dt, true, true))
                StartAtOrAfter(kWantGpu);
        } else if (phase == GpuDl::Failed) {
            if (Button(6, btnA, L"Retry", col::cyan, true, true, dt, true, true)) {
                if (offer.fail == 2 && !offer.file.empty()) BeginGpuInstall();
                else if (offer.ok && offer.canDownload) BeginGpuDownload();
                else BeginGpuLookup();
            }
            if (Button(7, btnB, L"Skip this step", col::muted, false, true, dt)) SkipGpuClean();
        }
    }

    void DrawReview(const D2D1_RECT_F& B, float dt) {
        rd_.Text(L"Review", D2D1::RectF(B.left, B.top + 8, B.right, B.top + 32), rd_.fPhase.Get(), col::green,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        rd_.Text(L"Confirm the results, then restart so kernel and driver flags take effect.",
                 D2D1::RectF(B.left + 8, B.top + 32, B.right - 8, B.top + 52), rd_.fSmall.Get(), col::muted,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        rd_.Text(L"LatencyMon or WPR \u2014 DPC/ISR should be quieter after PERFCHECK 5000 and ThreadDpcEnable=0.\n"
                 L"HWiNFO \u2014 CPU parked cores at 0, clocks held if you locked the multiplier in BIOS.\n"
                 L"GPU-Z \u2014 P0 / max performance; ReBAR only if BIOS Above 4G is on.\n"
                 L"MemTest86 (USB) and y-cruncher \u2014 RAM at the XMP/DOCP frequency with no errors. Hot DIMMs error.\n"
                 L"CrystalDiskMark \u2014 sequential/random near the SSD's spec after TRIM and write cache.\n"
                 L"If anything is unstable: Settings \u2192 System Restore \u2192 'Synchronus pre-tune', or Synchronus.exe --uninstall.",
                 D2D1::RectF(B.left + 12, B.top + 56, B.right - 12, B.bottom - 48), rd_.fBody.Get(), col::text);
        DrawFinishButtons(B, dt);
    }

    void DrawPpmPrompt(const D2D1_RECT_F& B, float dt) {
        const CpuProbeView p = ProbeCopy();
        const bool locked = p.mode == CpuProbeView::Mode::Locked;
        const bool dynamic = p.mode == CpuProbeView::Mode::Dynamic;
        const D2D1_COLOR_F accent = locked ? col::green : (dynamic ? col::amber : col::violet);
        const wchar_t* headline = locked ? L"LOCKED / STATIC FREQUENCY"
                                  : dynamic ? L"DYNAMIC BOOST (TURBO / PBO)"
                                            : L"MULTIPLIER UNCLEAR";
        rd_.Text(L"CPU Tune", D2D1::RectF(B.left, B.top + 8, B.right, B.top + 28),
                 rd_.fPhase.Get(), col::violet, DWRITE_TEXT_ALIGNMENT_CENTER);
        rd_.Text(headline, D2D1::RectF(B.left, B.top + 30, B.right, B.top + 50), rd_.fPhase.Get(), accent,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        rd_.Text(Fmt(L"%s   %u logical   idle %u MHz  busy %u MHz  cap %u MHz%s", p.cpuName.c_str(), p.nLogical,
                     p.idleMhz, p.busyMhz, p.maxMhz, p.hybrid ? L"   hybrid" : L""),
                 D2D1::RectF(B.left, B.top + 50, B.right, B.top + 68), rd_.fMonoSmall.Get(), col::muted,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        rd_.Text(p.alreadyApplied ? (p.reason + L"  \u00B7  reapplying current values")
                                  : p.reason,
                 D2D1::RectF(B.left, B.top + 68, B.right, B.top + 86), rd_.fSmall.Get(),
                 p.alreadyApplied ? col::amber : col::dim, DWRITE_TEXT_ALIGNMENT_CENTER);
        rd_.Text(locked ? L"On a locked clock the kernel still wakes every ~15 ms to re-evaluate P-states and parked "
                          L"cores. Those DPCs steal slices from the game and audio threads. This step disables parking, "
                          L"pins the processor at 100%, and sets the check interval to 5 s."
                        : L"A 5 s check interval delays turbo boost. Apply this only if the multiplier is locked in BIOS.",
                 D2D1::RectF(B.left + 10, B.top + 92, B.right - 10, B.bottom - 44), rd_.fBody.Get(), col::muted,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        const float cx = (B.left + B.right) / 2;
        const float bw = std::min(270.0f, (W(B) - 16) / 2);
        const D2D1_RECT_F btnA = D2D1::RectF(cx - bw - 6, B.bottom - 38, cx - 6, B.bottom - 4);
        const D2D1_RECT_F btnB = D2D1::RectF(cx + 6, B.bottom - 38, cx + 6 + bw, B.bottom - 4);
        if (Button(6, btnA, locked ? L"Continue  \u2014  apply CPU tweaks" : L"Continue anyway", accent, true, true, dt,
                   true, true))
            StartPpm();
        if (Button(7, btnB, L"Skip this step", col::muted, false, true, dt)) {
            state_.Phase(L"CPU TUNE", L"PPM tweaks skipped");
            StartAtOrAfter(kWantGpuClean);
        }
    }

    void DrawGpuPrompt(const D2D1_RECT_F& B, float dt) {
        const GpuProbeView g = GpuCopy();
        D2D1_COLOR_F accent = col::cyan;
        if (g.nvidia) accent = col::green;
        else if (g.amd) accent = col::red;
        rd_.Text(L"GPU Config", D2D1::RectF(B.left, B.top + 8, B.right, B.top + 28),
                 rd_.fPhase.Get(), col::violet, DWRITE_TEXT_ALIGNMENT_CENTER);
        rd_.Text(g.name, D2D1::RectF(B.left, B.top + 30, B.right, B.top + 50), rd_.fPhase.Get(), accent,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        rd_.Text(g.alreadyApplied ? (g.detail + L"  \u00B7  reapplying current values") : g.detail,
                 D2D1::RectF(B.left, B.top + 50, B.right, B.top + 70), rd_.fMonoSmall.Get(),
                 g.alreadyApplied ? col::amber : col::muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        rd_.Text(L"Writes Windows and driver latency settings: hardware-accelerated GPU scheduling, Game DVR "
                 L"and overlay off, NVIDIA global 3D (prefer maximum performance, high-performance filtering, "
                 L"VSync off, ultra low latency, ReBAR, CUDA P2 off, Ansel off), and AMD Anti-Lag / ULPS.",
                 D2D1::RectF(B.left + 10, B.top + 78, B.right - 10, B.bottom - 44), rd_.fBody.Get(), col::muted,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        const float cx = (B.left + B.right) / 2;
        const float bw = std::min(270.0f, (W(B) - 16) / 2);
        const D2D1_RECT_F btnA = D2D1::RectF(cx - bw - 6, B.bottom - 38, cx - 6, B.bottom - 4);
        const D2D1_RECT_F btnB = D2D1::RectF(cx + 6, B.bottom - 38, cx + 6 + bw, B.bottom - 4);
        if (Button(6, btnA, L"Continue  \u2014  apply GPU tweaks", accent, true, true, dt, true, true)) StartGpu();
        if (Button(7, btnB, L"Skip this step", col::muted, false, true, dt)) {
            gpuSkipped_ = true;
            state_.Phase(L"GPU CONFIG", L"GPU tweaks skipped");
            StartAtOrAfter(kWantMemory);
        }
    }

    void DrawTweakProgress(const D2D1_RECT_F& B, float dt, bool done, bool gpu) {
        (void)dt;
        const bool skipped = gpu && gpuSkipped_;
        const StepMeta meta = Meta(stage_.load());
        rd_.Text(done ? (skipped ? Fmt(L"%s skipped", meta.name).c_str() : Fmt(L"%s applied", meta.name).c_str())
                      : meta.title,
                 D2D1::RectF(B.left + 8, B.top + 6, B.right - 8, B.top + 28), rd_.fPhase.Get(),
                 done ? col::green : col::amber, DWRITE_TEXT_ALIGNMENT_CENTER);
        rd_.Text(done ? L"Complete." : L"Writing settings.",
                 D2D1::RectF(B.left + 8, B.top + 28, B.right - 8, B.top + 48), rd_.fSmall.Get(), col::muted,
                 DWRITE_TEXT_ALIGNMENT_CENTER);

        const auto& steps = snap_.ppm;
        if (steps.empty()) {
            rd_.Text(snap_.narrateBody.empty() ? L"Preparing\u2026" : snap_.narrateBody,
                     D2D1::RectF(B.left + 8, B.top + 56, B.right - 8, B.bottom - 8), rd_.fBody.Get(), col::muted);
            return;
        }

        int active = -1;
        for (size_t i = 0; i < steps.size(); ++i) {
            if (steps[i].status == 1) active = static_cast<int>(i);
        }
        if (active < 0) {
            for (int i = static_cast<int>(steps.size()) - 1; i >= 0; --i) {
                if (steps[static_cast<size_t>(i)].status == 2 || steps[static_cast<size_t>(i)].status == 3) {
                    active = i;
                    break;
                }
            }
        }

        const float listTop = B.top + 52;
        const float listBottom = done ? B.bottom - (restartConfirm_ ? 72.0f : 48.0f) : B.bottom - 86;
        const int visible = std::max(1, static_cast<int>((listBottom - listTop) / 18.0f));
        const int n = static_cast<int>(steps.size());
        int start = 0;
        if (n > visible) {
            start = std::max(0, (active < 0 ? 0 : active) - visible / 3);
            start = std::min(start, n - visible);
        }
        const int end = std::min(n, start + visible);
        const float rowH = (listBottom - listTop) / std::max(1, end - start);

        rd_.Text(Fmt(L"%d / %d", (active >= 0 ? active + 1 : end), n),
                 D2D1::RectF(B.right - 80, B.top + 8, B.right - 10, B.top + 28), rd_.fMonoSmall.Get(), col::dim,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);

        for (int i = start; i < end; ++i) {
            const auto& s = steps[static_cast<size_t>(i)];
            const float y = listTop + (i - start) * rowH;
            D2D1_COLOR_F c = col::dim;
            const wchar_t* mark = L"\u00B7";
            if (s.status == 1) {
                c = col::amber;
                mark = L"\u25B6";
                rd_.FillRound(D2D1::RectF(B.left + 6, y + 1, B.right - 6, y + rowH - 1), 4, Alpha(col::amber, 0.14f));
            } else if (s.status == 2) {
                c = col::green;
                mark = L"\u2713";
            } else if (s.status == 3) {
                c = col::red;
                mark = L"\u00D7";
            }
            rd_.Text(Fmt(L"%s  %s", mark, s.title.c_str()), D2D1::RectF(B.left + 12, y, B.left + W(B) * 0.55f, y + rowH),
                     rd_.fMonoSmall.Get(), c, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            if (!s.change.empty()) {
                rd_.Text(s.change, D2D1::RectF(B.left + W(B) * 0.55f, y, B.right - 12, y + rowH), rd_.fMonoSmall.Get(), c,
                         DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
        if (!done && active >= 0 && active < n && !steps[static_cast<size_t>(active)].why.empty()) {
            rd_.FillRound(D2D1::RectF(B.left + 6, B.bottom - 82, B.right - 6, B.bottom - 6), 8, Alpha(col::violet, 0.10f));
            rd_.Text(steps[static_cast<size_t>(active)].why,
                     D2D1::RectF(B.left + 14, B.bottom - 78, B.right - 14, B.bottom - 10), rd_.fBody.Get(), col::text);
        }
    }

    void OpenLog() { ShellExecuteW(nullptr, L"open", host_.logPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

    // ---- footer ----
    void DrawFooter(const D2D1_RECT_F& r, float dt) {
        (void)dt;
        std::wstring last;
        for (auto it = snap_.log.rbegin(); it != snap_.log.rend(); ++it) {
            if (!it->empty() && it->find(L"  res (ms)") == std::wstring::npos && (*it)[0] != L' ') {
                last = *it;
                break;
            }
        }
        const float cy = (r.top + r.bottom) / 2.0f;
        const float barW = std::min(360.0f, W(r) * 0.34f);
        const float mid = (r.left + r.right) / 2.0f;
        const D2D1_RECT_F track = D2D1::RectF(mid - barW / 2.0f, cy - 3.0f, mid + barW / 2.0f, cy + 3.0f);
        float frac = progress_;
        if ((stage_.load() == Stage::Running || stage_.load() == Stage::GpuClean) && snap_.progress > 0.001)
            frac = Clamp01(static_cast<float>(snap_.progress));
        rd_.FillRound(track, 3, col::panel2);
        const float fx = track.left + W(track) * Clamp01(frac);
        if (fx > track.left + 2) {
            ID2D1LinearGradientBrush* pg = rd_.ProgressGrad();
            pg->SetStartPoint(D2D1::Point2F(track.left, 0));
            pg->SetEndPoint(D2D1::Point2F(track.right, 0));
            pg->SetOpacity(1);
            rd_.Dc()->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(track.left, track.top, fx, track.bottom), 3, 3), pg);
        }
        rd_.Text(last, D2D1::RectF(r.left, r.top, track.left - 16, r.bottom), rd_.fMonoSmall.Get(), col::muted,
                 DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // ---- particles ----
    float Rand() {
        rng_ = rng_ * 1664525u + 1013904223u;
        return static_cast<float>(rng_ >> 8) / 16777216.0f;
    }
    void UpdateParticles(float dt) {
        for (auto& p : particles_) {
            p.life -= dt;
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            p.vy += 30 * dt;
        }
        particles_.erase(std::remove_if(particles_.begin(), particles_.end(), [](const Particle& p) { return p.life <= 0; }),
                         particles_.end());
    }
    void DrawParticles() {
        for (const auto& p : particles_) {
            const float a = Clamp01(p.life / p.maxLife);
            rd_.Glow(D2D1::Point2F(p.x, p.y), p.size * 4, p.c, 0.5f * a);
            rd_.Dot(D2D1::Point2F(p.x, p.y), p.size * a, Alpha(p.c, a));
        }
    }

    // ---- members ----
    const Host& host_;
    HWND hwnd_ = nullptr;
    float dpi_ = 96;
    Renderer rd_;
    SharedState state_;
    Snapshot snap_;
    std::thread worker_;
    std::thread updateThread_;
    std::mutex updateM_;
    AppUpdate updateOffer_{};
    std::wstring updateLine_;
    std::wstring updateErr_;
    bool updateReady_ = false;
    bool updateNewer_ = false;
    bool updatePrompted_ = false;
    bool updateFailed_ = false;
    std::atomic<bool> updateBusy_{false};
    std::atomic<bool> updateStop_{false};
    std::atomic<bool> relaunch_{false};
    std::mutex resultM_;
    ResultView result_;
    std::mutex probeM_;
    CpuProbeView probe_{};
    bool probeReady_ = false;
    std::mutex gpuM_;
    GpuProbeView gpu_{};
    bool gpuReady_ = false;
    GpuDl gpuPhase_ = GpuDl::Idle;
    GpuDriverOffer gpuOffer_{};
    std::atomic<bool> gpuStop_{false};
    std::atomic<Stage> stage_{Stage::Welcome};
    Stage lastStage_ = Stage::Running;
    std::atomic<bool> serviceStopped_{false};
    std::atomic<bool> installedThisSession_{false};
    bool gpuSkipped_ = false;
    bool want_[kWantCount]{};
    int fidelity_ = 1;
    bool restartConfirm_ = false;
    bool running_ = true;

    double lastFrame_ = 0;
    float time_ = 0, introT_ = 0, revealT_ = 0, installT_ = 0, spinner_ = 0, progress_ = 0, yMax_ = 0.6f, activeX_ = -1;
    std::vector<float> anScore_, anMean_, anMedian_, anLate_, anIdle_, anAppear_, finBar_, histH_, hover_;
    float statV_[7] = {};
    std::vector<Particle> particles_;
    uint32_t rng_ = 0x9E3779B9u;
    uint64_t lastSeq_ = 0;

    D2D1_POINT_2F mouse_ = D2D1::Point2F(-1, -1), clickPos_ = D2D1::Point2F(-1, -1);
    bool mouseDown_ = false, clickPending_ = false, enterPending_ = false, hoverButton_ = false;
    std::vector<ButtonRec> buttons_;
};

}  // namespace

int Run(const Host& host) {
    App app(host);
    return app.Run();
}

}  // namespace gui
