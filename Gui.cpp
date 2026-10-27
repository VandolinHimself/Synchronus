// Gui.cpp - Direct2D / DirectWrite front-end.
//
// The benchmark runs on a worker thread and streams into SharedState (a
// gui::Feed). The UI thread copies a Snapshot of that state once per frame
// and renders it at the monitor's refresh rate: the live sweep curve, an
// oscilloscope of individual Sleep(1) wake-ups, the wake-up distribution
// against the clock ticks, animated statistics, the verification bars and
// finally the result / install flow. The Synchronus mark and the Lucius &
// Wayne Systems producer logo are embedded as RCDATA and drawn through
// WIC. No other dependencies.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "Gui.h"
#include "Health.h"
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
#include <shlobj.h>
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

using gui::AppendHealthLog;
using gui::DriftCount;
using gui::HealthCheck;
using gui::HealthNeedsAttention;
using gui::HealthStatus;
using gui::HealthStatusText;
using gui::HealthSummary;
using gui::LoadMonitorPrefs;
using gui::LoadStepChoices;
using gui::MarkSetupComplete;
using gui::MonitorPrefs;
using gui::RunHealth;
using gui::SaveMonitorPrefs;
using gui::SaveStepChoices;
using gui::SetDriftCount;
using gui::SetupComplete;
using gui::StepChoices;
using gui::Summarize;

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
        fHomeBtn, fGuideTitle, fGuide, fSub, fTileLabel;

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
        fHomeBtn = Format(L"Philosopher", DWRITE_FONT_WEIGHT_BOLD, 16);
        fGuideTitle = Format(L"Philosopher", DWRITE_FONT_WEIGHT_BOLD, 22, true, false);
        fGuide = Format(L"Philosopher", DWRITE_FONT_WEIGHT_NORMAL, 16, true, false);
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
    ComPtr<ID2D1Bitmap> logo_;   // Lucius & Wayne producer mark (welcome only)
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
    Running, Result, Failed, Cancelled, Installing, Installed, InstallFailed,
    PpmPrompt, PpmApplying, PpmDone, PpmFailed,
    GpuClean,
    GpuPrompt, GpuApplying, GpuDone, GpuFailed,
    AfterburnerPrompt, AfterburnerApplying, AfterburnerDone, AfterburnerFailed,
    MemoryPrompt, MemoryApplying, MemoryDone, MemoryFailed,
    StoragePrompt, StorageApplying, StorageDone, StorageFailed,
    Review,
    Monitor,
    Home,
    Fleet
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
        case Stage::Running:
        case Stage::Failed:
        case Stage::Cancelled:
            return {2, L"Measure", L"Find this PC's fastest Sleep(1)"};
        case Stage::Result:
        case Stage::Installing:
        case Stage::Installed:
        case Stage::InstallFailed:
            return {3, L"Hold", L"Install the boot service"};
        case Stage::PpmPrompt:
        case Stage::PpmApplying:
        case Stage::PpmDone:
        case Stage::PpmFailed:
            return {4, L"CPU Tune", L"Ultimate Performance and PPM"};
        case Stage::GpuClean:
            return {5, L"GPU Clean", L"Latest driver, graphics only"};
        case Stage::GpuPrompt:
        case Stage::GpuApplying:
        case Stage::GpuDone:
        case Stage::GpuFailed:
            return {6, L"GPU Config", L"Power, latency, overlays"};
        case Stage::AfterburnerPrompt:
        case Stage::AfterburnerApplying:
        case Stage::AfterburnerDone:
        case Stage::AfterburnerFailed:
            return {7, L"Afterburner", L"download and scan, ~30 min"};
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
            return {10, L"Review", L"BIOS, then restart"};
        case Stage::Monitor:
            return {0, L"Health", L"live settings"};
        case Stage::Fleet:
            return {0, L"Full tune", L"selected steps, together"};
        case Stage::Home:
            return {0, L"Home", L"functions and live tweaks"};
        default:
            return {10, L"Review", L"BIOS, then restart"};
    }
}

bool IsTuneStage(Stage s) {
    switch (s) {
        case Stage::PreparePrompt:
        case Stage::PrepareApplying:
        case Stage::PrepareDone:
        case Stage::PrepareFailed:
        case Stage::PpmPrompt:
        case Stage::PpmApplying:
        case Stage::PpmDone:
        case Stage::PpmFailed:
        case Stage::GpuClean:
        case Stage::GpuPrompt:
        case Stage::GpuApplying:
        case Stage::GpuDone:
        case Stage::GpuFailed:
        case Stage::AfterburnerPrompt:
        case Stage::AfterburnerApplying:
        case Stage::AfterburnerDone:
        case Stage::AfterburnerFailed:
        case Stage::MemoryPrompt:
        case Stage::MemoryApplying:
        case Stage::MemoryDone:
        case Stage::MemoryFailed:
        case Stage::StoragePrompt:
        case Stage::StorageApplying:
        case Stage::StorageDone:
        case Stage::StorageFailed:
        case Stage::Review:
        case Stage::Monitor:
            return true;
        default:
            return false;
    }
}

enum {
    kWantPrepare = 0,
    kWantMeasure,
    kWantHold,
    kWantCpu,
    kWantGpuClean,
    kWantGpu,
    kWantAfterburner,
    kWantMemory,
    kWantStorage,
    kWantCount
};

enum class JobId : int {
    Prepare = 0,
    Measure,
    Hold,
    Cpu,
    GpuClean,
    Gpu,
    Afterburner,
    Memory,
    Storage,
    Count
};

enum class JobPhase : int { Idle = 0, Waiting, Running, Done, Failed, Cancelled };

struct JobSlot {
    std::thread thread;
    SharedState feed;
    std::atomic<int> phase{static_cast<int>(JobPhase::Idle)};
    std::atomic<bool> cancel{false};
    int code = 0;
};

struct Particle {
    float x, y, vx, vy, life, maxLife, size;
    D2D1_COLOR_F c;
};

INT_PTR CALLBACK StartupDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM) {
    if (msg == WM_INITDIALOG) {
        CheckDlgButton(dlg, 1001, BST_CHECKED);
        return TRUE;
    }
    if (msg == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        const bool keep = LOWORD(wp) == IDCANCEL || IsDlgButtonChecked(dlg, 1001) == BST_CHECKED;
        EndDialog(dlg, keep ? 1 : 0);
        return TRUE;
    }
    return FALSE;
}

