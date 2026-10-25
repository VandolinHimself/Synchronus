// Synchronus - single-executable timer resolution tuner and service.
//
//   Synchronus.exe                       Open the GUI.
//   Synchronus.exe --cli                 The same flow in a console.
//   Synchronus.exe --benchmark           Benchmark only (no changes installed).
//   Synchronus.exe --install             Benchmark (unless --resolution is
//                                        given) then install or update the service.
//   Synchronus.exe --apply               Hold a resolution in this console
//                                        until Ctrl+C.
//   Synchronus.exe --status              Show service and current resolution.
//   Synchronus.exe --uninstall           Stop and remove the service, restore the
//                                        registry values it changed.
//   Synchronus.exe --service             Internal: run under the Service
//                                        Control Manager (the installed
//                                        service's ImagePath).
//
// No third-party dependencies: Win32 + ntdll only, statically linked CRT, so
// the Release build is one self-contained .exe.

#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <intrin.h>
#include <powrprof.h>
#include <winioctl.h>

#include "Gui.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <io.h>
#include <numeric>
#include <string>
#include <vector>

extern "C" NTSYSAPI NTSTATUS NTAPI NtQueryTimerResolution(PULONG MinimumResolution, PULONG MaximumResolution, PULONG CurrentResolution);
extern "C" NTSYSAPI NTSTATUS NTAPI NtSetTimerResolution(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);

gui::MeasurePlan gui::MeasurePlanFor(int fidelity) {
    MeasurePlan p;
    int divisor = 1;
    p.passes = 3;
    if (fidelity <= 0) {
        divisor = 64;
        p.passes = 1;
    } else if (fidelity == 1) {
        divisor = 8;
    }
    // High is 150 back-to-back samples and 1500 verification samples.
    // Medium, the default, is one eighth of that. Low is one eighth of
    // medium, with a floor so one missed tick is not the whole run, and a
    // single pass. The 10 rounds match kVerifyRounds in the sweep.
    p.samples = std::max(8, 150 / divisor);
    p.verifySamples = std::max(100, 1500 / divisor);
    const int idleCount = std::max(p.samples / 2, 5);
    const int verifyIdle = std::max(p.verifySamples / 2, 10);
    const double blockMs = p.samples * 1.15 + idleCount * (1.15 + 3.5) + 30.0;
    const double verifyBlockMs = p.verifySamples * 1.15 + verifyIdle * (1.15 + 3.5) + 10 * 30.0;
    constexpr int kCandidates = 101;  // 0.50–0.60 ms at 0.001 ms, inclusive
    p.seconds = static_cast<double>(kCandidates) * p.passes * blockMs / 1000.0 + 6.0 * verifyBlockMs / 1000.0 + 2.0;
    return p;
}

namespace {

constexpr wchar_t kVersion[] = L"1.12.8";
constexpr wchar_t kProduct[] = L"Synchronus";
constexpr wchar_t kCompany[] = L"Lucius And Wayne Systems LLC";
constexpr wchar_t kCompanyUrl[] = L"https://luciuswayne.com";

// Set while the GUI is driving a benchmark: the core streams phases,
// progress, samples and snapshots into it (see Gui.h).
gui::Feed* g_feed = nullptr;

// Console state. The binary is a GUI-subsystem executable; command-line
// invocations attach to the parent console (or allocate one when there is
// none, e.g. after UAC re-launch) before printing anything.
bool g_console = false;
bool g_allocatedConsole = false;
constexpr wchar_t kServiceName[] = L"Synchronus";
constexpr wchar_t kLegacyServiceName[] = L"TimerResolution";
constexpr wchar_t kServiceDisplayName[] = L"Synchronus (timer resolution)";
constexpr wchar_t kServiceDescription[] =
    L"Holds the system timer resolution at the value chosen by Synchronus so "
    L"Sleep()/wait timers fire with the lowest measured latency on this machine.";
constexpr wchar_t kAppRegKey[] = L"SOFTWARE\\Synchronus";
constexpr wchar_t kLegacyAppRegKey[] = L"SOFTWARE\\TimerResolution";
constexpr wchar_t kKernelRegKey[] = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\kernel";
constexpr wchar_t kGlobalFlagName[] = L"GlobalTimerResolutionRequests";
constexpr wchar_t kThreadDpcName[] = L"ThreadDpcEnable";

// Resolutions are in 100-ns units throughout (10,000 == 1 ms), matching
// NtQuery/NtSetTimerResolution. The kernel reports:
//   MinimumResolution = slowest  (largest period,  e.g. 156250 == 15.625 ms)
//   MaximumResolution = fastest  (smallest period, e.g.   5000 ==  0.5    ms)

// ---------------------------------------------------------------- utilities

double UnitsToMs(ULONG units) { return units / 10000.0; }

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buffer[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    return buffer;
}

std::wstring EnvOr(const wchar_t* name, const wchar_t* fallback) {
    wchar_t buffer[MAX_PATH];
    const DWORD len = GetEnvironmentVariableW(name, buffer, _countof(buffer));
    if (len == 0 || len >= _countof(buffer)) return fallback;
    return buffer;
}

std::wstring ProgramDataDir() {
    return EnvOr(L"ProgramData", L"C:\\ProgramData") + L"\\Synchronus";
}

std::wstring LogFilePath() { return ProgramDataDir() + L"\\Synchronus.log"; }

std::wstring Timestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return Format(L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
}

std::string ToUtf8(const std::wstring& text) {
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), needed, nullptr, nullptr);
    return out;
}

// Append a line to the log file. Best-effort: logging must never fail a run
// (the service runs as SYSTEM, interactive runs may lack write access until
// elevated, in which case the console copy still exists).
void LogLine(const std::wstring& line) {
    CreateDirectoryW(ProgramDataDir().c_str(), nullptr);
    HANDLE file = CreateFileW(LogFilePath().c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const std::string entry = ToUtf8(Timestamp()) + "  " + ToUtf8(line) + "\r\n";
    DWORD written = 0;
    WriteFile(file, entry.data(), static_cast<DWORD>(entry.size()), &written, nullptr);
    CloseHandle(file);
}

bool HasConsole(DWORD which) {
    HANDLE h = GetStdHandle(which);
    DWORD mode = 0;
    return h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) != 0;
}

// Console (when attached) + log + GUI feed. Not used inside the QPC-Sleep-QPC
// window of a sample (I/O would skew timings).
void Out(const std::wstring& line) {
    if (HasConsole(STD_OUTPUT_HANDLE)) {
        fwprintf(stdout, L"%s\n", line.c_str());
        fflush(stdout);
    }
    LogLine(line);
    if (g_feed) g_feed->Log(line);
}

void Err(const std::wstring& line) {
    if (HasConsole(STD_ERROR_HANDLE) || HasConsole(STD_OUTPUT_HANDLE)) {
        fwprintf(stderr, L"%s\n", line.c_str());
        fflush(stderr);
    }
    LogLine(L"ERROR: " + line);
    if (g_feed) g_feed->Log(L"ERROR: " + line);
}

bool IsAdmin() {
    HANDLE token = nullptr;
    bool admin = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation{};
        DWORD size = 0;
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
            admin = elevation.TokenIsElevated != 0;
        }
        CloseHandle(token);
    }
    return admin;
}

std::wstring GetExePath() {
    wchar_t path[MAX_PATH * 2];
    const DWORD len = GetModuleFileNameW(nullptr, path, _countof(path));
    return std::wstring(path, len);
}

std::wstring GetInstallDir() {
    return EnvOr(L"ProgramFiles", L"C:\\Program Files") + L"\\Synchronus";
}

std::wstring GetInstallPath() { return GetInstallDir() + L"\\Synchronus.exe"; }

bool EqualPath(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// Re-launch ourselves elevated. Returns false if the user declines UAC or
// the launch fails. `extraGuard` adds the internal --elevated flag so a
// still-unelevated child errors out instead of looping on UAC prompts.
bool RelaunchElevated(const std::vector<std::wstring>& args) {
    std::wstring params;
    for (const auto& arg : args) {
        if (!params.empty()) params += L' ';
        params += L'"';
        for (wchar_t c : arg) {
            if (c == L'"') params += L'\\';
            params += c;
        }
        params += L'"';
    }
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    const std::wstring exe = GetExePath();
    sei.lpFile = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        return false;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return true;
}

// Arguments for the elevated copy of this invocation (everything after
// argv[0]) plus the anti-loop guard.
std::vector<std::wstring> ElevatedArgs(const std::vector<std::wstring>& args) {
    std::vector<std::wstring> out = args;
    out.emplace_back(L"--elevated");
    return out;
}

// ---------------------------------------------------------------- registry

bool RegSetDword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD value) {
    HKEY handle = nullptr;
    if (RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS) return false;
    const LONG rc = RegSetValueExW(handle, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(handle);
    return rc == ERROR_SUCCESS;
}

bool RegGetDword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD& value) {
    DWORD type = 0, size = sizeof(value);
    return RegGetValueW(root, key, name, RRF_RT_REG_DWORD, &type,
                        &value, &size) == ERROR_SUCCESS;
}

bool RegSetString(HKEY root, const wchar_t* key, const wchar_t* name, const std::wstring& value) {
    HKEY handle = nullptr;
    if (RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS) return false;
    const LONG rc = RegSetValueExW(handle, name, 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(value.c_str()),
                                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(handle);
    return rc == ERROR_SUCCESS;
}

bool RegGetString(HKEY root, const wchar_t* key, const wchar_t* name, std::wstring& value) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    DWORD type = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, &type, buf, &size) != ERROR_SUCCESS) return false;
    value = buf;
    return true;
}

bool RegDeleteValueName(HKEY root, const wchar_t* key, const wchar_t* name) {
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key, 0, KEY_SET_VALUE, &handle) != ERROR_SUCCESS) return false;
    const LONG rc = RegDeleteValueW(handle, name);
    RegCloseKey(handle);
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

bool ContainsI(const wchar_t* hay, const wchar_t* needle) {
    if (!hay || !needle || !*needle) return false;
    const size_t n = wcslen(needle);
    for (const wchar_t* p = hay; *p; ++p) {
        size_t i = 0;
        while (i < n && p[i] && towlower(p[i]) == towlower(needle[i])) ++i;
        if (i == n) return true;
    }
    return false;
}

std::wstring GuidBare(const GUID& g) {
    wchar_t buf[64] = {};
    StringFromGUID2(g, buf, _countof(buf));
    std::wstring s = buf;
    if (s.size() >= 2 && s.front() == L'{') s = s.substr(1, s.size() - 2);
    return s;
}

DWORD RunHiddenWait(const std::wstring& exe, const std::wstring& args, DWORD timeoutMs) {
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return GetLastError() ? GetLastError() : 1;
    }
    const DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1;
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        code = WAIT_TIMEOUT;
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

DWORD RunHidden(const std::wstring& exe, const std::wstring& args) { return RunHiddenWait(exe, args, 20000); }

std::wstring PowerShellExe() {
    return EnvOr(L"SystemRoot", L"C:\\Windows") + L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
}

DWORD RunPowerShell(const std::wstring& command, DWORD timeoutMs = 40000) {
    return RunHiddenWait(PowerShellExe(), L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"" + command + L"\"",
                         timeoutMs);
}

std::wstring System32(const wchar_t* name) {
    wchar_t dir[MAX_PATH] = {};
    GetSystemDirectoryW(dir, _countof(dir));
    return std::wstring(dir) + L"\\" + name;
}

DWORD PowerCfg(const std::wstring& args) { return RunHidden(System32(L"powercfg.exe"), args); }

// ---------------------------------------------------------------- timer core

bool QueryResolutions(ULONG& slowest, ULONG& fastest, ULONG& current) {
    return NtQueryTimerResolution(&slowest, &fastest, &current) == 0;
}

// Exempt this process from EcoQoS timer-resolution throttling (the trick the
// original SetTimerResolution used) so Windows does not silently ignore a
// background process's resolution request. No-op on Windows 7-era kernels
// where SetProcessInformation does not exist.
void DisablePowerThrottling() {
    using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) return;
    auto fn = reinterpret_cast<SetProcessInformationFn>(
        reinterpret_cast<void*>(GetProcAddress(kernel32, "SetProcessInformation")));
    if (!fn) return;
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask = 0;
    fn(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
}

// Measurement priority. REALTIME (what the reference MeasureSleep used) keeps
// scheduler noise from other threads out of the numbers; Windows silently
// downgrades it to HIGH when the caller is not elevated. The sampling thread
// sleeps >99% of the time, so it cannot starve anything.
std::wstring RaiseBenchmarkPriority() {
    SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    switch (GetPriorityClass(GetCurrentProcess())) {
        case REALTIME_PRIORITY_CLASS: return L"realtime";
        case HIGH_PRIORITY_CLASS: return L"high (run elevated for realtime)";
        default: return L"normal";
    }
}

// ------------------------------------------------------------- measurement
//
// Why the sweep looks the way it does (see the research linked in README):
//
// Sleep(1) is woken by the clock interrupt: it returns on the first tick at
// which the kernel's tick-quantised clock has advanced >= 1 ms. With period R
// that is the k-th tick where k*R >= 1 ms, so the overshoot is ~k*R - 1 ms.
// At exactly R = 0.5000 ms, 2*R == 1 ms is a tie that is decided by
// interrupt-latency jitter, so Sleep(1) randomly takes 2 ticks (~1.00 ms) or
// 3 ticks (~1.50 ms): the "0.5 ms is worse than 0.507 ms" effect. Just above
// the tie, 2 ticks always suffice and the overshoot is a tiny 2*R - 1. Where
// the tie stops biting is system-dependent (0.502 ms on the author's desktop,
// 0.600 ms on a laptop), hence a dense 0.001 ms sweep of 0.50-0.60 ms, a
// per-candidate "missed tick" rate, and a safety margin above the boundary.

struct Stats {
    size_t n = 0;
    double median = 0;   // ms over the requested 1.000 ms
    double mean = 0;     // winsorised at median + 2.5 ticks (hiccup-proof, flip-sensitive)
    double stdev = 0;    // likewise
    double p1 = 0;
    double p95 = 0;
    double p99 = 0;
    double max = 0;
    size_t late = 0;     // samples > median + R/2: Sleep(1) missed its tick
    double latePct = 0;
    double score = 0;    // mean + stdev, the reference metric (avg + STDEV)
    bool clean = false;  // unimodal (p99 - p1 < R/2) and latePct within limit
};

double Percentile(const std::vector<double>& sorted, double q) {
    if (sorted.empty()) return 0;
    size_t idx = static_cast<size_t>(std::ceil(q * static_cast<double>(sorted.size())));
    if (idx == 0) idx = 1;
    if (idx > sorted.size()) idx = sorted.size();
    return sorted[idx - 1];
}

Stats ComputeStats(const std::vector<double>& samples, ULONG resolutionUnits,
                   double cleanLatePctLimit, size_t minSamples) {
    Stats s;
    s.n = samples.size();
    if (s.n < 2) return s;
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    s.median = Percentile(sorted, 0.5);
    s.p1 = Percentile(sorted, 0.01);
    s.p95 = Percentile(sorted, 0.95);
    s.p99 = Percentile(sorted, 0.99);
    s.max = sorted.back();
    const double tick = UnitsToMs(resolutionUnits);
    const double halfTick = tick / 2.0;
    // A missed tick costs exactly one extra tick; anything far beyond that is
    // an unrelated hiccup (DPC storm, page fault). Clip hiccups so one 15 ms
    // stall cannot decide the ranking, but keep every missed tick at full
    // weight: the flip *rate* is what we are optimising against.
    const double cap = s.median + 2.5 * tick;
    double sum = 0;
    for (double v : sorted) sum += std::min(v, cap);
    s.mean = sum / static_cast<double>(s.n);
    double variance = 0;
    for (double v : sorted) {
        const double w = std::min(v, cap);
        variance += (w - s.mean) * (w - s.mean);
        if (v > s.median + halfTick) ++s.late;
    }
    s.stdev = std::sqrt(variance / static_cast<double>(s.n - 1));
    s.latePct = 100.0 * static_cast<double>(s.late) / static_cast<double>(s.n);
    s.score = s.mean + s.stdev;
    s.clean = s.n >= minSamples && (s.p99 - s.p1) < halfTick && s.latePct <= cleanLatePctLimit;
    return s;
}

enum class SampleStatus { Ok, Interference, Failed };

struct Candidate {
    ULONG requested = 0;        // grid value we ask for (what gets installed)
    ULONG actual = 0;           // what the kernel is really running (NtQuery, after warm-up)
    // Two tick-aligned call patterns, both real-world:
    //  loop: back-to-back Sleep(1) - a frame limiter's "while (early) Sleep(1)"
    //        inner loop; the core idles ~1 ms between calls.
    //  idle: Sleep(1) after a 2-5 ms idle - the reference MeasureSleep pattern;
    //        the core reaches deeper C-states, so wake-up latency is larger.
    std::vector<double> loop;
    std::vector<double> idle;
    size_t dropped = 0;         // samples discarded: clock changed under us mid-run
    ULONG interferer = 0;       // another process held this finer resolution
    int skippedPasses = 0;
    Stats loopStats;
    Stats idleStats;
    double score = 0;           // (loop + idle) / 2
    bool clean = false;         // both patterns clean
    bool HasData() const { return !loop.empty() && !idle.empty(); }
    size_t Samples() const { return loop.size() + idle.size(); }
};

void Progress(const std::wstring& text);

bool FeedCancelled() { return g_feed && g_feed->Cancelled(); }

gui::StatsView ViewOf(const Stats& s) {
    gui::StatsView v;
    v.n = static_cast<unsigned>(s.n);
    v.median = s.median;
    v.mean = s.mean;
    v.stdev = s.stdev;
    v.p99 = s.p99;
    v.latePct = s.latePct;
    v.score = s.score;
    return v;
}

gui::CandidateView ViewOf(const Candidate& c) {
    gui::CandidateView v;
    v.requested = c.requested;
    v.hasData = c.HasData();
    v.skippedPasses = c.skippedPasses;
    v.interferer = c.interferer;
    v.loop = ViewOf(c.loopStats);
    v.idle = ViewOf(c.idleStats);
    v.score = c.score;
    v.clean = c.clean;
    return v;
}

void PublishSweep(const std::vector<Candidate>& sweep, int activeIdx, int bestIdx = -1,
                  int boundaryIdx = -1, int targetIdx = -1, unsigned margin = 0,
                  const std::vector<double>* smooth = nullptr, int pass = 0, int passes = 0,
                  int passPos = 0) {
    if (!g_feed) return;
    gui::SweepView view;
    view.items.reserve(sweep.size());
    for (const auto& c : sweep) view.items.push_back(ViewOf(c));
    if (smooth) view.smooth = *smooth;
    view.bestIdx = bestIdx;
    view.boundaryIdx = boundaryIdx;
    view.targetIdx = targetIdx;
    view.margin = margin;
    view.pass = pass;
    view.passes = passes;
    view.passPos = passPos;
    g_feed->Sweep(view, activeIdx);
}

void PublishVerify(const std::vector<Candidate>& verify, int active, unsigned target) {
    if (!g_feed) return;
    std::vector<gui::CandidateView> v;
    v.reserve(verify.size());
    for (const auto& c : verify) v.push_back(ViewOf(c));
    g_feed->Verify(v, active, target);
}

void ReportPhase(const std::wstring& title, const std::wstring& detail) {
    if (g_feed) g_feed->Phase(title, detail);
}

void Narrate(const std::wstring& title, const std::wstring& body) {
    if (g_feed) g_feed->Narrate(title, body);
}

void PublishPpm(const std::vector<gui::PpmStepView>& steps) {
    if (g_feed) g_feed->Ppm(steps);
}

bool g_ppmDwell = true;

void Dwell(DWORD ms) {
    if (!ms || !g_ppmDwell) return;
    const ULONGLONG until = GetTickCount64() + ms;
    while (GetTickCount64() < until) {
        if (FeedCancelled()) return;
        Sleep(40);
    }
}

void ReportProgress(double fraction, double elapsedS, double remainingS, const std::wstring& text) {
    if (g_feed) g_feed->Progress(fraction, elapsedS, remainingS);
    Progress(text);
}

// Time one Sleep(1); returns the overshoot in ms, or false if the system
// resolution was not `expected` afterwards (someone changed the clock).
bool TimeOneSleep(const LARGE_INTEGER& freq, ULONG expected, double& overshoot, bool idle = false) {
    LARGE_INTEGER start{}, end{};
    QueryPerformanceCounter(&start);
    Sleep(1);
    QueryPerformanceCounter(&end);
    overshoot = static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 /
                    static_cast<double>(freq.QuadPart) - 1.0;
    bool ok = true;
    if (expected != 0) {
        ULONG slowest = 0, fastest = 0, current = 0;
        ok = QueryResolutions(slowest, fastest, current) && current == expected;
    }
    if (ok && g_feed) g_feed->Sample(expected, overshoot, idle);
    return ok;
}

// Request `c.requested`, take `loopCount` back-to-back samples and
// `idleCount` idle-gap samples, release. `preexisting` is the system
// resolution before we asked for anything; if the clock still runs at a
// finer value than ours after warm-up, another process owns it and the
// candidate cannot be measured right now.
SampleStatus MeasureBlock(Candidate& c, int loopCount, int idleCount, ULONG preexisting) {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);

    ULONG returned = 0;
    if (NtSetTimerResolution(c.requested, TRUE, &returned) != 0) return SampleStatus::Failed;
    auto release = [&] {
        ULONG ignored = 0;
        NtSetTimerResolution(c.requested, FALSE, &ignored);
    };

    // Warm-up: the clock is being re-programmed; the first ticks are not
    // steady ("unexpected results without a delay" in the reference script).
    for (int i = 0; i < 5; ++i) Sleep(1);
    Sleep(20);

    // What is the clock really doing now? NtSetTimerResolution's own return
    // value is not trustworthy for this on current kernels (it can report a
    // stale or global value), so ask.
    ULONG slowest = 0, fastest = 0, current = 0;
    if (!QueryResolutions(slowest, fastest, current)) {
        release();
        return SampleStatus::Failed;
    }
    if (current != c.requested) {
        release();
        if (current < c.requested) {
            c.interferer = current;
            return SampleStatus::Interference;
        }
        return SampleStatus::Failed;  // our request did not take effect
    }
    c.actual = current;
    (void)preexisting;

    double v = 0;
    c.loop.reserve(c.loop.size() + static_cast<size_t>(loopCount));
    for (int i = 0; i < loopCount; ++i) {
        if (FeedCancelled()) {
            release();
            return SampleStatus::Failed;
        }
        if (TimeOneSleep(freq, c.requested, v, false)) c.loop.push_back(v);
        else ++c.dropped;
    }
    c.idle.reserve(c.idle.size() + static_cast<size_t>(idleCount));
    for (int i = 0; i < idleCount; ++i) {
        if (FeedCancelled()) {
            release();
            return SampleStatus::Failed;
        }
        // Sleep() returns on a clock tick, so the sample still starts just
        // after a tick; cycling 2-5 ms varies how deep the core idles.
        Sleep(static_cast<DWORD>(2 + (i & 3)));
        if (TimeOneSleep(freq, c.requested, v, true)) c.idle.push_back(v);
        else ++c.dropped;
    }

    release();
    return SampleStatus::Ok;
}

void ComputeCandidateStats(Candidate& c, double cleanLatePctLimit, size_t minLoopSamples) {
    const ULONG res = c.actual ? c.actual : c.requested;
    c.loopStats = ComputeStats(c.loop, res, cleanLatePctLimit, minLoopSamples);
    c.idleStats = ComputeStats(c.idle, res, cleanLatePctLimit, std::max<size_t>(minLoopSamples / 2, 2));
    c.score = (c.loopStats.score + c.idleStats.score) / 2.0;
    c.clean = c.loopStats.clean && c.idleStats.clean;
}

bool IsConsole(FILE* stream) {
    DWORD mode = 0;
    return GetConsoleMode(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream))), &mode) != 0;
}

// Console-only progress line (overwritten in place, never logged).
void Progress(const std::wstring& text) {
    static const bool console = IsConsole(stdout);
    if (!console) return;
    fwprintf(stdout, L"\r%-78.78s", text.c_str());
    fflush(stdout);
}

void ProgressEnd() {
    static const bool console = IsConsole(stdout);
    if (!console) return;
    fwprintf(stdout, L"\r%-78s\r", L"");
    fflush(stdout);
}

std::wstring FormatDuration(double seconds) {
    const int total = static_cast<int>(seconds + 0.5);
    return Format(L"%d:%02d", total / 60, total % 60);
}

// One table row. `note` is free text (best / boundary / selected).
void PrintRow(const Candidate& c, const std::wstring& note) {
    if (!c.HasData()) {
        Out(Format(L"  %8.4f  %6s  %s", UnitsToMs(c.requested), L"-",
                   c.interferer ? Format(L"skipped: another process held %.4f ms", UnitsToMs(c.interferer)).c_str()
                                : L"skipped"));
        return;
    }
    const Stats& l = c.loopStats;
    const Stats& d = c.idleStats;
    Out(Format(L"  %8.4f  %6lu  %+8.4f  %+8.4f  %6.4f  %5.1f%%  | %+8.4f  %5.1f%%  |  %6.4f  %s",
               UnitsToMs(c.requested), static_cast<unsigned long>(c.Samples()), l.median, l.mean, l.stdev,
               l.latePct, d.mean, d.latePct, c.score, note.c_str()));
}

void PrintTableHeader() {
    Out(L"            back-to-back Sleep(1) loop ------------------  after 2-5 ms idle --");
    Out(L"  res (ms)       n    median      mean   stdev  late%  |     mean  late%  |   score");
}

// ---------------------------------------------------------------- service

bool WaitForServiceState(SC_HANDLE svc, DWORD wanted, DWORD timeoutMs);

bool ServiceExistsNamed(const wchar_t* name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
    const bool exists = svc != nullptr;
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return exists;
}

bool ServiceExists() { return ServiceExistsNamed(kServiceName) || ServiceExistsNamed(kLegacyServiceName); }

bool StopServiceNamed(const wchar_t* name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return true;
    }
    SERVICE_STATUS status{};
    ControlService(svc, SERVICE_CONTROL_STOP, &status);
    const bool ok = WaitForServiceState(svc, SERVICE_STOPPED, 15000);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

bool DeleteServiceNamed(const wchar_t* name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, name, DELETE | SERVICE_QUERY_STATUS | SERVICE_STOP);
    if (!svc) {
        CloseServiceHandle(scm);
        return true;
    }
    SERVICE_STATUS status{};
    ControlService(svc, SERVICE_CONTROL_STOP, &status);
    WaitForServiceState(svc, SERVICE_STOPPED, 8000);
    const bool ok = DeleteService(svc) != 0 || GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE;
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

// Returns: 0 = not installed, 1 = running, 2 = stopped/other.
int ServiceState() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return 0;
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS);
    if (!svc) svc = OpenServiceW(scm, kLegacyServiceName, SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return 0;
    }
    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    int state = 2;
    if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                             reinterpret_cast<BYTE*>(&status), sizeof(status), &needed) &&
        status.dwCurrentState == SERVICE_RUNNING) {
        state = 1;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return state;
}

bool WaitForServiceState(SC_HANDLE svc, DWORD wanted, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    do {
        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) {
            return false;
        }
        if (status.dwCurrentState == wanted) return true;
        Sleep(200);
    } while (GetTickCount64() < deadline);
    return false;
}

bool StopInstalledService() {
    const bool a = StopServiceNamed(kServiceName);
    const bool b = StopServiceNamed(kLegacyServiceName);
    return a && b;
}

bool StartServiceNamed(const wchar_t* name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return false;
    }
    const BOOL started = StartServiceW(svc, 0, nullptr);
    const DWORD rc = GetLastError();
    bool ok = started || rc == ERROR_SERVICE_ALREADY_RUNNING;
    if (ok) ok = WaitForServiceState(svc, SERVICE_RUNNING, 15000);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

// ---- service process side (runs as SYSTEM under the SCM) ----

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
HANDLE g_stopEvent = nullptr;
ULONG g_serviceResolution = 0;

void ReportServiceStatus(DWORD state, DWORD exitCode = 0) {
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted =
        (state == SERVICE_RUNNING) ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
    status.dwWin32ExitCode = exitCode;
    status.dwCheckPoint = (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) ? 1 : 0;
    status.dwWaitHint = 5000;
    if (g_statusHandle) SetServiceStatus(g_statusHandle, &status);
}

DWORD WINAPI ServiceCtrlHandler(DWORD control, DWORD, LPVOID, LPVOID) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            ReportServiceStatus(SERVICE_STOP_PENDING);
            if (g_stopEvent) SetEvent(g_stopEvent);
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ServiceCtrlHandler, nullptr);
    if (!g_statusHandle) return;
    ReportServiceStatus(SERVICE_START_PENDING);
    LogLine(L"service: starting");

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    // Resolution chosen at install time; fall back to the fastest the
    // kernel offers if the value is missing/corrupt.
    DWORD desired = 0;
    ULONG slowest = 0, fastest = 0, current = 0;
    QueryResolutions(slowest, fastest, current);
    if (RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"DesiredResolution", desired) && desired != 0) {
        g_serviceResolution = desired;
    } else if (RegGetDword(HKEY_LOCAL_MACHINE, kLegacyAppRegKey, L"DesiredResolution", desired) && desired != 0) {
        g_serviceResolution = desired;
    } else {
        g_serviceResolution = fastest;
    }
    if (g_serviceResolution < fastest) g_serviceResolution = fastest;
    if (slowest != 0 && g_serviceResolution > slowest) g_serviceResolution = slowest;

    DisablePowerThrottling();

    // NtSetTimerResolution's returned "current" value is unreliable on
    // recent kernels (it reports stale or system-wide values), so never
    // adopt it; read back what the kernel is really using with
    // NtQueryTimerResolution instead.
    RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"AppliedResolution", 0);
    ULONG ignored = 0;
    if (NtSetTimerResolution(g_serviceResolution, TRUE, &ignored) != 0) {
        LogLine(L"service: NtSetTimerResolution failed");
        ReportServiceStatus(SERVICE_STOPPED, 1);
        return;
    }
    Sleep(5);
    QueryResolutions(slowest, fastest, current);
    RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"AppliedResolution", current);
    LogLine(Format(L"service: holding resolution %lu (%.4f ms); kernel reports %.4f ms", g_serviceResolution,
                   UnitsToMs(g_serviceResolution), UnitsToMs(current)));
    ReportServiceStatus(SERVICE_RUNNING);

    // Hold the request for the life of the process. Re-assert every 30 s:
    // cheap insurance against anything releasing/altering the effective
    // resolution, and it refreshes the AppliedResolution heartbeat that
    // --status and the installer poll.
    while (WaitForSingleObject(g_stopEvent, 30000) == WAIT_TIMEOUT) {
        NtSetTimerResolution(g_serviceResolution, TRUE, &ignored);
        Sleep(5);
        QueryResolutions(slowest, fastest, current);
        RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"AppliedResolution", current);
    }

    NtSetTimerResolution(g_serviceResolution, FALSE, &ignored);
    RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"AppliedResolution", 0);
    LogLine(L"service: stopped, resolution released");
    ReportServiceStatus(SERVICE_STOPPED);
}

bool RunServiceDispatcher() {
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), ServiceMain},
        {nullptr, nullptr},
    };
    return StartServiceCtrlDispatcherW(table) != 0;
}

// ---- install / uninstall ----

// Install (or update) the service to hold `desired` (100-ns units).
// Caller must be elevated. Returns true only if the service is RUNNING and
// has confirmed (via the AppliedResolution registry heartbeat) that it is
// holding the requested resolution.
bool InstallService(ULONG desired) {
    ReportPhase(L"INSTALL", Format(L"holding %.4f ms from every boot", UnitsToMs(desired)));
    Narrate(L"Installing the auto-start service",
            Format(L"Registers a Windows service that requests %.4f ms at every boot. "
                   L"GlobalTimerResolutionRequests is set so other processes use the tighter Sleep/wait "
                   L"timers after the next restart.",
                   UnitsToMs(desired)));

    const std::wstring exePath = GetExePath();
    const std::wstring installDir = GetInstallDir();
    const std::wstring installPath = GetInstallPath();

    // Stop any previous instance before touching its binary/config.
    if (ServiceExists()) {
        Out(L"Stopping previously installed service...");
        StopInstalledService();
    }
    if (ServiceExistsNamed(kLegacyServiceName)) {
        Out(L"Removing the previous TimerResolution service (now Synchronus)...");
        DeleteServiceNamed(kLegacyServiceName);
    }

    // Copy ourselves to a permanent home: a service must never point into
    // Downloads/Desktop/temp where the .exe can be moved or deleted.
    if (!EqualPath(exePath, installPath)) {
        CreateDirectoryW(installDir.c_str(), nullptr);
        Out(Format(L"Copying executable to %s", installPath.c_str()));
        if (!CopyFileW(exePath.c_str(), installPath.c_str(), FALSE)) {
            Err(Format(L"CopyFile failed (error %lu). Is the old service still running?", GetLastError()));
            return false;
        }
    }

    // Persist the choice (the service reads DesiredResolution at start).
    RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"DesiredResolution", desired);
    RegSetString(HKEY_LOCAL_MACHINE, kAppRegKey, L"InstallPath", installPath);

    // Make the request effective system-wide, not just inside the service
    // process (Windows 11 / Server 2022+; read by the kernel at boot).
    // Remember the prior state so --uninstall can restore it exactly.
    DWORD previous = 0;
    const bool hadPrevious =
        RegGetDword(HKEY_LOCAL_MACHINE, kKernelRegKey, kGlobalFlagName, previous);
    DWORD alreadyRecorded = 0;
    if (!RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"GlobalFlagSaved", alreadyRecorded)) {
        RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"GlobalFlagSaved", 1);
        RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"GlobalFlagHadValue", hadPrevious ? 1 : 0);
        RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"GlobalFlagPrevious", hadPrevious ? previous : 0);
    }
    if (!hadPrevious || previous != 1) {
        RegSetDword(HKEY_LOCAL_MACHINE, kKernelRegKey, kGlobalFlagName, 1);
        Out(L"Set GlobalTimerResolutionRequests=1 (system-wide requests; "
            L"takes full effect after the next reboot).");
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        Err(L"OpenSCManager failed.");
        return false;
    }
    const std::wstring imagePath = L"\"" + installPath + L"\" --service";
    SC_HANDLE svc = CreateServiceW(
        scm, kServiceName, kServiceDisplayName, SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        imagePath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc && GetLastError() == ERROR_SERVICE_EXISTS) {
        svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
        if (svc) {
            ChangeServiceConfigW(svc, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                                 SERVICE_ERROR_NORMAL, imagePath.c_str(), nullptr,
                                 nullptr, nullptr, nullptr, nullptr, kServiceDisplayName);
        }
    }
    if (!svc) {
        Err(Format(L"CreateService failed (error %lu).", GetLastError()));
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_DESCRIPTIONW description{const_cast<LPWSTR>(kServiceDescription)};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &description);

    // Restart automatically if the service process ever dies.
    SC_ACTION actions[] = {
        {SC_ACTION_RESTART, 5000},
        {SC_ACTION_RESTART, 5000},
        {SC_ACTION_RESTART, 60000},
    };
    SERVICE_FAILURE_ACTIONSW failure{};
    failure.dwResetPeriod = 86400;
    failure.cActions = _countof(actions);
    failure.lpsaActions = actions;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

    bool ok = true;
    Out(L"Starting service...");
    if (!StartServiceW(svc, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        Err(Format(L"StartService failed (error %lu).", GetLastError()));
        ok = false;
    } else if (!WaitForServiceState(svc, SERVICE_RUNNING, 15000)) {
        Err(L"Service did not reach RUNNING state.");
        ok = false;
    }

    // Confirm the kernel really runs at the requested resolution (or finer,
    // if some other process currently asks for even more).
    if (ok) {
        DWORD applied = 0;
        bool confirmed = false;
        const ULONGLONG deadline = GetTickCount64() + 10000;
        while (GetTickCount64() < deadline) {
            if (RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"AppliedResolution", applied) && applied != 0 &&
                applied <= desired) {
                confirmed = true;
                break;
            }
            Sleep(200);
        }
        if (!confirmed) {
            Err(Format(L"Service is running but the kernel reports %.4f ms instead of %.4f ms (see the log).",
                       UnitsToMs(applied), UnitsToMs(desired)));
            ok = false;
        } else if (applied != desired) {
            Out(Format(L"Note: the kernel currently reports %.4f ms because another process asks for an even "
                       L"finer resolution; the service's %.4f ms request is in place.",
                       UnitsToMs(applied), UnitsToMs(desired)));
        }
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

// ---- PPM (processor power management) ------------------------------------
//
// PC-Tuning (amitxv / valleyofdoom): ntoskrnl schedules periodic DPCs to
// re-evaluate P-states and parked cores. With a *static* CPU frequency and
// core parking off those checks are noise, so raising PERFCHECK to 5000 ms
// (the maximum) drops them by orders of magnitude. On a turbo/PBO CPU the
// same change delays boost, so we only recommend it when frequency does not
// actually scale.

struct ProcessorPowerInformation {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
};

const GUID kSubProcessor = {0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
const GUID kSubPciExpress = {0x501a4d13, 0x42af, 0x4429, {0x9f, 0xd1, 0xa8, 0x21, 0xf2, 0xbb, 0x0b, 0x89}};
const GUID kSubDisk = {0x0012ee47, 0x9041, 0x4b5d, {0x9b, 0x77, 0x53, 0x5f, 0xba, 0x8b, 0x14, 0x42}};
const GUID kSubUsb = {0x2a737441, 0x1930, 0x4402, {0x8d, 0x77, 0xb2, 0xbe, 0xbb, 0xa3, 0x08, 0xa3}};

struct PpmTweak {
    const wchar_t* alias;
    const wchar_t* title;
    const wchar_t* unit;
    const wchar_t* why;
    DWORD target;
    DWORD dwellMs;
    GUID setting;
    GUID subgroup = kSubProcessor;
};

const PpmTweak kPpmTweaks[] = {
    {L"CPMINCORES", L"core parking min cores", L"%",
     L"Windows parks idle cores to save power. Unparking them later costs a DPC and a cache-cold wake. "
     L"Forcing 100% min cores keeps every logical processor live so a frame or note never waits on a parked core.",
     100, 1800, {0x0cc5b647, 0xc1df, 0x4637, {0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x83}}},
    {L"CPMAXCORES", L"core parking max cores", L"%",
     L"The matching ceiling. 100% means Windows is not allowed to park any core, even under a light load.",
     100, 1600, {0xea062031, 0x0e34, 0x4ff1, {0x9b, 0x6d, 0xeb, 0x10, 0x59, 0x33, 0x40, 0x28}}},
    {L"CPMINCORES1", L"core parking min cores (class 1)", L"%",
     L"The same parking floor for efficiency-class cores (E-cores on hybrid CPUs). Pinning them too means the "
     L"scheduler will not hide half the package behind a park DPC.",
     100, 1600, {0x0cc5b647, 0xc1df, 0x4637, {0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x84}}},
    {L"CPMAXCORES1", L"core parking max cores (class 1)", L"%",
     L"Ceiling for class-1 cores. Written even on non-hybrid CPUs; Windows ignores it if the class does not exist.",
     100, 1600, {0xea062031, 0x0e34, 0x4ff1, {0x9b, 0x6d, 0xeb, 0x10, 0x59, 0x33, 0x40, 0x29}}},
    {L"PERFCHECK", L"performance time check interval", L"ms",
     L"ntoskrnl schedules a periodic DPC to re-evaluate P-states and parked cores. Default is ~15 ms \u2014 "
     L"hundreds of extra kernel wake-ups a second. On a locked multiplier those checks cannot change frequency, "
     L"so stretching this to 5000 ms (the maximum) removes that background noise.",
     5000, 2200, {0x4d2b0152, 0x7d5c, 0x498b, {0x88, 0xe2, 0x34, 0x34, 0x53, 0x92, 0xa2, 0xc5}}},
    {L"IDLEDEMOTE", L"idle demote threshold", L"%",
     L"How idle the CPU must be before Windows drops a P-state. 95% means it only down-clocks when the package "
     L"is almost completely unused, so a brief lull in a game cannot yank frequency down under you.",
     95, 1600, {0x4b92d758, 0x5a24, 0x4851, {0xa4, 0x70, 0x81, 0x5d, 0x78, 0xae, 0xe1, 0x19}}},
    {L"IDLEPROMOTE", L"idle promote threshold", L"%",
     L"The matching climb-back threshold. Paired with demote at 95%, the idle governor stops fidgeting and "
     L"leaves a static clock alone.",
     95, 1600, {0x7b224883, 0xb3cc, 0x4d79, {0x81, 0x9f, 0x83, 0x74, 0x15, 0x2c, 0xbe, 0x7c}}},
    {L"PERFAUTONOMOUS", L"processor autonomous mode", L"",
     L"Autonomous / CPPC2 lets the CPU hardware pick its own P-states. Disabling it puts the OS in charge, "
     L"which is what we want once frequency is locked \u2014 hardware boost logic has nothing useful to do.",
     0, 1600, {0x8baa4a8a, 0x14c6, 0x4451, {0x8e, 0x8b, 0x14, 0xbd, 0xbd, 0x19, 0x75, 0x37}}},
    {L"PROCTHROTTLEMIN", L"minimum processor state", L"%",
     L"The floor of the Windows P-state range. 100% tells the kernel not to throttle below the current "
     L"multiplier, so SpeedStep cannot sneak the clock down between loads.",
     100, 1600, {0x893dee8e, 0x2bef, 0x41e0, {0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4c}}},
    {L"PROCTHROTTLEMAX", L"maximum processor state", L"%",
     L"The ceiling of that range. Held at 100% so nothing in the power plan can cap the locked multiplier.",
     100, 1600, {0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec}}},
    {L"PROCTHROTTLEMIN1", L"minimum processor state (class 1)", L"%",
     L"P-state floor for efficiency-class cores. Same 100% pin so hybrid packages cannot park E-cores in a slow bin.",
     100, 1500, {0x893dee8e, 0x2bef, 0x41e0, {0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4d}}},
    {L"PROCTHROTTLEMAX1", L"maximum processor state (class 1)", L"%",
     L"P-state ceiling for class-1 cores. Written on every machine; ignored if the class is absent.",
     100, 1500, {0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xed}}},
    {L"PERFBOOSTPOL", L"processor performance boost policy", L"%",
     L"How aggressively Windows asks for turbo/boost. 100% is opportunistic max \u2014 on a locked multiplier this "
     L"is a no-op, on a boosting CPU it prefers the high bin when work appears.",
     100, 1600, {0x45bcc044, 0xd885, 0x43e2, {0x86, 0x05, 0xee, 0x0e, 0xc6, 0xe9, 0x6b, 0x59}}},
    {L"PERFBOOSTMODE", L"processor performance boost mode", L"",
     L"0 = disabled. On a locked multiplier the OS boost governor has nothing to do; leaving it on still "
     L"wakes a periodic DPC. Off is the static-clock setting from the tuning guide.",
     0, 1600, {0xbe337238, 0x0d82, 0x4146, {0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7}}},
    {L"PERFEPP", L"energy performance preference", L"%",
     L"0 = performance, 100 = energy. Pinning 0 tells CPPC to prefer clocks over joules, which is what a gaming "
     L"pass wants.",
     0, 1500, {0x36687f9e, 0xe3a5, 0x4dbf, {0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0xdc, 0x86}}},
    {L"PERFEPP1", L"energy performance preference (class 1)", L"%",
     L"The same 0% (performance) bias for efficiency-class cores.",
     0, 1500, {0x36687f9e, 0xe3a5, 0x4dbf, {0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0xdc, 0x87}}},
    {L"LATENCYHINTPERF", L"latency hint processor performance", L"%",
     L"When a multimedia/game latency hint fires, jump to this P-state immediately instead of ramping. 100% "
     L"means a frame start does not wait for the governor.",
     100, 1500, {0x619b7505, 0x003b, 0x4e82, {0xb7, 0xa6, 0x4d, 0xd2, 0x9c, 0x30, 0x09, 0x71}}},
    {L"PERFDUTYCYCLING", L"processor duty cycling", L"",
     L"Duty cycling PWM-throttles a core by rapidly gating it. That is extra jitter. Off (0) keeps the clock "
     L"a clock.",
     0, 1500, {0x4e4450b3, 0x6179, 0x4c19, {0x8e, 0x8b, 0x24, 0xad, 0x11, 0x6f, 0xd4, 0x35}}},
    {L"PERFINCTHRESHOLD", L"performance increase threshold", L"%",
     L"How busy a core must be before Windows raises the P-state. 85% (the guide value) stops the governor "
     L"from climbing on brief spikes, which pairs with a locked clock: fewer P-state DPCs.",
     85, 1500, {0x06cadf0e, 0x64ed, 0x448a, {0x89, 0x27, 0xce, 0x7b, 0xf9, 0x0e, 0xb3, 0x5d}}},
    {L"PERFINCTHRESHOLD1", L"performance increase threshold (class 1)", L"%",
     L"The same 95% climb threshold for efficiency-class cores, matching the guide's PERFINCTHRESHOLD1.",
     95, 1400, {0x06cadf0e, 0x64ed, 0x448a, {0x89, 0x27, 0xce, 0x7b, 0xf9, 0x0e, 0xb3, 0x5e}}},
    {L"PERFINCPOL", L"performance increase policy", L"",
     L"1 = Single. The kernel steps one P-state at a time instead of jumping (Rocket). With boost off this "
     L"is mostly documentation; written so the plan matches the guide exactly.",
     1, 1400, {0x465e1f50, 0xb1da, 0x4e66, {0xaa, 0x78, 0xd0, 0x1b, 0xa8, 0xd9, 0x93, 0xc5}}},
    {L"PERFDECPOL", L"performance decrease policy", L"",
     L"1 = Single. Symmetric with the increase policy so the governor does not slam the clock down in one jump.",
     1, 1400, {0x40fbefc0, 0x2e9d, 0x4d25, {0xa8, 0x5c, 0x18, 0x32, 0x6e, 0x80, 0x6d, 0x24}}},
};

const PpmTweak kStorageTweaks[] = {
    {L"ASPM", L"PCI Express link-state power management", L"",
     L"ASPM L0s/L1 adds a wake latency on every PCIe transaction. Off keeps the GPU and NVMe link fully awake "
     L"so a frame submit does not wait on a sleeping lane.",
     0, 1600, {0xee12f906, 0xd277, 0x404b, {0xb6, 0xda, 0xe5, 0xfa, 0x1a, 0x57, 0x6d, 0xf5}}, kSubPciExpress},
    {L"DISKIDLE", L"turn off hard disk after", L"s",
     L"0 = never. Spinning a disk down (or parking an SSD's internal idle) to save a few watts is a hitch "
     L"the next time you touch the pagefile or a shader cache.",
     0, 1500, {0x6738e2c4, 0xe8a5, 0x4a42, {0xb1, 0x6a, 0xe0, 0x40, 0xe7, 0x69, 0x75, 0x6e}}, kSubDisk},
    {L"USBSUSPEND", L"USB selective suspend", L"",
     L"Selective suspend powers down USB devices between polls. Mice, keyboards and DACs then take a frame "
     L"to wake, which shows up as polling spikes. Off keeps the controller live.",
     0, 1600, {0x48e6b7a6, 0x50f5, 0x4782, {0xa5, 0xd4, 0x53, 0xbb, 0x8f, 0x07, 0xe2, 0x26}}, kSubUsb},
};

std::wstring CpuBrand() {
    int lim[4] = {};
    __cpuid(lim, 0x80000000);
    if (static_cast<unsigned>(lim[0]) < 0x80000004) return L"CPU";
    int regs[12] = {};
    __cpuid(&regs[0], 0x80000002);
    __cpuid(&regs[4], 0x80000003);
    __cpuid(&regs[8], 0x80000004);
    char brand[49] = {};
    memcpy(brand, regs, 48);
    std::wstring out;
    bool skipSpace = true;
    for (char c : brand) {
        if (!c) break;
        if (c == ' ' && skipSpace) continue;
        skipSpace = false;
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out.empty() ? L"CPU" : out;
}

bool CpuIsHybrid() {
    using Fn = BOOL(WINAPI*)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
    const auto getInfo = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetSystemCpuSetInformation"));
    if (!getInfo) return false;
    ULONG len = 0;
    getInfo(nullptr, 0, &len, GetCurrentProcess(), 0);
    if (!len) return false;
    std::vector<char> buf(len);
    if (!getInfo(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data()), len, &len, GetCurrentProcess(), 0)) {
        return false;
    }
    DWORD offset = 0;
    while (offset < len) {
        auto* info = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buf.data() + offset);
        if (info->Size == 0) break;
        if (info->Type == CpuSetInformation && info->CpuSet.EfficiencyClass != 0) return true;
        offset += info->Size;
    }
    return false;
}