class App {
public:
    explicit App(const Host& host) : host_(host) {
        fidelity_ = host_.fidelity;
        for (int i = 0; i < kWantCount; ++i) want_[i] = true;
        if (host_.prepareSkip) want_[kWantPrepare] = false;
        if (host_.ppmSkip) want_[kWantCpu] = false;
        if (host_.gpuSkip) want_[kWantGpu] = false;
        if (host_.afterburnerSkip) want_[kWantAfterburner] = false;
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
                                L"Synchronus  |  Lucius & Wayne Systems",
                                WS_OVERLAPPEDWINDOW, x, y, ww, wh, nullptr, nullptr, hinst, this);
        if (!hwnd_) return 1;
        SetTaskbarIcons();

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
        prefs_ = LoadMonitorPrefs();
        SetDriverStoreDir(prefs_.driverDir);
        SetAfterburnerRoot(prefs_.afterburnerDir);
        nextUpdateAt_ = GetTickCount64() +
                        static_cast<unsigned long long>(std::max(1, prefs_.updateMinutes)) * 60ull * 1000ull;
        showMsg_ = RegisterWindowMessageW(L"Synchronus.Show");
        AddTray();
        if (host_.tidyServices) host_.tidyServices();
        const bool firstOpen = !prefs_.startupAsked && !host_.autoInstall;
        ShowWindow(hwnd_, (!firstOpen && prefs_.startMinimized && !host_.autoInstall) ? SW_HIDE : SW_SHOW);
        SetTimer(hwnd_, 7, 60 * 1000, nullptr);
        nextScanAt_ = GetTickCount64() + static_cast<unsigned long long>(prefs_.scanHours) * 3600ull * 1000ull;
        if (!prefs_.paused) StartHealthScan();
        // The measurement thread takes realtime priority; keep the UI thread
        // well below it so rendering never delays a Sleep(1) wake-up.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        state_.System(host_.system());
        lastFrame_ = state_.Now();
        chainAuto_ = host_.autoInstall;
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
        } else if (!SetupComplete()) {
            stage_ = Stage::Welcome;
            state_.Phase(L"WELCOME", L"thank you for using Synchronus");
        } else {
            ApplySavedChoices();
            stage_ = Stage::Home;
            state_.Phase(L"HOME", L"choose a function");
        }
        StartUpdateCheck();
        if (firstOpen) AskStartupChoice();
        if (!host_.autoInstall && !prefs_.maintainAsked) AskMaintain();

        MSG msg{};
        while (running_) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) running_ = false;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (!running_) break;
            if (IsIconic(hwnd_) || !IsWindowVisible(hwnd_)) {
                WaitMessage();
                continue;
            }
            Frame();
        }

        gpuStop_ = true;
        updateStop_.store(true);
        state_.Cancel();
        CancelAllJobs();
        if (updateThread_.joinable()) updateThread_.join();
        healthStop_.store(true);
        if (healthThread_.joinable()) healthThread_.join();
        JoinJobs();
        if (taskbarBig_) DestroyIcon(taskbarBig_);
        if (taskbarSmall_) DestroyIcon(taskbarSmall_);
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
                SetTaskbarIcons();
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
                    if (settingsOpen_) settingsOpen_ = false;
                    else if (restartConfirm_) restartConfirm_ = false;
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
                if (relaunch_.load()) exitRequested_ = true;
                if (!exitRequested_ && prefs_.closeToTray) {
                    ShowWindow(hwnd_, SW_HIDE);
                    return 0;
                }
                RemoveTray();
                DestroyWindow(hwnd_);
                return 0;
            case WM_DESTROY:
                RemoveTray();
                PostQuitMessage(0);
                return 0;
            case WM_TIMER:
                if (wp == 7) {
                    const unsigned long long now = GetTickCount64();
                    if (now >= nextUpdateAt_) {
                        nextUpdateAt_ = now + static_cast<unsigned long long>(std::max(1, prefs_.updateMinutes)) * 60ull * 1000ull;
                        StartUpdateCheck();
                    }
                    OnScanTimer();
                }
                if (wp == 8 && updatePulse_) {
                    pulseDim_ = !pulseDim_;
                    UpdateTray();
                }
                return 0;
            case WM_MOUSEWHEEL:
                if (settingsOpen_) {
                    const int delta = GET_WHEEL_DELTA_WPARAM(wp);
                    settingsScroll_ = std::max(0, settingsScroll_ - delta / 120);
                    if (settingsScroll_ > settingsMaxScroll_) settingsScroll_ = settingsMaxScroll_;
                    return 0;
                }
                if (stage_.load() == Stage::Monitor || stage_.load() == Stage::Home) {
                    const int delta = GET_WHEEL_DELTA_WPARAM(wp);
                    healthScroll_ = std::max(0, healthScroll_ - delta / 120);
                }
                return 0;
            default:
                if (showMsg_ && msg == showMsg_) {
                    ShowWindow(hwnd_, SW_SHOW);
                    SetForegroundWindow(hwnd_);
                    return 0;
                }
                if (msg == WM_APP + 42) {
                    OnTray(lp);
                    return 0;
                }
                if (msg == WM_APP + 43) {
                    OnHealthReady();
                    return 0;
                }
                if (msg == WM_APP + 44) {
                    MaybeNotify();
                    return 0;
                }
                break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    // ---- one worker per tool ----
    static bool JobBusyPhase(int phase) {
        return phase == static_cast<int>(JobPhase::Waiting) || phase == static_cast<int>(JobPhase::Running);
    }

    bool JobBusy(JobId id) const {
        if (id == JobId::Count) return false;
        return JobBusyPhase(jobs_[static_cast<int>(id)].phase.load());
    }

    const wchar_t* JobTitle(JobId id) const {
        switch (id) {
            case JobId::Prepare: return L"PREPARE";
            case JobId::Measure: return L"MEASURE";
            case JobId::Hold: return L"INSTALLING";
            case JobId::Cpu: return L"CPU TUNE";
            case JobId::GpuClean: return L"GPU CLEAN";
            case JobId::Gpu: return L"GPU CONFIG";
            case JobId::Afterburner: return L"AFTERBURNER";
            case JobId::Memory: return L"MEMORY";
            case JobId::Storage: return L"STORAGE";
            default: return L"SYNCHRONUS";
        }
    }

    const wchar_t* JobLabel(JobId id) const {
        switch (id) {
            case JobId::Prepare: return L"Restore point";
            case JobId::Measure: return L"Measure";
            case JobId::Hold: return L"Timer service";
            case JobId::Cpu: return L"CPU power";
            case JobId::GpuClean: return L"GPU driver";
            case JobId::Gpu: return L"GPU settings";
            case JobId::Afterburner: return L"Afterburner";
            case JobId::Memory: return L"Memory";
            case JobId::Storage: return L"Storage";
            default: return L"";
        }
    }

    JobId StageJob(Stage stage) const {
        switch (stage) {
            case Stage::PreparePrompt:
            case Stage::PrepareApplying:
            case Stage::PrepareDone:
            case Stage::PrepareFailed: return JobId::Prepare;
            case Stage::Running:
            case Stage::Result:
            case Stage::Failed:
            case Stage::Cancelled: return JobId::Measure;
            case Stage::Installing:
            case Stage::Installed:
            case Stage::InstallFailed: return JobId::Hold;
            case Stage::PpmPrompt:
            case Stage::PpmApplying:
            case Stage::PpmDone:
            case Stage::PpmFailed: return JobId::Cpu;
            case Stage::GpuClean: return JobId::GpuClean;
            case Stage::GpuPrompt:
            case Stage::GpuApplying:
            case Stage::GpuDone:
            case Stage::GpuFailed: return JobId::Gpu;
            case Stage::AfterburnerPrompt:
            case Stage::AfterburnerApplying:
            case Stage::AfterburnerDone:
            case Stage::AfterburnerFailed: return JobId::Afterburner;
            case Stage::MemoryPrompt:
            case Stage::MemoryApplying:
            case Stage::MemoryDone:
            case Stage::MemoryFailed: return JobId::Memory;
            case Stage::StoragePrompt:
            case Stage::StorageApplying:
            case Stage::StorageDone:
            case Stage::StorageFailed: return JobId::Storage;
            default: return JobId::Count;
        }
    }

    void CancelJob(JobId id) {
        if (id == JobId::Count) return;
        auto& job = jobs_[static_cast<int>(id)];
        job.cancel.store(true);
        job.feed.Cancel();
        if (id == JobId::GpuClean) gpuStop_.store(true);
    }

    void CancelAllJobs() {
        for (int i = 0; i < static_cast<int>(JobId::Count); ++i) CancelJob(static_cast<JobId>(i));
    }

    void JoinJobs() {
        for (auto& job : jobs_) {
            if (job.thread.joinable()) job.thread.join();
        }
    }

    struct HeldLock {
        std::mutex* m = nullptr;
        ~HeldLock() {
            if (m) m->unlock();
        }
    };

    bool AcquireGate(JobId self, std::mutex& gate, const wchar_t* why, SharedState& feed, HeldLock& held) {
        auto& slot = jobs_[static_cast<int>(self)];
        if (!gate.try_lock()) {
            feed.Phase(JobTitle(self), std::wstring(L"waiting, ") + why);
            slot.phase.store(static_cast<int>(JobPhase::Waiting));
            while (!gate.try_lock()) {
                if (slot.cancel.load() || feed.Cancelled()) return false;
                Sleep(40);
            }
        }
        if (slot.cancel.load() || feed.Cancelled()) {
            gate.unlock();
            return false;
        }
        slot.phase.store(static_cast<int>(JobPhase::Running));
        held.m = &gate;
        return true;
    }

    void LaunchJob(JobId id, std::function<int(SharedState&)> work) {
        if (JobBusy(id)) return;
        auto& job = jobs_[static_cast<int>(id)];
        if (job.thread.joinable()) job.thread.join();
        job.cancel.store(false);
        job.code = 0;
        job.feed.Reset();
        if (host_.system) job.feed.System(host_.system());
        job.phase.store(static_cast<int>(JobPhase::Running));
        job.thread = std::thread([this, id, work = std::move(work)] {
            auto& slot = jobs_[static_cast<int>(id)];
            int rc = 1;
            try {
                rc = work(slot.feed);
            } catch (...) {
                rc = 1;
            }
            slot.code = rc;
            if (rc == 0)
                slot.phase.store(static_cast<int>(JobPhase::Done));
            else if (slot.cancel.load() || slot.feed.Cancelled())
                slot.phase.store(static_cast<int>(JobPhase::Cancelled));
            else
                slot.phase.store(static_cast<int>(JobPhase::Failed));
        });
    }

    std::wstring JobBanner() {
        std::wstring run, wait, bad;
        auto add = [](std::wstring& dest, const wchar_t* name) {
            if (!dest.empty()) dest += L", ";
            dest += name;
        };
        for (int i = 0; i < static_cast<int>(JobId::Count); ++i) {
            const auto id = static_cast<JobId>(i);
            const int phase = jobs_[i].phase.load();
            if (phase == static_cast<int>(JobPhase::Running)) add(run, JobLabel(id));
            else if (phase == static_cast<int>(JobPhase::Waiting)) add(wait, JobLabel(id));
            else if (phase == static_cast<int>(JobPhase::Failed) || phase == static_cast<int>(JobPhase::Cancelled))
                add(bad, JobLabel(id));
        }
        if (deferGpu_) {
            GpuDl phase;
            {
                std::lock_guard<std::mutex> g(gpuM_);
                phase = gpuPhase_;
            }
            if (phase == GpuDl::Offer || phase == GpuDl::Ready) add(wait, L"GPU driver choice");
        }
        std::wstring out;
        if (!run.empty()) out = L"Running: " + run;
        if (!wait.empty()) {
            if (!out.empty()) out += L"    ";
            out += L"Waiting: " + wait;
        }
        if (!bad.empty()) {
            if (!out.empty()) out += L"    ";
            out += L"Stopped: " + bad;
        }
        return out;
    }

    void OpenTool(JobId id, Stage prompt, Stage applying) {
        hubReturn_ = true;
        stage_ = JobBusy(id) ? applying : prompt;
    }

    void StartUpdateCheck() {
        if (!host_.checkUpdate || updateBusy_.load() || updateChecking_.load()) return;
        if (updateThread_.joinable()) updateThread_.join();
        updateChecking_.store(true);
        {
            std::lock_guard<std::mutex> g(updateM_);
            updateReady_ = false;
        }
        updateThread_ = std::thread([this] {
            AppUpdate offer;
            std::wstring err;
            const bool newer = host_.checkUpdate(offer, updateStop_, err);
            {
                std::lock_guard<std::mutex> g(updateM_);
                updateOffer_ = std::move(offer);
                updateNewer_ = newer;
                updateReady_ = true;
            }
            updateChecking_.store(false);
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

    // The version check starts with the window and repeats on the Settings interval.
    // A download starts only after the user agrees. The same version is announced once.
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
        if (updateBusy_.load()) return;

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
        if (showUpdatePrompt_ && newer) {
            showUpdatePrompt_ = false;
            updateAnnounced_ = offer.version;
        } else if (!newer || offer.version == updateAnnounced_) {
            return;
        } else {
            updateAnnounced_ = offer.version;
            updatePulse_ = true;
            SetTimer(hwnd_, 8, 450, nullptr);
            if (trayOn_) {
                nid_.uFlags = NIF_INFO | NIF_ICON | NIF_TIP | NIF_MESSAGE;
                wcsncpy_s(nid_.szInfoTitle, L"Synchronus update", _TRUNCATE);
                const std::wstring body = L"Version " + offer.version + L" is ready. Open Synchronus to install it.";
                wcsncpy_s(nid_.szInfo, body.c_str(), _TRUNCATE);
                nid_.dwInfoFlags = NIIF_INFO;
                Shell_NotifyIconW(NIM_MODIFY, &nid_);
            }
            return;
        }

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

    void StartBenchmark(bool show = true) {
        if (!JobBusy(JobId::Measure)) {
            const int fidelity = fidelity_;
            revealT_ = 0;
            installT_ = 0;
            particles_.clear();
            LaunchJob(JobId::Measure, [this, fidelity](SharedState& feed) {
                HeldLock clock;
                if (!AcquireGate(JobId::Measure, timerMu_, L"the timer service install is using the clock", feed, clock))
                    return 1;
                {
                    std::lock_guard<std::mutex> g(resultM_);
                    result_ = ResultView{};
                }
                ResultView r = host_.benchmark(feed, fidelity);
                {
                    std::lock_guard<std::mutex> g(resultM_);
                    result_ = r;
                }
                if (r.stoppedService && !JobBusy(JobId::Hold)) {
                    if (host_.restartService) host_.restartService();
                    serviceStopped_ = false;
                } else if (r.stoppedService) {
                    serviceStopped_ = true;
                }
                if (feed.Cancelled()) return 1;
                if (r.winner == 0) {
                    feed.Phase(L"FAILED", r.failure.empty() ? L"the measurement did not produce a resolution" : r.failure);
                    return 1;
                }
                feed.Progress(1.0, 0, 0);
                feed.Phase(L"RESULT", Fmt(L"%.4f ms selected after verification", Ms(r.winner)));
                return 0;
            });
        }
        if (show) stage_ = Stage::Running;
    }

    void StartInstall(bool show = true, unsigned unitsOverride = 0) {
        if (!JobBusy(JobId::Hold)) {
            LaunchJob(JobId::Hold, [this, unitsOverride](SharedState& feed) {
                auto& slot = jobs_[static_cast<int>(JobId::Hold)];
                unsigned units = unitsOverride;
                if (!units) {
                    while (JobBusy(JobId::Measure)) {
                        if (slot.cancel.load() || feed.Cancelled()) return 1;
                        feed.Phase(L"INSTALLING", L"waiting for the timer measurement to finish");
                        slot.phase.store(static_cast<int>(JobPhase::Waiting));
                        Sleep(40);
                    }
                    slot.phase.store(static_cast<int>(JobPhase::Running));
                    {
                        std::lock_guard<std::mutex> g(resultM_);
                        units = result_.winner;
                    }
                    if (!units) units = host_.pinnedResolution;
                }
                if (!units) {
                    feed.Phase(L"INSTALL FAILED", L"measure a timer resolution first");
                    return 1;
                }
                HeldLock clock;
                if (!AcquireGate(JobId::Hold, timerMu_, L"the timer measurement is using the clock", feed, clock))
                    return 1;
                if (!unitsOverride) {
                    std::lock_guard<std::mutex> g(resultM_);
                    if (result_.winner) units = result_.winner;
                }
                feed.Phase(L"INSTALLING", L"copying the executable, writing the registry, starting the service");
                const int rc = host_.install ? host_.install(units, feed) : 1;
                if (rc == 2) {
                    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                    return 0;
                }
                if (host_.system) feed.System(host_.system());
                if (rc == 0) {
                    installedThisSession_ = true;
                    feed.Phase(L"INSTALLED", Fmt(L"the service holds %.4f ms now and at every boot", Ms(units)));
                    if (host_.probeCpu) {
                        CpuProbeView p = host_.probeCpu();
                        std::lock_guard<std::mutex> g(probeM_);
                        probe_ = p;
                        probeReady_ = true;
                    }
                    return 0;
                }
                feed.Phase(L"INSTALL FAILED", L"see the log for details");
                return 1;
            });
        }
        if (show) stage_ = Stage::Installing;
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
            if (host_.autoInstall && i == kWantGpuClean) continue;
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
                                                     ? L"static frequency detected. PPM checks are just noise"
                                                     : p.mode == CpuProbeView::Mode::Dynamic
                                                           ? L"dynamic boost detected. A 5000 ms interval would delay turbo"
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
                case kWantAfterburner:
                    if (!host_.applyAfterburner) continue;
                    if (host_.autoInstall) {
                        StartAfterburner();
                        return;
                    }
                    stage_ = Stage::AfterburnerPrompt;
                    state_.Phase(L"AFTERBURNER", L"download and scan, about 30 minutes");
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
        state_.Phase(L"REVIEW", L"set BIOS, then restart");
    }

    void MaybeStartPpm() { StartAtOrAfter(kWantCpu); }
    void MaybeStartGpu() { StartAtOrAfter(kWantGpu); }
    void MaybeStartAfterburner() { StartAtOrAfter(kWantAfterburner); }
    void MaybeStartMemory() { StartAtOrAfter(kWantMemory); }
    void MaybeStartStorage() { StartAtOrAfter(kWantStorage); }

    void BeginWorkflow() {
        StepChoices choices;
        choices.prepare = Want(kWantPrepare);
        choices.measure = Want(kWantMeasure);
        choices.hold = Want(kWantHold);
        choices.cpu = Want(kWantCpu);
        choices.gpuClean = Want(kWantGpuClean);
        choices.gpu = Want(kWantGpu);
        choices.afterburner = Want(kWantAfterburner);
        choices.memory = Want(kWantMemory);
        choices.storage = Want(kWantStorage);
        SaveStepChoices(choices);
        MarkSetupComplete();
        chainAuto_ = false;
        hubReturn_ = false;
        deferGpu_ = false;
        fleet_ = true;
        afterburnerPrompt_ = false;
        afterburnerSkipped_ = false;
        for (int i = 0; i < static_cast<int>(JobId::Count); ++i) {
            if (JobBusy(static_cast<JobId>(i))) continue;
            const int phase = jobs_[i].phase.load();
            if (phase == static_cast<int>(JobPhase::Done) || phase == static_cast<int>(JobPhase::Failed) ||
                phase == static_cast<int>(JobPhase::Cancelled))
                jobs_[i].phase.store(static_cast<int>(JobPhase::Idle));
        }
        if (!JobBusy(JobId::GpuClean)) {
            std::lock_guard<std::mutex> g(gpuM_);
            gpuPhase_ = GpuDl::Idle;
            gpuOffer_ = {};
        }
        driverSettled_ = !Want(kWantGpuClean);
        prepareWaived_ = false;
        gpuStop_.store(false);
        stage_ = Stage::Fleet;
    }

    void ApplySavedChoices() {
        const StepChoices choices = LoadStepChoices();
        want_[kWantPrepare] = choices.prepare;
        want_[kWantMeasure] = choices.measure;
        want_[kWantHold] = choices.hold;
        want_[kWantCpu] = choices.cpu;
        want_[kWantGpuClean] = choices.gpuClean;
        want_[kWantGpu] = choices.gpu;
        want_[kWantAfterburner] = choices.afterburner;
        want_[kWantMemory] = choices.memory;
        want_[kWantStorage] = choices.storage;
    }

    void OpenHome() {
        ApplySavedChoices();
        homeLoaded_ = false;
        stage_ = Stage::Home;
        state_.Phase(L"HOME", L"choose a function");
        if (checks_.empty() && !healthBusy_.load()) StartHealthScan();
    }

    void ShowMain() {
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        if (SetupComplete()) OpenHome();
    }

    void OpenStep(Stage next) {
        hubReturn_ = true;
        stage_ = next;
    }

    void StartPpm(bool show = true) {
        if (!JobBusy(JobId::Cpu)) {
            LaunchJob(JobId::Cpu, [this](SharedState& feed) {
                HeldLock power;
                if (!AcquireGate(JobId::Cpu, powerMu_, L"storage settings are writing the power plan", feed, power))
                    return 1;
                feed.Phase(L"CPU TUNE", L"writing power-plan settings");
                const int rc = host_.applyPpm ? host_.applyPpm(feed) : 1;
                feed.Phase(rc == 0 ? L"PPM APPLIED" : L"PPM FAILED",
                           rc == 0 ? L"every processor power setting written" : L"see the log for details");
                return rc;
            });
        }
        if (show) stage_ = Stage::PpmApplying;
    }

    void StartGpu(bool show = true) {
        if (!JobBusy(JobId::Gpu)) {
            gpuSkipped_ = false;
            LaunchJob(JobId::Gpu, [this](SharedState& feed) {
                HeldLock gpu;
                if (!AcquireGate(JobId::Gpu, gpuMu_, L"the driver install or Afterburner is using the graphics driver", feed, gpu))
                    return 1;
                feed.Phase(L"GPU CONFIG", L"writing driver / Windows graphics settings");
                const int rc = host_.applyGpu ? host_.applyGpu(feed) : 1;
                feed.Phase(L"GPU CONFIG", rc == 0 ? L"HAGS, overlays, NVIDIA/AMD profile" : L"see the log for details");
                return rc;
            });
        }
        if (show) stage_ = Stage::GpuApplying;
    }

    void StartAfterburner(bool show = true) {
        if (!JobBusy(JobId::Afterburner)) {
            afterburnerStale_.store(false);
            LaunchJob(JobId::Afterburner, [this](SharedState& feed) {
                HeldLock gpu;
                if (!AcquireGate(JobId::Afterburner, gpuMu_, L"the driver install or GPU settings are using the graphics driver", feed, gpu))
                    return 1;
                feed.Phase(L"AFTERBURNER", L"installing Afterburner and RivaTuner");
                const int rc = host_.applyAfterburner ? host_.applyAfterburner(feed) : 1;
                feed.Phase(L"AFTERBURNER", rc == 0 ? L"overclock applies at every logon" : L"see the log for details");
                return rc;
            });
        }
        if (show) stage_ = Stage::AfterburnerApplying;
    }

    void StartPrepare(bool show = true) {
        if (!JobBusy(JobId::Prepare)) {
            LaunchJob(JobId::Prepare, [this](SharedState& feed) {
                HeldLock volume;
                if (!AcquireGate(JobId::Prepare, volumeMu_, L"a driver install is using the disk", feed, volume))
                    return 1;
                feed.Phase(L"PREPARE", L"creating a System Restore point");
                const int rc = host_.applyPrepare ? host_.applyPrepare(feed) : 1;
                feed.Phase(rc == 0 ? L"PREPARE" : L"PREPARE FAILED",
                           rc == 0 ? L"restore point created" : L"see the log for details");
                return rc;
            });
        }
        if (show) stage_ = Stage::PrepareApplying;
    }

    void StartMemory(bool show = true) {
        if (!JobBusy(JobId::Memory)) {
            LaunchJob(JobId::Memory, [this](SharedState& feed) {
                HeldLock volume;
                if (!AcquireGate(JobId::Memory, volumeMu_, L"a restore point or driver install is using the disk", feed, volume))
                    return 1;
                feed.Phase(L"MEMORY", L"pagefile and memory compression");
                const int rc = host_.applyMemory ? host_.applyMemory(feed) : 1;
                feed.Phase(rc == 0 ? L"MEMORY" : L"MEMORY FAILED",
                           rc == 0 ? L"pagefile and compression updated" : L"see the log for details");
                return rc;
            });
        }
        if (show) stage_ = Stage::MemoryApplying;
    }

    void StartStorage(bool show = true) {
        if (!JobBusy(JobId::Storage)) {
            LaunchJob(JobId::Storage, [this](SharedState& feed) {
                HeldLock power;
                if (!AcquireGate(JobId::Storage, powerMu_, L"CPU power settings are writing the power plan", feed, power))
                    return 1;
                feed.Phase(L"STORAGE", L"SSD, PCIe, USB, TRIM");
                const int rc = host_.applyStorage ? host_.applyStorage(feed) : 1;
                feed.Phase(rc == 0 ? L"STORAGE" : L"STORAGE FAILED",
                           rc == 0 ? L"storage settings written" : L"see the log for details");
                return rc;
            });
        }
        if (show) stage_ = Stage::StorageApplying;
    }

    bool PhaseIdle(JobId id) const {
        return jobs_[static_cast<int>(id)].phase.load() == static_cast<int>(JobPhase::Idle);
    }

    GpuDl GpuPhase() const {
        std::lock_guard<std::mutex> g(gpuM_);
        return gpuPhase_;
    }

    bool VolumeBusy() const {
        return JobBusy(JobId::Prepare) || JobBusy(JobId::Memory) || GpuPhase() == GpuDl::Installing;
    }
    bool PowerBusy() const { return JobBusy(JobId::Cpu) || JobBusy(JobId::Storage); }
    bool TimerBusy() const { return JobBusy(JobId::Measure) || JobBusy(JobId::Hold); }
    bool GpuResourceBusy() const {
        return JobBusy(JobId::Gpu) || JobBusy(JobId::Afterburner) || GpuPhase() == GpuDl::Installing;
    }

    bool RestoreReady() const {
        if (!Want(kWantPrepare) || prepareWaived_) return true;
        return jobs_[static_cast<int>(JobId::Prepare)].phase.load() == static_cast<int>(JobPhase::Done);
    }

    bool PrepareNeedsChoice() const {
        if (!Want(kWantPrepare) || prepareWaived_ || JobBusy(JobId::Prepare)) return false;
        const int phase = jobs_[static_cast<int>(JobId::Prepare)].phase.load();
        return phase == static_cast<int>(JobPhase::Failed) || phase == static_cast<int>(JobPhase::Cancelled);
    }

    bool MeasureBlocking() const { return Want(kWantMeasure) && JobBusy(JobId::Measure); }

    bool CpuSettled() const {
        if (!Want(kWantCpu)) return true;
        const int phase = jobs_[static_cast<int>(JobId::Cpu)].phase.load();
        return phase == static_cast<int>(JobPhase::Done) || phase == static_cast<int>(JobPhase::Failed) ||
               phase == static_cast<int>(JobPhase::Cancelled);
    }

    bool DriverSettled() const {
        if (!Want(kWantGpuClean) || driverSettled_) return true;
        return GpuPhase() == GpuDl::Done;
    }

    bool DriverNeedsChoice() const {
        if (!Want(kWantGpuClean) || driverSettled_ || JobBusy(JobId::GpuClean)) return false;
        const GpuDl phase = GpuPhase();
        return phase == GpuDl::Offer || phase == GpuDl::Ready || phase == GpuDl::Manual || phase == GpuDl::Failed;
    }

    bool HoldReady() const {
        if (!Want(kWantHold) || !PhaseIdle(JobId::Hold) || TimerBusy()) return false;
        if (Want(kWantMeasure) && jobs_[static_cast<int>(JobId::Measure)].phase.load() != static_cast<int>(JobPhase::Done))
            return false;
        unsigned units = 0;
        {
            std::lock_guard<std::mutex> g(resultM_);
            units = result_.winner;
        }
        if (!units) units = host_.pinnedResolution;
        return units != 0;
    }

    bool JobSettled(JobId id) const {
        if (id == JobId::Prepare) {
            if (prepareWaived_) return true;
            return jobs_[static_cast<int>(id)].phase.load() == static_cast<int>(JobPhase::Done);
        }
        if (id == JobId::GpuClean && DriverSettled()) return true;
        if (id == JobId::Afterburner && afterburnerSkipped_) return true;
        if (id == JobId::Hold && Want(kWantMeasure)) {
            const int measure = jobs_[static_cast<int>(JobId::Measure)].phase.load();
            if (measure == static_cast<int>(JobPhase::Failed) || measure == static_cast<int>(JobPhase::Cancelled))
                return true;
        }
        if (id == JobId::Hold && !Want(kWantMeasure) && !JobBusy(JobId::Hold) && PhaseIdle(JobId::Hold)) {
            unsigned units = 0;
            {
                std::lock_guard<std::mutex> g(resultM_);
                units = result_.winner;
            }
            if (!units && !host_.pinnedResolution) return true;
        }
        const int phase = jobs_[static_cast<int>(id)].phase.load();
        return phase == static_cast<int>(JobPhase::Done) || phase == static_cast<int>(JobPhase::Failed) ||
               phase == static_cast<int>(JobPhase::Cancelled);
    }

    bool FleetComplete() const {
        const JobId ids[] = {JobId::Prepare, JobId::Measure, JobId::Hold, JobId::Cpu, JobId::GpuClean,
                             JobId::Gpu, JobId::Afterburner, JobId::Memory, JobId::Storage};
        const int wants[] = {kWantPrepare, kWantMeasure, kWantHold, kWantCpu, kWantGpuClean,
                             kWantGpu, kWantAfterburner, kWantMemory, kWantStorage};
        for (int i = 0; i < kWantCount; ++i) {
            if (!Want(wants[i])) continue;
            if (!JobSettled(ids[i])) return false;
        }
        return !afterburnerPrompt_ && !DriverNeedsChoice() && !PrepareNeedsChoice();
    }

    int OccupiedSlots() const {
        int n = 0;
        for (int i = 0; i < static_cast<int>(JobId::Count); ++i) {
            if (JobBusy(static_cast<JobId>(i))) ++n;
        }
        if (PrepareNeedsChoice()) ++n;
        if (DriverNeedsChoice()) ++n;
        if (afterburnerPrompt_ && !JobBusy(JobId::Afterburner)) ++n;
        return n;
    }

    int FleetCapacity(float w, float h) const {
        const float gap = 12.0f;
        const float minW = 300.0f;
        const float minH = 196.0f;
        const int cols = std::max(1, static_cast<int>(std::floor((w + gap) / (minW + gap))));
        const int rows = std::max(1, static_cast<int>(std::floor((h + gap) / (minH + gap))));
        return std::min(4, cols) * std::min(3, rows);
    }

    bool StartNextSafe() {
        if (Want(kWantPrepare) && !prepareWaived_ && PhaseIdle(JobId::Prepare) && !VolumeBusy()) {
            StartPrepare(false);
            return true;
        }
        if (!RestoreReady()) return false;
        if (Want(kWantMeasure) && PhaseIdle(JobId::Measure) && !TimerBusy()) {
            StartBenchmark(false);
            return true;
        }
        if (MeasureBlocking()) return false;
        if (Want(kWantCpu) && PhaseIdle(JobId::Cpu) && !PowerBusy()) {
            StartPpm(false);
            return true;
        }
        if (Want(kWantMemory) && PhaseIdle(JobId::Memory) && !VolumeBusy()) {
            StartMemory(false);
            return true;
        }
        if (Want(kWantStorage) && PhaseIdle(JobId::Storage) && CpuSettled() && !PowerBusy()) {
            StartStorage(false);
            return true;
        }
        if (HoldReady()) {
            StartInstall(false);
            return true;
        }
        if (Want(kWantGpuClean) && !driverSettled_ && GpuPhase() == GpuDl::Idle && !JobBusy(JobId::GpuClean)) {
            BeginGpuLookup();
            return JobBusy(JobId::GpuClean);
        }
        if (Want(kWantGpu) && PhaseIdle(JobId::Gpu) && DriverSettled() && !GpuResourceBusy()) {
            StartGpu(false);
            return true;
        }
        return false;
    }

    bool ShouldOfferAfterburner() const {
        if (!Want(kWantAfterburner) || afterburnerSkipped_ || afterburnerPrompt_) return false;
        if (!PhaseIdle(JobId::Afterburner) || JobBusy(JobId::Afterburner)) return false;
        if (!RestoreReady() || MeasureBlocking()) return false;
        if (!DriverSettled() || GpuResourceBusy()) return false;
        if (Want(kWantGpu) && PhaseIdle(JobId::Gpu)) return false;
        return true;
    }

    void PumpFleet(int capacity) {
        if (!fleet_ || stage_.load() != Stage::Fleet) return;
        if (capacity < 1) capacity = 1;
        while (OccupiedSlots() < capacity) {
            if (!StartNextSafe()) break;
        }
        if (ShouldOfferAfterburner() && OccupiedSlots() < capacity) afterburnerPrompt_ = true;
        if (FleetComplete()) {
            fleet_ = false;
            afterburnerPrompt_ = false;
            stage_ = Stage::Review;
            state_.Phase(L"REVIEW", L"set BIOS, then restart");
        }
    }

    void FleetSkipDriver() {
        gpuStop_.store(true);
        CancelJob(JobId::GpuClean);
        {
            std::lock_guard<std::mutex> g(gpuM_);
            gpuPhase_ = GpuDl::Idle;
        }
        driverSettled_ = true;
    }

    void DrawFleet(const D2D1_RECT_F& B, float dt) {
        struct Pane {
            int kind;
            JobId id;
        };
        Pane panes[9];
        int n = 0;
        auto push = [&](int kind, JobId id) {
            if (n < 9) panes[n++] = Pane{kind, id};
        };
        for (int i = 0; i < static_cast<int>(JobId::Count); ++i) {
            const auto id = static_cast<JobId>(i);
            if (JobBusy(id)) push(0, id);
        }
        if (PrepareNeedsChoice()) push(3, JobId::Prepare);
        if (DriverNeedsChoice()) push(1, JobId::GpuClean);
        if (afterburnerPrompt_ && !JobBusy(JobId::Afterburner)) push(2, JobId::Afterburner);
        if (n == 0) {
            rd_.Text(L"The restore point runs first. Nothing else starts until it finishes.",
                     D2D1::RectF(B.left, B.top, B.right, B.top + 40), rd_.fPhase.Get(), col::cyan,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }

        const float gap = 12.0f;
        int cols = 1;
        while (cols < n && (cols + 1) * 300.0f + cols * gap <= W(B)) ++cols;
        const int rows = (n + cols - 1) / cols;
        const float pw = (W(B) - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        const float ph = (H(B) - gap * static_cast<float>(rows - 1)) / static_cast<float>(rows);
        for (int i = 0; i < n; ++i) {
            const float x = B.left + static_cast<float>(i % cols) * (pw + gap);
            const float y = B.top + static_cast<float>(i / cols) * (ph + gap);
            const D2D1_RECT_F pane = D2D1::RectF(x, y, x + pw, y + ph);
            DrawFleetPane(pane, panes[i].kind, panes[i].id, dt, 300 + i * 4);
        }
        if (FleetComplete()) {
            fleet_ = false;
            afterburnerPrompt_ = false;
            stage_ = Stage::Review;
            state_.Phase(L"REVIEW", L"set BIOS, then restart");
        }
    }

    void DrawFleetPane(const D2D1_RECT_F& pane, int kind, JobId id, float dt, int idBase) {
        rd_.FillRound(pane, 14, Alpha(col::panel, 0.9f));
        rd_.StrokeRound(pane, 14, Alpha(col::cyan, 0.28f), 1.2f);
        const wchar_t* title = kind == 2 ? L"Afterburner" : JobLabel(id);
        rd_.Text(title, D2D1::RectF(pane.left + 14, pane.top + 10, pane.right - 100, pane.top + 36), rd_.fPhase.Get(),
                 col::cyan);
        if (kind == 0 && JobBusy(id)) {
            if (Button(idBase, D2D1::RectF(pane.right - 86, pane.top + 10, pane.right - 12, pane.top + 36), L"Stop",
                       col::red, false, true, dt))
                CancelJob(id);
            Snapshot job;
            jobs_[static_cast<int>(id)].feed.Snap(job);
            const bool waiting = jobs_[static_cast<int>(id)].phase.load() == static_cast<int>(JobPhase::Waiting);
            rd_.Text(waiting ? L"Waiting" : L"Running",
                     D2D1::RectF(pane.left + 14, pane.top + 38, pane.right - 14, pane.top + 58), rd_.fSmall.Get(),
                     waiting ? col::amber : col::text);
            std::wstring detail = job.detail.empty() ? job.phase : job.detail;
            if (detail.empty()) detail = job.narrateBody.empty() ? job.narrateTitle : job.narrateBody;
            rd_.Text(detail, D2D1::RectF(pane.left + 14, pane.top + 60, pane.right - 14, pane.bottom - 28), rd_.fBody.Get(),
                     col::muted);
            if (job.progress > 0.001) {
                const D2D1_RECT_F bar = D2D1::RectF(pane.left + 14, pane.bottom - 18, pane.right - 14, pane.bottom - 10);
                rd_.FillRound(bar, 3, col::panel2);
                const float fx = bar.left + W(bar) * static_cast<float>(std::min(1.0, job.progress));
                if (fx > bar.left + 2) rd_.FillRound(D2D1::RectF(bar.left, bar.top, fx, bar.bottom), 3, col::cyan);
            }
            return;
        }
        if (kind == 3) {
            rd_.Text(L"The restore point did not finish. Nothing else has been changed.",
                     D2D1::RectF(pane.left + 14, pane.top + 44, pane.right - 14, pane.bottom - 52), rd_.fBody.Get(),
                     col::muted);
            const D2D1_RECT_F yes = D2D1::RectF(pane.left + 14, pane.bottom - 40, pane.left + W(pane) * 0.62f, pane.bottom - 10);
            const D2D1_RECT_F no = D2D1::RectF(yes.right + 8, pane.bottom - 40, pane.right - 12, pane.bottom - 10);
            if (Button(idBase, yes, L"Retry restore point", col::cyan, true, true, dt)) {
                jobs_[static_cast<int>(JobId::Prepare)].phase.store(static_cast<int>(JobPhase::Idle));
            }
            if (Button(idBase + 1, no, L"Continue anyway", col::amber, false, true, dt)) prepareWaived_ = true;
            return;
        }
        if (kind == 1) {
            GpuDriverOffer offer;
            GpuDl phase;
            {
                std::lock_guard<std::mutex> g(gpuM_);
                offer = gpuOffer_;
                phase = gpuPhase_;
            }
            std::wstring headline = L"Choose whether to install the graphics driver.";
            if (phase == GpuDl::Manual)
                headline = offer.error.empty() ? L"No vendor package can be installed from here." : offer.error;
            else if (phase == GpuDl::Failed)
                headline = offer.error.empty() ? L"The driver step failed." : offer.error;
            else if (!offer.gpuName.empty())
                headline = offer.gpuName;
            rd_.Text(headline, D2D1::RectF(pane.left + 14, pane.top + 40, pane.right - 14, pane.top + 88), rd_.fSmall.Get(),
                     col::text);
            std::wstring sub = offer.version.empty() ? L"Installs the graphics driver only." : offer.version;
            rd_.Text(sub, D2D1::RectF(pane.left + 14, pane.top + 90, pane.right - 14, pane.bottom - 48), rd_.fBody.Get(),
                     col::muted);
            const D2D1_RECT_F yes = D2D1::RectF(pane.left + 14, pane.bottom - 40, pane.left + W(pane) * 0.62f, pane.bottom - 10);
            const D2D1_RECT_F no = D2D1::RectF(yes.right + 8, pane.bottom - 40, pane.right - 12, pane.bottom - 10);
            if (phase == GpuDl::Offer && offer.canDownload) {
                if (Button(idBase, yes, L"Download driver", col::cyan, true, true, dt)) BeginGpuDownload();
            } else if (phase == GpuDl::Ready) {
                if (Button(idBase, yes, L"Install driver", col::green, true, true, dt)) BeginGpuInstall();
            } else if (phase == GpuDl::Failed) {
                if (Button(idBase, yes, L"Retry", col::cyan, true, true, dt)) {
                    if (offer.fail == 2 && !offer.file.empty()) BeginGpuInstall();
                    else if (offer.ok && offer.canDownload) BeginGpuDownload();
                    else BeginGpuLookup();
                }
            } else if (phase == GpuDl::Manual) {
                if (Button(idBase, yes, L"Continue", col::cyan, true, true, dt)) driverSettled_ = true;
            }
            if (Button(idBase + 1, no, L"Skip", col::muted, false, true, dt)) FleetSkipDriver();
            return;
        }
        rd_.Text(L"Afterburner downloads, scans the GPU, and saves the curve. This takes about 30 minutes.",
                 D2D1::RectF(pane.left + 14, pane.top + 44, pane.right - 14, pane.bottom - 52), rd_.fBody.Get(), col::muted);
        const D2D1_RECT_F yes = D2D1::RectF(pane.left + 14, pane.bottom - 40, pane.left + W(pane) * 0.62f, pane.bottom - 10);
        const D2D1_RECT_F no = D2D1::RectF(yes.right + 8, pane.bottom - 40, pane.right - 12, pane.bottom - 10);
        if (Button(idBase, yes, L"Start Afterburner", col::violet, true, true, dt)) {
            afterburnerPrompt_ = false;
            StartAfterburner(false);
        }
        if (Button(idBase + 1, no, L"Skip", col::muted, false, true, dt)) {
            afterburnerPrompt_ = false;
            afterburnerSkipped_ = true;
        }
    }

    void ReleaseGpuFollowers() {
        if (!deferGpu_) return;
        deferGpu_ = false;
        if (Want(kWantGpu)) StartGpu(false);
        if (Want(kWantAfterburner)) StartAfterburner(false);
    }

    void PollGpuFollowers() {
        if (!deferGpu_) return;
        GpuDl phase;
        {
            std::lock_guard<std::mutex> g(gpuM_);
            phase = gpuPhase_;
        }
        if (JobBusy(JobId::GpuClean)) return;
        if (phase == GpuDl::Looking || phase == GpuDl::Downloading || phase == GpuDl::Installing ||
            phase == GpuDl::Offer || phase == GpuDl::Ready)
            return;
        ReleaseGpuFollowers();
    }

    void PublishSnap(Stage stage) {
        const JobId focus = StageJob(stage);
        jobs_[static_cast<int>(JobId::Measure)].feed.Snap(snap_);
        if (focus == JobId::Measure) return;
        if (focus != JobId::Count) {
            Snapshot job;
            jobs_[static_cast<int>(focus)].feed.Snap(job);
            snap_.phase = job.phase;
            snap_.detail = job.detail;
            snap_.progress = job.progress;
            snap_.elapsed = job.elapsed;
            snap_.remaining = job.remaining;
            snap_.log = std::move(job.log);
            snap_.ppm = std::move(job.ppm);
            snap_.narrateTitle = job.narrateTitle;
            snap_.narrateBody = job.narrateBody;
            return;
        }
        if (stage == Stage::Fleet) {
            int wanted = 0;
            int settled = 0;
            const JobId ids[] = {JobId::Prepare, JobId::Measure, JobId::Hold, JobId::Cpu, JobId::GpuClean,
                                 JobId::Gpu, JobId::Afterburner, JobId::Memory, JobId::Storage};
            const int wants[] = {kWantPrepare, kWantMeasure, kWantHold, kWantCpu, kWantGpuClean,
                                 kWantGpu, kWantAfterburner, kWantMemory, kWantStorage};
            for (int i = 0; i < kWantCount; ++i) {
                if (!Want(wants[i])) continue;
                ++wanted;
                if (JobSettled(ids[i])) ++settled;
            }
            std::wstring detail = JobBanner();
            std::wstring finished;
            for (int i = 0; i < static_cast<int>(JobId::Count); ++i) {
                if (jobs_[i].phase.load() != static_cast<int>(JobPhase::Done)) continue;
                if (!finished.empty()) finished += L", ";
                finished += JobLabel(static_cast<JobId>(i));
            }
            if (!finished.empty()) {
                if (!detail.empty()) detail += L"    ";
                detail += L"Finished: " + finished;
            }
            state_.Phase(L"FULL TUNE", detail.empty() ? L"running the selected steps" : detail);
            state_.Progress(wanted ? static_cast<double>(settled) / static_cast<double>(wanted) : 0, 0, 0);
        } else if (stage == Stage::Home) {
            const std::wstring banner = JobBanner();
            state_.Phase(L"HOME", banner.empty() ? L"choose a function" : banner);
        } else if (stage == Stage::Monitor) {
            state_.Phase(L"HEALTH", L"read-only check of applied settings");
        } else if (stage == Stage::Review) {
            state_.Phase(L"REVIEW", L"set BIOS, then restart");
        }
        Snapshot shell;
        state_.Snap(shell);
        snap_.phase = shell.phase;
        snap_.detail = shell.detail;
        snap_.progress = stage == Stage::Fleet ? shell.progress : 0;
        snap_.elapsed = 0;
        snap_.remaining = 0;
    }

    void AdvanceChain() {
        if (!chainAuto_) return;
        for (int i = 0; i < static_cast<int>(JobId::Count); ++i) {
            const int phase = jobs_[i].phase.load();
            const int prev = chainPhase_[i];
            chainPhase_[i] = phase;
            if (prev == phase || phase != static_cast<int>(JobPhase::Done)) continue;
            const auto id = static_cast<JobId>(i);
            if (id == JobId::Prepare) StartAtOrAfter(kWantMeasure);
            else if (id == JobId::Measure && !JobBusy(JobId::Hold) &&
                     jobs_[static_cast<int>(JobId::Hold)].phase.load() != static_cast<int>(JobPhase::Done))
                StartInstall(true);
            else if (id == JobId::Hold) StartAtOrAfter(kWantCpu);
            else if (id == JobId::Cpu) StartAtOrAfter(kWantGpuClean);
            else if (id == JobId::Gpu) StartAtOrAfter(kWantAfterburner);
            else if (id == JobId::Afterburner) StartAtOrAfter(kWantMemory);
            else if (id == JobId::Memory) StartAtOrAfter(kWantStorage);
            else if (id == JobId::Storage) {
                stage_ = Stage::Review;
                state_.Phase(L"REVIEW", L"set BIOS, then restart");
            }
            return;
        }
    }

    void PollJobs() {
        struct Map {
            JobId id;
            Stage run;
            Stage done;
            Stage fail;
            Stage cancel;
        };
        const Map maps[] = {
            {JobId::Prepare, Stage::PrepareApplying, Stage::PrepareDone, Stage::PrepareFailed, Stage::PrepareFailed},
            {JobId::Measure, Stage::Running, Stage::Result, Stage::Failed, Stage::Cancelled},
            {JobId::Hold, Stage::Installing, Stage::Installed, Stage::InstallFailed, Stage::InstallFailed},
            {JobId::Cpu, Stage::PpmApplying, Stage::PpmDone, Stage::PpmFailed, Stage::PpmFailed},
            {JobId::Gpu, Stage::GpuApplying, Stage::GpuDone, Stage::GpuFailed, Stage::GpuFailed},
            {JobId::Afterburner, Stage::AfterburnerApplying, Stage::AfterburnerDone, Stage::AfterburnerFailed, Stage::AfterburnerFailed},
            {JobId::Memory, Stage::MemoryApplying, Stage::MemoryDone, Stage::MemoryFailed, Stage::MemoryFailed},
            {JobId::Storage, Stage::StorageApplying, Stage::StorageDone, Stage::StorageFailed, Stage::StorageFailed},
        };
        const Stage stage = stage_.load();
        for (const Map& map : maps) {
            if (stage != map.run) continue;
            const int phase = jobs_[static_cast<int>(map.id)].phase.load();
            if (phase == static_cast<int>(JobPhase::Done)) {
                if (map.id == JobId::Measure) revealT_ = 0.0001f;
                if (map.id == JobId::Hold) installT_ = 0.0001f;
                stage_ = map.done;
            } else if (phase == static_cast<int>(JobPhase::Failed)) {
                stage_ = map.fail;
            } else if (phase == static_cast<int>(JobPhase::Cancelled)) {
                stage_ = map.cancel;
            }
        }
        if (replayGpu_.exchange(false)) StartGpu(false);
        PollGpuFollowers();
        if (repairJob_ >= 0 && !JobBusy(static_cast<JobId>(repairJob_))) {
            const int phase = jobs_[repairJob_].phase.load();
            if (phase == static_cast<int>(JobPhase::Done) || phase == static_cast<int>(JobPhase::Failed) ||
                phase == static_cast<int>(JobPhase::Cancelled)) {
                AppendHealthLog(phase == static_cast<int>(JobPhase::Done) ? L"repair finished" : L"repair failed");
                repairJob_ = -1;
                repairRescan_ = true;
            }
        }
        if (repairRescan_ && !healthBusy_.load()) {
            repairRescan_ = false;
            StartHealthScan();
        }
        bool maintainFinished = false;
        bool maintainStill = false;
        for (int i = 0; i < static_cast<int>(JobId::Count); ++i) {
            if (!maintainWatch_[i]) continue;
            if (JobBusy(static_cast<JobId>(i))) {
                maintainStill = true;
                continue;
            }
            const int phase = jobs_[i].phase.load();
            if (phase == static_cast<int>(JobPhase::Idle)) continue;
            maintainWatch_[i] = false;
            maintainFinished = true;
            AppendHealthLog(phase == static_cast<int>(JobPhase::Done) ? L"automatic repair finished" : L"automatic repair failed");
        }
        if (maintainFinished && !maintainStill && !healthBusy_.load() && !repairRescan_) StartHealthScan();
        KickHold();
        AdvanceChain();
    }

    void KickHold() {
        if (!HoldReady()) return;
        const Stage s = stage_.load();
        const bool show = s == Stage::Running || s == Stage::Result || s == Stage::Installing;
        StartInstall(show);
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

        PollJobs();
        Stage stage = stage_.load();
        PublishSnap(stage);
        if (revealT_ > 0) revealT_ = std::min(1.0f, revealT_ + dt * 0.8f);
        if (installT_ > 0) installT_ = std::min(1.0f, installT_ + dt * 1.2f);
        introT_ = std::min(1.0f, introT_ + dt * 1.4f);
        spinner_ += dt * 4.0f;

        buttons_.clear();
        hoverButton_ = false;
        tipTitle_ = L"Home";
        tipBody_ = stage == Stage::Home
                       ? L"Move the pointer over a tool or a tweak. This panel describes what it does."
                       : L"Move the pointer over a tool, a tweak, or the house. This panel describes what it does.";

        const D2D1_SIZE_F size = rd_.Size();
        const float w = size.width, h = size.height;
        const float pad = 16;
        const D2D1_RECT_F header = D2D1::RectF(pad, pad, w - pad, pad + 78);
        const D2D1_RECT_F bar = D2D1::RectF(pad, header.bottom + 8, w - pad, header.bottom + 14);
        const D2D1_RECT_F footer = D2D1::RectF(pad, h - pad - 28, w - pad, h - pad);
        const D2D1_RECT_F body = D2D1::RectF(pad, bar.bottom + 14, w - pad, footer.top - 10);
        if (stage == Stage::Fleet) {
            PumpFleet(FleetCapacity(W(body), H(body)));
            stage = stage_.load();
            PublishSnap(stage);
        }

        if (stage != Stage::Welcome && snap_.lat.size() == static_cast<size_t>(kLat) * kLat &&
            snap_.col.size() == static_cast<size_t>(kLat)) {
            rd_.RenderLattice(time_, snap_.lat.data(), snap_.col.data());
        }

        rd_.Begin();
        DrawBackground(w, h);
        const bool wasSettings = settingsOpen_;
        const bool savedClick = clickPending_;
        const bool savedEnter = enterPending_;
        if (wasSettings) {
            clickPending_ = false;
            enterPending_ = false;
        }
        if (stage == Stage::Welcome) {
            DrawWelcome(D2D1::RectF(pad, pad, w - pad, footer.top - 10), dt);
        } else if (stage == Stage::Fleet) {
            DrawHeader(header, bar, stage, dt);
            DrawFleet(body, dt);
        } else if (stage == Stage::Home || stage == Stage::Monitor) {
            DrawHeader(header, bar, stage, dt);
            if (stage == Stage::Home) DrawHome(body, dt);
            else DrawMonitor(body, dt);
        } else {
            DrawHeader(header, bar, stage, dt);
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
        if (settingsOpen_) {
            clickPending_ = savedClick;
            enterPending_ = savedEnter;
            DrawSettings(w, h, dt, wasSettings);
        }
        if (!rd_.End()) rd_.Release();
        if (pendingBrowse_ != 0) {
            const int which = pendingBrowse_;
            pendingBrowse_ = 0;
            clickPending_ = false;
            enterPending_ = false;
            const std::wstring picked = PickFolder(which == 1 ? L"Choose where driver packages are stored"
                                                              : L"Choose where MSI Afterburner is installed");
            if (!picked.empty()) {
                if (which == 1) prefs_.driverDir = picked;
                else prefs_.afterburnerDir = picked;
                CommitPrefs();
            }
        }

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
    std::wstring PickFolder(const wchar_t* title) {
        BROWSEINFOW bi{};
        bi.hwndOwner = hwnd_;
        bi.lpszTitle = title;
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX | BIF_USENEWUI;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (!pidl) return {};
        wchar_t path[MAX_PATH]{};
        std::wstring out;
        if (SHGetPathFromIDListW(pidl, path)) out = path;
        CoTaskMemFree(pidl);
        if (out.size() > 200 || out.find(L'"') != std::wstring::npos) return {};
        return out;
    }

    void CommitPrefs() {
        prefs_.scanHours = std::clamp(prefs_.scanHours, 1, 168);
        prefs_.maintainMinutes = std::clamp(prefs_.maintainMinutes, 1, 180);
        prefs_.updateMinutes = std::clamp(prefs_.updateMinutes, 1, 1440);
        prefs_.notifyMinutes = std::clamp(prefs_.notifyMinutes, 1, 1440);
        SaveMonitorPrefs(prefs_);
        SetDriverStoreDir(prefs_.driverDir);
        SetAfterburnerRoot(prefs_.afterburnerDir);
    }

    void SaveCurrentChoices() {
        StepChoices choices;
        choices.prepare = Want(kWantPrepare);
        choices.measure = Want(kWantMeasure);
        choices.hold = Want(kWantHold);
        choices.cpu = Want(kWantCpu);
        choices.gpuClean = Want(kWantGpuClean);
        choices.gpu = Want(kWantGpu);
        choices.afterburner = Want(kWantAfterburner);
        choices.memory = Want(kWantMemory);
        choices.storage = Want(kWantStorage);
        SaveStepChoices(choices);
    }

    bool DrawSettingsButton(const D2D1_RECT_F& r, float dt) {
        const bool clicked = HitTarget(251, r, true, dt);
        const float hv = hover_.size() > 251 ? hover_[251] : 0.0f;
        NoteTip(r, L"Settings",
                L"Driver folder, Afterburner folder, startup, notifications, and the timers Synchronus uses.");
        if (hv > 0.02f) rd_.FillRound(r, 6, Alpha(col::cyan, 0.12f * hv));
        const D2D1_COLOR_F c = MixC(col::cyan, col::white, 0.30f + 0.55f * hv);
        const float left = r.left + 3.5f;
        const float right = r.right - 3.5f;
        const float knobs[3] = {0.76f, 0.30f, 0.54f};
        for (int i = 0; i < 3; ++i) {
            const float y = r.top + 5.5f + static_cast<float>(i) * ((H(r) - 8.0f) / 3.0f);
            rd_.Line(D2D1::Point2F(left, y), D2D1::Point2F(right, y), Alpha(c, 0.9f), 1.35f, rd_.Round());
            const float kx = left + (right - left) * knobs[i];
            rd_.Dot(D2D1::Point2F(kx, y), 2.35f, c);
        }
        return clicked;
    }

    void DrawSettings(float w, float h, float dt, bool interactive) {
        if (!interactive) {
            clickPending_ = false;
            enterPending_ = false;
        }
        rd_.Fill(D2D1::RectF(0, 0, w, h), Alpha(col::bg, 0.62f));
        const float pw = std::min(720.0f, w - 40.0f);
        const float ph = std::min(640.0f, h - 36.0f);
        const float left = (w - pw) * 0.5f;
        const float top = (h - ph) * 0.5f;
        const D2D1_RECT_F win = D2D1::RectF(left, top, left + pw, top + ph);
        if (interactive && clickPending_ && !PtIn(clickPos_, win)) {
            settingsOpen_ = false;
            return;
        }
        rd_.FillRound(win, 14, col::panel);
        rd_.StrokeRound(win, 14, Alpha(col::cyan, 0.55f), 1.4f);
        rd_.Text(L"Settings", D2D1::RectF(left + 18, top + 8, left + pw - 54, top + 36), rd_.fPhase.Get(), col::text,
                 DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        const D2D1_RECT_F closeR = D2D1::RectF(left + pw - 44, top + 8, left + pw - 12, top + 36);
        if (Button(400, closeR, L"X", col::muted, false, true, dt, false, false, rd_.fSmall.Get())) settingsOpen_ = false;
        const D2D1_RECT_F view = D2D1::RectF(left + 10, top + 46, left + pw - 10, top + ph - 12);
        rd_.Clip(view);
        const float x0 = view.left + 6;
        const float x1 = view.right - 6;
        float y = view.top - static_cast<float>(settingsScroll_) * 32.0f;
        const float startY = y;
        auto shown = [&](const D2D1_RECT_F& row) { return row.bottom > view.top + 1 && row.top < view.bottom - 1; };
        auto heading = [&](const wchar_t* text) {
            const D2D1_RECT_F row = D2D1::RectF(x0, y, x1, y + 22);
            if (shown(row)) rd_.Text(text, row, rd_.fSmall.Get(), col::cyan);
            y += 24;
        };
        heading(L"App");
        auto toggle = [&](int id, bool on, const wchar_t* label) {
            const D2D1_RECT_F row = D2D1::RectF(x0, y, x1, y + 30);
            y += 32;
            if (!shown(row)) return false;
            return CheckRow(id, row, on, false, label, dt);
        };
        if (toggle(401, prefs_.launchAtLogin, L"Start with Windows, minimized to the tray")) {
            prefs_.launchAtLogin = !prefs_.launchAtLogin;
            prefs_.startMinimized = prefs_.launchAtLogin;
            if (prefs_.launchAtLogin) prefs_.closeToTray = true;
            CommitPrefs();
        }
        if (toggle(402, prefs_.closeToTray, L"Close the window to the tray")) {
            prefs_.closeToTray = !prefs_.closeToTray;
            CommitPrefs();
        }
        if (toggle(403, prefs_.notify, L"Notify when a monitored setting needs attention")) {
            prefs_.notify = !prefs_.notify;
            CommitPrefs();
        }
        if (toggle(404, prefs_.paused, L"Pause health monitoring")) {
            prefs_.paused = !prefs_.paused;
            CommitPrefs();
        }
        if (toggle(405, prefs_.maintainTiming, L"Maintain timing automatically")) SetMaintain(!prefs_.maintainTiming);
        heading(L"Timers");
        auto stepper = [&](int minusId, int plusId, const wchar_t* label, int value, int lo, int hi, int& dest, int kind) {
            const D2D1_RECT_F row = D2D1::RectF(x0, y, x1, y + 30);
            y += 32;
            if (!shown(row)) return;
            rd_.Text(label, D2D1::RectF(row.left + 10, row.top, row.right - 150, row.bottom), rd_.fSmall.Get(), col::text,
                     DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const D2D1_RECT_F minus = D2D1::RectF(row.right - 138, row.top + 2, row.right - 104, row.bottom - 2);
            const D2D1_RECT_F plus = D2D1::RectF(row.right - 42, row.top + 2, row.right - 8, row.bottom - 2);
            rd_.Text(std::to_wstring(value), D2D1::RectF(minus.right, row.top, plus.left, row.bottom), rd_.fSmall.Get(), col::cyan,
                     DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            if (Button(minusId, minus, L"-", col::cyan, false, value > lo, dt, false, false, rd_.fSmall.Get()) && value > lo) {
                dest = value - 1;
                CommitPrefs();
                if (kind == 1)
                    nextMaintainAt_ = GetTickCount64() + static_cast<unsigned long long>(dest) * 60ull * 1000ull;
                if (kind == 2)
                    nextUpdateAt_ = GetTickCount64() + static_cast<unsigned long long>(dest) * 60ull * 1000ull;
                if (kind == 3)
                    nextScanAt_ = GetTickCount64() + static_cast<unsigned long long>(dest) * 3600ull * 1000ull;
            }
            if (Button(plusId, plus, L"+", col::cyan, false, value < hi, dt, false, false, rd_.fSmall.Get()) && value < hi) {
                dest = value + 1;
                CommitPrefs();
                if (kind == 1)
                    nextMaintainAt_ = GetTickCount64() + static_cast<unsigned long long>(dest) * 60ull * 1000ull;
                if (kind == 2)
                    nextUpdateAt_ = GetTickCount64() + static_cast<unsigned long long>(dest) * 60ull * 1000ull;
                if (kind == 3)
                    nextScanAt_ = GetTickCount64() + static_cast<unsigned long long>(dest) * 3600ull * 1000ull;
            }
        };
        stepper(410, 411, L"Health scan, hours", prefs_.scanHours, 1, 168, prefs_.scanHours, 3);
        stepper(412, 413, L"Maintain check, minutes", prefs_.maintainMinutes, 1, 180, prefs_.maintainMinutes, 1);
        stepper(414, 415, L"Update check, minutes", prefs_.updateMinutes, 1, 1440, prefs_.updateMinutes, 2);
        stepper(416, 417, L"Notification gap, minutes", prefs_.notifyMinutes, 1, 1440, prefs_.notifyMinutes, 0);
        heading(L"Folders");
        auto pathRow = [&](int browseId, int resetId, const wchar_t* label, const std::wstring& shownPath, bool driver) {
            const D2D1_RECT_F row = D2D1::RectF(x0, y, x1, y + 30);
            y += 32;
            if (!shown(row)) return;
            rd_.Text(label, D2D1::RectF(row.left + 10, row.top, row.left + 132, row.bottom), rd_.fSmall.Get(), col::text,
                     DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            rd_.Text(shownPath, D2D1::RectF(row.left + 136, row.top, row.right - 168, row.bottom), rd_.fMonoSmall.Get(), col::muted,
                     DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const D2D1_RECT_F browse = D2D1::RectF(row.right - 160, row.top + 2, row.right - 84, row.bottom - 2);
            const D2D1_RECT_F reset = D2D1::RectF(row.right - 78, row.top + 2, row.right - 4, row.bottom - 2);
            if (Button(browseId, browse, L"Browse", col::cyan, false, true, dt, false, false, rd_.fSmall.Get()))
                pendingBrowse_ = driver ? 1 : 2;
            if (Button(resetId, reset, L"Default", col::muted, false, true, dt, false, false, rd_.fSmall.Get())) {
                if (driver) prefs_.driverDir.clear();
                else prefs_.afterburnerDir.clear();
                CommitPrefs();
            }
        };
        pathRow(430, 431, L"Drivers", prefs_.driverDir.empty() ? DefaultDriverStoreDir() : prefs_.driverDir, true);
        pathRow(432, 433, L"Afterburner", prefs_.afterburnerDir.empty() ? DefaultAfterburnerRoot() : prefs_.afterburnerDir, false);
        const D2D1_RECT_F note = D2D1::RectF(x0, y, x1, y + 32);
        if (shown(note)) {
            rd_.Text(L"Empty folders stay on the usual locations. RivaTuner stays in its own folder.", note, rd_.fMonoSmall.Get(),
                     col::dim, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        y += 34;
        heading(L"Full tune steps");
        static const wchar_t* kLabels[kWantCount] = {
            L"Restore point", L"Timer measure", L"Boot service", L"CPU tune", L"GPU driver",
            L"GPU settings",  L"Afterburner",   L"Memory",       L"Storage",
        };
        const float colW = (x1 - x0 - 10) * 0.5f;
        for (int i = 0; i < kWantCount; ++i) {
            const float colN = (i < 5) ? 0.0f : 1.0f;
            const float rowN = static_cast<float>(i < 5 ? i : i - 5);
            const float yy = y + rowN * 32.0f;
            const D2D1_RECT_F cell = D2D1::RectF(x0 + colN * (colW + 10), yy, x0 + colN * (colW + 10) + colW, yy + 30);
            if (!shown(cell)) continue;
            if (CheckRow(440 + i, cell, want_[i], false, kLabels[i], dt)) {
                want_[i] = !want_[i];
                SaveCurrentChoices();
            }
        }
        y += 5 * 32.0f;
        rd_.Unclip();
        const float used = (y - startY);
        const int maxScroll = std::max(0, static_cast<int>(std::ceil((used - H(view)) / 32.0f)));
        settingsMaxScroll_ = maxScroll;
        if (settingsScroll_ > settingsMaxScroll_) settingsScroll_ = settingsMaxScroll_;
    }

    void DrawHeader(const D2D1_RECT_F& r, const D2D1_RECT_F& bar, Stage stage, float dt) {
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
        rd_.Text(L"Lucius & Wayne Systems  |  luciuswayne.com",
                 D2D1::RectF(textLeft, r.top + 32, textLeft + 560, r.top + 50), rd_.fSmall.Get(), col::cyan);
        const StepMeta meta = Meta(stage);
        rd_.Text((meta.number || stage == Stage::Home || stage == Stage::Monitor)
                     ? Fmt(L"%s - %s | v%s", meta.name, meta.title, host_.version.c_str())
                     : Fmt(L"Synchronus | v%s", host_.version.c_str()),
                 D2D1::RectF(textLeft, r.top + 50, textLeft + 560, r.top + 68), rd_.fSub.Get(), col::muted);

        // phase, right aligned
        const std::wstring phase = snap_.phase.empty() ? Upper(std::wstring(meta.name)) : snap_.phase;
        D2D1_COLOR_F phaseColor = col::cyan;
        if (stage == Stage::Result || stage == Stage::Installed || stage == Stage::PpmDone || stage == Stage::GpuDone ||
            stage == Stage::AfterburnerDone ||
            stage == Stage::PrepareDone || stage == Stage::MemoryDone || stage == Stage::StorageDone ||
            stage == Stage::Review)
            phaseColor = col::green;
        if (stage == Stage::Failed || stage == Stage::InstallFailed || stage == Stage::Cancelled ||
            stage == Stage::PpmFailed || stage == Stage::GpuFailed || stage == Stage::AfterburnerFailed ||
            stage == Stage::PrepareFailed ||
            stage == Stage::MemoryFailed || stage == Stage::StorageFailed)
            phaseColor = col::red;
        if (stage == Stage::Installing || stage == Stage::PpmApplying || stage == Stage::GpuApplying ||
            stage == Stage::AfterburnerApplying ||
            stage == Stage::PrepareApplying || stage == Stage::MemoryApplying || stage == Stage::StorageApplying)
            phaseColor = col::amber;
        if (stage == Stage::PpmPrompt || stage == Stage::GpuPrompt || stage == Stage::AfterburnerPrompt ||
            stage == Stage::Welcome ||
            stage == Stage::PreparePrompt || stage == Stage::GpuClean ||
            stage == Stage::MemoryPrompt || stage == Stage::StoragePrompt)
            phaseColor = col::violet;
        const float pulse = 0.75f + 0.25f * std::sin(time_ * 3.0f);
        const float hcx = (r.left + r.right) * 0.5f;
        const float half = std::min(270.0f, W(r) * 0.22f);
        if (stage != Stage::Home &&
            DrawEtchedHome(D2D1::RectF(hcx - half, r.top, hcx + half, r.bottom), dt))
            OpenHome();
        const float side = 26.0f;
        const D2D1_RECT_F settingsR = D2D1::RectF(r.right - side, r.top, r.right, r.top + side);
        if (DrawSettingsButton(settingsR, dt)) {
            settingsOpen_ = !settingsOpen_;
            if (settingsOpen_) settingsScroll_ = 0;
        }
        const D2D1_RECT_F pr = D2D1::RectF(r.left + 420, r.top + 2, settingsR.left - 10.0f, r.top + 26);
        rd_.Text(meta.number ? Fmt(L"STEP %d OF %d  |  %s", meta.number, kStepCount, Upper(phase).c_str())
                             : Upper(phase),
                 pr, rd_.fPhase.Get(), Alpha(phaseColor, stage == Stage::Running ? pulse : 1.0f),
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
        std::wstring detail = snap_.detail;
        const bool liveJob = JobBusy(StageJob(stage));
        if ((stage == Stage::Running || liveJob) && snap_.elapsed > 0) {
            detail += Fmt(L"   elapsed %s", Duration(snap_.elapsed).c_str());
            if (snap_.remaining > 0) detail += Fmt(L"  |  remaining ~%s", Duration(snap_.remaining).c_str());
        }
        rd_.Text(detail, D2D1::RectF(r.left + 420, r.top + 28, r.right, r.top + 48), rd_.fSmall.Get(), col::muted,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);

        // progress bar
        const float stepFrac = meta.number > 0 ? (static_cast<float>(meta.number - 1) +
                                                  (stage == Stage::Running ? static_cast<float>(snap_.progress) : 1.0f)) /
                                                     static_cast<float>(kStepCount)
                                               : 0.0f;
        const bool reportProgress = stage == Stage::Running || stage == Stage::Fleet || (liveJob && snap_.progress > 0);
        progress_ = Approach(progress_, reportProgress ? static_cast<float>(snap_.progress) : stepFrac, 0.016f, 6.0f);
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
                bool enabled, float dt, bool pulse = false, bool acceptEnter = false,
                IDWriteTextFormat* font = nullptr, bool flash = false) {
        if (hover_.size() <= static_cast<size_t>(id)) hover_.resize(static_cast<size_t>(id) + 1, 0.0f);
        const bool over = enabled && PtIn(mouse_, r);
        if (over) hoverButton_ = true;
        hover_[id] = Approach(hover_[id], over ? 1.0f : 0.0f, dt, 12.0f);
        const float hv = hover_[id];
        const bool pressed = over && mouseDown_;
        const float breathe = pulse && enabled && !flash ? 0.5f + 0.5f * std::sin(time_ * 3.1f) : 0.0f;
        const bool lit = !flash || pulseDim_;
        const D2D1_POINT_2F c = D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2);
        if (primary) {
            const float glowA = !enabled ? 0.0f : flash ? (lit ? 0.9f : 0.06f)
                                                        : (pulse ? 0.28f + 0.55f * breathe : 0.22f + 0.35f * hv);
            rd_.Glow(c, W(r) * (flash ? (lit ? 0.95f : 0.35f) : (0.55f + 0.35f * breathe)), accent, glowA);
            if (pulse && enabled && !flash) rd_.Glow(c, W(r) * 1.15f, accent, 0.10f + 0.16f * breathe);
            D2D1_COLOR_F fill = col::panel2;
            if (enabled && flash) fill = lit ? MixC(accent, col::white, 0.24f) : Alpha(accent, 0.16f);
            else if (enabled) fill = MixC(accent, col::white, 0.10f * hv + 0.08f * breathe);
            if (pressed) fill = MixC(accent, col::bg, 0.25f);
            rd_.FillRound(r, 8, fill);
            const float edge = flash ? (lit ? 0.75f : 0.12f) : ((pulse ? 0.28f : 0.18f) + 0.25f * hv + 0.25f * breathe);
            rd_.StrokeRound(r, 8, Alpha(col::white, edge), flash && lit ? 1.8f : (pulse ? 1.6f : 1.0f));
            rd_.Text(label, r, font ? font : rd_.fButton.Get(),
                     enabled && lit ? col::text : Alpha(col::text, enabled ? 0.4f : 0.35f),
                     DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        } else {
            rd_.FillRound(r, 8, Alpha(accent, enabled ? 0.08f + 0.12f * hv : 0.03f));
            rd_.StrokeRound(r, 8, Alpha(accent, enabled ? 0.45f + 0.4f * hv : 0.15f));
            rd_.Text(label, r, font ? font : rd_.fButton.Get(), enabled ? MixC(accent, col::white, 0.3f * hv) : col::dim,
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
              verify ? Fmt(L"verification  |  128\u00B3  |  n = %u  |  missed %u", snap_.histTotal,
                           snap_.histLate)
                     : pass ? Fmt(L"pass %d of %d  |  late %% \u00D7 latency \u00D7 n  |  n = %u",
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
        Panel(R, L"Candidate", req ? Fmt(L"%.4f ms  |  %s", Ms(req), who.c_str()) : L"statistics of the candidate being measured");

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
            std::wstring value = L"-";
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
        rd_.Text(c ? Fmt(L"%.3f", statV_[6]) : L"-", D2D1::RectF(G.left, gc.y - 22, G.right, gc.y + 6),
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

    int TraySlotPx() const {
        UINT dpi = 96;
        const HMODULE shcore = LoadLibraryW(L"Shcore.dll");
        if (shcore) {
            using GetDpi = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
            const auto fn = reinterpret_cast<GetDpi>(GetProcAddress(shcore, "GetDpiForMonitor"));
            if (fn) {
                UINT x = 96, y = 96;
                const HMONITOR mon = MonitorFromWindow(hwnd_ ? hwnd_ : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
                if (SUCCEEDED(fn(mon, 0, &x, &y)) && x >= 96) dpi = x;
            }
            FreeLibrary(shcore);
        }
        int px = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
        // The mark has a margin, and a 16px tray image lands smaller than the other icons.
        // A filled 32px image is what the shell scales into the slot.
        if (px < 32) px = 32;
        if (px > 64) px = 64;
        return px;
    }

    HICON MakeFilledIcon(int slot, bool dim) const {
        if (slot < 1) slot = 1;
        constexpr int kSrc = 128;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = kSrc;
        bi.bmiHeader.biHeight = -kSrc;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* srcBits = nullptr;
        HDC screen = GetDC(nullptr);
        HBITMAP srcBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &srcBits, nullptr, 0);
        HDC srcDc = CreateCompatibleDC(screen);
        SelectObject(srcDc, srcBmp);
        HICON raw = reinterpret_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, kSrc, kSrc, LR_DEFAULTCOLOR));
        if (raw) DrawIconEx(srcDc, 0, 0, raw, kSrc, kSrc, 0, nullptr, DI_NORMAL);
        const auto* px = static_cast<const BYTE*>(srcBits);
        int minX = kSrc, minY = kSrc, maxX = 0, maxY = 0;
        if (px) {
            for (int y = 0; y < kSrc; ++y) {
                for (int x = 0; x < kSrc; ++x) {
                    if (px[(y * kSrc + x) * 4 + 3] > 20) {
                        if (x < minX) minX = x;
                        if (y < minY) minY = y;
                        if (x > maxX) maxX = x;
                        if (y > maxY) maxY = y;
                    }
                }
            }
        }
        if (maxX < minX) {
            minX = 0;
            minY = 0;
            maxX = kSrc - 1;
            maxY = kSrc - 1;
        }
        const int bw = maxX - minX + 1;
        const int bh = maxY - minY + 1;
        BITMAPINFO outInfo = bi;
        outInfo.bmiHeader.biWidth = slot;
        outInfo.bmiHeader.biHeight = -slot;
        void* outBits = nullptr;
        HBITMAP color = CreateDIBSection(screen, &outInfo, DIB_RGB_COLORS, &outBits, nullptr, 0);
        auto* dst = static_cast<BYTE*>(outBits);
        if (px && dst) {
            for (int y = 0; y < slot; ++y) {
                const int sy = minY + (bh <= 1 ? 0 : y * (bh - 1) / std::max(1, slot - 1));
                for (int x = 0; x < slot; ++x) {
                    const int sx = minX + (bw <= 1 ? 0 : x * (bw - 1) / std::max(1, slot - 1));
                    const BYTE* s = px + (sy * kSrc + sx) * 4;
                    BYTE* d = dst + (y * slot + x) * 4;
                    BYTE a = s[3];
                    BYTE b = s[0], g = s[1], r = s[2];
                    if (dim) {
                        a = static_cast<BYTE>(a / 3);
                        b = static_cast<BYTE>(b / 3);
                        g = static_cast<BYTE>(g / 3);
                        r = static_cast<BYTE>(r / 3);
                    }
                    d[0] = b;
                    d[1] = g;
                    d[2] = r;
                    d[3] = a;
                }
            }
        }
        HBITMAP mask = CreateBitmap(slot, slot, 1, 1, nullptr);
        ICONINFO ii{};
        ii.fIcon = TRUE;
        ii.hbmColor = color;
        ii.hbmMask = mask;
        HICON icon = CreateIconIndirect(&ii);
        if (raw) DestroyIcon(raw);
        DeleteObject(mask);
        DeleteObject(color);
        DeleteObject(srcBmp);
        DeleteDC(srcDc);
        ReleaseDC(nullptr, screen);
        return icon;
    }

    HICON MakeTrayIcon(bool dim) { return MakeFilledIcon(TraySlotPx(), dim); }

    void SetTaskbarIcons() {
        const UINT dpi = dpi_ > 0 ? static_cast<UINT>(dpi_) : 96u;
        int bigPx = GetSystemMetricsForDpi(SM_CXICON, dpi);
        if (bigPx < 48) bigPx = 48;
        int smallPx = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
        if (smallPx < 24) smallPx = 24;
        if (taskbarBig_) DestroyIcon(taskbarBig_);
        if (taskbarSmall_) DestroyIcon(taskbarSmall_);
        taskbarBig_ = MakeFilledIcon(bigPx, false);
        taskbarSmall_ = MakeFilledIcon(smallPx, false);
        if (taskbarBig_) SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(taskbarBig_));
        if (taskbarSmall_) SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(taskbarSmall_));
    }

    void AddTray() {
        nid_.cbSize = sizeof(nid_);
        nid_.hWnd = hwnd_;
        nid_.uID = 1;
        nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid_.uCallbackMessage = WM_APP + 42;
        trayIcon_ = MakeTrayIcon(false);
        nid_.hIcon = trayIcon_ ? trayIcon_ : LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
        wcscpy_s(nid_.szTip, L"Synchronus");
        Shell_NotifyIconW(NIM_ADD, &nid_);
        nid_.uVersion = NOTIFYICON_VERSION;
        Shell_NotifyIconW(NIM_SETVERSION, &nid_);
        trayOn_ = true;
    }

    void RemoveTray() {
        if (!trayOn_) return;
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        trayOn_ = false;
        if (trayIcon_) {
            DestroyIcon(trayIcon_);
            trayIcon_ = nullptr;
        }
    }

    void UpdateTray() {
        if (!trayOn_) return;
        HealthSummary sum;
        {
            std::lock_guard<std::mutex> g(healthM_);
            sum = Summarize(checks_);
        }
        const bool bad = sum.attention > 0;
        HICON next = MakeTrayIcon(updatePulse_ && pulseDim_);
        if (next) {
            if (trayIcon_) DestroyIcon(trayIcon_);
            trayIcon_ = next;
            nid_.hIcon = trayIcon_;
        } else if (bad) {
            nid_.hIcon = LoadIconW(nullptr, IDI_WARNING);
        }
        nid_.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        std::wstring tip = L"Synchronus";
        if (updatePulse_) tip += L": update available";
        else if (bad) tip += L": " + sum.headline;
        else if (sum.pass + sum.attention + sum.info + sum.other > 0) tip += L": nothing needs attention";
        wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &nid_);
    }

    void OnTray(LPARAM lp) {
        if (lp == WM_LBUTTONDBLCLK || lp == WM_LBUTTONUP) {
            ShowMain();
            return;
        }
        if (lp == NIN_BALLOONUSERCLICK) {
            ShowWindow(hwnd_, SW_SHOW);
            SetForegroundWindow(hwnd_);
            if (updatePulse_) showUpdatePrompt_ = true;
            else OpenHome();
            return;
        }
        if (lp != WM_RBUTTONUP) return;
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 4100, L"Open Synchronus");
        AppendMenuW(menu, MF_STRING, 4101, L"Run health check");
        AppendMenuW(menu, MF_STRING, 4102, prefs_.paused ? L"Resume monitoring" : L"Pause monitoring");
        AppendMenuW(menu, MF_STRING, 4103, L"Settings");
        AppendMenuW(menu, MF_STRING, 4105,
                    prefs_.launchAtLogin ? L"Start with Windows, minimized: on" : L"Start with Windows, minimized: off");
        AppendMenuW(menu, MF_STRING, 4106, prefs_.notify ? L"Notifications: on" : L"Notifications: off");
        AppendMenuW(menu, MF_STRING, 4107, prefs_.maintainTiming ? L"Maintain timing: on" : L"Maintain timing: off");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 4104, L"Exit");
        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);
        const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
        if (cmd == 4100) {
            ShowMain();
        } else if (cmd == 4101) {
            ShowWindow(hwnd_, SW_SHOW);
            SetForegroundWindow(hwnd_);
            OpenHealth();
            StartHealthScan();
        } else if (cmd == 4102) {
            prefs_.paused = !prefs_.paused;
            SaveMonitorPrefs(prefs_);
        } else if (cmd == 4103) {
            ShowMain();
            settingsOpen_ = true;
        } else if (cmd == 4105) {
            prefs_.launchAtLogin = !prefs_.launchAtLogin;
            prefs_.startMinimized = prefs_.launchAtLogin;
            if (prefs_.launchAtLogin) prefs_.closeToTray = true;
            SaveMonitorPrefs(prefs_);
        } else if (cmd == 4106) {
            prefs_.notify = !prefs_.notify;
            SaveMonitorPrefs(prefs_);
        } else if (cmd == 4107) {
            SetMaintain(!prefs_.maintainTiming);
        } else if (cmd == 4104) {
            exitRequested_ = true;
            DestroyWindow(hwnd_);
        }
    }

    void OnScanTimer() {
        if (prefs_.paused) return;
        const unsigned long long now = GetTickCount64();
        if (prefs_.maintainTiming) {
            if (healthBusy_.load() || maintainAfterScan_) return;
            if (now < nextMaintainAt_) return;
            nextMaintainAt_ = now + static_cast<unsigned long long>(std::max(1, prefs_.maintainMinutes)) * 60ull * 1000ull;
            maintainAfterScan_ = true;
            StartHealthScan();
            return;
        }
        if (healthBusy_.load()) return;
        if (now < nextScanAt_) return;
        StartHealthScan();
    }

    void OpenHealth() {
        stage_ = Stage::Monitor;
        state_.Phase(L"HEALTH", L"read-only check of applied settings");
        if (checks_.empty() && !healthBusy_.load()) StartHealthScan();
    }

    void StartHealthScan() {
        if (healthBusy_.load()) return;
        if (healthThread_.joinable()) healthThread_.join();
        healthStop_.store(false);
        healthBusy_.store(true);
        healthThread_ = std::thread([this] {
            const auto found = RunHealth(&healthStop_);
            std::vector<std::wstring> fresh;
            {
                std::lock_guard<std::mutex> g(healthM_);
                for (const auto& c : found) {
                    if (!HealthNeedsAttention(c.status)) continue;
                    bool seen = false;
                    for (const auto& id : noted_) {
                        if (id == c.id) seen = true;
                    }
                    if (!seen) fresh.push_back(c.id + L"|" + c.name);
                }
                checks_ = found;
                noted_.clear();
                for (const auto& c : checks_) {
                    if (HealthNeedsAttention(c.status)) noted_.push_back(c.id);
                }
                lastScanAt_ = GetTickCount64();
            }
            nextScanAt_ = GetTickCount64() + static_cast<unsigned long long>(prefs_.scanHours) * 3600ull * 1000ull;
            healthBusy_.store(false);
            if (prefs_.notify && !fresh.empty()) PostMessageW(hwnd_, WM_APP + 44, 0, 0);
            PostMessageW(hwnd_, WM_APP + 43, 0, 0);
        });
    }

    void OnHealthReady() {
        UpdateTray();
        if (!maintainAfterScan_) return;
        maintainAfterScan_ = false;
        if (prefs_.maintainTiming && !prefs_.paused) AutoRepairDrift();
    }

    void AskStartupChoice() {
        const INT_PTR keep = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(100), hwnd_, StartupDlgProc, 0);
        const bool on = keep != 0;
        prefs_.launchAtLogin = on;
        prefs_.startMinimized = on;
        prefs_.closeToTray = on;
        prefs_.startupAsked = true;
        prefs_.trayPrompted = true;
        SaveMonitorPrefs(prefs_);
    }

    void AskMaintain() {
        const int def = (prefs_.maintainTiming || !prefs_.maintainAsked) ? MB_DEFBUTTON1 : MB_DEFBUTTON2;
        const int answer = MessageBoxW(
            hwnd_,
            L"Automatically maintain the timing structure?\n\n"
            L"On the interval in Settings, Synchronus reads the monitored settings and puts back any that have drifted. "
            L"The usual interval is one minute. You can change this later from Settings, the tray menu, or the home screen.",
            L"Synchronus", MB_YESNO | MB_ICONQUESTION | def);
        SetMaintain(answer == IDYES);
    }

    void SetMaintain(bool on) {
        prefs_.maintainTiming = on;
        prefs_.maintainAsked = true;
        SaveMonitorPrefs(prefs_);
        if (!on) return;
        nextMaintainAt_ = GetTickCount64() + static_cast<unsigned long long>(std::max(1, prefs_.maintainMinutes)) * 60ull * 1000ull;
        if (!healthBusy_.load() && !maintainAfterScan_) {
            maintainAfterScan_ = true;
            StartHealthScan();
        }
    }

    void AutoRepairDrift() {
        std::vector<std::wstring> kinds;
        std::vector<std::wstring> harden;
        {
            std::lock_guard<std::mutex> g(healthM_);
            for (const auto& c : checks_) {
                if (c.status == HealthStatus::Pass) {
                    SetDriftCount(c.id, 0);
                    continue;
                }
                if (!HealthNeedsAttention(c.status) || c.repair.empty() || c.repair == L"afterburner") continue;
                const int times = DriftCount(c.id) + 1;
                SetDriftCount(c.id, times);
                bool seen = false;
                for (const auto& k : kinds) {
                    if (k == c.repair) seen = true;
                }
                if (!seen) kinds.push_back(c.repair);
                if (times >= 2) {
                    bool already = false;
                    for (const auto& k : harden) {
                        if (k == c.repair) already = true;
                    }
                    if (!already) harden.push_back(c.repair);
                }
            }
        }
        for (const auto& kind : harden) {
            if (host_.harden) host_.harden(kind);
        }
        for (const auto& kind : kinds) LaunchRepair(kind, false);
    }

    void LaunchRepair(const std::wstring& kind, bool ask) {
        if (kind.empty()) return;
        if (ask) {
            const int answer = MessageBoxW(hwnd_,
                                            L"This changes the selected Windows setting using the existing Synchronus step. "
                                            L"It does not install a driver unless you chose the graphics or Afterburner repair.",
                                            L"Synchronus", MB_ICONWARNING | MB_OKCANCEL);
            if (answer != IDOK) return;
        }
        JobId id = JobId::Count;
        unsigned timerUnits = 0;
        if (kind == L"ppm") id = JobId::Cpu;
        else if (kind == L"gpu") id = JobId::Gpu;
        else if (kind == L"memory") id = JobId::Memory;
        else if (kind == L"storage") id = JobId::Storage;
        else if (kind == L"afterburner") id = JobId::Afterburner;
        else if (kind == L"timer") {
            id = JobId::Hold;
            if (host_.system) timerUnits = host_.system().serviceResolution;
            if (!timerUnits || JobBusy(JobId::Measure)) {
                if (ask && !timerUnits)
                    MessageBoxW(hwnd_, L"Measure a timer resolution before repairing the timer service.", L"Synchronus", MB_ICONINFORMATION | MB_OK);
                return;
            }
        } else {
            return;
        }
        if (JobBusy(id)) {
            if (ask) MessageBoxW(hwnd_, L"That step is already running.", L"Synchronus", MB_ICONINFORMATION | MB_OK);
            return;
        }
        if (ask) repairJob_ = static_cast<int>(id);
        else {
            maintainWatch_[static_cast<int>(id)] = true;
            AppendHealthLog(L"automatic repair " + kind);
        }
        if (id == JobId::Cpu) StartPpm(false);
        else if (id == JobId::Gpu) StartGpu(false);
        else if (id == JobId::Memory) StartMemory(false);
        else if (id == JobId::Storage) StartStorage(false);
        else if (id == JobId::Afterburner) StartAfterburner(false);
        else if (id == JobId::Hold) StartInstall(false, timerUnits);
    }

    void MaybeNotify() {
        if (!prefs_.notify || !trayOn_) return;
        std::wstring text;
        {
            std::lock_guard<std::mutex> g(healthM_);
            for (const auto& c : checks_) {
                if (!HealthNeedsAttention(c.status)) continue;
                text = c.name + L": " + c.observed;
                break;
            }
        }
        if (text.empty()) return;
        const unsigned long long now = GetTickCount64();
        if (now - lastNotifyAt_ < static_cast<unsigned long long>(std::max(1, prefs_.notifyMinutes)) * 60ull * 1000ull) return;
        lastNotifyAt_ = now;
        nid_.uFlags = NIF_INFO;
        wcsncpy_s(nid_.szInfoTitle, L"Synchronus", _TRUNCATE);
        wcsncpy_s(nid_.szInfo, text.c_str(), _TRUNCATE);
        nid_.dwInfoFlags = NIIF_WARNING;
        Shell_NotifyIconW(NIM_MODIFY, &nid_);
        nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    }

    void StartRepair(const std::wstring& kind) { LaunchRepair(kind, true); }

    void DrawMonitor(const D2D1_RECT_F& B, float dt) {
        std::vector<HealthCheck> rows;
        HealthSummary sum;
        bool busy = healthBusy_.load();
        {
            std::lock_guard<std::mutex> g(healthM_);
            sum = Summarize(checks_);
            for (const auto& c : checks_) {
                if (healthFilter_ == 1 && !HealthNeedsAttention(c.status)) continue;
                if (healthFilter_ == 2 && c.status != HealthStatus::Pass) continue;
                rows.push_back(c);
            }
        }
        rd_.Text(L"Health", D2D1::RectF(B.left, B.top + 6, B.right, B.top + 28), rd_.fPhase.Get(), col::cyan,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        const std::wstring head = busy ? L"Checking the live settings. Nothing is being changed." : sum.headline;
        rd_.Text(head, D2D1::RectF(B.left + 8, B.top + 28, B.right - 8, B.top + 48), rd_.fSmall.Get(), col::muted,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        rd_.Text(Fmt(L"%d passing   %d attention   %d other", sum.pass, sum.attention, sum.info + sum.other),
                 D2D1::RectF(B.left + 8, B.top + 46, B.right - 8, B.top + 64), rd_.fMonoSmall.Get(), col::dim,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        const float gap = 6;
        const float bw = (W(B) - 24 - gap * 2) / 3;
        auto chip = [&](int id, float x, const wchar_t* label, bool on) {
            return Button(id, D2D1::RectF(x, B.top + 66, x + bw, B.top + 88), label, col::cyan, on, !busy, dt);
        };
        if (chip(60, B.left + 8, L"All", healthFilter_ == 0)) healthFilter_ = 0;
        if (chip(61, B.left + 8 + bw + gap, L"Attention", healthFilter_ == 1)) healthFilter_ = 1;
        if (chip(62, B.left + 8 + (bw + gap) * 2, L"Passing", healthFilter_ == 2)) healthFilter_ = 2;

        const float listTop = B.top + 96;
        const float listBottom = B.bottom - 78;
        const float rowH = 36;
        const int visible = std::max(1, static_cast<int>((listBottom - listTop) / rowH));
        if (healthScroll_ > std::max(0, static_cast<int>(rows.size()) - visible)) {
            healthScroll_ = std::max(0, static_cast<int>(rows.size()) - visible);
        }
        for (int i = 0; i < visible; ++i) {
            const int idx = healthScroll_ + i;
            if (idx < 0 || idx >= static_cast<int>(rows.size())) break;
            const auto& c = rows[static_cast<size_t>(idx)];
            const float y = listTop + i * rowH;
            const bool on = idx == healthSelected_;
            const D2D1_RECT_F row = D2D1::RectF(B.left + 8, y, B.right - 8, y + rowH - 4);
            if (Button(80 + i, row, L"", on ? col::cyan : col::muted, on, true, dt)) healthSelected_ = idx;
            D2D1_COLOR_F mark = col::dim;
            if (c.status == HealthStatus::Pass) mark = col::green;
            else if (HealthNeedsAttention(c.status)) mark = col::amber;
            else if (c.status == HealthStatus::Error || c.status == HealthStatus::Inaccessible) mark = col::red;
            rd_.FillRound(D2D1::RectF(row.left + 6, y + 12, row.left + 16, y + 22), 5, mark);
            rd_.Text(c.name, D2D1::RectF(row.left + 22, y + 2, row.right - 8, y + 18), rd_.fSmall.Get(), col::text);
            rd_.Text(std::wstring(HealthStatusText(c.status)) + L" | " + c.observed,
                     D2D1::RectF(row.left + 22, y + 16, row.right - 8, y + 32), rd_.fMonoSmall.Get(), col::muted);
        }
        if (healthSelected_ >= 0 && healthSelected_ < static_cast<int>(rows.size())) {
            const auto& c = rows[static_cast<size_t>(healthSelected_)];
            rd_.Text(c.explanation, D2D1::RectF(B.left + 8, B.bottom - 76, B.right - 8, B.bottom - 44), rd_.fSmall.Get(),
                     col::text);
            const float cx = (B.left + B.right) / 2;
            if (Button(70, D2D1::RectF(B.left + 8, B.bottom - 38, cx - 4, B.bottom - 6), L"Check again", col::cyan, false,
                       !busy, dt))
                StartHealthScan();
            const bool canFix = !c.repair.empty() && HealthNeedsAttention(c.status);
            if (Button(71, D2D1::RectF(cx + 4, B.bottom - 38, B.right - 8, B.bottom - 6),
                       canFix ? L"Repair this" : L"No repair", canFix ? col::green : col::muted, false, canFix && !busy, dt) &&
                canFix)
                StartRepair(c.repair);
        } else if (Button(70, D2D1::RectF(B.left + 8, B.bottom - 38, B.right - 8, B.bottom - 6), L"Run health check", col::cyan,
                          true, !busy, dt)) {
            StartHealthScan();
        }
        (void)dt;
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
            const_cast<LPWSTR>(L"Synchronus: restarting. In BIOS, lock the CPU multiplier, disable deep C-states, enable XMP or DOCP, and turn on Above 4G if you want Resizable BAR."),
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
            rd_.Text(L"Windows will restart immediately. Set the BIOS items above on the next boot if they are not set yet.",
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

    bool HitTarget(int id, const D2D1_RECT_F& r, bool enabled, float dt) {
        if (hover_.size() <= static_cast<size_t>(id)) hover_.resize(static_cast<size_t>(id) + 1, 0.0f);
        const bool over = enabled && PtIn(mouse_, r);
        if (over) hoverButton_ = true;
        hover_[id] = Approach(hover_[id], over ? 1.0f : 0.0f, dt, 10.0f);
        buttons_.push_back({id, r});
        return enabled && clickPending_ && PtIn(clickPos_, r);
    }

    void NoteTip(const D2D1_RECT_F& r, const std::wstring& title, const std::wstring& body) {
        if (PtIn(mouse_, r)) {
            tipTitle_ = title;
            tipBody_ = body;
        }
    }

    void DrawHouse(const D2D1_RECT_F& r, D2D1_COLOR_F c, float glow) {
        const float left = r.left + 1;
        const float right = r.right - 1;
        const float top = r.top + 1;
        const float bottom = r.bottom - 1;
        const float cx = (left + right) * 0.5f;
        const float eave = top + (bottom - top) * 0.40f;
        rd_.GlowLine({D2D1::Point2F(left, eave), D2D1::Point2F(cx, top), D2D1::Point2F(right, eave)}, c, 1.5f, glow);
        rd_.GlowLine({D2D1::Point2F(left + 2.5f, eave), D2D1::Point2F(left + 2.5f, bottom),
                      D2D1::Point2F(right - 2.5f, bottom), D2D1::Point2F(right - 2.5f, eave)},
                     c, 1.5f, glow);
        const float doorW = (right - left) * 0.16f;
        const float doorTop = bottom - (bottom - eave) * 0.48f;
        rd_.GlowLine({D2D1::Point2F(cx - doorW, bottom), D2D1::Point2F(cx - doorW, doorTop),
                      D2D1::Point2F(cx + doorW, doorTop), D2D1::Point2F(cx + doorW, bottom)},
                     c, 1.25f, glow);
    }

    bool DrawEtchedHome(const D2D1_RECT_F& band, float dt) {
        const float hcx = (band.left + band.right) * 0.5f;
        const float midY = (band.top + band.bottom) * 0.5f;
        const float halfW = W(band) * 0.5f;
        const float halfH = H(band) * 0.5f;
        const D2D1_RECT_F hit = D2D1::RectF(hcx - halfW * 0.58f, band.top, hcx + halfW * 0.58f, band.bottom);
        const bool clicked = HitTarget(250, hit, true, dt);
        const float hv = hover_.size() > 250 ? hover_[250] : 0.0f;
        NoteTip(hit, L"Home",
                L"Returns to this screen. The house is cut into the window rather than drawn as a button. "
                L"A tune you already started keeps its place until that step finishes.");
        const D2D1_POINT_2F c = D2D1::Point2F(hcx, midY);
        auto wash = [&](ID2D1RadialGradientBrush* br, float rx, float ry, float opacity) {
            if (!br) return;
            br->SetCenter(c);
            br->SetGradientOriginOffset(D2D1::Point2F(0, 0));
            br->SetRadiusX(rx);
            br->SetRadiusY(ry);
            br->SetOpacity(Clamp01(opacity));
            rd_.Dc()->FillEllipse(D2D1::Ellipse(c, rx, ry), br);
        };
        wash(rd_.GlowBrush(col::violet), halfW * 0.92f, halfH * 1.15f, 0.10f + 0.08f * hv);
        wash(rd_.GlowBrush(col::cyan), halfW, halfH * 0.95f, 0.20f + 0.28f * hv);
        const int slices = 32;
        for (int i = 0; i < slices; ++i) {
            const float t0 = -1.0f + 2.0f * static_cast<float>(i) / slices;
            const float t1 = -1.0f + 2.0f * static_cast<float>(i + 1) / slices;
            const float tm = (t0 + t1) * 0.5f;
            const float fade = std::exp(-tm * tm * 3.1f);
            const float y = band.bottom - 7.0f;
            rd_.Line(D2D1::Point2F(hcx + t0 * halfW, y), D2D1::Point2F(hcx + t1 * halfW, y),
                     Alpha(col::cyan, (0.10f + 0.16f * hv) * fade), 4.0f);
            rd_.Line(D2D1::Point2F(hcx + t0 * halfW, y), D2D1::Point2F(hcx + t1 * halfW, y),
                     Alpha(col::white, (0.28f + 0.45f * hv) * fade), 1.15f);
        }
        const float houseH = std::min(34.0f, H(band) - 16.0f);
        const float houseW = houseH * 1.15f;
        DrawHouse(D2D1::RectF(hcx - houseW * 0.5f, midY - houseH * 0.58f, hcx + houseW * 0.5f, midY - houseH * 0.58f + houseH),
                  MixC(col::cyan, col::white, 0.25f + 0.55f * hv), 0.65f + 0.7f * hv);
        return clicked;
    }

    void LoadHomeLattice() {
        homeLoaded_ = true;
        homeHas_ = false;
        homeLat_.assign(static_cast<size_t>(kLat) * kLat, {});
        homeCol_.assign(kLat, {});
        WIN32_FIND_DATAW fd{};
        const HANDLE find = FindFirstFileW((host_.dataDir + L"\\benchmark-*.csv").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) return;
        std::wstring best;
        unsigned long long bestTime = 0;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const unsigned long long stamp = (static_cast<unsigned long long>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                                             fd.ftLastWriteTime.dwLowDateTime;
            if (stamp >= bestTime) {
                bestTime = stamp;
                best = fd.cFileName;
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
        if (best.empty()) return;
        const std::wstring path = host_.dataDir + L"\\" + best;
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        const DWORD size = GetFileSize(file, nullptr);
        std::string bytes(size ? size : 1, '\0');
        DWORD got = 0;
        ReadFile(file, bytes.data(), size, &got, nullptr);
        CloseHandle(file);
        bytes.resize(got);
        auto deposit = [&](float latePct, float mean, float stdev, int n, bool idle) {
            if (n <= 0) return;
            const int blobs = std::min(n, 40);
            const float sigma = std::max(0.02f, stdev);
            for (int i = 0; i < blobs; ++i) {
                const float z = ((i + 0.5f) / blobs - 0.5f) * 2.4f;
                const int ix = MapLateX(latePct);
                const int iy = MapLatY(mean + z * sigma);
                LatCell& cell = homeLat_[static_cast<size_t>(iy) * kLat + ix];
                const int add = std::max(1, n / blobs);
                const int room = 65535 - cell.n;
                const int put = std::min(add, room);
                cell.n = static_cast<uint16_t>(cell.n + put);
                if (idle) cell.idle = static_cast<uint16_t>(std::min(65535, cell.idle + put));
                else cell.loop = static_cast<uint16_t>(std::min(65535, cell.loop + put));
                if (latePct > 0.5f) cell.late = static_cast<uint16_t>(std::min(65535, cell.late + put / 2));
            }
            const int ix = MapLateX(latePct);
            ColStat& col = homeCol_[static_cast<size_t>(ix)];
            col.loopU = LatYU(mean);
            col.idleU = LatYU(mean);
            col.p99U = LatYU(mean + stdev);
            col.stdevU = stdev / (2.0f - kHistMin) * 0.80f;
            col.late01 = latePct / 100.0f;
            col.present = 1.0f;
        };
        size_t pos = 0;
        bool header = true;
        bool any = false;
        while (pos < bytes.size()) {
            size_t end = bytes.find('\n', pos);
            if (end == std::string::npos) end = bytes.size();
            std::string line = bytes.substr(pos, end - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pos = end + 1;
            if (line.empty()) continue;
            if (header) {
                header = false;
                continue;
            }
            std::vector<std::string> fields;
            size_t a = 0;
            while (a <= line.size()) {
                const size_t b = line.find(',', a);
                fields.push_back(line.substr(a, (b == std::string::npos ? line.size() : b) - a));
                if (b == std::string::npos) break;
                a = b + 1;
            }
            if (fields.size() < 24 || fields[0] != "sweep") continue;
            const int loopN = std::atoi(fields[4].c_str());
            const float loopMean = static_cast<float>(std::atof(fields[6].c_str()));
            const float loopStd = static_cast<float>(std::atof(fields[7].c_str()));
            const float loopLate = static_cast<float>(std::atof(fields[12].c_str()));
            const int idleN = std::atoi(fields[15].c_str());
            const float idleMean = static_cast<float>(std::atof(fields[17].c_str()));
            const float idleStd = static_cast<float>(std::atof(fields[18].c_str()));
            const float idleLate = static_cast<float>(std::atof(fields[23].c_str()));
            deposit(loopLate, loopMean, loopStd, loopN, false);
            deposit(idleLate, idleMean, idleStd, idleN, true);
            any = true;
        }
        homeHas_ = any;
        homeScanLabel_ = best;
    }

    void DrawGuide(const D2D1_RECT_F& r) {
        rd_.FillRound(r, 18, Alpha(col::panel, 0.78f));
        rd_.StrokeRound(r, 18, Alpha(col::cyan, 0.20f), 1.2f);
        rd_.Glow(D2D1::Point2F((r.left + r.right) * 0.5f, r.top + 8), W(r) * 0.45f, col::cyan, 0.10f);
        rd_.Text(tipTitle_, D2D1::RectF(r.left + 22, r.top + 20, r.right - 22, r.top + 92), rd_.fGuideTitle.Get(), col::cyan);
        rd_.Text(tipBody_, D2D1::RectF(r.left + 22, r.top + 100, r.right - 22, r.bottom - 22), rd_.fGuide.Get(), col::text);
    }

    void DrawHome(const D2D1_RECT_F& B, float dt) {
        const float gap = 14.0f;
        float leftW = std::floor(W(B) * 0.30f);
        float rightW = std::floor(W(B) * 0.42f);
        if (W(B) - leftW - rightW - gap * 2 < 240.0f) {
            const float spare = std::max(0.0f, W(B) - 240.0f - gap * 2);
            leftW = std::floor(spare * 0.42f);
            rightW = std::floor(spare * 0.58f);
        }
        const float centerL = B.left + leftW + gap;
        const float centerR = B.right - rightW - gap;
        const wchar_t* labels[] = {L"Full tune", L"Health", L"Measure timing", L"CPU power", L"GPU driver",
                                   L"GPU settings", L"Afterburner", L"Memory", L"Storage", L"Restart notes"};
        const wchar_t* blurbs[] = {
            L"Makes a restore point first, then measures timing before changing anything else. CPU, memory, and the driver lookup then run together. Storage follows the CPU step. GPU settings and Afterburner wait until the driver choice is finished.",
            L"Opens the health screen. It reads the live settings and lists anything that has drifted. Nothing changes until you choose a repair.",
            L"Times Sleep(1) across timer resolutions and saves a scan. The heatmap on this screen uses the newest of those scans.",
            L"Writes the processor power plan. Core parking and throttling are turned off, and the plan asks the CPU to stay at a high, steady state.",
            L"Looks up the current graphics driver and can install a clean copy. This step does not install Afterburner.",
            L"Turns on hardware-accelerated GPU scheduling, turns off Game DVR and Game Bar capture, and writes the NVIDIA or AMD low-latency profile when that vendor is present.",
            L"Installs MSI Afterburner and RivaTuner, runs the overclock scanner on the PCI-E graphics card, and saves that curve so it applies at sign-in. The download and scan take about 30 minutes.",
            L"Sets the page file and turns memory compression off.",
            L"Turns SysMain off and checks that TRIM is enabled for the system drive.",
            L"Shows the BIOS settings to change by hand, then asks you to restart. Windows cannot write those settings for you.",
        };
        rd_.Text(L"Tools", D2D1::RectF(B.left, B.top, B.left + leftW, B.top + 28), rd_.fPhase.Get(), col::cyan);
        const std::wstring banner = JobBanner();
        if (!banner.empty()) {
            rd_.Text(banner, D2D1::RectF(B.left, B.top + 28, B.left + leftW, B.top + 48), rd_.fSmall.Get(), col::muted);
        }
        const bool showUpdate = updatePulse_;
        const float heatH = std::max(180.0f, H(B) * 0.36f);
        const float btnTop = B.top + (banner.empty() ? 40.0f : 52.0f);
        const float btnBottom = B.bottom - heatH - 46.0f - (showUpdate ? 52.0f : 12.0f);
        const float btnGap = 8.0f;
        const float btnH = std::max(40.0f, (btnBottom - btnTop - 4.0f * btnGap) / 5.0f);
        const float colW = (leftW - 10.0f) / 2.0f;
        for (int i = 0; i < 10; ++i) {
            const float x = B.left + (i % 2) * (colW + 10);
            const float y = btnTop + (i / 2) * (btnH + btnGap);
            const D2D1_RECT_F cell = D2D1::RectF(x, y, x + colW, y + btnH);
            NoteTip(cell, labels[i], blurbs[i]);
            if (!Button(200 + i, cell, labels[i], col::cyan, i == 0, true, dt, false, false, rd_.fHomeBtn.Get())) continue;
            if (i == 0) {
                stage_ = fleet_ ? Stage::Fleet : Stage::Welcome;
            } else if (i == 1) {
                OpenHealth();
            } else if (i == 2) {
                if (JobBusy(JobId::Measure)) stage_ = Stage::Running;
                else StartBenchmark(true);
            } else if (i == 3) {
                OpenTool(JobId::Cpu, Stage::PpmPrompt, Stage::PpmApplying);
            } else if (i == 4) {
                hubReturn_ = true;
                stage_ = Stage::GpuClean;
                if (!JobBusy(JobId::GpuClean)) {
                    GpuDl phase;
                    {
                        std::lock_guard<std::mutex> g(gpuM_);
                        phase = gpuPhase_;
                    }
                    if (phase == GpuDl::Idle) BeginGpuLookup();
                }
            } else if (i == 5) {
                OpenTool(JobId::Gpu, Stage::GpuPrompt, Stage::GpuApplying);
            } else if (i == 6) {
                OpenTool(JobId::Afterburner, Stage::AfterburnerPrompt, Stage::AfterburnerApplying);
            } else if (i == 7) {
                OpenTool(JobId::Memory, Stage::MemoryPrompt, Stage::MemoryApplying);
            } else if (i == 8) {
                OpenTool(JobId::Storage, Stage::StoragePrompt, Stage::StorageApplying);
            } else {
                hubReturn_ = true;
                stage_ = Stage::Review;
                state_.Phase(L"REVIEW", L"set BIOS, then restart");
            }
        }
        const float buttonsBottom = btnTop + 5 * btnH + 4 * btnGap;
        const D2D1_RECT_F updateR = D2D1::RectF(B.left, buttonsBottom + 8, B.left + leftW, buttonsBottom + 44);
        if (showUpdate) {
            NoteTip(updateR, L"Install update",
                    L"A newer Synchronus build is ready. This downloads it and starts the installer.");
            if (Button(220, updateR, L"Install update", col::green, true, true, dt, false, false, rd_.fHomeBtn.Get(), true))
                showUpdatePrompt_ = true;
        }
        if (!homeLoaded_ && !JobBusy(JobId::Measure)) LoadHomeLattice();
        if (JobBusy(JobId::Measure)) homeLoaded_ = false;
        const float maintainTop = showUpdate ? updateR.bottom + 8.0f : buttonsBottom + 10.0f;
        const D2D1_RECT_F maintainR = D2D1::RectF(B.left, maintainTop, B.left + leftW, maintainTop + 34.0f);
        NoteTip(maintainR, prefs_.maintainTiming ? L"Maintain timing: on" : L"Maintain timing: off",
                L"When this is on, Synchronus reads the monitored settings once a minute and puts back any that have drifted. If the same one keeps resetting, it applies a stronger fix where that is possible.");
        if (Button(230, maintainR, prefs_.maintainTiming ? L"Maintain timing: on" : L"Maintain timing: off",
                   col::cyan, prefs_.maintainTiming, true, dt, false, false, rd_.fHomeBtn.Get()))
            SetMaintain(!prefs_.maintainTiming);
        const float heatTop = maintainR.bottom + 10.0f;
        const D2D1_RECT_F heat = D2D1::RectF(B.left, heatTop, B.left + leftW, B.bottom);
        const bool scanning = JobBusy(JobId::Measure);
        NoteTip(heat, scanning ? L"Live timer scan" : L"Last timer scan",
                scanning ? L"This 3D chart is the measurement that is running now. It updates as each sample lands."
                         : (homeHas_ ? L"This 3D chart is the newest finished timer-resolution scan. Brighter cells are where Sleep(1) landed."
                                     : L"No timer scan has been saved yet. Run Measure timing and this chart fills in from that result."));
        rd_.FillRound(heat, 16, Alpha(col::panel, 0.88f));
        rd_.StrokeRound(heat, 16, Alpha(col::cyan, scanning ? 0.7f : 0.28f), 1.2f);
        const std::wstring heatTitle = scanning ? L"Live timer scan" : (homeHas_ ? (L"Last timer scan  " + homeScanLabel_) : L"No timer scan saved yet");
        rd_.Text(heatTitle, D2D1::RectF(heat.left + 12, heat.top + 8, heat.right - 12, heat.top + 30), rd_.fSmall.Get(),
                 scanning ? col::text : col::cyan);
        const bool liveLattice = scanning && snap_.lat.size() == static_cast<size_t>(kLat) * kLat &&
                                 snap_.col.size() == static_cast<size_t>(kLat);
        if (liveLattice && rd_.LatticeReady()) {
            const D2D1_RECT_F plot = D2D1::RectF(heat.left + 10, heat.top + 34, heat.right - 10, heat.bottom - 10);
            const float side = std::min(W(plot), H(plot));
            const float pcx = (plot.left + plot.right) * 0.5f;
            const float pcy = (plot.top + plot.bottom) * 0.5f;
            rd_.DrawBitmap(rd_.Lattice(), D2D1::RectF(pcx - side * 0.5f, pcy - side * 0.5f, pcx + side * 0.5f, pcy + side * 0.5f), 1.0f);
        } else if (!scanning && homeHas_ && rd_.LatticeReady()) {
            rd_.RenderLattice(time_, homeLat_.data(), homeCol_.data());
            const D2D1_RECT_F plot = D2D1::RectF(heat.left + 10, heat.top + 34, heat.right - 10, heat.bottom - 10);
            const float side = std::min(W(plot), H(plot));
            const float pcx = (plot.left + plot.right) * 0.5f;
            const float pcy = (plot.top + plot.bottom) * 0.5f;
            rd_.DrawBitmap(rd_.Lattice(), D2D1::RectF(pcx - side * 0.5f, pcy - side * 0.5f, pcx + side * 0.5f, pcy + side * 0.5f), 1.0f);
        } else if (!scanning && !homeHas_) {
            rd_.Text(L"Run Measure timing to draw this.",
                     D2D1::RectF(heat.left + 8, heat.top + 28, heat.right - 8, heat.bottom - 8), rd_.fSmall.Get(), col::dim);
        } else if (scanning) {
            rd_.Text(L"The scan is starting.",
                     D2D1::RectF(heat.left + 8, heat.top + 28, heat.right - 8, heat.bottom - 8), rd_.fSmall.Get(), col::dim);
        }
        std::vector<HealthCheck> rows;
        HealthSummary sum;
        {
            std::lock_guard<std::mutex> g(healthM_);
            rows = checks_;
            sum = Summarize(checks_);
        }
        const float rx = centerR;
        rd_.Text(healthBusy_.load() ? L"Checking live settings" : sum.headline,
                 D2D1::RectF(rx, B.top, B.right, B.top + 28), rd_.fPhase.Get(), col::cyan, DWRITE_TEXT_ALIGNMENT_TRAILING);
        rd_.Text(Fmt(L"%d live    %d need attention    %d other", sum.pass, sum.attention, sum.info + sum.other),
                 D2D1::RectF(rx, B.top + 28, B.right, B.top + 44), rd_.fMonoSmall.Get(), col::dim,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
        rd_.Text(L"Not every item applies on all hardware.",
                 D2D1::RectF(rx, B.top + 42, B.right, B.top + 58), rd_.fSmall.Get(), col::dim,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
        const wchar_t* groupNames[] = {L"Hold timer", L"CPU power", L"GPU driver", L"GPU settings",
                                       L"Afterburner", L"Memory", L"Storage"};
        const wchar_t* groupCats[] = {L"Timer Resolution", L"Power and Performance", L"Graphics Drivers",
                                      L"Windows Configuration", L"Supporting Utilities", L"Memory and CPU", L"Storage"};
        struct Group {
            const wchar_t* title;
            std::vector<int> items;
        };
        Group groups[8]{};
        for (int g = 0; g < 7; ++g) groups[g].title = groupNames[g];
        groups[7].title = L"Other";
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            int g = 7;
            for (int k = 0; k < 7; ++k) {
                if (rows[static_cast<size_t>(i)].category == groupCats[k]) g = k;
            }
            groups[g].items.push_back(i);
        }
        const float listTop = B.top + 64;
        const float availH = std::max(40.0f, B.bottom - listTop);
        float itemH = 20.0f;
        const float headH = 22.0f;
        auto groupHeight = [&](size_t n) { return 8.0f + headH + n * itemH + 6.0f; };
        float tallest = 0;
        for (const auto& g : groups) {
            if (!g.items.empty()) tallest = std::max(tallest, groupHeight(g.items.size()));
        }
        while (tallest > availH - 4.0f && itemH > 12.0f) {
            itemH -= 1.0f;
            tallest = 0;
            for (const auto& g : groups) {
                if (!g.items.empty()) tallest = std::max(tallest, groupHeight(g.items.size()));
            }
        }
        const float cardGap = 8.0f;
        const float paneW = std::max(80.0f, B.right - rx);
        int columns = 1;
        float used = 0;
        for (const auto& g : groups) {
            if (g.items.empty()) continue;
            const float h = groupHeight(g.items.size()) + cardGap;
            if (used > 0 && used + h > availH) {
                ++columns;
                used = 0;
            }
            used += h;
        }
        float groupW = 228.0f;
        if (columns * (groupW + cardGap) > paneW) groupW = std::max(128.0f, (paneW - cardGap * columns) / columns);
        float xRight = B.right;
        float y = listTop;
        bool placed = false;
        for (int gi = 0; gi < 8; ++gi) {
            const auto& g = groups[gi];
            if (g.items.empty()) continue;
            const float h = groupHeight(g.items.size());
            if (placed && y + h > B.bottom - 2) {
                xRight -= groupW + cardGap;
                y = listTop;
            }
            placed = true;
            const D2D1_RECT_F card = D2D1::RectF(xRight - groupW, y, xRight, y + h);
            const wchar_t* groupBlurbs[] = {
                L"The boot service and the global flag that hold the chosen timer resolution, plus the resolution Windows is using right now.",
                L"Each processor power setting from the CPU step. Green means the live value still matches the tune. Red means it has drifted.",
                L"The installed graphics driver. This check does not decide whether a newer driver exists.",
                L"Hardware-accelerated GPU scheduling, Game DVR, Game Bar, and the vendor profile.",
                L"Whether Afterburner and RivaTuner are installed and set to start at sign-in. A green box does not by itself mean an overclock is active.",
                L"The page file and memory compression.",
                L"SysMain and TRIM.",
                L"Checks that do not belong to one of the steps above.",
            };
            NoteTip(card, g.title, groupBlurbs[gi]);
            rd_.FillRound(card, 12, Alpha(col::panel, 0.92f));
            rd_.StrokeRound(card, 12, Alpha(col::cyan, 0.35f), 1.2f);
            rd_.Text(g.title, D2D1::RectF(card.left + 8, card.top + 4, card.right - 8, card.top + 4 + headH),
                     itemH >= 16.0f ? rd_.fSmall.Get() : rd_.fMonoSmall.Get(), col::cyan,
                     DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            float iy = card.top + 6 + headH;
            for (int index : g.items) {
                const auto& c = rows[static_cast<size_t>(index)];
                const bool applied = c.status == HealthStatus::Pass;
                const D2D1_COLOR_F mark = applied ? col::green : col::red;
                const D2D1_RECT_F row = D2D1::RectF(card.left + 4, iy, card.right - 4, iy + itemH);
                std::wstring about = c.explanation.empty() ? std::wstring(L"This check only reads the live value.") : c.explanation;
                about += L"\n\nStatus: ";
                about += HealthStatusText(c.status);
                if (!c.observed.empty()) about += L"\nLive: " + c.observed;
                if (!c.expected.empty()) about += L"\nExpected: " + c.expected;
                NoteTip(row, c.name, about);
                const float box = std::max(8.0f, itemH - 6.0f);
                const D2D1_RECT_F markR = D2D1::RectF(card.right - 8 - box, iy + (itemH - box) * 0.5f, card.right - 8,
                                                     iy + (itemH + box) * 0.5f);
                rd_.Glow(D2D1::Point2F((markR.left + markR.right) * 0.5f, (markR.top + markR.bottom) * 0.5f), box, mark, 0.35f);
                rd_.FillRound(markR, 2, mark);
                rd_.Text(c.name, D2D1::RectF(card.left + 8, iy, markR.left - 6, iy + itemH),
                         itemH >= 16.0f ? rd_.fSmall.Get() : rd_.fMonoSmall.Get(), col::text,
                         DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                iy += itemH;
            }
            y += h + cardGap;
        }
        DrawGuide(D2D1::RectF(centerL, B.top, centerR, B.bottom));
    }

    void DrawWelcome(const D2D1_RECT_F& R, float dt) {
        const float cx = (R.left + R.right) / 2;
        const float cardW = std::min(820.0f, W(R) - 40);
        const D2D1_RECT_F card = D2D1::RectF(cx - cardW / 2, R.top + 8, cx + cardW / 2, R.bottom);
        rd_.FillRound(card, 16, Alpha(col::panel, 0.92f));
        rd_.StrokeRound(card, 16, Alpha(col::cyan, 0.22f), 1.4f);
        const float etchHalf = std::min(250.0f, (W(card) - 80.0f) * 0.5f);
        if (DrawEtchedHome(D2D1::RectF(cx - etchHalf, card.top + 2, cx + etchHalf, card.top + 58), dt)) OpenHome();
        if (DrawSettingsButton(D2D1::RectF(card.right - 40.0f, card.top + 10.0f, card.right - 12.0f, card.top + 38.0f), dt)) {
            settingsOpen_ = !settingsOpen_;
            if (settingsOpen_) settingsScroll_ = 0;
        }
        rd_.Glow(D2D1::Point2F(cx, card.top + 118), 180, col::cyan, 0.16f + 0.08f * (0.5f + 0.5f * std::sin(time_ * 1.4f)));
        rd_.Glow(D2D1::Point2F(cx, card.top + 118), 240, col::violet, 0.08f);

        float y = card.top + 62;
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

        const std::wstring company = L"Lucius & Wayne Systems";
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
        rd_.Text(L"Timer resolution, CPU power, GPU, memory, and storage. Low-level settings are tuned to harden the machine for a more responsive, lower-latency configuration, without unneeded versatility or fluff. Perfect for gamers and HPC. Uncheck any step you want to skip. Begin makes a restore point first, measures timing next, then runs the remaining steps in a safe order. It asks only before a driver install or Afterburner.",
                 D2D1::RectF(card.left + 48, y, card.right - 48, y + 108), rd_.fBody.Get(), col::muted,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 116;

        rd_.Text(L"Select the steps to run  -  everything is on by default",
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
            L"Prepare - restore point",
            L"Measure - Sleep(1) sweep",
            L"Hold - boot service",
            L"CPU Tune - Ultimate + PPM",
            L"GPU Clean - latest driver, no extras",
            L"GPU Config - power & latency",
            L"Afterburner - download and scan, ~30 min",
            L"Memory - pagefile, compression",
            L"Storage - ASPM, TRIM, USB",
            L"Review - BIOS, then restart",
        };
        const float colW = (W(card) - 88) / 2;
        const int rows = 5;
        const float rowH = 28.0f;
        for (int i = 0; i < kWantCount + 1; ++i) {
            const float colN = i < rows ? 0.0f : 1.0f;
            const float row = static_cast<float>(i < rows ? i : i - rows);
            const float x = card.left + 36 + colN * (colW + 16);
            const float yy = y + row * rowH;
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
        std::wstring hint = canBegin ? L"The restart screen lists the BIOS settings to set by hand. Press Enter to begin."
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
        if (Button(3, btnB, L"Health", col::violet, false, true, dt)) OpenHealth();
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
                    rd_.Text(Fmt(L"untuned Sleep(1) overshoots by %+.2f ms  -  default ~15.6 ms clock",
                                 snap_.baseline.median),
                             D2D1::RectF(B.left + 8, B.bottom - 62, B.right - 8, B.bottom - 44), rd_.fMonoSmall.Get(),
                             col::dim);
                }
                if (Button(1, D2D1::RectF(cx - 70, B.bottom - 36, cx + 70, B.bottom - 4), L"Stop", col::red, false, true, dt)) {
                    CancelJob(JobId::Measure);
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
                        unsigned units = Winner();
                        if (!units) units = host_.pinnedResolution;
                        rd_.Text(units ? L"The boot service installs on its own and holds this period at every startup."
                                       : L"There is no measured period yet, so the boot service was not installed.",
                                 D2D1::RectF(B.left, B.top + 108, B.right, B.top + 132), rd_.fSmall.Get(), col::muted,
                                 DWRITE_TEXT_ALIGNMENT_CENTER);
                        if (Button(2, btnA, L"Continue", col::cyan, true, true, dt, true, true)) OpenHome();
                        if (Button(4, btnB, L"Close", col::muted, false, true, dt)) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                    } else {
                        rd_.Text(L"The boot service will not be installed. Continue to the next selected step.",
                                 D2D1::RectF(B.left, B.top + 108, B.right, B.top + 132), rd_.fSmall.Get(), col::muted,
                                 DWRITE_TEXT_ALIGNMENT_CENTER);
                        if (Button(2, btnA, L"Continue", col::cyan, true, true, dt, true, true)) OpenHome();
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
                               L"Continue  -  create restore point", col::cyan,
                               [this] { StartPrepare(); },
                               [this] { OpenHome(); });
                break;
            case Stage::PrepareApplying:
            case Stage::PrepareDone:
                DrawTweakProgress(B, dt, stage == Stage::PrepareDone, false);
                break;
            case Stage::PrepareFailed:
                DrawPackFail(B, dt, L"Restore point could not be created. System Protection may be off.",
                             [this] { StartPrepare(); },
                             [this] { OpenHome(); });
                break;
            case Stage::PpmPrompt:
                DrawPpmPrompt(B, dt);
                break;
            case Stage::PpmApplying:
            case Stage::PpmDone:
                DrawTweakProgress(B, dt, stage == Stage::PpmDone, false);
                break;
            case Stage::PpmFailed:
                rd_.Text(L"PPM apply failed - see the log. Timer resolution is still installed.",
                         D2D1::RectF(B.left, B.top + 16, B.right, B.top + 40), rd_.fSub.Get(), col::red,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
                rd_.Text(L"Timer resolution is still held. Retry the power-plan writes, or skip to the next step.",
                         D2D1::RectF(B.left + 8, B.top + 44, B.right - 8, B.bottom - 44), rd_.fBody.Get(), col::muted,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
                if (Button(6, btnA, L"Retry PPM", col::violet, true, true, dt, true, true)) StartPpm();
                if (Button(7, btnB, L"Skip", col::muted, false, true, dt)) OpenHome();
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
                rd_.Text(L"GPU apply failed - see the log. Timer resolution is still installed.",
                         D2D1::RectF(B.left, B.top + 16, B.right, B.top + 40), rd_.fSub.Get(), col::red,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
                rd_.Text(L"Timer resolution is still held. Retry the GPU writes, or skip to the next step.",
                         D2D1::RectF(B.left + 8, B.top + 44, B.right - 8, B.bottom - 44), rd_.fBody.Get(), col::muted,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
                if (Button(6, btnA, L"Retry GPU", col::violet, true, true, dt, true, true)) StartGpu();
                if (Button(7, btnB, L"Skip", col::muted, false, true, dt)) stage_ = Stage::GpuDone;
                break;
            case Stage::AfterburnerPrompt:
                DrawPackPrompt(B, dt, L"Afterburner and RivaTuner",
                               L"The Afterburner download and the OC scan take about 30 minutes. MSI Afterburner 4.6.6 and "
                               L"RivaTuner are installed if they are missing. On an NVIDIA GPU the scanner starts on its own. "
                               L"The curve it finds is saved as profile 1 and applied every time you log on. This step does not change the GPU driver.",
                               L"Continue  -  install and scan", col::green,
                               [this] { StartAfterburner(); },
                               [this] { OpenHome(); });
                break;
            case Stage::AfterburnerApplying:
            case Stage::AfterburnerDone:
                DrawTweakProgress(B, dt, stage == Stage::AfterburnerDone, false);
                break;
            case Stage::AfterburnerFailed:
                DrawPackFail(B, dt, L"Afterburner failed - see the log. The GPU driver settings are unchanged.",
                             [this] { StartAfterburner(); },
                             [this] { OpenHome(); });
                break;
            case Stage::MemoryPrompt:
                DrawPackPrompt(B, dt, L"Pagefile and memory compression",
                               L"Keeps a system-managed pagefile so commit never hits a zero-pagefile cliff, and "
                               L"turns off memory compression so standby pages are not compressed on a CPU thread.",
                               L"Continue  -  apply memory tweaks", col::violet,
                               [this] { StartMemory(); },
                               [this] { OpenHome(); });
                break;
            case Stage::MemoryApplying:
            case Stage::MemoryDone:
                DrawTweakProgress(B, dt, stage == Stage::MemoryDone, false);
                break;
            case Stage::MemoryFailed:
                DrawPackFail(B, dt, L"Memory tweaks failed - see the log. Earlier writes are still in place.",
                             [this] { StartMemory(); },
                             [this] { OpenHome(); });
                break;
            case Stage::StoragePrompt:
                DrawPackPrompt(B, dt, L"SSD, PCIe, USB, TRIM",
                               L"PCIe ASPM off, disks never spun down, USB selective suspend off, TRIM on, "
                               L"SysMain/Superfetch disabled, hibernate off, write cache on, NTFS last-access "
                               L"timestamps off. Windows Search is left running so Start-menu search still works.",
                               L"Continue  -  apply storage tweaks", col::amber,
                               [this] { StartStorage(); },
                               [this] { OpenHome(); });
                break;
            case Stage::StorageApplying:
            case Stage::StorageDone:
                DrawTweakProgress(B, dt, stage == Stage::StorageDone, true);
                break;
            case Stage::StorageFailed:
                DrawPackFail(B, dt, L"Storage tweaks failed - see the log. Retry, or skip to Review.",
                             [this] { StartStorage(); },
                             [this] { OpenHome(); });
                break;
            case Stage::Review:
                DrawReview(B, dt);
                break;
            case Stage::Monitor:
                DrawMonitor(B, dt);
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

    void BeginGpuLookup() {
        if (JobBusy(JobId::GpuClean)) return;
        {
            std::lock_guard<std::mutex> g(gpuM_);
            if (gpuPhase_ == GpuDl::Looking || gpuPhase_ == GpuDl::Downloading || gpuPhase_ == GpuDl::Installing) return;
            gpuPhase_ = GpuDl::Looking;
            gpuOffer_ = {};
        }
        gpuStop_.store(false);
        LaunchJob(JobId::GpuClean, [this](SharedState& feed) {
            feed.Phase(L"GPU CLEAN", L"looking up the current driver");
            const GpuDriverOffer o = LookupGpuDriver(&feed, gpuStop_);
            std::wstring phase = L"manual driver install";
            {
                std::lock_guard<std::mutex> g(gpuM_);
                if (gpuStop_.load() || feed.Cancelled()) return 1;
                gpuOffer_ = o;
                if (o.ok) {
                    gpuPhase_ = GpuDl::Offer;
                    phase = o.version.empty() ? L"driver package found" : o.version;
                } else {
                    gpuPhase_ = GpuDl::Manual;
                }
            }
            feed.Phase(L"GPU CLEAN", phase);
            return 0;
        });
    }

    void BeginGpuDownload() {
        if (JobBusy(JobId::GpuClean)) return;
        {
            std::lock_guard<std::mutex> g(gpuM_);
            if (gpuPhase_ != GpuDl::Offer && gpuPhase_ != GpuDl::Failed) return;
            if (!gpuOffer_.ok || !gpuOffer_.canDownload || gpuOffer_.url.empty()) return;
            gpuPhase_ = GpuDl::Downloading;
            gpuOffer_.error.clear();
        }
        gpuStop_.store(false);
        LaunchJob(JobId::GpuClean, [this](SharedState& feed) {
            GpuDriverOffer o;
            {
                std::lock_guard<std::mutex> g(gpuM_);
                o = gpuOffer_;
            }
            feed.Phase(L"GPU CLEAN", L"downloading the driver package");
            const int rc = DownloadGpuDriver(feed, o, gpuStop_);
            std::wstring detail = L"package ready to install";
            int code = 0;
            {
                std::lock_guard<std::mutex> g(gpuM_);
                if (gpuStop_.load() || feed.Cancelled()) return 1;
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
                    code = 1;
                }
            }
            feed.Phase(L"GPU CLEAN", detail);
            return code;
        });
    }

    void BeginGpuInstall() {
        if (JobBusy(JobId::GpuClean)) return;
        {
            std::lock_guard<std::mutex> g(gpuM_);
            const bool retryInstall = gpuPhase_ == GpuDl::Failed && gpuOffer_.fail == 2 && !gpuOffer_.file.empty();
            if (gpuPhase_ != GpuDl::Ready && !retryInstall) return;
            if (gpuOffer_.file.empty()) return;
            gpuPhase_ = GpuDl::Installing;
            gpuOffer_.error.clear();
        }
        gpuStop_.store(false);
        LaunchJob(JobId::GpuClean, [this](SharedState& feed) {
            HeldLock volume;
            HeldLock gpu;
            if (!AcquireGate(JobId::GpuClean, volumeMu_, L"a restore point is using the disk", feed, volume)) return 1;
            if (!AcquireGate(JobId::GpuClean, gpuMu_, L"GPU settings or Afterburner are using the graphics driver", feed, gpu))
                return 1;
            const bool replayConfig = jobs_[static_cast<int>(JobId::Gpu)].phase.load() == static_cast<int>(JobPhase::Done);
            if (jobs_[static_cast<int>(JobId::Afterburner)].phase.load() == static_cast<int>(JobPhase::Done)) {
                afterburnerStale_.store(true);
                jobs_[static_cast<int>(JobId::Afterburner)].feed.Log(
                    L"A graphics driver install started after the Afterburner scan. Run Afterburner again if the curve is gone.");
            }
            GpuDriverOffer o;
            {
                std::lock_guard<std::mutex> g(gpuM_);
                o = gpuOffer_;
            }
            feed.Phase(L"GPU CLEAN", L"installing the trimmed driver");
            const int rc = InstallTrimmedGpuDriver(feed, o);
            std::wstring err;
            {
                std::lock_guard<std::mutex> g(gpuM_);
                if (feed.Cancelled()) return 1;
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
            feed.Phase(L"GPU CLEAN", err.empty() ? L"trimmed driver installed" : L"driver install failed");
            if (!err.empty()) feed.Log(err);
            if (rc == 0 && replayConfig) replayGpu_.store(true);
            return rc == 0 ? 0 : 1;
        });
    }

    void SkipGpuClean() {
        GpuDl phase;
        {
            std::lock_guard<std::mutex> g(gpuM_);
            phase = gpuPhase_;
        }
        if (phase == GpuDl::Installing) {
            OpenHome();
            return;
        }
        gpuStop_.store(true);
        CancelJob(JobId::GpuClean);
        {
            std::lock_guard<std::mutex> g(gpuM_);
            gpuPhase_ = GpuDl::Idle;
        }
        ReleaseGpuFollowers();
        OpenHome();
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
            if (Button(6, btnA, L"Continue", col::cyan, true, true, dt, true, true)) {
                ReleaseGpuFollowers();
                if (JobBusy(JobId::Gpu)) stage_ = Stage::GpuApplying;
                else if (Want(kWantGpu)) stage_ = Stage::GpuPrompt;
                else OpenHome();
            }
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
            if (offer.bytes) go = L"Download  |  " + sizeText(offer.bytes);
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
            if (Button(6, btnA, L"Continue  -  configure GPU", col::green, true, true, dt, true, true)) {
                ReleaseGpuFollowers();
                if (JobBusy(JobId::Gpu)) stage_ = Stage::GpuApplying;
                else if (Want(kWantGpu)) stage_ = Stage::GpuPrompt;
                else OpenHome();
            }
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
        rd_.Text(L"Before you restart", D2D1::RectF(B.left, B.top + 8, B.right, B.top + 32), rd_.fPhase.Get(), col::green,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        rd_.Text(L"Windows cannot change BIOS. Set these, then restart so they and the kernel flags take effect.",
                 D2D1::RectF(B.left + 8, B.top + 32, B.right - 8, B.top + 52), rd_.fSmall.Get(), col::muted,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        const float bodyBottom = B.bottom - (restartConfirm_ ? 72.0f : 48.0f);
        rd_.Text(L"Lock the CPU multiplier. Disable Intel Turbo / EIST / Speed Shift, or AMD PBO / Precision Boost.\n"
                 L"Disable deep C-states. Leave C1 if the machine will not POST without it.\n"
                 L"Enable XMP (Intel) or DOCP (AMD) at the RAM's rated frequency, and DRAM voltage if the profile needs it.\n"
                 L"SATA/NVMe in AHCI. Enable Above 4G / Resizable BAR if you want ReBAR.\n"
                 L"If anything is unstable: Settings \u2192 System Restore \u2192 'Synchronus pre-tune', or Synchronus.exe --uninstall.",
                 D2D1::RectF(B.left + 12, B.top + 56, B.right - 12, bodyBottom), rd_.fBody.Get(), col::text);
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
        rd_.Text(p.alreadyApplied ? (p.reason + L"  |  reapplying current values")
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
        if (Button(6, btnA, locked ? L"Continue  -  apply CPU tweaks" : L"Continue anyway", accent, true, true, dt,
                   true, true))
            StartPpm();
        if (Button(7, btnB, L"Skip this step", col::muted, false, true, dt)) OpenHome();
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
        rd_.Text(g.alreadyApplied ? (g.detail + L"  |  reapplying current values") : g.detail,
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
        if (Button(6, btnA, L"Continue  -  apply GPU tweaks", accent, true, true, dt, true, true)) StartGpu();
        if (Button(7, btnB, L"Skip this step", col::muted, false, true, dt)) {
            gpuSkipped_ = true;
            OpenHome();
        }
    }

    void DrawTweakProgress(const D2D1_RECT_F& B, float dt, bool done, bool gpu) {
        (void)dt;
        const bool skipped = gpu && gpuSkipped_;
        const StepMeta meta = Meta(stage_.load());
        rd_.Text(done ? (skipped ? Fmt(L"%s skipped", meta.name).c_str() : Fmt(L"%s applied", meta.name).c_str())
                      : meta.title,
                 D2D1::RectF(B.left + 8, B.top + 6, B.right - 110, B.top + 28), rd_.fPhase.Get(),
                 done ? col::green : col::amber, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (!done && Button(8, D2D1::RectF(B.right - 96, B.top + 4, B.right - 8, B.top + 30), L"Stop", col::red, false, true, dt)) {
            CancelJob(StageJob(stage_.load()));
        }
        const wchar_t* progressLine = L"Writing settings.";
        if (!done && !snap_.detail.empty()) progressLine = snap_.detail.c_str();
        std::wstring doneLine = L"Complete.";
        if (done && stage_.load() == Stage::AfterburnerDone && afterburnerStale_.load()) {
            doneLine = L"Complete. A later driver install may have cleared the curve. Run this step again if the overclock is gone.";
        }
        rd_.Text(done ? doneLine.c_str() : progressLine,
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
            const wchar_t* mark = L"|";
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
    JobSlot jobs_[static_cast<int>(JobId::Count)];
    std::mutex timerMu_, powerMu_, gpuMu_, volumeMu_;
    bool chainAuto_ = false;
    bool deferGpu_ = false;
    bool fleet_ = false;
    bool driverSettled_ = false;
    bool prepareWaived_ = false;
    bool afterburnerSkipped_ = false;
    bool afterburnerPrompt_ = false;
    std::atomic<bool> replayGpu_{false};
    std::atomic<bool> afterburnerStale_{false};
    int repairJob_ = -1;
    bool repairRescan_ = false;
    bool maintainAfterScan_ = false;
    bool maintainWatch_[static_cast<int>(JobId::Count)]{};
    unsigned long long nextMaintainAt_ = 0;
    int chainPhase_[static_cast<int>(JobId::Count)]{};
    std::thread updateThread_;
    std::mutex updateM_;
    AppUpdate updateOffer_{};
    std::wstring updateLine_;
    std::wstring updateErr_;
    bool updateReady_ = false;
    bool updateNewer_ = false;
    bool updatePrompted_ = false;
    std::wstring updateAnnounced_;
    bool updateFailed_ = false;
    std::atomic<bool> updateBusy_{false};
    std::atomic<bool> updateChecking_{false};
    std::atomic<bool> updateStop_{false};
    std::atomic<bool> relaunch_{false};
    mutable std::mutex resultM_;
    ResultView result_;
    std::mutex probeM_;
    CpuProbeView probe_{};
    bool probeReady_ = false;
    mutable std::mutex gpuM_;
    GpuProbeView gpu_{};
    bool gpuReady_ = false;
    GpuDl gpuPhase_ = GpuDl::Idle;
    GpuDriverOffer gpuOffer_{};
    std::atomic<bool> gpuStop_{false};
    std::atomic<Stage> stage_{Stage::Welcome};
    std::atomic<bool> serviceStopped_{false};
    std::atomic<bool> installedThisSession_{false};
    bool gpuSkipped_ = false;
    bool want_[kWantCount]{};
    int fidelity_ = 1;
    bool restartConfirm_ = false;
    bool running_ = true;
    bool exitRequested_ = false;
    bool trayOn_ = false;
    bool hubReturn_ = false;
    bool updatePulse_ = false;
    bool pulseDim_ = false;
    bool showUpdatePrompt_ = false;
    HICON trayIcon_ = nullptr;
    HICON taskbarBig_ = nullptr;
    HICON taskbarSmall_ = nullptr;
    bool homeLoaded_ = false;
    bool homeHas_ = false;
    std::wstring homeScanLabel_;
    std::vector<LatCell> homeLat_;
    std::vector<ColStat> homeCol_;
    bool settingsOpen_ = false;
    int settingsScroll_ = 0;
    int settingsMaxScroll_ = 0;
    int pendingBrowse_ = 0;
    unsigned long long nextUpdateAt_ = 0;
    NOTIFYICONDATAW nid_{};
    UINT showMsg_ = 0;
    MonitorPrefs prefs_{};
    std::mutex healthM_;
    std::vector<HealthCheck> checks_;
    std::vector<std::wstring> noted_;
    std::thread healthThread_;
    std::atomic<bool> healthStop_{false};
    std::atomic<bool> healthBusy_{false};
    unsigned long long lastScanAt_ = 0;
    unsigned long long nextScanAt_ = 0;
    unsigned long long lastNotifyAt_ = 0;
    int healthFilter_ = 0;
    int healthScroll_ = 0;
    int healthSelected_ = -1;

    double lastFrame_ = 0;
    float time_ = 0, introT_ = 0, revealT_ = 0, installT_ = 0, spinner_ = 0, progress_ = 0, yMax_ = 0.6f, activeX_ = -1;
    std::vector<float> anScore_, anMean_, anMedian_, anLate_, anIdle_, anAppear_, finBar_, histH_, hover_;
    float statV_[7] = {};
    std::vector<Particle> particles_;
    uint32_t rng_ = 0x9E3779B9u;
    uint64_t lastSeq_ = 0;

    D2D1_POINT_2F mouse_ = D2D1::Point2F(-1, -1), clickPos_ = D2D1::Point2F(-1, -1);
    bool mouseDown_ = false, clickPending_ = false, enterPending_ = false, hoverButton_ = false;
    std::wstring tipTitle_;
    std::wstring tipBody_;
    std::vector<ButtonRec> buttons_;
};

}  // namespace

int Run(const Host& host) {
    App app(host);
    return app.Run();
}

}  // namespace gui