bool ReadPowerIndex(GUID* scheme, const GUID& sub, const GUID& setting, bool ac, DWORD& value) {
    const DWORD rc = ac ? PowerReadACValueIndex(nullptr, scheme, const_cast<GUID*>(&sub), const_cast<GUID*>(&setting), &value)
                        : PowerReadDCValueIndex(nullptr, scheme, const_cast<GUID*>(&sub), const_cast<GUID*>(&setting), &value);
    return rc == ERROR_SUCCESS;
}

bool WritePowerIndex(GUID* scheme, const GUID& sub, const GUID& setting, bool ac, DWORD value) {
    const DWORD rc = ac ? PowerWriteACValueIndex(nullptr, scheme, const_cast<GUID*>(&sub), const_cast<GUID*>(&setting), value)
                        : PowerWriteDCValueIndex(nullptr, scheme, const_cast<GUID*>(&sub), const_cast<GUID*>(&setting), value);
    return rc == ERROR_SUCCESS;
}

DWORD CurrentPerfCheckMs() {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return 0;
    DWORD value = 0;
    bool ok = false;
    for (const auto& t : kPpmTweaks) {
        if (wcscmp(t.alias, L"PERFCHECK") == 0) {
            ok = ReadPowerIndex(scheme, t.subgroup, t.setting, true, value);
            break;
        }
    }
    LocalFree(scheme);
    return ok ? value : 0;
}

gui::CpuProbeView ProbeCpu() {
    gui::CpuProbeView p;
    p.cpuName = CpuBrand();
    p.hybrid = CpuIsHybrid();
    p.perfCheckMs = CurrentPerfCheckMs();
    DWORD applied = 0;
    p.alreadyApplied = RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"PpmApplied", applied) && applied != 0;

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    p.nLogical = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
    std::vector<ProcessorPowerInformation> info(p.nLogical);
    auto query = [&]() -> bool {
        return CallNtPowerInformation(ProcessorInformation, nullptr, 0, info.data(),
                                      static_cast<ULONG>(info.size() * sizeof(ProcessorPowerInformation))) == 0;
    };

    Sleep(120);
    if (!query()) {
        p.mode = gui::CpuProbeView::Mode::Unknown;
        p.reason = L"CallNtPowerInformation(ProcessorInformation) failed";
        return p;
    }
    unsigned idleMin = 0xFFFFFFFFu, idleMax = 0, cap = 0;
    for (const auto& c : info) {
        idleMin = std::min(idleMin, static_cast<unsigned>(c.CurrentMhz));
        idleMax = std::max(idleMax, static_cast<unsigned>(c.CurrentMhz));
        cap = std::max(cap, static_cast<unsigned>(c.MaxMhz));
    }
    p.idleMhz = idleMin;
    p.maxMhz = cap;

    const ULONGLONG busyUntil = GetTickCount64() + 90;
    volatile unsigned sink = 0;
    while (GetTickCount64() < busyUntil) sink += 1;
    (void)sink;
    if (!query()) {
        p.mode = gui::CpuProbeView::Mode::Unknown;
        p.reason = L"frequency query failed after the load probe";
        return p;
    }
    unsigned busyMax = 0;
    for (const auto& c : info) {
        busyMax = std::max(busyMax, static_cast<unsigned>(c.CurrentMhz));
        cap = std::max(cap, static_cast<unsigned>(c.MaxMhz));
    }
    p.busyMhz = busyMax;
    p.maxMhz = cap;

    const unsigned swing = (busyMax > idleMin) ? busyMax - idleMin : 0;
    const bool atCap = cap != 0 && idleMin + 50 >= cap && busyMax + 50 >= cap;
    if (swing > 80) {
        p.mode = gui::CpuProbeView::Mode::Dynamic;
        p.reason = Format(L"frequency scaled %u -> %u MHz under load (turbo/PBO/SpeedStep is active)", idleMin,
                          busyMax);
    } else if (atCap) {
        p.mode = gui::CpuProbeView::Mode::Locked;
        p.reason = Format(L"frequency held at %u MHz idle and busy (static / locked multiplier)", busyMax);
    } else if (cap && idleMax + 50 < cap && swing <= 80) {
        // Parked below max but didn't climb in 90 ms: still treat as dynamic so
        // we do not delay a slow boost.
        p.mode = gui::CpuProbeView::Mode::Dynamic;
        p.reason = Format(L"running at %u MHz with headroom to %u MHz; not treating as locked", idleMin, cap);
    } else {
        p.mode = gui::CpuProbeView::Mode::Unknown;
        p.reason = Format(L"idle %u MHz, busy %u MHz, max %u MHz — could not classify", idleMin, busyMax, cap);
    }
    if (p.hybrid) p.reason += L" (hybrid P+E CPU)";
    return p;
}

void PrintCpuProbe(const gui::CpuProbeView& p) {
    Out(Format(L"CPU: %s  (%u logical processors%s)", p.cpuName.c_str(), p.nLogical,
               p.hybrid ? L", hybrid" : L""));
    Out(Format(L"  frequency: idle %u MHz, busy %u MHz, cap %u MHz", p.idleMhz, p.busyMhz, p.maxMhz));
    if (p.perfCheckMs) Out(Format(L"  current processor performance time check interval: %u ms", p.perfCheckMs));
    const wchar_t* label = p.mode == gui::CpuProbeView::Mode::Locked ? L"LOCKED / static"
                          : p.mode == gui::CpuProbeView::Mode::Dynamic ? L"DYNAMIC (turbo/PBO)"
                                                                      : L"UNCLEAR";
    Out(Format(L"  classified: %s — %s", label, p.reason.c_str()));
}

void UnhidePowerSetting(const GUID& sub, const GUID& setting) {
    PowerWriteSettingAttributes(const_cast<GUID*>(&sub), const_cast<GUID*>(&setting), 0);
    const std::wstring key = Format(L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerSettings\\%s\\%s",
                                    GuidBare(sub).c_str(), GuidBare(setting).c_str());
    DWORD attr = 0;
    if (RegGetDword(HKEY_LOCAL_MACHINE, key.c_str(), L"Attributes", attr)) {
        RegSetDword(HKEY_LOCAL_MACHINE, key.c_str(), L"Attributes", attr & ~1u);
    } else {
        RegSetDword(HKEY_LOCAL_MACHINE, key.c_str(), L"Attributes", 2);
    }
    PowerCfg(Format(L"/attributes %s %s -ATTRIB_HIDE", GuidBare(sub).c_str(), GuidBare(setting).c_str()));
}

bool PowerCfgWriteIndex(const GUID& sub, const GUID& setting, DWORD value, bool ac) {
    return PowerCfg(Format(L"/%s SCHEME_CURRENT %s %s %lu", ac ? L"setacvalueindex" : L"setdcvalueindex",
                           GuidBare(sub).c_str(), GuidBare(setting).c_str(), value)) == 0;
}

std::wstring FormatPpmChange(const PpmTweak& t, DWORD from, bool had) {
    if (!had) {
        if (t.unit[0] == L'\0') return Format(L"(hidden/unread)  \u2192  %lu", t.target);
        return Format(L"(hidden/unread)  \u2192  %lu%s", t.target, t.unit);
    }
    if (t.unit[0] == L'\0') return Format(L"%lu  \u2192  %lu", from, t.target);
    return Format(L"%lu%s  \u2192  %lu%s", from, t.unit, t.target, t.unit);
}

bool ActivateUltimatePerformance(std::wstring& detail) {
    const wchar_t* kUltimate = L"e9a42b02-d5df-448d-aa00-03a2192483d7";
    const wchar_t* kHigh = L"8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c";
    if (PowerCfg(Format(L"/setactive %s", kUltimate)) == 0) {
        detail = L"Ultimate Performance is now the active plan";
        return true;
    }
    PowerCfg(Format(L"/duplicatescheme %s", kUltimate));
    if (PowerCfg(Format(L"/setactive %s", kUltimate)) == 0) {
        detail = L"duplicated Ultimate Performance and set it active";
        return true;
    }
    if (PowerCfg(Format(L"/setactive %s", kHigh)) == 0) {
        detail = L"Ultimate Performance is not on this SKU; High Performance is active instead";
        return true;
    }
    detail = L"could not switch the Windows power plan";
    return false;
}

bool ApplyPpmTweaks() {
    GUID* previous = nullptr;
    if (PowerGetActiveScheme(nullptr, &previous) == ERROR_SUCCESS && previous) {
        wchar_t guidText[64] = {};
        if (StringFromGUID2(*previous, guidText, _countof(guidText)) > 0) {
            RegSetString(HKEY_LOCAL_MACHINE, kAppRegKey, L"PpmPreviousScheme", guidText);
        }
        LocalFree(previous);
    }

    Narrate(L"CPU Tune",
            L"Switch to Ultimate Performance, then write every Processor Power Management value from the "
            L"tuning guide: parking off, min/max 100%, PERFCHECK 5000 ms, boost mode off, increase "
            L"threshold 85/95, increase/decrease policy Single, autonomous off. Threaded DPCs are "
            L"disabled in the kernel so those periodic checks stop stealing slices from the game thread.");
    ReportPhase(L"CPU TUNE", L"Ultimate Performance, PPM, ThreadDpcEnable");
    Out(L"Applying CPU Tune: Ultimate Performance power plan, then every PPM setting, then ThreadDpcEnable=0.");

    std::vector<gui::PpmStepView> steps;
    steps.reserve(_countof(kPpmTweaks) + 2);
    {
        gui::PpmStepView s;
        s.alias = L"ULTIMATE";
        s.title = L"Ultimate Performance power plan";
        s.why = L"The Ultimate Performance plan (or High Performance if this SKU lacks it) is the base the "
                L"guide writes PPM values onto. Balanced will keep re-parking cores underneath our settings.";
        s.change = L"SCHEME_CURRENT  \u2192  Ultimate Performance";
        steps.push_back(std::move(s));
    }
    for (const auto& t : kPpmTweaks) {
        gui::PpmStepView s;
        s.alias = t.alias;
        s.title = t.title;
        s.why = t.why;
        s.status = 0;
        steps.push_back(std::move(s));
    }
    {
        gui::PpmStepView s;
        s.alias = L"THREADDPC";
        s.title = L"threaded DPCs";
        s.why = L"ThreadDpcEnable lets the kernel run DPCs on extra threads, which is more throughput and "
                L"more jitter. 0 forces DPCs onto the interrupting CPU so a frame is not preempted by a "
                L"park/P-state DPC on another core. Takes effect after the next reboot.";
        s.change = L"ThreadDpcEnable  \u2192  0";
        steps.push_back(std::move(s));
    }
    PublishPpm(steps);
    Dwell(1200);

    steps[0].status = 1;
    PublishPpm(steps);
    Narrate(steps[0].title, steps[0].why);
    Dwell(1600);
    std::wstring planDetail;
    const bool planOk = ActivateUltimatePerformance(planDetail);
    steps[0].change = planDetail;
    steps[0].status = planOk ? 2 : 3;
    PublishPpm(steps);
    Out(Format(L"  power plan: %s", planDetail.c_str()));
    Dwell(600);

    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) {
        Err(L"PowerGetActiveScheme failed; cannot apply PPM tweaks.");
        return false;
    }
    wchar_t guidText[64] = {};
    if (StringFromGUID2(*scheme, guidText, _countof(guidText)) > 0) {
        RegSetString(HKEY_LOCAL_MACHINE, kAppRegKey, L"PpmScheme", guidText);
    }

    int wrote = 0, failed = 0;
    for (size_t i = 0; i < _countof(kPpmTweaks); ++i) {
        if (FeedCancelled()) {
            LocalFree(scheme);
            return false;
        }
        const auto& t = kPpmTweaks[i];
        const size_t si = i + 1;
        UnhidePowerSetting(t.subgroup, t.setting);
        DWORD ac = 0, dc = 0;
        const bool hadAc = ReadPowerIndex(scheme, t.subgroup, t.setting, true, ac);
        const bool hadDc = ReadPowerIndex(scheme, t.subgroup, t.setting, false, dc);
        const DWORD from = hadAc ? ac : dc;
        steps[si].status = 1;
        steps[si].change = FormatPpmChange(t, from, hadAc || hadDc);
        PublishPpm(steps);
        Narrate(t.title, t.why);
        ReportPhase(L"CPU TUNE", Format(L"%s  %s", t.alias, steps[si].change.c_str()));

        Out(L"");
        Out(Format(L"  %s (%s)", t.title, t.alias));
        Out(Format(L"    %s", t.why));
        Out(Format(L"    %s", steps[si].change.c_str()));

        Dwell(t.dwellMs);
        if (FeedCancelled()) {
            LocalFree(scheme);
            return false;
        }

        if (hadAc) {
            RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Ppm_%s_ac", t.alias).c_str(), ac);
            RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Ppm_%s_hadAc", t.alias).c_str(), 1);
        }
        if (hadDc) {
            RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Ppm_%s_dc", t.alias).c_str(), dc);
            RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Ppm_%s_hadDc", t.alias).c_str(), 1);
        }

        bool acOk = WritePowerIndex(scheme, t.subgroup, t.setting, true, t.target) ||
                    PowerCfgWriteIndex(t.subgroup, t.setting, t.target, true);
        bool dcOk = WritePowerIndex(scheme, t.subgroup, t.setting, false, t.target) ||
                    PowerCfgWriteIndex(t.subgroup, t.setting, t.target, false);
        DWORD confirm = 0;
        const bool confirmed = ReadPowerIndex(scheme, t.subgroup, t.setting, true, confirm) && confirm == t.target;
        if (!acOk && !dcOk && !confirmed) {
            Err(Format(L"  write failed for %s (API and powercfg). Continuing with the rest.", t.alias));
            steps[si].status = 3;
            steps[si].change = Format(L"%s  (write failed)", steps[si].change.c_str());
            PublishPpm(steps);
            ++failed;
            Dwell(600);
            continue;
        }
        Out(Format(L"    written%s.", confirmed ? Format(L" (confirmed %lu)", confirm).c_str() : L""));
        steps[si].status = 2;
        if (confirmed) steps[si].change = FormatPpmChange(t, from, hadAc || hadDc);
        PublishPpm(steps);
        ++wrote;
        Dwell(500);
    }

    PowerSetActiveScheme(nullptr, scheme);
    PowerCfg(L"/setactive SCHEME_CURRENT");
    LocalFree(scheme);

    {
        const size_t ti = steps.size() - 1;
        steps[ti].status = 1;
        PublishPpm(steps);
        Narrate(steps[ti].title, steps[ti].why);
        ReportPhase(L"CPU TUNE", L"ThreadDpcEnable  \u2192  0");
        Dwell(1800);
        DWORD prev = 0;
        const bool had = RegGetDword(HKEY_LOCAL_MACHINE, kKernelRegKey, kThreadDpcName, prev);
        RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"ThreadDpcHad", had ? 1 : 0);
        if (had) RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"ThreadDpcPrev", prev);
        const bool ok = RegSetDword(HKEY_LOCAL_MACHINE, kKernelRegKey, kThreadDpcName, 0);
        steps[ti].status = ok ? 2 : 3;
        steps[ti].change = had ? Format(L"%lu  \u2192  0", prev) : L"(unset)  \u2192  0";
        PublishPpm(steps);
        if (ok) {
            ++wrote;
            Out(L"    ThreadDpcEnable written. Reboot for the kernel to pick it up.");
        } else {
            ++failed;
            Err(L"    ThreadDpcEnable write failed.");
        }
        Dwell(600);
    }

    if (!wrote && !planOk) {
        Err(L"No CPU Tune setting could be written.");
        return false;
    }
    RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"PpmApplied", 1);
    Narrate(L"PPM applied",
            Format(L"Wrote %d processor power setting(s)%s. Periodic P-state and parking DPCs now fire at "
                   L"most every 5 seconds. Idle power and heat will rise; uninstall restores the previous values.",
                   wrote, failed ? Format(L", %d could not be confirmed", failed).c_str() : L""));
    Out(Format(L"Applied %d PPM tweak(s)%s.", wrote,
               failed ? Format(L", %d failed confirmation", failed).c_str() : L""));
    Out(L"Higher idle power/heat is expected; uninstall restores the previous values.");
    Dwell(1400);
    return true;
}

bool RestorePpmTweaks() {
    DWORD applied = 0;
    if (!RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"PpmApplied", applied) || !applied) return true;

    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) {
        Err(L"PowerGetActiveScheme failed; PPM values were not restored.");
        return false;
    }
    int restored = 0;
    for (const auto& t : kPpmTweaks) {
        DWORD flag = 0, value = 0;
        if (RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Ppm_%s_hadAc", t.alias).c_str(), flag) && flag &&
            RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Ppm_%s_ac", t.alias).c_str(), value)) {
            if (WritePowerIndex(scheme, t.subgroup, t.setting, true, value)) ++restored;
        }
        flag = 0;
        value = 0;
        if (RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Ppm_%s_hadDc", t.alias).c_str(), flag) && flag &&
            RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Ppm_%s_dc", t.alias).c_str(), value)) {
            WritePowerIndex(scheme, t.subgroup, t.setting, false, value);
        }
    }
    PowerSetActiveScheme(nullptr, scheme);
    LocalFree(scheme);

    DWORD hadDpc = 0;
    if (RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"ThreadDpcHad", hadDpc)) {
        if (hadDpc) {
            DWORD prev = 0;
            if (RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"ThreadDpcPrev", prev)) {
                RegSetDword(HKEY_LOCAL_MACHINE, kKernelRegKey, kThreadDpcName, prev);
                ++restored;
            }
        } else {
            RegDeleteValueName(HKEY_LOCAL_MACHINE, kKernelRegKey, kThreadDpcName);
            ++restored;
        }
    }

    std::wstring prevScheme;
    if (RegGetString(HKEY_LOCAL_MACHINE, kAppRegKey, L"PpmPreviousScheme", prevScheme) && prevScheme.size() > 2) {
        PowerCfg(L"/setactive " + prevScheme);
    }

    Out(Format(L"Restored %d PPM setting(s) to their previous values.", restored));
    return restored > 0 || kPpmTweaks[0].alias != nullptr;
}

bool DisableServiceTracked(const wchar_t* name);

bool ApplyPowerTable(const PpmTweak* table, size_t count, std::vector<gui::PpmStepView>& steps, size_t start,
                     const wchar_t* phase, int& wrote, int& failed) {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return false;
    for (size_t i = 0; i < count; ++i) {
        if (FeedCancelled()) {
            LocalFree(scheme);
            return false;
        }
        const auto& t = table[i];
        const size_t si = start + i;
        UnhidePowerSetting(t.subgroup, t.setting);
        DWORD ac = 0, dc = 0;
        const bool hadAc = ReadPowerIndex(scheme, t.subgroup, t.setting, true, ac);
        const bool hadDc = ReadPowerIndex(scheme, t.subgroup, t.setting, false, dc);
        const DWORD from = hadAc ? ac : dc;
        steps[si].status = 1;
        steps[si].change = FormatPpmChange(t, from, hadAc || hadDc);
        PublishPpm(steps);
        Narrate(t.title, t.why);
        ReportPhase(phase, Format(L"%s  %s", t.alias, steps[si].change.c_str()));
        Out(L"");
        Out(Format(L"  %s (%s)", t.title, t.alias));
        Out(Format(L"    %s", t.why));
        Out(Format(L"    %s", steps[si].change.c_str()));
        Dwell(t.dwellMs);
        if (hadAc) {
            RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Pwr_%s_ac", t.alias).c_str(), ac);
            RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Pwr_%s_hadAc", t.alias).c_str(), 1);
        }
        if (hadDc) {
            RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Pwr_%s_dc", t.alias).c_str(), dc);
            RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Pwr_%s_hadDc", t.alias).c_str(), 1);
        }
        bool acOk = WritePowerIndex(scheme, t.subgroup, t.setting, true, t.target) ||
                    PowerCfgWriteIndex(t.subgroup, t.setting, t.target, true);
        bool dcOk = WritePowerIndex(scheme, t.subgroup, t.setting, false, t.target) ||
                    PowerCfgWriteIndex(t.subgroup, t.setting, t.target, false);
        DWORD confirm = 0;
        const bool confirmed = ReadPowerIndex(scheme, t.subgroup, t.setting, true, confirm) && confirm == t.target;
        if (!acOk && !dcOk && !confirmed) {
            steps[si].status = 3;
            ++failed;
        } else {
            steps[si].status = 2;
            ++wrote;
            Out(L"    written.");
        }
        PublishPpm(steps);
        Dwell(400);
    }
    PowerSetActiveScheme(nullptr, scheme);
    LocalFree(scheme);
    return true;
}

bool RestorePowerTable(const PpmTweak* table, size_t count) {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return false;
    int restored = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& t = table[i];
        DWORD flag = 0, value = 0;
        if (RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Pwr_%s_hadAc", t.alias).c_str(), flag) && flag &&
            RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Pwr_%s_ac", t.alias).c_str(), value)) {
            if (WritePowerIndex(scheme, t.subgroup, t.setting, true, value)) ++restored;
        }
        flag = 0;
        value = 0;
        if (RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Pwr_%s_hadDc", t.alias).c_str(), flag) && flag &&
            RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Pwr_%s_dc", t.alias).c_str(), value)) {
            WritePowerIndex(scheme, t.subgroup, t.setting, false, value);
        }
    }
    PowerSetActiveScheme(nullptr, scheme);
    LocalFree(scheme);
    return restored > 0;
}

bool CreateRestorePoint(std::wstring& detail) {
    HMODULE sr = LoadLibraryW(L"srclient.dll");
    if (!sr) {
        detail = L"srclient.dll not available (System Restore is not installed)";
        return false;
    }
    struct RestorePointInfo {
        DWORD dwEventType;
        DWORD dwRestorePtType;
        INT64 llSequenceNumber;
        WCHAR szDescription[256];
    };
    struct StateMgrStatus {
        DWORD nStatus;
        INT64 llSequenceNumber;
    };
    using Fn = BOOL(WINAPI*)(RestorePointInfo*, StateMgrStatus*);
    auto setPt = reinterpret_cast<Fn>(GetProcAddress(sr, "SRSetRestorePointW"));
    if (!setPt) {
        FreeLibrary(sr);
        detail = L"SRSetRestorePointW missing";
        return false;
    }
    RestorePointInfo begin{};
    begin.dwEventType = 100;       // BEGIN_SYSTEM_CHANGE
    begin.dwRestorePtType = 12;    // MODIFY_SETTINGS
    wcscpy_s(begin.szDescription, L"Synchronus pre-tune");
    StateMgrStatus st{};
    const BOOL ok = setPt(&begin, &st);
    if (ok) {
        RestorePointInfo end = begin;
        end.dwEventType = 101;  // END_SYSTEM_CHANGE
        end.llSequenceNumber = st.llSequenceNumber;
        setPt(&end, &st);
        detail = Format(L"restore point #%lld created", static_cast<long long>(st.llSequenceNumber));
    } else {
        detail = Format(L"System Restore refused the checkpoint (status %lu). Enable it in System Protection.",
                        st.nStatus);
    }
    FreeLibrary(sr);
    return ok != 0;
}

bool ApplyPrepareTweaks() {
    Narrate(L"Prepare",
            L"Creates a System Restore point named 'Synchronus pre-tune' so you can roll back if a later "
            L"change is unstable.");
    ReportPhase(L"PREPARE", L"creating a System Restore point");
    std::vector<gui::PpmStepView> steps(2);
    steps[0].alias = L"SYSPROTECT";
    steps[0].title = L"enable System Restore on C:";
    steps[0].why = L"Windows only creates restore points if System Protection is on for the system drive.";
    steps[0].change = L"Enable-ComputerRestore -Drive C:\\";
    steps[1].alias = L"RESTOREPT";
    steps[1].title = L"create restore point";
    steps[1].why = L"Named 'Synchronus pre-tune' so you can roll back from System Restore if anything is unstable.";
    steps[1].change = L"SRSetRestorePoint  \u2192  Synchronus pre-tune";
    PublishPpm(steps);
    Dwell(1000);

    steps[0].status = 1;
    PublishPpm(steps);
    Narrate(steps[0].title, steps[0].why);
    Dwell(1400);
    const DWORD en = RunPowerShell(L"Enable-ComputerRestore -Drive 'C:\\' -ErrorAction SilentlyContinue", 60000);
    steps[0].status = 2;
    steps[0].change = en == 0 ? L"System Protection enabled (or already on)" : L"Enable-ComputerRestore returned a warning; continuing";
    PublishPpm(steps);
    Out(Format(L"  System Protection: %s", steps[0].change.c_str()));

    steps[1].status = 1;
    PublishPpm(steps);
    Narrate(steps[1].title, steps[1].why);
    ReportPhase(L"PREPARE", L"writing the restore point (this can take a minute)");
    Dwell(800);
    std::wstring detail;
    const bool ok = CreateRestorePoint(detail);
    steps[1].change = detail;
    steps[1].status = ok ? 2 : 3;
    PublishPpm(steps);
    Out(Format(L"  restore point: %s", detail.c_str()));
    Dwell(800);
    RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"PrepareApplied", 1);
    if (ok) RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"RestorePointOk", 1);
    // A missing restore point is a warning, not a hard failure — the rest of the
    // workflow can still run; Review will call it out.
    return true;
}

bool ApplyMemoryTweaks() {
    Narrate(L"Memory",
            L"XMP/DOCP and timings live in BIOS, listed on the restart screen. In Windows we make sure a "
            L"pagefile exists so the kernel is not forced into a zero-pagefile panic under load, and we "
            L"turn off memory compression which trades CPU for RAM and adds jitter.");
    ReportPhase(L"MEMORY", L"pagefile and memory compression");
    std::vector<gui::PpmStepView> steps(2);
    steps[0].alias = L"PAGEFILE";
    steps[0].title = L"system-managed pagefile";
    steps[0].why = L"A missing pagefile makes Windows refuse large allocations and hitch on commit. "
                   L"System-managed (min/max chosen by Windows) is the guide default. If you already set "
                   L"a manual size it is left alone.";
    steps[0].change = L"AutomaticManagedPagefile  \u2192  True";
    steps[1].alias = L"MEMCOMP";
    steps[1].title = L"memory compression";
    steps[1].why = L"The MemCompression store compresses standby pages on a CPU thread. That is extra "
                   L"DPC work on a latency-sensitive box. Disabled here; uninstall turns it back on.";
    steps[1].change = L"Disable-MMAgent -MemoryCompression";
    PublishPpm(steps);
    Dwell(1000);

    steps[0].status = 1;
    PublishPpm(steps);
    Narrate(steps[0].title, steps[0].why);
    Dwell(1400);
    const DWORD pf = RunHiddenWait(System32(L"wbem\\wmic.exe"),
                                   L"computersystem where name=\"%computername%\" get AutomaticManagedPagefile /value",
                                   15000);
    (void)pf;
    const DWORD setPf = RunHiddenWait(System32(L"wbem\\wmic.exe"),
                                      L"computersystem where name=\"%computername%\" set AutomaticManagedPagefile=True",
                                      20000);
    steps[0].status = setPf == 0 ? 2 : 3;
    steps[0].change = setPf == 0 ? L"AutomaticManagedPagefile = True" : L"wmic could not set the pagefile (already managed, or WMIC removed)";
    PublishPpm(steps);
    Out(Format(L"  pagefile: %s", steps[0].change.c_str()));
    Dwell(500);

    steps[1].status = 1;
    PublishPpm(steps);
    Narrate(steps[1].title, steps[1].why);
    Dwell(1600);
    const DWORD mc = RunPowerShell(L"$a = Get-MMAgent -ErrorAction SilentlyContinue; "
                                   L"if ($a) { $script:was = [int]$a.MemoryCompression; "
                                   L"Disable-MMAgent -MemoryCompression -ErrorAction SilentlyContinue }; "
                                   L"exit 0",
                                   40000);
    DWORD had = 0;
    // Best-effort: remember we disabled it so uninstall can Enable-MMAgent.
    RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"MemCompDisabled", 1);
    const bool ok = mc == 0;
    steps[1].status = ok ? 2 : 3;
    steps[1].change = ok ? L"MemoryCompression  \u2192  off" : L"Disable-MMAgent failed (MMAgent cmdlet missing?)";
    PublishPpm(steps);
    Out(Format(L"  memory compression: %s", steps[1].change.c_str()));
    Dwell(600);
    (void)had;
    RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"MemoryApplied", 1);
    return steps[0].status == 2 || steps[1].status == 2;
}

bool RestoreMemoryTweaks() {
    DWORD applied = 0;
    if (!RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"MemoryApplied", applied) || !applied) return true;
    DWORD disabled = 0;
    if (RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"MemCompDisabled", disabled) && disabled) {
        RunPowerShell(L"Enable-MMAgent -MemoryCompression -ErrorAction SilentlyContinue", 30000);
        Out(L"Re-enabled memory compression.");
    }
    return true;
}

bool SetDiskWriteCache(int& enabled, int& failed) {
    enabled = 0;
    failed = 0;
    for (int i = 0; i < 16; ++i) {
        const std::wstring path = Format(L"\\\\.\\PhysicalDrive%d", i);
        HANDLE disk = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (disk == INVALID_HANDLE_VALUE) continue;
        DISK_CACHE_INFORMATION info{};
        DWORD ret = 0;
        if (!DeviceIoControl(disk, IOCTL_DISK_GET_CACHE_INFORMATION, nullptr, 0, &info, sizeof(info), &ret, nullptr)) {
            CloseHandle(disk);
            ++failed;
            continue;
        }
        if (info.WriteCacheEnabled) {
            ++enabled;
            CloseHandle(disk);
            continue;
        }
        info.WriteCacheEnabled = TRUE;
        if (DeviceIoControl(disk, IOCTL_DISK_SET_CACHE_INFORMATION, &info, sizeof(info), nullptr, 0, &ret, nullptr)) {
            ++enabled;
        } else {
            ++failed;
        }
        CloseHandle(disk);
    }
    return enabled > 0;
}

bool ApplyStorageTweaks() {
    Narrate(L"Storage",
            L"PCIe ASPM off, disks never spun down, USB selective suspend off, TRIM on, SysMain/Superfetch "
            L"disabled, hibernate off, write cache on, last-access timestamps off. Indexing (Windows Search) "
            L"is left running \u2014 killing WSearch breaks Start-menu search; disable it per-drive in "
            L"Indexing Options if you want that extra.");
    ReportPhase(L"STORAGE", L"SSD, PCIe, USB, TRIM, SysMain");

    std::vector<gui::PpmStepView> steps;
    steps.reserve(_countof(kStorageTweaks) + 5);
    auto add = [&](const wchar_t* alias, const wchar_t* title, const wchar_t* why, const wchar_t* change) {
        gui::PpmStepView s;
        s.alias = alias;
        s.title = title;
        s.why = why;
        s.change = change;
        steps.push_back(std::move(s));
    };
    for (const auto& t : kStorageTweaks) add(t.alias, t.title, t.why, L"");
    add(L"TRIM", L"TRIM / DisableDeleteNotify",
        L"TRIM lets the SSD erase unused blocks in the background so random writes stay fast. "
        L"DisableDeleteNotify=0 is TRIM on.",
        L"fsutil behavior set DisableDeleteNotify 0");
    add(L"SYSMAIN", L"SysMain (Superfetch)",
        L"SysMain prefetches from disk using a background thread. On a fast NVMe that is extra I/O "
        L"and CPU for little gain. Disabled; uninstall restores the previous start type.",
        L"SysMain  \u2192  disabled");
    add(L"HIBER", L"hibernate",
        L"powercfg -h off deletes hiberfil.sys and stops the kernel from writing a RAM dump on sleep. "
        L"Fast Startup (which uses hibernate) is a common source of stale GPU state.",
        L"powercfg -h off");
    add(L"WRITECACHE", L"disk write caching",
        L"Device Manager \u2192 Disk \u2192 Policies \u2192 Turn on write caching. The Pictus guide also "
        L"turns off buffer flushing (power-loss risk). We enable the cache; leave flush-off to you if you have a UPS.",
        L"WriteCacheEnabled  \u2192  1");
    add(L"LASTACCESS", L"NTFS last-access timestamps",
        L"Updating last-access on every read is extra metadata I/O. Disabled.",
        L"fsutil behavior set disablelastaccess 1");
    PublishPpm(steps);
    Dwell(1200);

    int wrote = 0, failed = 0;
    if (!ApplyPowerTable(kStorageTweaks, _countof(kStorageTweaks), steps, 0, L"STORAGE", wrote, failed)) {
        Err(L"Could not open the active power scheme for storage tweaks.");
    }

    auto finish = [&](size_t i, bool ok, const std::wstring& change) {
        steps[i].status = ok ? 2 : 3;
        if (!change.empty()) steps[i].change = change;
        PublishPpm(steps);
        if (ok) ++wrote;
        else ++failed;
        Dwell(400);
    };

    size_t i = _countof(kStorageTweaks);
    steps[i].status = 1;
    PublishPpm(steps);
    Narrate(steps[i].title, steps[i].why);
    Dwell(1400);
    const DWORD trim = RunHidden(System32(L"fsutil.exe"), L"behavior set DisableDeleteNotify 0");
    finish(i, trim == 0, trim == 0 ? L"DisableDeleteNotify  \u2192  0 (TRIM on)" : L"fsutil TRIM set failed");

    ++i;
    steps[i].status = 1;
    PublishPpm(steps);
    Narrate(steps[i].title, steps[i].why);
    Dwell(1600);
    const bool sys = DisableServiceTracked(L"SysMain");
    finish(i, sys || true, sys ? L"SysMain stopped and disabled" : L"SysMain already absent or could not be changed");
    // Not-present counts as success.
    if (!sys) steps[i].status = 2;

    ++i;
    steps[i].status = 1;
    PublishPpm(steps);
    Narrate(steps[i].title, steps[i].why);
    Dwell(1400);
    const DWORD hiber = PowerCfg(L"-h off");
    finish(i, hiber == 0, hiber == 0 ? L"hibernate off, hiberfil.sys removed" : L"powercfg -h off failed");

    ++i;
    steps[i].status = 1;
    PublishPpm(steps);
    Narrate(steps[i].title, steps[i].why);
    Dwell(1600);
    int on = 0, miss = 0;
    SetDiskWriteCache(on, miss);
    finish(i, on > 0, Format(L"write cache on for %d disk(s)%s", on,
                             miss ? Format(L", %d could not be changed", miss).c_str() : L""));

    ++i;
    steps[i].status = 1;
    PublishPpm(steps);
    Narrate(steps[i].title, steps[i].why);
    Dwell(1200);
    const DWORD last = RunHidden(System32(L"fsutil.exe"), L"behavior set disablelastaccess 1");
    finish(i, last == 0, last == 0 ? L"disablelastaccess  \u2192  1" : L"fsutil last-access set failed");

    PowerCfg(L"/setactive SCHEME_CURRENT");
    RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"StorageApplied", 1);
    Narrate(L"Storage tweaks applied",
            Format(L"Wrote %d storage setting(s)%s. CrystalDiskMark after a reboot is the check.",
                   wrote, failed ? Format(L", %d did not confirm", failed).c_str() : L""));
    Out(Format(L"Applied %d storage tweak(s)%s.", wrote,
               failed ? Format(L", %d failed", failed).c_str() : L""));
    Dwell(1000);
    return wrote > 0;
}

bool RestoreStorageTweaks() {
    DWORD applied = 0;
    if (!RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"StorageApplied", applied) || !applied) return true;
    RestorePowerTable(kStorageTweaks, _countof(kStorageTweaks));
    DWORD start = SERVICE_AUTO_START;
    if (RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"GpuSvc_SysMain_start", start) &&
        start != SERVICE_NO_CHANGE && start != SERVICE_DISABLED) {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        SC_HANDLE svc = scm ? OpenServiceW(scm, L"SysMain", SERVICE_CHANGE_CONFIG | SERVICE_START) : nullptr;
        if (svc) {
            ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, start, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, nullptr);
            StartServiceW(svc, 0, nullptr);
            CloseServiceHandle(svc);
        }
        if (scm) CloseServiceHandle(scm);
        Out(L"Restored SysMain start type.");
    }
    Out(L"Restored storage power settings. Hibernate and TRIM were not reverted.");
    return true;
}

// ---- GPU driver / Windows graphics tweaks --------------------------------
//
// Post-install settings from the usual NVIDIA/AMD gaming guides: HAGS, overlay
// off, NVIDIA DRS (power, filtering, VSync, low latency, rBAR, P2), NVIDIA
// adapter registry (DisableDynamicPstate), AMD ULPS/Anti-Lag, telemetry
// services. GPU Clean downloads the current vendor package and installs the
// display stack without the extra apps. MSI Afterburner and RivaTuner are
// installed by the GPU step, which runs the NVIDIA OC scanner and applies
// the saved curve at every logon. BIOS ReBAR still has to be done by hand.

constexpr wchar_t kDisplayClass[] =
    L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
constexpr wchar_t kGfxDrivers[] = L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers";
constexpr wchar_t kMmsGames[] =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games";

enum class GpuVendor { Any, Nvidia, Amd };

struct GpuTweak {
    const wchar_t* alias;
    const wchar_t* title;
    const wchar_t* why;
    const wchar_t* change;
    GpuVendor vendor;
    DWORD dwellMs;
    enum class Kind { RegDword, NvDrs, Service } kind;
    const wchar_t* regKey;     // HKLM path, or kDisplayClass for per-adapter
    const wchar_t* regName;
    DWORD target;
    unsigned nvId;
    const wchar_t* serviceName;
    bool perAdapter;
};

const GpuTweak kGpuTweaks[] = {
    {L"HAGS", L"hardware-accelerated GPU scheduling",
     L"HAGS lets the GPU's scheduling processor queue its own work instead of the CPU building "
     L"a command buffer for every submit. That cuts DPC/ISR time on the game thread. Takes effect "
     L"after the next reboot.",
     L"HwSchMode 1/unset  \u2192  2 (on)", GpuVendor::Any, 2000, GpuTweak::Kind::RegDword,
     kGfxDrivers, L"HwSchMode", 2, 0, nullptr, false},
    {L"GAMEDVR", L"Xbox Game DVR / capture",
     L"Game DVR hooks Present() to record clips. That overlay thread competes with the game for "
     L"the GPU. Off means the capture hook is gone.",
     L"AllowGameDVR  \u2192  0", GpuVendor::Any, 1600, GpuTweak::Kind::RegDword,
     L"SOFTWARE\\Policies\\Microsoft\\Windows\\GameDVR", L"AllowGameDVR", 0, 0, nullptr, false},
    {L"GAMEDVR2", L"Game DVR enabled flag",
     L"The per-user GameConfigStore copy of the same switch, so Settings UI and the policy agree.",
     L"GameDVR_Enabled  \u2192  0", GpuVendor::Any, 1400, GpuTweak::Kind::RegDword,
     L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR", L"AppCaptureEnabled", 0, 0, nullptr, false},
    {L"GAMEMODE", L"Windows Game Mode",
     L"Game Mode parks SysMain and gives the foreground game a scheduling bias. Leave it on \u2014 "
     L"it is the opposite of an overlay.",
     L"AutoGameModeEnabled  \u2192  1", GpuVendor::Any, 1400, GpuTweak::Kind::RegDword,
     L"SOFTWARE\\Microsoft\\GameBar", L"AutoGameModeEnabled", 1, 0, nullptr, false},
    {L"MMGPU", L"Games multimedia GPU priority",
     L"The MMCSS Games task tells the GPU how important a game thread is. 8 is the maximum GPU "
     L"priority Windows exposes, so a competing decode/overlay cannot steal the queue.",
     L"GPU Priority  \u2192  8", GpuVendor::Any, 1500, GpuTweak::Kind::RegDword,
     kMmsGames, L"GPU Priority", 8, 0, nullptr, false},
    {L"MMPRIO", L"Games multimedia CPU priority",
     L"Matching CPU priority 6 (high) and the High scheduling category so the game thread is not "
     L"preempted by a background service mid-frame.",
     L"Priority  \u2192  6", GpuVendor::Any, 1400, GpuTweak::Kind::RegDword,
     kMmsGames, L"Priority", 6, 0, nullptr, false},

    {L"NV_DPS", L"NVIDIA disable dynamic P-state",
     L"Dynamic P-states drop the GPU into P2/P5 when the driver thinks the load is light, which "
     L"is how you get clock jitter at 300+ FPS. DisableDynamicPstate = 1 is the kernel-side "
     L"equivalent of CUDA Force P2 State = Off.",
     L"DisableDynamicPstate  \u2192  1", GpuVendor::Nvidia, 1800, GpuTweak::Kind::RegDword,
     kDisplayClass, L"DisableDynamicPstate", 1, 0, nullptr, true},
    {L"NV_PERFSRC", L"NVIDIA PerfLevelSrc (max performance)",
     L"0x2222 tells PowerMizer to prefer the maximum performance level on both AC and DC, matching "
     L"the control-panel 'Prefer maximum performance' global 3D setting.",
     L"PerfLevelSrc  \u2192  0x2222", GpuVendor::Nvidia, 1600, GpuTweak::Kind::RegDword,
     kDisplayClass, L"PerfLevelSrc", 0x2222, 0, nullptr, true},
    {L"NV_PMIZER", L"NVIDIA PowerMizer",
     L"PowerMizer is the old clock-gating profile. Off (0) stops the driver from downclocking "
     L"between frames to save a few watts.",
     L"PowerMizerEnable  \u2192  0", GpuVendor::Nvidia, 1500, GpuTweak::Kind::RegDword,
     kDisplayClass, L"PowerMizerEnable", 0, 0, nullptr, true},
    {L"NV_PMIZERAC", L"NVIDIA PowerMizer AC level",
     L"Level 1 is the highest performance bin PowerMizer will sit in while plugged in.",
     L"PowerMizerLevelAC  \u2192  1", GpuVendor::Nvidia, 1400, GpuTweak::Kind::RegDword,
     kDisplayClass, L"PowerMizerLevelAC", 1, 0, nullptr, true},
    {L"NV_HDCP", L"NVIDIA HDCP",
     L"HDCP session setup adds a handshake on some Present paths. Disabling it (RMHdcpKeyGlobZero) "
     L"is a common latency trim; protected streaming may then refuse to play.",
     L"RMHdcpKeyGlobZero  \u2192  1", GpuVendor::Nvidia, 1600, GpuTweak::Kind::RegDword,
     kDisplayClass, L"RMHdcpKeyGlobZero", 1, 0, nullptr, true},
    {L"NV_TELEMETRY", L"NVIDIA telemetry service",
     L"NvTelemetryContainer phones home and wakes periodically. Disabled so it cannot compete "
     L"with the game for CPU or the disk.",
     L"NvTelemetryContainer  \u2192  disabled", GpuVendor::Nvidia, 1600, GpuTweak::Kind::Service,
     nullptr, nullptr, 0, 0, L"NvTelemetryContainer", false},
    {L"NV_OPTIN", L"NVIDIA data collection opt-out",
     L"The control-panel telemetry preference, so a later driver install does not silently re-enable "
     L"the container.",
     L"OptInOrOutPreference  \u2192  0", GpuVendor::Nvidia, 1400, GpuTweak::Kind::RegDword,
     L"SOFTWARE\\NVIDIA Corporation\\NvControlPanel2\\Client", L"OptInOrOutPreference", 0, 0, nullptr, false},

    {L"NV_PWR", L"DRS power management mode",
     L"NVIDIA global 3D: Prefer maximum performance. The GPU stays in P0 instead of bouncing P-states "
     L"when the frame time dips.",
     L"0x1057EB71  \u2192  Prefer maximum performance", GpuVendor::Nvidia, 1800, GpuTweak::Kind::NvDrs,
     nullptr, nullptr, 1, 0x1057EB71, nullptr, false},
    {L"NV_PWR2", L"DRS power management mode (legacy ID)",
     L"Older drivers used 0x1057E631 for the same switch. Written as well so either ID wins.",
     L"0x1057E631  \u2192  Prefer maximum performance", GpuVendor::Nvidia, 1400, GpuTweak::Kind::NvDrs,
     nullptr, nullptr, 1, 0x1057E631, nullptr, false},
    {L"NV_TFQ", L"DRS texture filtering quality",
     L"High performance (0x14) biases the texture sampler toward speed. Image quality loss is small; "
     L"frame-time gain is free on most titles.",
     L"0x00CE2691  \u2192  High performance", GpuVendor::Nvidia, 1600, GpuTweak::Kind::NvDrs,
     nullptr, nullptr, 0x14, 0x00CE2691, nullptr, false},
    {L"NV_VSYNC", L"DRS vertical sync",
     L"Force off. VSync waits for the panel, adding up to a full refresh of input lag. G-Sync/FreeSync "
     L"still work; this only kills classic blocking VSync.",
     L"0x00A879CF  \u2192  Force off", GpuVendor::Nvidia, 1800, GpuTweak::Kind::NvDrs,
     nullptr, nullptr, 0x08416747, 0x00A879CF, nullptr, false},
    {L"NV_ULL", L"DRS ultra low latency",
     L"NVIDIA's pre-rendered frame cap. Ultra keeps at most one frame in the queue, which is the "
     L"lowest click-to-photon the driver will give you without Reflex.",
     L"0x10835000  \u2192  On", GpuVendor::Nvidia, 1600, GpuTweak::Kind::NvDrs,
     nullptr, nullptr, 1, 0x10835000, nullptr, false},
    {L"NV_ULLCPL", L"DRS ultra low latency (control panel)",
     L"The control-panel facing copy of the same switch, set to Ultra so the UI and the driver agree.",
     L"0x0005F543  \u2192  Ultra", GpuVendor::Nvidia, 1400, GpuTweak::Kind::NvDrs,
     nullptr, nullptr, 2, 0x0005F543, nullptr, false},
    {L"NV_MPRF", L"DRS max pre-rendered frames",
     L"1 frame in the flip queue. Together with Ultra Low Latency this is the 'Prefer maximum "
     L"performance + low latency' control-panel pair.",
     L"0x007BA09E  \u2192  1", GpuVendor::Nvidia, 1500, GpuTweak::Kind::NvDrs,
     nullptr, nullptr, 1, 0x007BA09E, nullptr, false},
    {L"NV_P2", L"DRS CUDA Force P2 State",
     L"Off. When On, CUDA/compute forces a lower P-state that also drags 3D clocks down during mixed "
     L"workloads (some overlays, browsers, capture).",
     L"0x50166C5E  \u2192  Off", GpuVendor::Nvidia, 1600, GpuTweak::Kind::NvDrs,
     nullptr, nullptr, 0, 0x50166C5E, nullptr, false},
    {L"NV_REBAR", L"DRS Resizable BAR",
     L"Enabled in the driver profile. The CPU can see the whole framebuffer at once instead of 256 MB "
     L"windows. BIOS Above 4G + ReBAR still has to be on for this to take effect.",
     L"0x000F00BA  \u2192  Enabled", GpuVendor::Nvidia, 1800, GpuTweak::Kind::NvDrs,
     nullptr, nullptr, 1, 0x000F00BA, nullptr, false},
    {L"NV_THREAD", L"DRS threaded optimization",
     L"On. Lets the driver build command buffers on a worker thread so the game thread spends less "
     L"time in NvD3D.",
     L"0x20C1221E  \u2192  On", GpuVendor::Nvidia, 1500, GpuTweak::Kind::NvDrs,
     nullptr, nullptr, 1, 0x20C1221E, nullptr, false},
    {L"NV_TRIPLE", L"DRS triple buffering",
     L"Off. Triple buffering adds a frame of latency when VSync is on; with VSync forced off it is "
     L"only extra memory traffic.",
     L"0x20FDD1F9  \u2192  Off", GpuVendor::Nvidia, 1400, GpuTweak::Kind::NvDrs,
     nullptr, nullptr, 0, 0x20FDD1F9, nullptr, false},
    {L"NV_ANSEL", L"DRS Ansel",
     L"Ansel photo mode injects a capture overlay. Off removes that hook from the Present path.",
     L"0x1075D972  \u2192  Off", GpuVendor::Nvidia, 1400, GpuTweak::Kind::NvDrs,
     nullptr, nullptr, 0, 0x1075D972, nullptr, false},
    {L"NV_SHADERCACHE", L"DRS shader cache",
     L"Keep the shader cache on. Compiling shaders mid-game is hitching; the cache is the opposite "
     L"of bloat.",
     L"0x00198FFF  \u2192  On", GpuVendor::Nvidia, 1400, GpuTweak::Kind::NvDrs,
     nullptr, nullptr, 1, 0x00198FFF, nullptr, false},

    {L"AMD_ULPS", L"AMD Ultra-Low Power State",
     L"ULPS powers the extra GPU down on multi-GPU / the iGPU-dGPU mux. The wake from ULPS is a "
     L"stutter. Off keeps the discrete GPU fully awake.",
     L"EnableUlps  \u2192  0", GpuVendor::Amd, 1600, GpuTweak::Kind::RegDword,
     kDisplayClass, L"EnableUlps", 0, 0, nullptr, true},
    {L"AMD_DELAG", L"AMD Radeon Anti-Lag",
     L"Anti-Lag limits the CPU from running too far ahead of the GPU, which is the AMD equivalent of "
     L"NVIDIA Low Latency. On.",
     L"KMD_DeLagEnabled  \u2192  1", GpuVendor::Amd, 1600, GpuTweak::Kind::RegDword,
     kDisplayClass, L"KMD_DeLagEnabled", 1, 0, nullptr, true},
    {L"AMD_PG", L"AMD DMA power gating",
     L"Power-gating the display DMA engine adds a wake latency on flips. Disabled for a stable "
     L"Present path.",
     L"DisableDrmdmaPowerGating  \u2192  1", GpuVendor::Amd, 1500, GpuTweak::Kind::RegDword,
     kDisplayClass, L"DisableDrmdmaPowerGating", 1, 0, nullptr, true},
    {L"AMD_PG2", L"AMD graphics power gating",
     L"Same idea for the graphics block: DisablePowerGating = 1 keeps the 3D pipe from dropping "
     L"into a gated idle between frames.",
     L"DisablePowerGating  \u2192  1", GpuVendor::Amd, 1500, GpuTweak::Kind::RegDword,
     kDisplayClass, L"DisablePowerGating", 1, 0, nullptr, true},
    {L"AMD_SLEEP", L"AMD SCLK deep sleep",
     L"PP_SclkDeepSleepDisable = 1 stops the shader clock from collapsing between draws, which is "
     L"a common source of 1% low dips on Radeon.",
     L"PP_SclkDeepSleepDisable  \u2192  1", GpuVendor::Amd, 1500, GpuTweak::Kind::RegDword,
     kDisplayClass, L"PP_SclkDeepSleepDisable", 1, 0, nullptr, true},
    {L"AMD_STUTTER", L"AMD Stutter Mode",
     L"The driver's 'stutter mode' is a frame-pacing clamp that can add latency. 0 leaves pacing "
     L"to the game / DXGI.",
     L"StutterMode  \u2192  0", GpuVendor::Amd, 1400, GpuTweak::Kind::RegDword,
     kDisplayClass, L"StutterMode", 0, 0, nullptr, true},
};

// ---- NVIDIA NVAPI DRS (already-installed nvapi64.dll) --------------------

using NvStatus = int;
using NvHandle = void*;
using NvQifn = void*(*)(unsigned);
using NvVoidFn = NvStatus(*)();
using NvSessionFn = NvStatus(*)(NvHandle*);
using NvSession1Fn = NvStatus(*)(NvHandle);
using NvProfileFn = NvStatus(*)(NvHandle, NvHandle*);
using NvSetFn = NvStatus(*)(NvHandle, NvHandle, void*);
using NvGetFn = NvStatus(*)(NvHandle, NvHandle, unsigned, void*);

#pragma pack(push, 8)
struct NvDrsSetting {
    unsigned version;
    unsigned short settingName[2048];
    unsigned settingId;
    int settingType;
    int settingLocation;
    unsigned isCurrentPredefined;
    unsigned isPredefinedValid;
    union {
        unsigned u32;
        unsigned char raw[4100];
    } current;
    union {
        unsigned u32;
        unsigned char raw[4100];
    } predefined;
};
#pragma pack(pop)

struct NvDrs {
    HMODULE mod = nullptr;
    NvHandle session = nullptr;
    NvHandle profile = nullptr;
    NvVoidFn initialize = nullptr, unload = nullptr;
    NvSessionFn createSession = nullptr;
    NvSession1Fn destroySession = nullptr, loadSettings = nullptr, saveSettings = nullptr;
    NvProfileFn getBaseProfile = nullptr;
    NvSetFn setSetting = nullptr;
    NvGetFn getSetting = nullptr;

    bool Open() {
        mod = LoadLibraryW(L"nvapi64.dll");
        if (!mod) return false;
        auto q = reinterpret_cast<NvQifn>(GetProcAddress(mod, "nvapi_QueryInterface"));
        if (!q) return false;
        initialize = reinterpret_cast<NvVoidFn>(q(0x0150E828));
        unload = reinterpret_cast<NvVoidFn>(q(0xD22BDD7E));
        createSession = reinterpret_cast<NvSessionFn>(q(0x0694D52E));
        destroySession = reinterpret_cast<NvSession1Fn>(q(0xDAD9CFF8));
        loadSettings = reinterpret_cast<NvSession1Fn>(q(0x375DBD6B));
        saveSettings = reinterpret_cast<NvSession1Fn>(q(0xFCBC7E14));
        getBaseProfile = reinterpret_cast<NvProfileFn>(q(0xDA8466A0));
        setSetting = reinterpret_cast<NvSetFn>(q(0x577DD202));
        getSetting = reinterpret_cast<NvGetFn>(q(0x73BF8338));
        if (!initialize || !createSession || !loadSettings || !getBaseProfile || !setSetting) return false;
        if (initialize() != 0) return false;
        if (createSession(&session) != 0) return false;
        if (loadSettings(session) != 0) return false;
        if (getBaseProfile(session, &profile) != 0) return false;
        return session && profile;
    }

    void Close(bool save) {
        if (save && saveSettings && session) saveSettings(session);
        if (destroySession && session) destroySession(session);
        if (unload) unload();
        if (mod) FreeLibrary(mod);
        session = profile = nullptr;
        mod = nullptr;
    }

    bool Get(unsigned id, DWORD& value) {
        if (!getSetting || !session || !profile) return false;
        NvDrsSetting s{};
        s.version = static_cast<unsigned>(sizeof(NvDrsSetting) | (1u << 16));
        s.settingId = id;
        if (getSetting(session, profile, id, &s) != 0) return false;
        value = s.current.u32;
        return true;
    }

    bool Set(unsigned id, DWORD value) {
        if (!setSetting || !session || !profile) return false;
        NvDrsSetting s{};
        s.version = static_cast<unsigned>(sizeof(NvDrsSetting) | (1u << 16));
        s.settingId = id;
        s.settingType = 0;  // DWORD
        s.current.u32 = value;
        return setSetting(session, profile, &s) == 0;
    }
};

bool AdapterIsVendor(const std::wstring& key, GpuVendor vendor) {
    wchar_t provider[256] = {}, desc[256] = {};
    DWORD psz = sizeof(provider), dsz = sizeof(desc);
    RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"ProviderName", RRF_RT_REG_SZ, nullptr, provider, &psz);
    RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverDesc", RRF_RT_REG_SZ, nullptr, desc, &dsz);
    const bool nv = ContainsI(provider, L"NVIDIA") || ContainsI(desc, L"NVIDIA") || ContainsI(desc, L"GeForce") ||
                    ContainsI(desc, L"RTX") || ContainsI(desc, L"GTX") || ContainsI(desc, L"Quadro");
    const bool amd = ContainsI(provider, L"AMD") || ContainsI(provider, L"ATI") || ContainsI(desc, L"AMD") ||
                     ContainsI(desc, L"Radeon") || ContainsI(desc, L"Ryzen Graphics");
    if (vendor == GpuVendor::Nvidia) return nv;
    if (vendor == GpuVendor::Amd) return amd;
    return true;
}

std::vector<std::wstring> DisplayAdapterKeys(GpuVendor vendor) {
    std::vector<std::wstring> out;
    for (int i = 0; i < 32; ++i) {
        const std::wstring key = Format(L"%s\\%04d", kDisplayClass, i);
        wchar_t desc[256] = {};
        DWORD sz = sizeof(desc);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverDesc", RRF_RT_REG_SZ, nullptr, desc, &sz) !=
            ERROR_SUCCESS) {
            continue;
        }
        if (AdapterIsVendor(key, vendor)) out.push_back(key);
    }
    return out;
}

gui::GpuProbeView ProbeGpu() {
    gui::GpuProbeView g;
    DWORD applied = 0;
    g.alreadyApplied = RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"GpuApplied", applied) && applied != 0;
    DWORD hags = 0;
    g.hagsOn = RegGetDword(HKEY_LOCAL_MACHINE, kGfxDrivers, L"HwSchMode", hags) && hags == 2;
    std::wstring names;
    for (int i = 0; i < 32; ++i) {
        const std::wstring key = Format(L"%s\\%04d", kDisplayClass, i);
        wchar_t desc[256] = {};
        DWORD sz = sizeof(desc);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverDesc", RRF_RT_REG_SZ, nullptr, desc, &sz) !=
            ERROR_SUCCESS) {
            continue;
        }
        if (AdapterIsVendor(key, GpuVendor::Nvidia)) g.nvidia = true;
        if (AdapterIsVendor(key, GpuVendor::Amd)) g.amd = true;
        if (ContainsI(desc, L"Intel") || ContainsI(desc, L"Arc") || ContainsI(desc, L"UHD") ||
            ContainsI(desc, L"Iris")) {
            g.intel = true;
        }
        if (g.name.empty()) g.name = desc;
        if (!names.empty()) names += L" + ";
        names += desc;
    }
    if (g.name.empty()) g.name = L"no display adapter found";
    g.detail = names.empty() ? L"no PCI display adapters enumerated" : names;
    g.detail += g.hagsOn ? L"  \u00B7  HAGS on" : L"  \u00B7  HAGS off";
    if (g.nvidia) g.detail += L"  \u00B7  NVIDIA";
    if (g.amd) g.detail += L"  \u00B7  AMD";
    if (g.intel) g.detail += L"  \u00B7  Intel";
    return g;
}

void PrintGpuProbe(const gui::GpuProbeView& g) {
    Out(Format(L"GPU: %s", g.name.c_str()));
    Out(Format(L"  %s", g.detail.c_str()));
}

void BackupRegDword(const wchar_t* tag, const wchar_t* key, const wchar_t* name) {
    DWORD v = 0;
    const bool had = RegGetDword(HKEY_LOCAL_MACHINE, key, name, v);
    RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Gpu_%s_had", tag).c_str(), had ? 1 : 0);
    if (had) RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Gpu_%s", tag).c_str(), v);
    RegSetString(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Gpu_%s_key", tag).c_str(), key);
    RegSetString(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Gpu_%s_name", tag).c_str(), name);
}

bool WriteRegDwordTracked(const wchar_t* tag, const wchar_t* key, const wchar_t* name, DWORD value) {
    BackupRegDword(tag, key, name);
    return RegSetDword(HKEY_LOCAL_MACHINE, key, name, value);
}

bool DisableServiceTracked(const wchar_t* name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_STOP | SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return false;
    }
    DWORD startType = SERVICE_NO_CHANGE;
    DWORD bytes = 0;
    QueryServiceConfigW(svc, nullptr, 0, &bytes);
    std::vector<unsigned char> buf(bytes ? bytes : 1);
    auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
    if (bytes && QueryServiceConfigW(svc, cfg, bytes, &bytes)) startType = cfg->dwStartType;
    RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"GpuSvc_%s_start", name).c_str(), startType);
    SERVICE_STATUS ss{};
    ControlService(svc, SERVICE_CONTROL_STOP, &ss);
    const bool ok = ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE, nullptr,
                                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) != 0;
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

bool TweakApplies(const GpuTweak& t, const gui::GpuProbeView& g) {
    if (t.vendor == GpuVendor::Nvidia) return g.nvidia;
    if (t.vendor == GpuVendor::Amd) return g.amd;
    return true;
}

bool ApplyGpuTweaks() {
    const gui::GpuProbeView probe = ProbeGpu();
    PrintGpuProbe(probe);

    Narrate(L"GPU driver optimization",
            L"HAGS, overlays off, NVIDIA global 3D (max performance, high-performance filtering, "
            L"VSync off, ultra low latency, ReBAR, P2 off) and AMD Anti-Lag / ULPS.");
    ReportPhase(L"GPU", L"writing Windows / driver graphics settings");
    Out(L"Applying GPU driver optimization tweaks.");

    std::vector<const GpuTweak*> plan;
    for (const auto& t : kGpuTweaks) {
        if (TweakApplies(t, probe)) plan.push_back(&t);
    }
    if (plan.empty()) {
        Err(L"No GPU tweaks apply to the adapters on this machine.");
        return false;
    }

    std::vector<gui::PpmStepView> steps;
    steps.reserve(plan.size());
    for (const auto* t : plan) {
        gui::PpmStepView s;
        s.alias = t->alias;
        s.title = t->title;
        s.why = t->why;
        s.change = t->change;
        s.status = 0;
        steps.push_back(std::move(s));
    }
    PublishPpm(steps);
    Dwell(1600);

    NvDrs nv;
    const bool nvOk = probe.nvidia && nv.Open();
    if (probe.nvidia && !nvOk) {
        Out(L"  nvapi64.dll could not open a DRS session; NVIDIA control-panel (DRS) values "
            L"will be retried via registry only.");
    }

    int wrote = 0, failed = 0;
    for (size_t i = 0; i < plan.size(); ++i) {
        if (FeedCancelled()) {
            nv.Close(wrote > 0);
            return false;
        }
        const GpuTweak& t = *plan[i];
        steps[i].status = 1;
        PublishPpm(steps);
        Narrate(t.title, t.why);
        ReportPhase(L"GPU", Format(L"%s  %s", t.alias, t.change));
        Out(L"");
        Out(Format(L"  %s (%s)", t.title, t.alias));
        Out(Format(L"    %s", t.why));
        Out(Format(L"    %s", t.change));
        Dwell(t.dwellMs);
        if (FeedCancelled()) {
            nv.Close(wrote > 0);
            return false;
        }

        bool ok = false;
        if (t.kind == GpuTweak::Kind::RegDword) {
            if (t.perAdapter) {
                const auto keys = DisplayAdapterKeys(t.vendor);
                int n = 0;
                for (size_t k = 0; k < keys.size(); ++k) {
                    const std::wstring tag = Format(L"%s_%02u", t.alias, static_cast<unsigned>(k));
                    if (WriteRegDwordTracked(tag.c_str(), keys[k].c_str(), t.regName, t.target)) ++n;
                }
                ok = n > 0;
                if (ok) Out(Format(L"    wrote on %d adapter key(s).", n));
            } else {
                HKEY hive = HKEY_LOCAL_MACHINE;
                const wchar_t* key = t.regKey;
                // Game Bar / GameDVR per-user copies also live under HKCU.
                if (key && wcsstr(key, L"GameBar")) {
                    BackupRegDword(t.alias, key, t.regName);
                    ok = RegSetDword(HKEY_CURRENT_USER, key, t.regName, t.target);
                    WriteRegDwordTracked(Format(L"%s_LM", t.alias).c_str(), key, t.regName, t.target);
                } else if (key && wcsstr(key, L"CurrentVersion\\GameDVR")) {
                    ok = RegSetDword(HKEY_CURRENT_USER, key, t.regName, t.target);
                    WriteRegDwordTracked(t.alias, key, t.regName, t.target);
                } else {
                    (void)hive;
                    ok = WriteRegDwordTracked(t.alias, key, t.regName, t.target);
                }
            }
        } else if (t.kind == GpuTweak::Kind::NvDrs) {
            DWORD prev = 0;
            const bool had = nvOk && nv.Get(t.nvId, prev);
            if (had) {
                RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"GpuDrs_%08X", t.nvId).c_str(), prev);
                RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"GpuDrs_%08X_had", t.nvId).c_str(), 1);
            }
            ok = nvOk && nv.Set(t.nvId, t.target);
            if (!ok) Out(L"    NVAPI SetSetting failed (driver too old, or this ID is not in the profile).");
        } else if (t.kind == GpuTweak::Kind::Service) {
            ok = DisableServiceTracked(t.serviceName);
            if (!ok) Out(L"    service not present (already gone, or this driver did not install it).");
            // Not-present telemetry is a success: the bloat is not running.
            if (!ok && t.serviceName) {
                SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
                SC_HANDLE svc = scm ? OpenServiceW(scm, t.serviceName, SERVICE_QUERY_STATUS) : nullptr;
                if (!svc) ok = true;
                if (svc) CloseServiceHandle(svc);
                if (scm) CloseServiceHandle(scm);
            }
        }

        steps[i].status = ok ? 2 : 3;
        PublishPpm(steps);
        if (ok) {
            Out(L"    written.");
            ++wrote;
        } else {
            ++failed;
        }
        Dwell(500);
    }

    nv.Close(wrote > 0);
    if (!wrote) {
        Err(L"No GPU setting could be written.");
        return false;
    }
    RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"GpuApplied", 1);
    Narrate(L"GPU tweaks applied",
            Format(L"Wrote %d graphics setting(s)%s. Restart so HAGS and ReBAR take effect.",
                   wrote, failed ? Format(L", %d did not confirm", failed).c_str() : L""));
    Out(Format(L"Applied %d GPU tweak(s)%s. Reboot for HAGS / ReBAR. Uninstall restores the previous values.",
               wrote, failed ? Format(L", %d failed", failed).c_str() : L""));
    Dwell(1600);
    return true;
}

bool ApplyAfterburner() {
    Narrate(L"Afterburner",
            L"The Afterburner download and OC scan take about 30 minutes. MSI Afterburner and RivaTuner "
            L"are installed if they are missing. On an NVIDIA GPU the scanner starts on its own. The curve "
            L"is saved as profile 1 and applied at every logon.");
    ReportPhase(L"AFTERBURNER", L"download and scan, about 30 minutes");
    Out(L"Installing MSI Afterburner and RivaTuner, then scanning a startup overclock. This takes about 30 minutes.");
    std::wstring err;
    if (!gui::InstallAfterburner(g_feed, err)) {
        Err(err.empty() ? L"Afterburner did not install." : err);
        return false;
    }
    Out(L"Afterburner and RivaTuner will apply profile 1 at every logon.");
    return true;
}

bool RestoreGpuTweaks() {
    gui::RemoveAfterburnerStartup();
    DWORD applied = 0;
    if (!RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"GpuApplied", applied) || !applied) return true;
    int restored = 0;

    NvDrs nv;
    const bool nvOk = nv.Open();
    for (const auto& t : kGpuTweaks) {
        if (t.kind == GpuTweak::Kind::NvDrs) {
            DWORD flag = 0, value = 0;
            if (nvOk &&
                RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"GpuDrs_%08X_had", t.nvId).c_str(), flag) &&
                flag &&
                RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"GpuDrs_%08X", t.nvId).c_str(), value)) {
                if (nv.Set(t.nvId, value)) ++restored;
            }
            continue;
        }
        if (t.kind == GpuTweak::Kind::Service && t.serviceName) {
            DWORD start = 0;
            if (RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"GpuSvc_%s_start", t.serviceName).c_str(), start) &&
                start && start != SERVICE_DISABLED) {
                SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
                SC_HANDLE svc = scm ? OpenServiceW(scm, t.serviceName, SERVICE_CHANGE_CONFIG | SERVICE_START) : nullptr;
                if (svc) {
                    if (ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, start, SERVICE_NO_CHANGE, nullptr, nullptr,
                                             nullptr, nullptr, nullptr, nullptr, nullptr)) {
                        StartServiceW(svc, 0, nullptr);
                        ++restored;
                    }
                    CloseServiceHandle(svc);
                }
                if (scm) CloseServiceHandle(scm);
            }
            continue;
        }
        auto restoreOne = [&](const std::wstring& tag) {
            DWORD had = 0;
            if (!RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Gpu_%s_had", tag.c_str()).c_str(), had)) return;
            std::wstring key, name;
            if (!RegGetString(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Gpu_%s_key", tag.c_str()).c_str(), key)) return;
            if (!RegGetString(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Gpu_%s_name", tag.c_str()).c_str(), name)) return;
            if (had) {
                DWORD value = 0;
                if (RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, Format(L"Gpu_%s", tag.c_str()).c_str(), value)) {
                    if (RegSetDword(HKEY_LOCAL_MACHINE, key.c_str(), name.c_str(), value)) ++restored;
                }
            } else {
                if (RegDeleteValueName(HKEY_LOCAL_MACHINE, key.c_str(), name.c_str())) ++restored;
            }
        };
        restoreOne(t.alias);
        restoreOne(Format(L"%s_LM", t.alias));
        for (unsigned k = 0; k < 32; ++k) restoreOne(Format(L"%s_%02u", t.alias, k));
    }
    nv.Close(true);
    Out(Format(L"Restored %d GPU setting(s) to their previous values.", restored));
    return true;
}

bool UninstallService() {
    bool ok = true;
    if (ServiceExists()) {
        Out(L"Stopping service...");
        StopInstalledService();
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        SC_HANDLE svc = scm ? OpenServiceW(scm, kServiceName, DELETE | SERVICE_QUERY_STATUS) : nullptr;
        if (svc) {
            if (!DeleteService(svc)) {
                Err(Format(L"DeleteService failed (error %lu).", GetLastError()));
                ok = false;
            }
            CloseServiceHandle(svc);
        }
        if (scm) CloseServiceHandle(scm);
    } else {
        Out(L"Service is not installed.");
    }

    DeleteServiceNamed(kLegacyServiceName);
    RestorePpmTweaks();
    RestoreGpuTweaks();
    RestoreMemoryTweaks();
    RestoreStorageTweaks();

    // Restore GlobalTimerResolutionRequests to whatever it was before us.
    DWORD saved = 0, hadValue = 0, previous = 0;
    if (RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"GlobalFlagSaved", saved) && saved) {
        RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"GlobalFlagHadValue", hadValue);
        RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"GlobalFlagPrevious", previous);
        if (hadValue) {
            RegSetDword(HKEY_LOCAL_MACHINE, kKernelRegKey, kGlobalFlagName, previous);
        } else {
            HKEY handle = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kKernelRegKey, 0, KEY_SET_VALUE, &handle) == ERROR_SUCCESS) {
                RegDeleteValueW(handle, kGlobalFlagName);
                RegCloseKey(handle);
            }
        }
        Out(L"Restored GlobalTimerResolutionRequests to its previous value "
            L"(reboot to revert the system-wide effect).");
    }
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kAppRegKey);

    // Remove the installed copy. If we *are* the installed copy we cannot
    // delete a running image; schedule it for the next reboot instead.
    const std::wstring installPath = GetInstallPath();
    if (EqualPath(GetExePath(), installPath)) {
        MoveFileExW(installPath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        Out(L"The running executable will be deleted on the next reboot.");
    } else if (GetFileAttributesW(installPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (DeleteFileW(installPath.c_str())) {
            RemoveDirectoryW(GetInstallDir().c_str());
            Out(Format(L"Removed %s", installPath.c_str()));
        } else {
            Err(Format(L"Could not delete %s (error %lu); delete it manually.",
                       installPath.c_str(), GetLastError()));
        }
    }
    return ok;
}

// ---------------------------------------------------------------- benchmark

struct BenchmarkResult {
    ULONG winner = 0;          // requested units to install (0 = no safe choice)
    Stats winnerStats{};       // back-to-back pattern
    Stats winnerIdleStats{};   // idle-gap pattern
    double winnerScore = 0;
    Stats baseline{};
    ULONG optimum = 0;         // best measured resolution (smoothed sweep score)
    ULONG boundary = 0;        // lowest resolution that never missed a tick (0 = none in range)
    ULONG margin = 0;          // safety margin applied above the optimum
    bool atRangeEnd = false;   // optimum sat on the last candidate: range may be too narrow
    std::wstring failure;      // why winner == 0
    std::wstring csvPath;
};

struct BenchmarkConfig {
    ULONG from = 0;            // 100-ns units; 0 = kernel fastest
    ULONG to = 0;              // 100-ns units; 0 = from + 1000 (+0.1 ms)
    ULONG step = 10;           // 100-ns units (0.001 ms)
    int samples = 150;         // high-fidelity reference; medium (the default) divides this by 8
    int passes = 3;
    int verifySamples = 1500;  // high-fidelity reference; medium divides this by 8
    ULONG margin = 0;          // 100-ns units; used only when marginSet
    bool marginSet = false;
};

constexpr double kSweepCleanLatePct = 0.5;    // <= 1 of 200 sweep samples may miss
constexpr double kVerifyCleanLatePct = 0.2;   // <= 3 of 1500 verification samples
constexpr ULONG kMinAutoMarginUnits = 50;     // 0.005 ms
constexpr ULONG kMaxAutoMarginUnits = 300;    // 0.030 ms
constexpr int kVerifyRounds = 10;

// Deterministic Fisher-Yates so a shuffled pass is reproducible.
void ShuffleOrder(std::vector<size_t>& order, uint32_t seed) {
    uint32_t state = seed * 2654435761u + 12345u;
    for (size_t i = order.size(); i > 1; --i) {
        state = state * 1664525u + 1013904223u;
        std::swap(order[i - 1], order[(state >> 8) % i]);
    }
}

std::wstring CsvStats(const Stats& s) {
    return Format(L"%lu,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.3f,%.5f,%d", static_cast<unsigned long>(s.n),
                  s.median, s.mean, s.stdev, s.p1, s.p95, s.p99, s.max, s.latePct, s.score, s.clean ? 1 : 0);
}

std::wstring CsvLine(const wchar_t* phase, const Candidate& c) {
    return Format(L"%s,%lu,%lu,%.4f,%s,%s,%.5f,%d,%lu,%lu\r\n", phase, c.requested, c.actual,
                  UnitsToMs(c.requested), CsvStats(c.loopStats).c_str(), CsvStats(c.idleStats).c_str(),
                  c.score, c.clean ? 1 : 0, static_cast<unsigned long>(c.dropped), c.interferer);
}

std::wstring CsvFileName() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return ProgramDataDir() + Format(L"\\benchmark-%04u%02u%02u-%02u%02u%02u.csv", st.wYear, st.wMonth,
                                     st.wDay, st.wHour, st.wMinute, st.wSecond);
}

void WriteCsv(const std::wstring& path, const std::vector<Candidate>& sweep,
              const std::vector<Candidate>& verify) {
    CreateDirectoryW(ProgramDataDir().c_str(), nullptr);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const wchar_t* statCols[] = {L"n", L"median_ms", L"mean_ms", L"stdev_ms", L"p1_ms", L"p95_ms",
                                 L"p99_ms", L"max_ms", L"late_pct", L"score_ms", L"clean"};
    std::wstring header = L"phase,requested_units,actual_units,requested_ms";
    for (const wchar_t* prefix : {L"loop_", L"idle_"}) {
        for (const wchar_t* col : statCols) header += Format(L",%s%s", prefix, col);
    }
    header += L",score_ms,clean,dropped,interferer_units\r\n";
    std::string text = ToUtf8(header);
    for (const auto& c : sweep) text += ToUtf8(CsvLine(L"sweep", c));
    for (const auto& c : verify) text += ToUtf8(CsvLine(L"verify", c));
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

// Dense multi-pass sweep -> boundary + margin -> interleaved verification.
BenchmarkResult RunBenchmark(const BenchmarkConfig& cfg) {
    BenchmarkResult result;
    ULONG slowest = 0, fastest = 0, current = 0;
    if (!QueryResolutions(slowest, fastest, current)) {
        result.failure = L"NtQueryTimerResolution failed.";
        return result;
    }

    // ---- candidate grid -------------------------------------------------
    ULONG from = cfg.from ? cfg.from : fastest;
    ULONG to = cfg.to ? cfg.to : from + 1000;
    from = std::max(from, fastest);
    if (slowest) to = std::min(to, slowest);
    if (to <= from) to = from + cfg.step;
    std::vector<Candidate> sweep;
    for (ULONG r = from; r < to; r += cfg.step) sweep.push_back(Candidate{r});
    sweep.push_back(Candidate{to});
    const size_t count = sweep.size();

    // Per candidate per block: `samples` back-to-back + `samples/2` idle-gap.
    const int loopCount = cfg.samples;
    const int idleCount = std::max(cfg.samples / 2, 5);
    const int verifyLoop = cfg.verifySamples;
    const int verifyIdle = std::max(cfg.verifySamples / 2, 10);
    const double blockMs = loopCount * 1.15 + idleCount * (1.15 + 3.5) + 30.0;
    const double verifyBlockMs = verifyLoop * 1.15 + verifyIdle * (1.15 + 3.5) + kVerifyRounds * 30.0;
    const double sweepSeconds = static_cast<double>(count) * cfg.passes * blockMs / 1000.0;
    const double verifySeconds = 6.0 * verifyBlockMs / 1000.0;

    if (g_feed) {
        gui::SystemView sys;
        sys.fastest = fastest;
        sys.slowest = slowest;
        sys.current = current;
        sys.admin = IsAdmin();
        sys.serviceState = ServiceState();
        DWORD desired = 0;
        RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"DesiredResolution", desired);
        sys.serviceResolution = desired;
        g_feed->System(sys);
    }
    ReportPhase(L"SWEEP", Format(L"%.4f-%.4f ms in %.4f ms steps, %d pass%s",
                                 UnitsToMs(from), UnitsToMs(to), UnitsToMs(cfg.step), cfg.passes,
                                 cfg.passes == 1 ? L"" : L"es"));
    Narrate(L"Why timer resolution matters",
            L"Windows Sleep(1) waits for the next timer interrupt at least 1 ms away. The default clock "
            L"is ~15.6 ms, so Sleep(1) often wakes ~15 ms late. This step times real Sleep(1) calls across "
            L"finer periods and keeps the fastest setting for this machine.");

    Out(Format(L"Kernel timer range: fastest %.4f ms, slowest %.4f ms, system currently at %.4f ms",
               UnitsToMs(fastest), UnitsToMs(slowest), UnitsToMs(current)));
    Out(Format(L"Sweep: %.4f-%.4f ms in %.4f ms steps = %lu candidates x %d passes x (%d back-to-back + "
               L"%d idle-gap) samples = %lu Sleep(1) calls",
               UnitsToMs(from), UnitsToMs(to), UnitsToMs(cfg.step), static_cast<unsigned long>(count),
               cfg.passes, loopCount, idleCount,
               static_cast<unsigned long>(count * static_cast<size_t>(loopCount + idleCount) * cfg.passes)));
    Out(Format(L"Verification: up to 6 finalists x (%d + %d) samples, interleaved in %d rounds. "
               L"Estimated time ~%s",
               verifyLoop, verifyIdle, kVerifyRounds, FormatDuration(sweepSeconds + verifySeconds + 2).c_str()));
    if (current < to) {
        Out(Format(L"WARNING: some process already holds %.4f ms, inside the sweep range. "
                   L"Candidates it masks will be skipped; close games/browsers for a clean run.",
                   UnitsToMs(current)));
    }

    const std::wstring prio = RaiseBenchmarkPriority();
    Out(Format(L"Measurement priority: %s", prio.c_str()));
    if (g_feed) {
        gui::SystemView sys;
        ULONG cur = current;
        QueryResolutions(slowest, fastest, cur);
        sys.fastest = fastest;
        sys.slowest = slowest;
        sys.current = cur;
        sys.admin = IsAdmin();
        sys.serviceState = ServiceState();
        DWORD desired = 0;
        RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"DesiredResolution", desired);
        sys.serviceResolution = desired;
        sys.priority = prio;
        g_feed->System(sys);
    }
    DisablePowerThrottling();
    PublishSweep(sweep, -1);

    // ---- baseline -------------------------------------------------------
    {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        std::vector<double> samples;
        ReportPhase(L"BASELINE", L"Sleep(1) with nothing requested by this process");
        Narrate(L"Untuned baseline",
                L"Sleep(1) with no timer request \u2014 the ~15.6 ms default clock. Later scores are "
                L"compared against this overshoot.");
        Progress(L"Baseline (no request)...");
        for (int i = 0; i < 40; ++i) {
            if (FeedCancelled()) {
                result.failure = L"Cancelled.";
                return result;
            }
            double v = 0;
            TimeOneSleep(freq, 0, v, true);
            samples.push_back(v);
            Sleep(2);
        }
        ULONG reported = 0;
        QueryResolutions(slowest, fastest, reported);
        result.baseline = ComputeStats(samples, reported ? reported : slowest, 100.0, 2);
        if (g_feed) g_feed->Baseline(ViewOf(result.baseline), reported);
        ProgressEnd();
        Out(Format(L"\nBaseline, nothing requested by this process: Sleep(1) overshoot median %+0.4f ms "
                   L"(p99 %+0.4f) while the kernel reports %.4f ms",
                   result.baseline.median, result.baseline.p99, UnitsToMs(reported)));
        if (reported < slowest && result.baseline.median > UnitsToMs(reported) * 2) {
            Out(L"  (another process requested that resolution; since Windows 10 2004 requests are "
                L"per-process, so this process still waits at the default until it asks itself)");
        }
    }

    // ---- dense sweep, several passes in different orders ----------------
    const ULONGLONG t0 = GetTickCount64();
    std::vector<size_t> order(count);
    for (size_t i = 0; i < count; ++i) order[i] = i;
    for (int pass = 0; pass < cfg.passes; ++pass) {
        const wchar_t* orderName = L"ascending";
        if (pass % 3 == 1) {
            std::reverse(order.begin(), order.end());
            orderName = L"descending";
        } else if (pass % 3 == 2) {
            std::sort(order.begin(), order.end());
            ShuffleOrder(order, static_cast<uint32_t>(pass));
            orderName = L"shuffled";
        } else {
            std::sort(order.begin(), order.end());
        }

        ULONG preexisting = 0;
        QueryResolutions(slowest, fastest, preexisting);
        PublishSweep(sweep, -1, -1, -1, -1, 0, nullptr, pass + 1, cfg.passes, 0);

        for (size_t k = 0; k < count; ++k) {
            Candidate& c = sweep[order[k]];
            const double elapsed = static_cast<double>(GetTickCount64() - t0) / 1000.0;
            const double done = static_cast<double>(pass) * static_cast<double>(count) + static_cast<double>(k);
            const double total = static_cast<double>(cfg.passes) * static_cast<double>(count);
            const double eta = done > 0 ? elapsed * (total - done) / done : sweepSeconds;
            const double sweepShare = sweepSeconds / (sweepSeconds + verifySeconds + 1.0);
            const double frac = (done / total) * sweepShare;
            ReportProgress(frac, elapsed, eta,
                           Format(L"Sweep pass %d/%d (%s)  %.4f ms  [%lu/%lu]  elapsed %s  remaining ~%s",
                                  pass + 1, cfg.passes, orderName, UnitsToMs(c.requested),
                                  static_cast<unsigned long>(k + 1), static_cast<unsigned long>(count),
                                  FormatDuration(elapsed).c_str(), FormatDuration(eta).c_str()));
            ReportPhase(L"SWEEP", Format(L"pass %d/%d (%s)  %.4f ms", pass + 1, cfg.passes, orderName,
                                         UnitsToMs(c.requested)));
            {
                const double r = UnitsToMs(c.requested);
                const double expected = std::max(0.0, 2.0 * r - 1.0);
                Narrate(Format(L"Trying %.4f ms  \u00B7  pass %d/%d %s", r, pass + 1, cfg.passes, orderName),
                        Format(L"Sleep(1) waits for the first interrupt at this period that is at least 1 ms away "
                               L"\u2014 typically %+0.3f ms late. A miss (C-state exit or a long DPC) waits one extra "
                               L"tick. Finer is not always faster: 0.50 ms often misses more ticks than ~0.52\u20130.56 ms. "
                               L"Score = mean + stdev of that overshoot. Lower and tighter is less lag.",
                               expected));
            }
            if (FeedCancelled()) {
                result.failure = L"Cancelled.";
                return result;
            }
            if (MeasureBlock(c, loopCount, idleCount, preexisting) != SampleStatus::Ok) ++c.skippedPasses;
            if (FeedCancelled()) {
                result.failure = L"Cancelled.";
                return result;
            }
            ComputeCandidateStats(c, kSweepCleanLatePct, static_cast<size_t>(loopCount));
            PublishSweep(sweep, static_cast<int>(order[k]), -1, -1, -1, 0, nullptr, pass + 1, cfg.passes,
                         static_cast<int>(k + 1));
        }
    }
    ProgressEnd();

    size_t droppedTotal = 0;
    for (auto& c : sweep) {
        ComputeCandidateStats(c, kSweepCleanLatePct, static_cast<size_t>(loopCount));
        droppedTotal += c.dropped;
    }

    // ---- analysis -----------------------------------------------------------
    auto hasData = [&](size_t i) { return i < count && sweep[i].HasData(); };

    // Measured optimum: lowest score, smoothed over the +-2 neighbouring
    // grid points so a lucky or unlucky run of samples cannot crown an
    // outlier. Ties go to the longer period.
    std::vector<double> smooth(count, 1e9);
    for (size_t i = 0; i < count; ++i) {
        if (!hasData(i)) continue;
        double sum = 0;
        int n = 0;
        for (size_t j = (i >= 2 ? i - 2 : 0); j <= i + 2 && j < count; ++j) {
            if (!hasData(j)) continue;
            sum += sweep[j].score;
            ++n;
        }
        smooth[i] = sum / n;
    }
    size_t bestIdx = count;
    for (size_t i = 0; i < count; ++i) {
        if (smooth[i] < 1e9 && (bestIdx == count || smooth[i] <= smooth[bestIdx])) bestIdx = i;
    }

    // Optional: the first resolution that never misses a tick ("clean" and
    // in the fast cluster, so a candidate that misses *every* tick cannot
    // pose as clean). Exists on quiet systems; absent when wake-up latency
    // is high enough that every resolution misses some ticks.
    double bestCleanMedian = 1e9;
    for (const auto& c : sweep) {
        if (c.HasData() && c.clean) bestCleanMedian = std::min(bestCleanMedian, c.loopStats.median);
    }
    auto cleanLow = [&](size_t i) {
        return hasData(i) && sweep[i].clean &&
               sweep[i].loopStats.median <= bestCleanMedian + UnitsToMs(sweep[i].requested) / 2.0;
    };
    size_t boundaryIdx = count;
    for (size_t i = 0; i < count; ++i) {
        bool stable = cleanLow(i);
        for (size_t j = i + 1; stable && j < std::min(i + 3, count); ++j) stable = cleanLow(j);
        if (stable) {
            boundaryIdx = i;
            break;
        }
    }
    size_t lastMissIdx = count;
    if (boundaryIdx != count) {
        for (size_t i = boundaryIdx; i-- > 0;) {
            if (hasData(i) && !cleanLow(i)) {
                lastMissIdx = i;
                break;
            }
        }
    }

    Out(Format(L"\nSweep results, %d passes merged. Overshoot is ms beyond the requested 1 ms; late%% = share "
               L"of samples that missed a tick (slept > median + half a tick); score = mean + stdev "
               L"averaged over both patterns:", cfg.passes));
    PrintTableHeader();
    size_t skipped = 0;
    for (size_t i = 0; i < count; ++i) {
        const Candidate& c = sweep[i];
        if (!c.HasData()) ++skipped;
        std::wstring note;
        if (i == bestIdx) note = L"<- best measured (smoothed)";
        if (i == boundaryIdx) note += L" <- first resolution that never misses a tick";
        if (c.skippedPasses && hasData(i)) note += Format(L" (%d pass(es) skipped)", c.skippedPasses);
        PrintRow(c, note);
    }
    if (skipped) {
        Out(Format(L"WARNING: %lu candidate(s) could not be measured because another process held a finer "
                   L"resolution for the whole run.", static_cast<unsigned long>(skipped)));
    }
    if (droppedTotal) {
        Out(Format(L"NOTE: %lu sample(s) were discarded because another process changed the clock while they "
                   L"were being taken.", static_cast<unsigned long>(droppedTotal)));
    }

    result.csvPath = CsvFileName();

    if (bestIdx == count) {
        result.failure = L"No candidate could be measured (another process held the clock for the whole "
                         L"run). Close it, or stop the Synchronus service, and re-run.";
        WriteCsv(result.csvPath, sweep, {});
        return result;
    }

    const ULONG optimum = sweep[bestIdx].requested;
    result.optimum = optimum;
    result.atRangeEnd = bestIdx + 1 == count;
    if (boundaryIdx != count) result.boundary = sweep[boundaryIdx].requested;

    // Safety margin above the measured optimum: a load-induced increase in
    // wake-up latency shifts every curve towards longer periods, so sit a
    // little to the right of the measured minimum. Cheap: the mean rises by
    // only ~2 us per 1 us of period. When a clean boundary exists, the width
    // of its transition band is the natural measure of how jittery the edge
    // is; otherwise use the minimum.
    ULONG transition = 0;
    if (boundaryIdx != count && lastMissIdx != count) {
        transition = sweep[boundaryIdx].requested - sweep[lastMissIdx].requested;
    }
    const ULONG margin = cfg.marginSet
                             ? cfg.margin
                             : std::min(kMaxAutoMarginUnits, std::max(kMinAutoMarginUnits, transition));
    size_t targetIdx = count - 1;
    for (size_t i = bestIdx; i < count; ++i) {
        if (hasData(i) && sweep[i].requested >= optimum + margin) {
            targetIdx = i;
            break;
        }
    }
    if (!hasData(targetIdx)) targetIdx = bestIdx;
    const ULONG target = sweep[targetIdx].requested;

    Out(Format(L"\nBest measured: %.4f ms (smoothed score %.4f ms).", UnitsToMs(optimum), smooth[bestIdx]));
    if (boundaryIdx != count) {
        Out(Format(L"First resolution that never missed a tick: %.4f ms%s.", UnitsToMs(result.boundary),
                   lastMissIdx != count
                       ? Format(L" (last misses at %.4f ms; transition %.4f ms wide)",
                                UnitsToMs(sweep[lastMissIdx].requested), UnitsToMs(transition)).c_str()
                       : L" (no misses anywhere in the range)"));
    } else {
        Out(L"Every resolution in the range missed some ticks: wake-up latency on this system "
            L"(C-state exit, DPCs) sometimes exceeds the slack 2R - 1 ms, so the score weighs each "
            L"candidate's miss rate against its base overshoot.");
    }
    if (result.atRangeEnd) {
        Out(Format(L"NOTE: the optimum sits on the last candidate; it may lie beyond the range. Re-run with "
                   L"--to %lu to check.", to + 1000));
    }
    Out(Format(L"Safety margin: +%.4f ms%s -> target %.4f ms", UnitsToMs(margin),
               cfg.marginSet ? L" (--margin)"
                             : (transition > kMinAutoMarginUnits ? L" (= transition width)" : L" (minimum)"),
               UnitsToMs(target)));
    PublishSweep(sweep, -1, static_cast<int>(bestIdx),
                 boundaryIdx == count ? -1 : static_cast<int>(boundaryIdx), static_cast<int>(targetIdx), margin,
                 &smooth);
    if (!cfg.marginSet && transition > kMaxAutoMarginUnits) {
        Out(L"WARNING: the transition band is unusually wide (noisy system?); the automatic margin was "
            L"capped. Re-run on an idle system or set --margin explicitly.");
    }

    // ---- finalists ------------------------------------------------------------
    std::vector<size_t> finalistIdx;
    auto addFinalist = [&](size_t i) {
        if (hasData(i) && std::find(finalistIdx.begin(), finalistIdx.end(), i) == finalistIdx.end()) {
            finalistIdx.push_back(i);
        }
    };
    addFinalist(targetIdx);
    addFinalist(bestIdx);
    addFinalist(targetIdx + 1);
    if (bestIdx > 0) addFinalist(bestIdx - 1);      // evidence: the edge below the optimum
    if (boundaryIdx != count) addFinalist(boundaryIdx);
    std::vector<size_t> ranked;
    for (size_t i = 0; i < count; ++i) if (hasData(i)) ranked.push_back(i);
    std::sort(ranked.begin(), ranked.end(), [&](size_t a, size_t b) {
        if (sweep[a].score != sweep[b].score) return sweep[a].score < sweep[b].score;
        return sweep[a].requested > sweep[b].requested;
    });
    for (size_t i = 0; i < ranked.size() && finalistIdx.size() < 6; ++i) addFinalist(ranked[i]);
    std::sort(finalistIdx.begin(), finalistIdx.end());

    std::vector<Candidate> verify;
    for (size_t i : finalistIdx) verify.push_back(Candidate{sweep[i].requested});

    // ---- interleaved verification -----------------------------------------
    const int loopBlock = (verifyLoop + kVerifyRounds - 1) / kVerifyRounds;
    const int idleBlock = (verifyIdle + kVerifyRounds - 1) / kVerifyRounds;
    Out(Format(L"\nVerification: %lu finalists x (%d back-to-back + %d idle-gap) samples, interleaved in %d "
               L"rounds so they see identical conditions:", static_cast<unsigned long>(verify.size()),
               loopBlock * kVerifyRounds, idleBlock * kVerifyRounds, kVerifyRounds));
    ReportPhase(L"VERIFICATION", Format(L"%lu finalists, %d interleaved rounds",
                                        static_cast<unsigned long>(verify.size()), kVerifyRounds));
    Narrate(L"Interleaved verification",
            L"Candidates take turns in the same round so background noise hits them equally. Only a "
            L"period at or above the safety margin can be installed.");
    PublishVerify(verify, -1, target);
    const ULONGLONG t1 = GetTickCount64();
    const double sweepShare = sweepSeconds / (sweepSeconds + verifySeconds + 1.0);
    for (int round = 0; round < kVerifyRounds; ++round) {
        ULONG preexisting = 0;
        QueryResolutions(slowest, fastest, preexisting);
        for (size_t k = 0; k < verify.size(); ++k) {
            const size_t vi = (k + static_cast<size_t>(round)) % verify.size();
            Candidate& c = verify[vi];
            const double elapsed = static_cast<double>(GetTickCount64() - t1) / 1000.0;
            const double remaining = round > 0 ? elapsed * (kVerifyRounds - round) / round : verifySeconds;
            ReportProgress(sweepShare + (1.0 - sweepShare) * (static_cast<double>(round) / kVerifyRounds),
                           elapsed, remaining,
                           Format(L"Verification round %d/%d  %.4f ms  elapsed %s  remaining ~%s", round + 1,
                                  kVerifyRounds, UnitsToMs(c.requested), FormatDuration(elapsed).c_str(),
                                  FormatDuration(remaining).c_str()));
            ReportPhase(L"VERIFICATION", Format(L"round %d/%d  %.4f ms", round + 1, kVerifyRounds,
                                                UnitsToMs(c.requested)));
            Narrate(Format(L"Verifying %.4f ms  \u00B7  round %d/%d", UnitsToMs(c.requested), round + 1,
                           kVerifyRounds),
                    L"Same Sleep(1) measurement, but every finalist sees identical conditions this round. "
                    L"We will install the lowest-score eligible period (ties prefer the longer one \u2014 "
                    L"fewer wake-ups, more headroom).");
            if (FeedCancelled()) {
                result.failure = L"Cancelled.";
                return result;
            }
            if (MeasureBlock(c, loopBlock, idleBlock, preexisting) != SampleStatus::Ok) ++c.skippedPasses;
            ComputeCandidateStats(c, kVerifyCleanLatePct, static_cast<size_t>(verifyLoop) / 2);
            PublishVerify(verify, static_cast<int>(vi), target);
        }
    }
    ProgressEnd();
    for (auto& c : verify) {
        ComputeCandidateStats(c, kVerifyCleanLatePct, static_cast<size_t>(verifyLoop) / 2);
    }

    // ---- final selection ------------------------------------------------------
    // Only finalists at or above the margin target are eligible; the ones
    // below were verified as evidence. Among the eligible, take the lowest
    // verified score, then prefer the longest period whose score is within
    // 3% of it (indistinguishable, fewer wake-ups, more headroom).
    const Candidate* chosen = nullptr;
    for (const auto& c : verify) {
        if (!c.HasData() || c.requested < target) continue;
        if (!chosen || c.score < chosen->score) chosen = &c;
    }
    if (!chosen) {
        for (const auto& c : verify) {
            if (c.HasData() && (!chosen || c.score < chosen->score)) chosen = &c;
        }
    }
    if (chosen) {
        for (const auto& c : verify) {
            if (c.HasData() && c.requested > chosen->requested && c.requested >= target &&
                c.score <= chosen->score * 1.03) {
                chosen = &c;
            }
        }
    }

    PrintTableHeader();
    for (const auto& c : verify) {
        std::wstring note;
        if (c.requested < target) note = L"below margin target (evidence only)";
        if (c.requested == optimum) note = L"best measured in sweep";
        if (chosen && c.requested == chosen->requested) note = L"<- SELECTED";
        PrintRow(c, note);
    }
    WriteCsv(result.csvPath, sweep, verify);

    if (!chosen) {
        result.failure = L"Verification could not measure any finalist (another process took the clock). "
                         L"Close it and re-run.";
        return result;
    }

    result.winner = chosen->requested;
    result.winnerStats = chosen->loopStats;
    result.winnerIdleStats = chosen->idleStats;
    result.winnerScore = chosen->score;
    result.margin = chosen->requested >= optimum ? chosen->requested - optimum : 0;
    Narrate(L"Winner chosen",
            Format(L"%.4f ms is the fastest verified resolution on this machine "
                   L"(best measured %.4f ms + %.4f ms margin).",
                   UnitsToMs(result.winner), UnitsToMs(result.optimum), UnitsToMs(result.margin)));
    return result;
}

void PrintBenchmarkSummary(const BenchmarkResult& r) {
    const Stats& s = r.winnerStats;
    const Stats& d = r.winnerIdleStats;
    Out(Format(L"\nSelected: %lu (%.4f ms), score %.4f ms", r.winner, UnitsToMs(r.winner), r.winnerScore));
    Out(Format(L"  back-to-back Sleep(1): overshoot median %+0.4f ms, mean %+0.4f, stdev %0.4f, p99 %+0.4f; "
               L"%lu of %lu samples (%.2f%%) slept one tick longer",
               s.median, s.mean, s.stdev, s.p99, static_cast<unsigned long>(s.late),
               static_cast<unsigned long>(s.n), s.latePct));
    Out(Format(L"  after 2-5 ms idle:     overshoot median %+0.4f ms, mean %+0.4f, stdev %0.4f, p99 %+0.4f; "
               L"%lu of %lu samples (%.2f%%) slept one tick longer",
               d.median, d.mean, d.stdev, d.p99, static_cast<unsigned long>(d.late),
               static_cast<unsigned long>(d.n), d.latePct));
    Out(Format(L"  = best measured resolution (%.4f ms) + %.4f ms safety margin", UnitsToMs(r.optimum),
               UnitsToMs(r.margin)));
    if (r.boundary) {
        Out(Format(L"  first resolution that never missed a tick: %.4f ms", UnitsToMs(r.boundary)));
    } else {
        Out(L"  no resolution in the range was free of missed ticks; misses are wake-up latency (deep "
            L"C-states, long DPCs) exceeding the slack 2R - 1 ms. A High Performance power plan or "
            L"disabling deep C-states in firmware usually lowers the miss rate.");
    }
    if (r.baseline.n > 0) {
        Out(Format(L"  vs %+0.4f ms median overshoot with no resolution requested", r.baseline.median));
    }
    if (!r.csvPath.empty()) Out(Format(L"  per-candidate data: %s", r.csvPath.c_str()));
}

// ---------------------------------------------------------------- commands

void PrintHelp() {
    wprintf(
        L"Synchronus %s - timer resolution, CPU/GPU/memory/storage latency tuning\n"
        L"Software of %s  \u00B7  %s\n"
        L"GPLv3 - timer core based on https://github.com/amitxv/TimerResolution\n\n"
        L"Usage:\n"
        L"  Synchronus.exe                       Open the GUI: named-step tuning workflow.\n"
        L"  Synchronus.exe --cli                 The same default flow in a console (no GUI).\n"
        L"  Synchronus.exe --gui                 Force the GUI (default when no command is given).\n"
        L"  Synchronus.exe --benchmark           Benchmark only; makes no permanent changes.\n"
        L"  Synchronus.exe --install             Benchmark + install/update the service.\n"
        L"      [--resolution N]                 Skip the benchmark; use N (100-ns units,\n"
        L"                                       e.g. 5070 = 0.507 ms).\n"
        L"  Benchmark tuning (0.50-0.60 ms in 0.001 ms steps):\n"
        L"      --fidelity low|medium|high       Wakes timed at each interval. Default\n"
        L"                                       medium is 1/8 of high: 18 back-to-back\n"
        L"                                       per pass, half as many after an idle gap,\n"
        L"                                       3 passes, verify 187. About 35 seconds.\n"
        L"                                       High is 150 / 3 passes / verify 1500,\n"
        L"                                       about 3 min 20 s. Low is 8 samples,\n"
        L"                                       1 pass, verify 100, about 10 seconds.\n"
        L"      --from N / --to N                Sweep range in 100-ns units\n"
        L"                                       (default: kernel fastest .. +1000).\n"
        L"      --step N                         Grid step in 100-ns units (default 10).\n"
        L"      --samples N                      Back-to-back Sleep(1) samples per candidate per\n"
        L"                                       pass; half as many idle-gap samples are added.\n"
        L"                                       Overrides --fidelity for this count.\n"
        L"      --passes N                       Sweep passes, each in a different order.\n"
        L"                                       Overrides --fidelity (default 3, or 1 on low).\n"
        L"      --verify N                       Samples per finalist in the interleaved\n"
        L"                                       verification. Overrides --fidelity.\n"
        L"      --margin N                       Safety margin above the boundary in 100-ns\n"
        L"                                       units (default: auto, >= 50 = 0.005 ms).\n"
        L"      --quick                          Coarse preview (~20 s): step 20, 1 pass,\n"
        L"                                       30 samples, verify 300.\n"
        L"  Synchronus.exe --apply               Hold a resolution in this console until\n"
        L"      [--resolution N]                 Ctrl+C (default: kernel fastest). One-off,\n"
        L"                                       nothing is installed.\n"
        L"  Synchronus.exe --status              Show installed/current resolution state.\n"
        L"  Synchronus.exe --stop                Stop the service (keeps it installed).\n"
        L"  Synchronus.exe --start               Start the installed service.\n"
        L"  Synchronus.exe --uninstall           Remove the service and restore tweaks.\n"
        L"  Synchronus.exe --apply-prepare       Create a System Restore point only.\n"
        L"  Synchronus.exe --apply-ppm           Apply processor-power (PPM) tweaks only.\n"
        L"      --ppm                            Force them even if turbo/PBO is detected.\n"
        L"      --no-ppm                         Skip the CPU Tune step.\n"
        L"  Synchronus.exe --apply-gpu           Apply GPU driver / Windows graphics tweaks only.\n"
        L"      --gpu                            Force the GPU Config step.\n"
        L"      --no-gpu                         Skip the GPU Config step.\n"
        L"  Synchronus.exe --apply-afterburner  Install Afterburner and RivaTuner, scan, and\n"
        L"                                       apply the overclock at every logon. This step only.\n"
        L"      --afterburner                    Force that step in the full flow.\n"
        L"      --no-afterburner                 Skip the Afterburner step.\n"
        L"  Synchronus.exe --apply-memory        Pagefile + disable memory compression only.\n"
        L"  Synchronus.exe --apply-storage       PCIe/USB/disk/TRIM/SysMain tweaks only.\n"
        L"      --no-prepare / --no-memory / --no-storage\n"
        L"                                       Skip that step in the full flow.\n"
        L"  Synchronus.exe --yes                 Do not prompt for confirmation.\n"
        L"  Synchronus.exe --no-pause            Do not pause at the end, and skip the\n"
        L"                                       per-tweak dwell (scripting).\n"
        L"  Synchronus.exe --service             Internal: service entry point (SCM only).\n\n"
        L"Log: %s\n",
        kVersion, kCompany, kCompanyUrl, LogFilePath().c_str());
}

struct Options {
    std::wstring command;  // benchmark | install | apply | status | stop | start | uninstall | service | ppm | gpu | default
    ULONG resolution = 0;  // 0 = not specified
    BenchmarkConfig bench;
    bool yes = false;
    bool noPause = false;
    bool elevated = false;
    bool cli = false;
    bool gui = false;
    bool ppm = false;      // force PPM tweaks even on turbo CPUs
    bool noPpm = false;    // skip the PPM step
    bool gpu = false;      // force GPU tweaks
    bool noGpu = false;    // skip the GPU step
    bool afterburner = false;
    bool noAfterburner = false;
    bool noPrepare = false;
    bool noMemory = false;
    bool noStorage = false;
    int fidelity = 1;          // 0 low, 1 medium (default), 2 high
    bool fidelitySet = false;
    bool samplesSet = false;
    bool passesSet = false;
    bool verifySet = false;
};

bool ParseOptions(const std::vector<std::wstring>& args, Options& opts) {
    bool quick = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        // Parses the next argument as an unsigned number >= minimum.
        auto needValue = [&](ULONG& target, ULONG minimum, const wchar_t* what) {
            if (i + 1 >= args.size()) {
                Err(Format(L"%s requires a value.", what));
                return false;
            }
            wchar_t* end = nullptr;
            const unsigned long v = wcstoul(args[++i].c_str(), &end, 10);
            if (end == args[i].c_str() || *end != L'\0' || v < minimum) {
                Err(Format(L"%s requires a whole number >= %lu.", what, minimum));
                return false;
            }
            target = v;
            return true;
        };
        ULONG v = 0;
        if (a == L"--benchmark" || a == L"--install" || a == L"--apply" ||
            a == L"--status" || a == L"--stop" || a == L"--start" ||
            a == L"--uninstall" || a == L"--service") {
            opts.command = a.substr(2);
        } else if (a == L"--apply-ppm") {
            opts.command = L"ppm";
        } else if (a == L"--apply-gpu") {
            opts.command = L"gpu";
        } else if (a == L"--apply-afterburner") {
            opts.command = L"afterburner";
        } else if (a == L"--afterburner") {
            opts.afterburner = true;
        } else if (a == L"--no-afterburner") {
            opts.noAfterburner = true;
        } else if (a == L"--apply-prepare") {
            opts.command = L"prepare";
        } else if (a == L"--apply-memory") {
            opts.command = L"memory";
        } else if (a == L"--apply-storage") {
            opts.command = L"storage";
        } else if (a == L"--resolution") {
            if (!needValue(opts.resolution, 1, L"--resolution (100-ns units, e.g. 5070 = 0.507 ms)")) return false;
        } else if (a == L"--from") {
            if (!needValue(opts.bench.from, 1, L"--from (100-ns units)")) return false;
        } else if (a == L"--to") {
            if (!needValue(opts.bench.to, 1, L"--to (100-ns units)")) return false;
        } else if (a == L"--step") {
            if (!needValue(opts.bench.step, 1, L"--step (100-ns units)")) return false;
        } else if (a == L"--fidelity") {
            if (i + 1 >= args.size()) {
                Err(L"--fidelity requires low, medium, or high.");
                return false;
            }
            const std::wstring f = args[++i];
            if (f == L"low") opts.fidelity = 0;
            else if (f == L"med" || f == L"medium") opts.fidelity = 1;
            else if (f == L"high") opts.fidelity = 2;
            else {
                Err(L"--fidelity requires low, medium, or high.");
                return false;
            }
            opts.fidelitySet = true;
        } else if (a == L"--samples") {
            if (!needValue(v, 10, L"--samples")) return false;
            opts.bench.samples = static_cast<int>(v);
            opts.samplesSet = true;
        } else if (a == L"--passes") {
            if (!needValue(v, 1, L"--passes")) return false;
            opts.bench.passes = static_cast<int>(std::min<ULONG>(v, 20));
            opts.passesSet = true;
        } else if (a == L"--verify") {
            if (!needValue(v, 100, L"--verify")) return false;
            opts.bench.verifySamples = static_cast<int>(v);
            opts.verifySet = true;
        } else if (a == L"--margin") {
            if (!needValue(v, 0, L"--margin (100-ns units)")) return false;
            opts.bench.margin = v;
            opts.bench.marginSet = true;
        } else if (a == L"--quick") {
            quick = true;
        } else if (a == L"--cli") {
            opts.cli = true;
        } else if (a == L"--gui") {
            opts.gui = true;
        } else if (a == L"--ppm") {
            opts.ppm = true;
        } else if (a == L"--no-ppm") {
            opts.noPpm = true;
        } else if (a == L"--gpu") {
            opts.gpu = true;
        } else if (a == L"--no-gpu") {
            opts.noGpu = true;
        } else if (a == L"--no-prepare") {
            opts.noPrepare = true;
        } else if (a == L"--no-memory") {
            opts.noMemory = true;
        } else if (a == L"--no-storage") {
            opts.noStorage = true;
        } else if (a == L"--yes" || a == L"-y") {
            opts.yes = true;
        } else if (a == L"--no-pause") {
            opts.noPause = true;
        } else if (a == L"--elevated") {
            opts.elevated = true;
        } else if (a == L"--help" || a == L"-h" || a == L"/?") {
            opts.command = L"help";
        } else if (a == L"--version") {
            opts.command = L"version";
        } else {
            Err(Format(L"Unknown argument: %s (try --help)", a.c_str()));
            return false;
        }
    }
    if (quick) {
        // Coarse preview; explicit flags given alongside still win.
        BenchmarkConfig defaults;
        if (opts.bench.step == defaults.step) opts.bench.step = 20;
        if (!opts.passesSet && opts.bench.passes == defaults.passes) {
            opts.bench.passes = 1;
            opts.passesSet = true;
        }
        if (!opts.samplesSet && opts.bench.samples == defaults.samples) {
            opts.bench.samples = 40;
            opts.samplesSet = true;
        }
        if (!opts.verifySet && opts.bench.verifySamples == defaults.verifySamples) {
            opts.bench.verifySamples = 300;
            opts.verifySet = true;
        }
    }
    if (opts.bench.from && opts.bench.to && opts.bench.to <= opts.bench.from) {
        Err(L"--to must be greater than --from.");
        return false;
    }
    if (opts.ppm && opts.noPpm) {
        Err(L"--ppm and --no-ppm cannot be used together.");
        return false;
    }
    if (opts.gpu && opts.noGpu) {
        Err(L"--gpu and --no-gpu cannot be used together.");
        return false;
    }
    if (opts.afterburner && opts.noAfterburner) {
        Err(L"--afterburner and --no-afterburner cannot be used together.");
        return false;
    }
    return true;
}

void RestartInstalledService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceW(scm, kServiceName, SERVICE_START) : nullptr;
    if (svc) {
        StartServiceW(svc, 0, nullptr);
        CloseServiceHandle(svc);
    }
    if (scm) CloseServiceHandle(scm);
}

// Shared by --benchmark and the install flow. Stops a running service for the
// duration when we are allowed to (its request would mask every candidate
// coarser than it). `stoppedService` tells the caller to restart it.
BenchmarkResult BenchmarkSystem(const Options& opts, bool& stoppedService) {
    stoppedService = false;
    if (ServiceState() == 1) {
        if (IsAdmin()) {
            Out(L"Stopping the running Synchronus service for the duration of the benchmark "
                L"(its request would mask every candidate coarser than it)...");
            stoppedService = StopInstalledService();
        } else {
            Out(L"WARNING: the Synchronus service is running and this console is not elevated, "
                L"so candidates coarser than the service's resolution cannot be measured. Run "
                L"elevated or use --stop first.");
        }
    }

    Out(L"\nBenchmarking Sleep(1) response across timer resolutions. Each candidate is requested "
        L"with NtSetTimerResolution, Sleep(1) is timed with QueryPerformanceCounter, then the "
        L"request is released. A sample 'misses a tick' when it sleeps at least half a tick "
        L"longer than the candidate's typical value (e.g. 1.5 ms instead of 1.0 ms).");
    BenchmarkConfig cfg = opts.bench;
    const gui::MeasurePlan plan = gui::MeasurePlanFor(opts.fidelity);
    if (!opts.samplesSet) cfg.samples = plan.samples;
    if (!opts.passesSet) cfg.passes = plan.passes;
    if (!opts.verifySet) cfg.verifySamples = plan.verifySamples;
    return RunBenchmark(cfg);
}

bool Confirm(const Options& opts, const std::wstring& question, bool defaultYes = true) {
    if (opts.yes) return true;
    DWORD mode = 0;
    const HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
    if (!GetConsoleMode(stdinHandle, &mode)) return defaultYes;  // redirected: don't block scripts
    wprintf(L"%s %s ", question.c_str(), defaultYes ? L"[Y/n]" : L"[y/N]");
    wchar_t answer[16];
    if (!fgetws(answer, _countof(answer), stdin)) return defaultYes;
    if (answer[0] == L'\0' || answer[0] == L'\n') return defaultYes;
    return answer[0] == L'y' || answer[0] == L'Y';
}

// Console only. The GUI asks on its welcome screen instead. --yes, an explicit
// --fidelity, or an explicit --samples keeps the parsed choice.
Options PromptFidelity(Options opts) {
    if (opts.yes || opts.fidelitySet || opts.samplesSet) return opts;
    DWORD mode = 0;
    const HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
    if (!GetConsoleMode(stdinHandle, &mode)) return opts;
    const gui::MeasurePlan plans[3] = {gui::MeasurePlanFor(0), gui::MeasurePlanFor(1), gui::MeasurePlanFor(2)};
    Out(L"How thoroughly should each interval be measured?");
    const wchar_t* names[3] = {L"Low", L"Medium", L"High"};
    for (int i = 0; i < 3; ++i) {
        Out(Format(L"  [%d] %-7s %3d back-to-back samples, %d pass%s, verify %d   ~%s%s", i + 1, names[i],
                   plans[i].samples, plans[i].passes, plans[i].passes == 1 ? L"" : L"es", plans[i].verifySamples,
                   FormatDuration(plans[i].seconds).c_str(), i == 1 ? L"   (default)" : L""));
    }
    wprintf(L"Choice [2]: ");
    wchar_t answer[32];
    if (!fgetws(answer, _countof(answer), stdin)) return opts;
    if (answer[0] == L'\0' || answer[0] == L'\n' || answer[0] == L'\r') return opts;
    if (answer[0] == L'1' || _wcsnicmp(answer, L"low", 3) == 0) opts.fidelity = 0;
    else if (answer[0] == L'2' || _wcsnicmp(answer, L"med", 3) == 0) opts.fidelity = 1;
    else if (answer[0] == L'3' || _wcsnicmp(answer, L"high", 4) == 0) opts.fidelity = 2;
    else Out(L"Unrecognised choice; using medium.");
    opts.fidelitySet = true;
    return opts;
}

// Returns 0 applied, 1 failed, 2 skipped.
int OfferPpm(const Options& opts) {
    if (opts.noPpm) {
        Out(L"Skipping PPM tweaks (--no-ppm).");
        return 2;
    }
    const gui::CpuProbeView p = ProbeCpu();
    PrintCpuProbe(p);
    if (p.alreadyApplied) {
        Out(L"PPM tweaks were applied on a previous run; writing them again to harden and verify.");
    }

    const bool locked = p.mode == gui::CpuProbeView::Mode::Locked;
    const bool dynamic = p.mode == gui::CpuProbeView::Mode::Dynamic;
    bool apply = opts.ppm;
    if (!apply) {
        if (opts.yes) {
            apply = locked;
        } else if (locked) {
            apply = Confirm(opts,
                            L"This CPU looks like a locked/static frequency. Apply PPM tweaks "
                            L"(PERFCHECK 5000 ms, disable core parking, idle thresholds 95%) to cut "
                            L"periodic power-management DPCs?");
        } else if (dynamic) {
            apply = Confirm(opts,
                            L"This CPU uses dynamic boost. The 5000 ms PPM interval is meant for "
                            L"locked-multiplier CPUs and can delay turbo. Apply anyway?",
                            false);
        } else {
            apply = Confirm(opts,
                            L"Could not tell whether the multiplier is locked. Apply PPM tweaks anyway "
                            L"(safe to revert with --uninstall)?",
                            false);
        }
    }
    if (!apply) {
        Out(L"PPM tweaks not applied. Re-run with --apply-ppm after locking the multiplier in BIOS, "
            L"or pass --ppm to force them.");
        return 2;
    }
    g_ppmDwell = !opts.noPause;
    Out(L"Applying processor power-management tweaks (from PC-Tuning, for static-frequency CPUs).");
    Out(L"--no-pause skips the per-setting dwells.");
    return ApplyPpmTweaks() ? 0 : 1;
}

int OfferGpu(const Options& opts) {
    if (opts.noGpu) {
        Out(L"Skipping GPU tweaks (--no-gpu).");
        return 2;
    }
    const gui::GpuProbeView g = ProbeGpu();
    PrintGpuProbe(g);
    if (g.alreadyApplied) {
        Out(L"GPU tweaks were applied on a previous run; writing them again to harden and verify.");
    }
    bool apply = opts.gpu || opts.yes;
    if (!apply) {
        apply = Confirm(opts,
                        L"Apply GPU driver optimization (HAGS, overlays off, NVIDIA/AMD control-panel "
                        L"equivalents: max performance, VSync off, low latency, ReBAR, P2 off)?");
    }
    if (!apply) {
        Out(L"GPU tweaks not applied. Re-run with --apply-gpu, or pass --gpu to force them.");
        return 2;
    }
    g_ppmDwell = !opts.noPause;
    Out(L"Applying GPU driver optimization tweaks. Each change is explained before it is written.");
    return ApplyGpuTweaks() ? 0 : 1;
}

int OfferAfterburner(const Options& opts) {
    if (opts.noAfterburner) {
        Out(L"Skipping Afterburner (--no-afterburner).");
        return 2;
    }
    bool apply = opts.afterburner || opts.yes;
    if (!apply) {
        apply = Confirm(opts,
                        L"Download MSI Afterburner if it is missing and scan an NVIDIA overclock? "
                        L"The download and scan take about 30 minutes. The curve is saved as profile 1 "
                        L"and applied at every logon.");
    }
    if (!apply) {
        Out(L"Afterburner was not installed. Re-run with --apply-afterburner.");
        return 2;
    }
    return ApplyAfterburner() ? 0 : 1;
}

int CmdApplyPpm(const Options& opts) {
    Options forced = opts;
    forced.ppm = true;
    return OfferPpm(forced) == 1 ? 1 : 0;
}

int CmdApplyGpu(const Options& opts) {
    Options forced = opts;
    forced.gpu = true;
    return OfferGpu(forced) == 1 ? 1 : 0;
}

// Returns process exit code.
// If admin is required but missing, re-launches elevated and returns 0 in
// the parent (the elevated copy does the work in its own console).
int RequireAdmin(const Options& opts, const std::vector<std::wstring>& rawArgs, bool& relaunched) {
    relaunched = false;
    if (IsAdmin()) return -1;  // proceed in-process (sentinel: no early exit)
    if (opts.elevated) {
        Err(L"Administrator privileges are required and elevation did not take effect.");
        return 1;
    }
    Out(L"Requesting administrator privileges (approve the UAC prompt)...");
    if (!RelaunchElevated(ElevatedArgs(rawArgs))) {
        Err(L"Elevation was declined or failed. Synchronus must run as administrator.");
        return 1;
    }
    relaunched = true;
    return 0;
}

int CmdStatus() {
    ULONG slowest = 0, fastest = 0, current = 0;
    QueryResolutions(slowest, fastest, current);
    Out(Format(L"Current timer resolution: %.4f ms (kernel range %.4f-%.4f ms)",
               UnitsToMs(current), UnitsToMs(fastest), UnitsToMs(slowest)));

    DWORD globalFlag = 0;
    const bool hasGlobal = RegGetDword(HKEY_LOCAL_MACHINE, kKernelRegKey, kGlobalFlagName, globalFlag);
    Out(Format(L"GlobalTimerResolutionRequests: %s",
               hasGlobal ? (globalFlag ? L"1 (system-wide requests)" : L"0 (per-process only)")
                         : L"<not set> (per-process only)"));

    DWORD desired = 0, applied = 0;
    const bool configured = RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"DesiredResolution", desired);
    RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"AppliedResolution", applied);

    switch (ServiceState()) {
        case 0:
            Out(L"Service: not installed. Run Synchronus.exe to benchmark and install it.");
            break;
        case 1:
            Out(Format(L"Service: RUNNING (auto-start, LocalSystem), requesting %.4f ms; kernel reports %.4f ms%s",
                       UnitsToMs(desired), UnitsToMs(applied),
                       applied && desired && applied < desired ? L" (another process asks for finer)"
                       : applied && desired && applied > desired ? L" (request NOT honoured)"
                                                                 : L""));
            break;
        default:
            Out(L"Service: installed but not running (see --start).");
            break;
    }
    if (configured) {
        Out(Format(L"Configured resolution: %lu (%.4f ms) at %s", desired,
                   UnitsToMs(desired), GetInstallPath().c_str()));
    }
    DWORD ppm = 0;
    if (RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"PpmApplied", ppm) && ppm) {
        Out(Format(L"PPM tweaks: applied (PERFCHECK %u ms, core parking off, idle thresholds 95%%)",
                   CurrentPerfCheckMs()));
    } else {
        const gui::CpuProbeView p = ProbeCpu();
        Out(Format(L"PPM tweaks: not applied (%s)", p.reason.c_str()));
    }
    DWORD gpu = 0;
    if (RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"GpuApplied", gpu) && gpu) {
        Out(L"GPU tweaks: applied (HAGS, overlays, NVIDIA/AMD driver profile)");
    } else {
        const gui::GpuProbeView g = ProbeGpu();
        Out(Format(L"GPU tweaks: not applied (%s)", g.detail.c_str()));
    }
    return 0;
}

int CmdBenchmark(const Options& opts) {
    const Options chosen = PromptFidelity(opts);
    bool stoppedService = false;
    const BenchmarkResult r = BenchmarkSystem(chosen, stoppedService);
    int rc = 0;
    if (r.winner == 0) {
        Err(r.failure.empty() ? L"Benchmark failed." : r.failure);
        if (!r.csvPath.empty()) Out(Format(L"Per-candidate data: %s", r.csvPath.c_str()));
        rc = 1;
    } else {
        PrintBenchmarkSummary(r);
        Out(Format(L"\nNothing was changed. Run with no arguments (or --install) to hold %.4f ms "
                   L"permanently via the startup service.", UnitsToMs(r.winner)));
    }
    if (stoppedService) {
        Out(L"Restarting the Synchronus service...");
        RestartInstalledService();
    }
    return rc;
}

// The default flow: benchmark (unless --resolution pins a value), then
// install/update the service at the winner.
int CmdInstall(const Options& opts, bool interactiveDefault) {
    if (!opts.noPrepare) {
        if (opts.yes || Confirm(opts, L"Create a System Restore point before tuning?")) {
            g_ppmDwell = !opts.noPause;
            ApplyPrepareTweaks();
        }
    }
    ULONG desired = opts.resolution;

    if (desired != 0) {
        ULONG slowest = 0, fastest = 0, current = 0;
        QueryResolutions(slowest, fastest, current);
        if (desired < fastest) {
            Out(Format(L"Requested %lu is below the kernel fastest %lu; clamping.",
                       desired, fastest));
            desired = fastest;
        }
        if (slowest != 0 && desired > slowest) {
            Out(Format(L"Requested %lu is above the kernel slowest %lu; clamping.",
                       desired, slowest));
            desired = slowest;
        }
        Out(Format(L"Using requested resolution: %lu (%.4f ms)", desired, UnitsToMs(desired)));
    } else {
        // The install below (re)starts the service, so do not restart it here.
        bool stoppedService = false;
        const BenchmarkResult r = BenchmarkSystem(PromptFidelity(opts), stoppedService);
        if (r.winner == 0) {
            Err(r.failure.empty() ? L"Benchmark failed; nothing was installed."
                                  : r.failure + L" Nothing was installed.");
            if (!r.csvPath.empty()) Out(Format(L"Per-candidate data: %s", r.csvPath.c_str()));
            Out(L"To pin a value anyway: Synchronus.exe --install --resolution <100-ns units>");
            return 2;
        }
        desired = r.winner;
        PrintBenchmarkSummary(r);
        RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"BenchmarkBoundary", r.boundary);
        RegSetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"BenchmarkMedianMicroMs",
                    static_cast<DWORD>(r.winnerStats.median * 1000.0));
    }

    if (interactiveDefault &&
        !Confirm(opts, Format(L"Install the auto-start service at %.4f ms?", UnitsToMs(desired)))) {
        Out(L"Cancelled. Nothing was changed (use --apply for a one-off session).");
        return 0;
    }

    if (!InstallService(desired)) {
        Err(L"Install failed; see the log for details.");
        return 1;
    }

    Out(Format(L"\nInstalled. The '%s' service (auto-start, LocalSystem) is now "
               L"holding %.4f ms and will do so at every boot.",
               kServiceName, UnitsToMs(desired)));
    DWORD globalFlag = 0;
    if (RegGetDword(HKEY_LOCAL_MACHINE, kKernelRegKey, kGlobalFlagName, globalFlag) && globalFlag == 1) {
        Out(L"Note: GlobalTimerResolutionRequests is set, so this request applies "
            L"system-wide rather than only per-process once the machine has "
            L"rebooted with the flag present. The service itself is already "
            L"holding the resolution now.");
    }
    Out(L"Check anytime with:  Synchronus.exe --status");
    Out(L"Remove with:          Synchronus.exe --uninstall");
    OfferPpm(opts);
    OfferGpu(opts);
    OfferAfterburner(opts);
    if (!opts.noMemory) {
        if (opts.yes || Confirm(opts, L"Apply memory tweaks (system-managed pagefile, disable memory compression)?")) {
            g_ppmDwell = !opts.noPause;
            ApplyMemoryTweaks();
        }
    }
    if (!opts.noStorage) {
        if (opts.yes || Confirm(opts, L"Apply storage tweaks (PCIe ASPM off, USB suspend off, TRIM, SysMain off)?")) {
            g_ppmDwell = !opts.noPause;
            ApplyStorageTweaks();
        }
    }
    Out(L"Restart when you are ready. Windows cannot change BIOS, so set these by hand:");
    Out(L"  Lock the CPU multiplier. Disable Intel Turbo / EIST / Speed Shift, or AMD PBO / Precision Boost.");
    Out(L"  Disable deep C-states. Leave C1 if the machine will not POST without it.");
    Out(L"  Enable XMP (Intel) or DOCP (AMD) at the RAM's rated frequency, and DRAM voltage if the profile needs it.");
    Out(L"  SATA/NVMe in AHCI. Enable Above 4G / Resizable BAR if you want ReBAR.");
    return 0;
}

HANDLE g_applyStop = nullptr;
BOOL WINAPI ApplyCtrlHandler(DWORD) {
    if (g_applyStop) SetEvent(g_applyStop);
    return TRUE;
}

int CmdApply(const Options& opts) {
    ULONG slowest = 0, fastest = 0, current = 0;
    if (!QueryResolutions(slowest, fastest, current)) {
        Err(L"NtQueryTimerResolution failed.");
        return 1;
    }
    ULONG desired = opts.resolution != 0 ? opts.resolution : fastest;
    desired = std::max<ULONG>(desired, fastest);
    if (slowest != 0) desired = std::min<ULONG>(desired, slowest);

    DisablePowerThrottling();
    ULONG applied = 0;
    if (NtSetTimerResolution(desired, TRUE, &applied) != 0) {
        Err(L"NtSetTimerResolution failed.");
        return 1;
    }
    Out(Format(L"Holding timer resolution at %.4f ms in this process. Ctrl+C to release.",
               UnitsToMs(applied)));
    if (ServiceState() == 1) {
        Out(L"(The Synchronus service is also running; the lower of the two requests wins.)");
    }
    g_applyStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(ApplyCtrlHandler, TRUE);
    WaitForSingleObject(g_applyStop, INFINITE);
    NtSetTimerResolution(desired, FALSE, &current);
    Out(L"\nResolution released.");
    return 0;
}

int CmdStartStop(bool start) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        Err(L"OpenSCManager failed.");
        return 1;
    }
    SC_HANDLE svc = OpenServiceW(scm, kServiceName,
                                 SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) {
        Err(L"Service is not installed.");
        CloseServiceHandle(scm);
        return 1;
    }
    bool ok = true;
    if (start) {
        ok = StartServiceW(svc, 0, nullptr) != 0 ||
             GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
        ok = ok && WaitForServiceState(svc, SERVICE_RUNNING, 15000);
        Out(ok ? L"Service started." : L"Service failed to start.");
    } else {
        ok = StopInstalledService();
        Out(ok ? L"Service stopped." : L"Service did not stop cleanly.");
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok ? 0 : 1;
}

void EnsureConsole() {
    if (HasConsole(STD_OUTPUT_HANDLE)) {
        g_console = true;
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_console = true;
    } else if (AllocConsole()) {
        g_console = true;
        g_allocatedConsole = true;
        SetConsoleTitleW(L"Synchronus  \u00B7  Lucius And Wayne Systems LLC");
    } else {
        return;
    }
    FILE* out = nullptr;
    FILE* err = nullptr;
    FILE* in = nullptr;
    freopen_s(&out, "CONOUT$", "w", stdout);
    freopen_s(&err, "CONOUT$", "w", stderr);
    freopen_s(&in, "CONIN$", "r", stdin);
}

int RunCli(const Options& opts, const std::vector<std::wstring>& args) {
    if (opts.command == L"help") {
        PrintHelp();
        return 0;
    }
    if (opts.command == L"version") {
        wprintf(L"Synchronus %s\n%s\n%s\n", kVersion, kCompany, kCompanyUrl);
        return 0;
    }
    if (opts.command == L"status") return CmdStatus();
    if (opts.command == L"apply") return CmdApply(opts);

    // Remaining commands mutate the system and/or benchmark.
    const bool interactiveDefault = opts.command.empty();
    const bool needsAdmin = interactiveDefault || opts.command == L"install" ||
                            opts.command == L"uninstall" || opts.command == L"start" ||
                            opts.command == L"stop" || opts.command == L"ppm" ||
                            opts.command == L"gpu" || opts.command == L"afterburner" ||
                            opts.command == L"benchmark" ||
                            opts.command == L"prepare" || opts.command == L"memory" ||
                            opts.command == L"storage";

    // One tuner at a time: two benchmarks fighting over the resolution
    // would invalidate each other's measurements.
    HANDLE mutex = nullptr;
    if (needsAdmin || opts.command == L"benchmark") {
        mutex = CreateMutexW(nullptr, FALSE, L"Global\\Synchronus.SingleInstance");
        if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            Err(L"Another Synchronus operation is already running.");
            CloseHandle(mutex);
            return 1;
        }
    }

    if (opts.command == L"benchmark") {
        const int rc = CmdBenchmark(opts);
        if (mutex) CloseHandle(mutex);
        return rc;
    }

    int exitCode = 0;
    bool done = false;
    if (needsAdmin) {
        bool relaunched = false;
        const int adminRc = RequireAdmin(opts, args, relaunched);
        if (adminRc != -1) {
            exitCode = adminRc;
            done = true;  // either relaunched (child does the work) or failed
        }
    }

    if (!done) {
        if (interactiveDefault || opts.command == L"install") {
            exitCode = CmdInstall(opts, interactiveDefault);
        } else if (opts.command == L"uninstall") {
            exitCode = UninstallService() ? 0 : 1;
            if (exitCode == 0) Out(L"Uninstalled.");
        } else if (opts.command == L"start") {
            exitCode = CmdStartStop(true);
        } else if (opts.command == L"stop") {
            exitCode = CmdStartStop(false);
        } else if (opts.command == L"ppm") {
            exitCode = CmdApplyPpm(opts);
        } else if (opts.command == L"gpu") {
            exitCode = CmdApplyGpu(opts);
        } else if (opts.command == L"afterburner") {
            exitCode = ApplyAfterburner() ? 0 : 1;
        } else if (opts.command == L"prepare") {
            g_ppmDwell = !opts.noPause;
            exitCode = ApplyPrepareTweaks() ? 0 : 1;
        } else if (opts.command == L"memory") {
            g_ppmDwell = !opts.noPause;
            exitCode = ApplyMemoryTweaks() ? 0 : 1;
        } else if (opts.command == L"storage") {
            g_ppmDwell = !opts.noPause;
            exitCode = ApplyStorageTweaks() ? 0 : 1;
        }
    }

    if (mutex) CloseHandle(mutex);

    // Double-clicked runs (no arguments) would otherwise vanish before the
    // result can be read. Only the copy that did the work pauses: the
    // pre-elevation parent exits at once while its elevated child (which
    // IsAdmin() here) pauses in its own console.
    if (interactiveDefault && !opts.noPause && IsAdmin()) {
        wprintf(L"\nPress Enter to close...");
        wchar_t line[8];
        fgetws(line, _countof(line), stdin);
    }

    return exitCode;
}

void DeleteLeftover(const std::wstring& path) {
    for (int i = 0; i < 10; ++i) {
        if (DeleteFileW(path.c_str())) return;
        const DWORD rc = GetLastError();
        if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND) return;
        if (rc != ERROR_SHARING_VIOLATION && rc != ERROR_ACCESS_DENIED) return;
        Sleep(50);
    }
}

void CleanupUpdateBackups() {
    const std::wstring current = GetExePath() + L".old";
    const std::wstring installed = GetInstallPath() + L".old";
    DeleteLeftover(current);
    if (!EqualPath(current, installed)) DeleteLeftover(installed);
}

bool RelaunchUpdated() {
    const std::wstring exe = GetExePath();
    std::wstring dir = exe;
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    std::wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Download the announced exe, swap it over this copy and the installed
// service copy, then restart the service if it was running. 0 means the
// caller should relaunch. stop aborts the download only.
int ApplyAppUpdate(const gui::AppUpdate& offer, const std::function<void(const std::wstring&)>& status,
                   const std::atomic<bool>& stop, std::wstring& err) {
    if (stop.load()) return 1;
    wchar_t tempDir[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, tempDir);
    if (n == 0 || n >= MAX_PATH) {
        err = L"Could not find a temporary folder.";
        return 1;
    }
    const std::wstring dest = std::wstring(tempDir) + L"Synchronus-update.exe";
    DeleteFileW(dest.c_str());
    const bool downloaded = gui::DownloadAppUpdate(
        offer, dest,
        [&](uint64_t got, uint64_t total) {
            if (!status) return;
            if (total == 0) status(L"Downloading " + offer.version + L"\u2026");
            else {
                status(Format(L"Downloading %s\u2026  %.0f%%", offer.version.c_str(),
                              (100.0 * static_cast<double>(got)) / static_cast<double>(total)));
            }
        },
        stop, err);
    if (!downloaded) {
        DeleteFileW(dest.c_str());
        if (!stop.load() && !err.empty()) LogLine(L"Update failed: " + err);
        return 1;
    }
    if (stop.load()) {
        DeleteFileW(dest.c_str());
        return 1;
    }
    if (status) status(L"Replacing this copy\u2026");
    if (!gui::ReportAppDownload(offer.version)) {
        LogLine(L"The download counter could not be updated.");
    }

    const bool serviceWasRunning = ServiceState() == 1;
    const bool haveService = ServiceExists();
    if ((haveService || serviceWasRunning) && !StopInstalledService()) {
        err = L"The Synchronus service did not stop, so its copy could not be replaced.";
        DeleteFileW(dest.c_str());
        LogLine(L"Update failed: " + err);
        return 1;
    }

    const std::wstring current = GetExePath();
    const std::wstring installed = GetInstallPath();
    const bool installedExists = GetFileAttributesW(installed.c_str()) != INVALID_FILE_ATTRIBUTES;
    const bool replaceInstalled = (haveService || installedExists) && !EqualPath(current, installed);
    auto restartIfNeeded = [&] {
        if (!serviceWasRunning) return;
        const wchar_t* name = ServiceExistsNamed(kServiceName) ? kServiceName : kLegacyServiceName;
        if (!StartServiceNamed(name)) LogLine(L"The Synchronus service did not restart.");
    };
    if (replaceInstalled) CreateDirectoryW(GetInstallDir().c_str(), nullptr);
    if (replaceInstalled && !gui::ReplaceExecutable(installed, dest, err)) {
        restartIfNeeded();
        DeleteFileW(dest.c_str());
        LogLine(L"Update failed: " + err);
        return 1;
    }
    std::wstring replaceErr;
    if (!gui::ReplaceExecutable(current, dest, replaceErr)) {
        err = replaceInstalled ? (L"The installed copy was updated, but this copy could not be replaced. " + replaceErr)
                               : replaceErr;
        restartIfNeeded();
        DeleteFileW(dest.c_str());
        LogLine(L"Update failed: " + err);
        return 1;
    }
    DeleteFileW(dest.c_str());
    restartIfNeeded();
    LogLine(L"Updated to " + offer.version + L".");
    return 0;
}

gui::SystemView CurrentSystem(const std::wstring& priority = {}) {
    gui::SystemView s;
    ULONG slowest = 0, fastest = 0, current = 0;
    QueryResolutions(slowest, fastest, current);
    s.fastest = fastest;
    s.slowest = slowest;
    s.current = current;
    s.admin = IsAdmin();
    s.serviceState = ServiceState();
    DWORD desired = 0;
    RegGetDword(HKEY_LOCAL_MACHINE, kAppRegKey, L"DesiredResolution", desired);
    s.serviceResolution = desired;
    s.priority = priority;
    return s;
}

int RunGui(const Options& opts, const std::vector<std::wstring>&) {
    CleanupUpdateBackups();
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Global\\Synchronus.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Another Synchronus window is already running.", L"Synchronus",
                    MB_ICONINFORMATION | MB_OK);
        CloseHandle(mutex);
        return 1;
    }

    gui::Host host;
    host.version = kVersion;
    host.logPath = LogFilePath();
    host.dataDir = ProgramDataDir();
    host.autoInstall = opts.yes;
    host.ppmForce = opts.ppm;
    host.ppmSkip = opts.noPpm;
    host.gpuForce = opts.gpu;
    host.gpuSkip = opts.noGpu;
    host.afterburnerSkip = opts.noAfterburner;
    host.prepareSkip = opts.noPrepare;
    host.memorySkip = opts.noMemory;
    host.storageSkip = opts.noStorage;
    host.pinnedResolution = opts.resolution;
    host.fidelity = opts.fidelity;
    host.system = [] { return CurrentSystem(); };
    host.benchmark = [&opts](gui::Feed& feed, int fidelity) -> gui::ResultView {
        g_feed = &feed;
        bool stopped = false;
        Options chosen = opts;
        chosen.fidelity = fidelity;
        chosen.fidelitySet = true;
        const BenchmarkResult r = BenchmarkSystem(chosen, stopped);
        gui::ResultView v;
        v.winner = r.winner;
        v.optimum = r.optimum;
        v.boundary = r.boundary;
        v.margin = r.margin;
        v.loop = ViewOf(r.winnerStats);
        v.idle = ViewOf(r.winnerIdleStats);
        v.score = r.winnerScore;
        v.failure = r.failure;
        v.csvPath = r.csvPath;
        v.stoppedService = stopped;
        g_feed = nullptr;
        return v;
    };
    host.install = [](unsigned units, gui::Feed& feed) -> int {
        g_feed = &feed;
        if (!IsAdmin()) {
            feed.Log(L"Requesting administrator privileges (approve the UAC prompt)...");
            std::vector<std::wstring> elevated = {
                Format(L"--resolution"), Format(L"%lu", static_cast<unsigned long>(units)), L"--yes"};
            const bool ok = RelaunchElevated(ElevatedArgs(elevated));
            g_feed = nullptr;
            if (!ok) {
                feed.Log(L"ERROR: elevation was declined or failed.");
                return 1;
            }
            return 2;
        }
        const bool ok = InstallService(units);
        g_feed = nullptr;
        return ok ? 0 : 1;
    };
    host.probeCpu = [] { return ProbeCpu(); };
    host.probeGpu = [] { return ProbeGpu(); };
    host.applyPpm = [](gui::Feed& feed) -> int {
        g_feed = &feed;
        g_ppmDwell = true;
        feed.Phase(L"PPM", L"applying processor power-management tweaks");
        const bool ok = ApplyPpmTweaks();
        g_feed = nullptr;
        return ok ? 0 : 1;
    };
    host.applyGpu = [](gui::Feed& feed) -> int {
        g_feed = &feed;
        g_ppmDwell = true;
        feed.Phase(L"GPU CONFIG", L"applying GPU driver / Windows graphics tweaks");
        const bool ok = ApplyGpuTweaks();
        g_feed = nullptr;
        return ok ? 0 : 1;
    };
    host.applyAfterburner = [](gui::Feed& feed) -> int {
        g_feed = &feed;
        g_ppmDwell = true;
        feed.Phase(L"AFTERBURNER", L"installing Afterburner and RivaTuner");
        const bool ok = ApplyAfterburner();
        g_feed = nullptr;
        return ok ? 0 : 1;
    };
    host.applyPrepare = [](gui::Feed& feed) -> int {
        g_feed = &feed;
        g_ppmDwell = true;
        feed.Phase(L"PREPARE", L"creating a System Restore point");
        const bool ok = ApplyPrepareTweaks();
        g_feed = nullptr;
        return ok ? 0 : 1;
    };
    host.applyMemory = [](gui::Feed& feed) -> int {
        g_feed = &feed;
        g_ppmDwell = true;
        feed.Phase(L"MEMORY", L"pagefile and memory compression");
        const bool ok = ApplyMemoryTweaks();
        g_feed = nullptr;
        return ok ? 0 : 1;
    };
    host.applyStorage = [](gui::Feed& feed) -> int {
        g_feed = &feed;
        g_ppmDwell = true;
        feed.Phase(L"STORAGE", L"SSD, PCIe, USB, TRIM, SysMain");
        const bool ok = ApplyStorageTweaks();
        g_feed = nullptr;
        return ok ? 0 : 1;
    };
    host.restartService = [] { RestartInstalledService(); };
    if (!opts.yes) {
        host.checkUpdate = [](gui::AppUpdate& offer, const std::atomic<bool>& stop, std::wstring& err) {
            const bool newer = gui::CheckAppUpdate(kVersion, offer, stop, err);
            if (stop.load()) return false;
            if (!err.empty()) LogLine(L"Update check: " + err);
            else if (newer) LogLine(L"Update available: " + offer.version);
            return newer;
        };
        host.applyUpdate = [](const gui::AppUpdate& offer, const std::function<void(const std::wstring&)>& status,
                              const std::atomic<bool>& stop, std::wstring& err) {
            return ApplyAppUpdate(offer, status, stop, err);
        };
    }

    const int rc = gui::Run(host);
    if (mutex) {
        CloseHandle(mutex);
        mutex = nullptr;
    }
    if (rc == 2) {
        if (!RelaunchUpdated()) {
            MessageBoxW(nullptr, L"The new version is in place, but Synchronus could not reopen it.", L"Synchronus",
                        MB_ICONWARNING | MB_OK);
            return 1;
        }
        return 0;
    }
    return rc;
}

}  // namespace

static bool LooksLikeCli(const std::vector<std::wstring>& args, const Options& opts) {
    if (opts.gui) return false;
    if (opts.cli) return true;
    if (!opts.command.empty()) return true;
    for (const auto& a : args) {
        if (a == L"--help" || a == L"-h" || a == L"/?" || a == L"--version") return true;
    }
    return false;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    if (argv) {
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
        LocalFree(argv);
    }

    Options opts;
    const bool parsed = ParseOptions(args, opts);
    if (opts.command == L"service") {
        return RunServiceDispatcher() ? 0 : 1;
    }

    if (!parsed) {
        EnsureConsole();
        PrintHelp();
        return 1;
    }

    const bool allowUnelevated = opts.command == L"help" || opts.command == L"version";
    if (!allowUnelevated && !IsAdmin()) {
        if (opts.elevated) {
            if (LooksLikeCli(args, opts)) {
                EnsureConsole();
                Err(L"Administrator privileges are required and elevation did not take effect.");
            } else {
                MessageBoxW(nullptr,
                            L"Synchronus must run as administrator, and elevation did not take effect.",
                            L"Synchronus", MB_ICONERROR | MB_OK);
            }
            return 1;
        }
        if (!RelaunchElevated(ElevatedArgs(args))) {
            if (LooksLikeCli(args, opts)) {
                EnsureConsole();
                Err(L"Elevation was declined or failed. Synchronus must run as administrator.");
            } else {
                MessageBoxW(nullptr,
                            L"Synchronus must run as administrator. Approve the UAC prompt and try again.",
                            L"Synchronus", MB_ICONWARNING | MB_OK);
            }
            return 1;
        }
        return 0;
    }

    if (LooksLikeCli(args, opts)) {
        EnsureConsole();
        return RunCli(opts, args);
    }
    return RunGui(opts, args);
}
