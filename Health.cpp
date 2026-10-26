#include "Health.h"

#include <windows.h>
#include <powrprof.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <string>
#include <vector>

namespace gui {
namespace {

constexpr wchar_t kApp[] = L"SOFTWARE\\Synchronus";
constexpr wchar_t kMonitor[] = L"Software\\Synchronus\\Monitor";
constexpr wchar_t kKernel[] = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\kernel";
constexpr wchar_t kGfx[] = L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers";
constexpr wchar_t kDisplay[] = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";

const GUID kSubProcessor = {0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
const GUID kSubPci = {0x501a4d13, 0x42af, 0x4429, {0x9f, 0xd1, 0xa8, 0x21, 0xf2, 0xbb, 0x0b, 0x89}};
const GUID kSubDisk = {0x0012ee47, 0x9041, 0x4b5d, {0x9b, 0x77, 0x53, 0x5f, 0xba, 0x8b, 0x14, 0x42}};
const GUID kSubUsb = {0x2a737441, 0x1930, 0x4402, {0x8d, 0x77, 0xb2, 0xbe, 0xbb, 0xa3, 0x08, 0xa3}};
const GUID kPerfCheck = {0x4d2b0152, 0x7d5c, 0x498b, {0x88, 0xe2, 0x34, 0x34, 0x53, 0x92, 0xa2, 0xc5}};
const GUID kParkMin = {0x0cc5b647, 0xc1df, 0x4637, {0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x83}};
const GUID kBoostMode = {0xbe337238, 0x0d82, 0x4146, {0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7}};
const GUID kAspm = {0xee12f906, 0xd277, 0x404b, {0xb6, 0xda, 0xe5, 0xfa, 0x1a, 0x57, 0x6d, 0xf5}};
const GUID kDiskIdle = {0x6738e2c4, 0xe8a5, 0x4a42, {0xb1, 0x6a, 0xe0, 0x40, 0xe7, 0x69, 0x75, 0x6e}};
const GUID kUsbSuspend = {0x48e6b7a6, 0x50f5, 0x4782, {0xa5, 0xd4, 0x53, 0xbb, 0x8f, 0x07, 0xe2, 0x26}};

using NtQueryTimerResolutionFn = LONG(WINAPI*)(PULONG, PULONG, PULONG);

bool Stopped(const std::atomic<bool>* cancelled) { return cancelled && cancelled->load(); }

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buffer[768];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    return buffer;
}

bool ReadDword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD& value) {
    DWORD type = 0, size = sizeof(value), data = 0;
    const LSTATUS rc = RegGetValueW(root, key, name, RRF_RT_REG_DWORD, &type, &data, &size);
    if (rc != ERROR_SUCCESS) return false;
    value = data;
    return true;
}

bool FlagOn(const wchar_t* name) {
    DWORD v = 0;
    return ReadDword(HKEY_LOCAL_MACHINE, kApp, name, v) && v != 0;
}

bool ReadPower(const GUID& sub, const GUID& setting, DWORD& value) {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return false;
    const DWORD rc = PowerReadACValueIndex(nullptr, scheme, const_cast<GUID*>(&sub), const_cast<GUID*>(&setting), &value);
    LocalFree(scheme);
    return rc == ERROR_SUCCESS;
}

int ServiceRunState(const wchar_t* name) {
    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return -1;
    const SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return 0;
    }
    SERVICE_STATUS_PROCESS status{};
    DWORD need = 0;
    const BOOL ok = QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &need);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    if (!ok) return -1;
    if (status.dwCurrentState == SERVICE_RUNNING) return 1;
    return 2;
}

int ServiceStartType(const wchar_t* name, DWORD& start) {
    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return -1;
    const SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_CONFIG);
    if (!svc) {
        CloseServiceHandle(scm);
        return 0;
    }
    BYTE buf[4096];
    DWORD need = 0;
    const BOOL ok = QueryServiceConfigW(svc, reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf), sizeof(buf), &need);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    if (!ok) return -1;
    start = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf)->dwStartType;
    return 1;
}

bool FileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring ProgramFilesX86() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"ProgramFiles(x86)", buf, _countof(buf));
    if (n > 0 && n < _countof(buf)) return buf;
    return L"C:\\Program Files (x86)";
}

bool TaskExists(const wchar_t* name) {
    const std::wstring sch = L"C:\\Windows\\System32\\schtasks.exe";
    const std::wstring args = L"\"" + sch + L"\" /Query /TN \"" + name + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd(args.begin(), args.end());
    cmd.push_back(0);
    if (!CreateProcessW(sch.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 8000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

bool ProcessRunning(const wchar_t* exeName) {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

std::wstring RunCapture(const std::wstring& exe, const std::wstring& args, DWORD timeoutMs) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) return {};
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = write;
    si.hStdError = write;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(read);
        CloseHandle(write);
        return {};
    }
    CloseHandle(write);
    WaitForSingleObject(pi.hProcess, timeoutMs);
    char raw[2048]{};
    DWORD got = 0;
    ReadFile(read, raw, sizeof(raw) - 1, &got, nullptr);
    CloseHandle(read);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (!got) return {};
    const int n = MultiByteToWideChar(CP_OEMCP, 0, raw, static_cast<int>(got), nullptr, 0);
    std::wstring out(n, L'\0');
    if (n) MultiByteToWideChar(CP_OEMCP, 0, raw, static_cast<int>(got), out.data(), n);
    return out;
}

HealthCheck Make(const wchar_t* id, const wchar_t* category, const wchar_t* name, const wchar_t* repair) {
    HealthCheck c;
    c.id = id;
    c.category = category;
    c.name = name;
    c.repair = repair ? repair : L"";
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    c.checkedAt = u.QuadPart;
    return c;
}

void FinishPolicy(HealthCheck& c, bool readable, bool tracked, bool matches, const std::wstring& expected,
                  const std::wstring& observed) {
    c.expected = expected;
    c.observed = observed;
    if (CheckDisabled(c.id)) {
        c.status = HealthStatus::Disabled;
        c.severity = 0;
        c.explanation = L"This check is turned off in Settings. Synchronus is not treating it as healthy or failed.";
        c.repair.clear();
        return;
    }
    if (!readable) {
        c.status = HealthStatus::Inaccessible;
        c.severity = 1;
        c.explanation = L"The live value could not be read. That is not a pass.";
        return;
    }
    if (!tracked) {
        c.status = HealthStatus::Info;
        c.severity = 0;
        c.explanation = matches
                            ? L"The live value already matches the tuned setting. Synchronus has not recorded this as one of its applied tweaks."
                            : L"Synchronus has not applied this tweak on this machine, so the difference is informational.";
        c.repair.clear();
        return;
    }
    if (matches) {
        c.status = HealthStatus::Pass;
        c.severity = 0;
        c.explanation = L"The live setting matches what Synchronus last applied.";
        return;
    }
    c.status = HealthStatus::Attention;
    c.severity = 1;
    c.explanation = L"The live setting no longer matches what Synchronus applied. The saved flag alone is not treated as proof.";
}

}  // namespace

const wchar_t* HealthStatusText(HealthStatus status) {
    switch (status) {
        case HealthStatus::Pass: return L"Passing";
        case HealthStatus::Attention: return L"Needs attention";
        case HealthStatus::Info: return L"Informational";
        case HealthStatus::Disabled: return L"Disabled";
        case HealthStatus::Unsupported: return L"Unsupported";
        case HealthStatus::Unknown: return L"Unknown";
        case HealthStatus::Inaccessible: return L"Inaccessible";
        case HealthStatus::Error: return L"Error";
        case HealthStatus::Reboot: return L"Restart required";
    }
    return L"Unknown";
}

bool HealthCountsAsPass(HealthStatus status) { return status == HealthStatus::Pass; }

bool HealthNeedsAttention(HealthStatus status) {
    return status == HealthStatus::Attention || status == HealthStatus::Error || status == HealthStatus::Reboot;
}

HealthSummary Summarize(const std::vector<HealthCheck>& checks) {
    HealthSummary s;
    for (const auto& c : checks) {
        if (c.status == HealthStatus::Pass) ++s.pass;
        else if (HealthNeedsAttention(c.status)) ++s.attention;
        else if (c.status == HealthStatus::Info) ++s.info;
        else ++s.other;
    }
    if (s.attention == 0 && s.other == 0) s.headline = L"Nothing needs attention.";
    else if (s.attention == 0) s.headline = L"No drifted tweaks. Some checks could not be completed.";
    else s.headline = Format(L"%d item%s need attention.", s.attention, s.attention == 1 ? L"" : L"s");
    return s;
}

bool CheckDisabled(const std::wstring& id) {
    DWORD v = 0;
    return ReadDword(HKEY_CURRENT_USER, kMonitor, (L"Disabled_" + id).c_str(), v) && v != 0;
}

void SetCheckDisabled(const std::wstring& id, bool disabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kMonitor, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    const std::wstring name = L"Disabled_" + id;
    if (disabled) {
        const DWORD one = 1;
        RegSetValueExW(key, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    } else {
        RegDeleteValueW(key, name.c_str());
    }
    RegCloseKey(key);
}

MonitorPrefs LoadMonitorPrefs() {
    MonitorPrefs p;
    DWORD v = 0;
    if (ReadDword(HKEY_CURRENT_USER, kMonitor, L"CloseToTray", v)) p.closeToTray = v != 0;
    if (ReadDword(HKEY_CURRENT_USER, kMonitor, L"StartMinimized", v)) p.startMinimized = v != 0;
    if (ReadDword(HKEY_CURRENT_USER, kMonitor, L"LaunchAtLogin", v)) p.launchAtLogin = v != 0;
    if (ReadDword(HKEY_CURRENT_USER, kMonitor, L"Notify", v)) p.notify = v != 0;
    if (ReadDword(HKEY_CURRENT_USER, kMonitor, L"Paused", v)) p.paused = v != 0;
    if (ReadDword(HKEY_CURRENT_USER, kMonitor, L"ScanHours", v) && v >= 1 && v <= 168) p.scanHours = static_cast<int>(v);
    return p;
}

void SaveMonitorPrefs(const MonitorPrefs& prefs) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kMonitor, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    auto put = [&](const wchar_t* name, DWORD value) {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    };
    put(L"CloseToTray", prefs.closeToTray ? 1 : 0);
    put(L"StartMinimized", prefs.startMinimized ? 1 : 0);
    put(L"LaunchAtLogin", prefs.launchAtLogin ? 1 : 0);
    put(L"Notify", prefs.notify ? 1 : 0);
    put(L"Paused", prefs.paused ? 1 : 0);
    put(L"ScanHours", static_cast<DWORD>(prefs.scanHours < 1 ? 6 : prefs.scanHours));
    RegCloseKey(key);
    ApplyLaunchAtLogin(prefs.launchAtLogin);
}

void ApplyLaunchAtLogin(bool enabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (!enabled) {
        RegDeleteValueW(key, L"Synchronus");
        RegCloseKey(key);
        return;
    }
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        RegCloseKey(key);
        return;
    }
    const std::wstring quoted = L"\"" + std::wstring(path) + L"\"";
    RegSetValueExW(key, L"Synchronus", 0, REG_SZ, reinterpret_cast<const BYTE*>(quoted.c_str()),
                   static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

void AppendHealthLog(const std::wstring& line) {
    wchar_t env[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"ProgramData", env, MAX_PATH);
    const std::wstring dir = (n > 0 && n < MAX_PATH ? std::wstring(env) : L"C:\\ProgramData") + L"\\Synchronus";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\Synchronus.log";
    const HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const std::wstring text = Format(L"%04u-%02u-%02u %02u:%02u:%02u  health  %s\r\n", st.wYear, st.wMonth, st.wDay,
                                     st.wHour, st.wMinute, st.wSecond, line.c_str());
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes > 1) {
        std::string utf8(static_cast<size_t>(bytes - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), bytes, nullptr, nullptr);
        DWORD wrote = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &wrote, nullptr);
    }
    CloseHandle(file);
}

std::vector<HealthCheck> RunHealth(const std::atomic<bool>* cancelled) {
    std::vector<HealthCheck> out;
    auto push = [&](HealthCheck c) {
        if (!Stopped(cancelled)) out.push_back(std::move(c));
    };

    {
        HealthCheck c = Make(L"timer.service", L"Timer Resolution", L"Timer service", L"timer");
        const int state = ServiceRunState(L"Synchronus");
        const int legacy = state == 0 ? ServiceRunState(L"TimerResolution") : 0;
        const int use = state != 0 ? state : legacy;
        const bool tracked = FlagOn(L"DesiredResolution");
        if (use < 0) {
            c.status = HealthStatus::Inaccessible;
            c.severity = 1;
            c.observed = L"service control manager could not be queried";
            c.explanation = L"The service state could not be read. That is not a pass.";
        } else if (use == 0) {
            FinishPolicy(c, true, tracked, false, L"Synchronus service running", L"service is not installed");
        } else if (use == 1) {
            FinishPolicy(c, true, tracked, true, L"Synchronus service running",
                         state == 1 ? L"Synchronus service is running" : L"legacy TimerResolution service is running");
        } else {
            FinishPolicy(c, true, tracked, false, L"Synchronus service running", L"service is installed but not running");
        }
        push(c);
    }
    if (Stopped(cancelled)) return out;

    {
        HealthCheck c = Make(L"timer.global", L"Timer Resolution", L"System-wide timer requests", L"timer");
        DWORD flag = 0;
        const bool readable = ReadDword(HKEY_LOCAL_MACHINE, kKernel, L"GlobalTimerResolutionRequests", flag);
        FinishPolicy(c, readable, FlagOn(L"GlobalFlagSaved"), readable && flag == 1, L"GlobalTimerResolutionRequests = 1",
                     readable ? Format(L"GlobalTimerResolutionRequests = %lu", flag) : L"value unreadable");
        if (c.status == HealthStatus::Pass) {
            c.explanation += L" A reboot is still required before this flag affects every process.";
        }
        push(c);
    }

    {
        HealthCheck c = Make(L"timer.effective", L"Timer Resolution", L"Effective timer resolution", L"");
        auto query = reinterpret_cast<NtQueryTimerResolutionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryTimerResolution"));
        DWORD desired = 0;
        const bool haveDesired = ReadDword(HKEY_LOCAL_MACHINE, kApp, L"DesiredResolution", desired) && desired != 0;
        ULONG slow = 0, fast = 0, current = 0;
        const bool queried = query && query(&slow, &fast, &current) == 0;
        c.repair.clear();
        if (CheckDisabled(c.id)) {
            c.status = HealthStatus::Disabled;
            c.explanation = L"This check is turned off in Settings.";
        } else if (!queried) {
            c.status = HealthStatus::Error;
            c.severity = 2;
            c.observed = L"NtQueryTimerResolution failed";
            c.explanation = L"The effective resolution could not be read. That is not a pass.";
        } else if (!haveDesired) {
            c.status = HealthStatus::Info;
            c.severity = 0;
            c.expected = L"no Synchronus resolution has been chosen";
            c.observed = Format(L"kernel reports %.4f ms", current / 10000.0);
            c.explanation = L"This is the kernel's current report. It is not a configured Synchronus request, and this check does not hold a timer open.";
        } else {
            const long delta = static_cast<long>(current) - static_cast<long>(desired);
            const bool closeEnough = delta < 0 ? -delta <= 500 : delta <= 500;
            c.expected = Format(L"configured %.4f ms", desired / 10000.0);
            c.observed = Format(L"kernel reports %.4f ms", current / 10000.0);
            if (closeEnough) {
                c.status = HealthStatus::Pass;
                c.explanation = L"The kernel's current resolution is close to the value Synchronus saved. This check did not call NtSetTimerResolution.";
            } else {
                c.status = HealthStatus::Attention;
                c.severity = 1;
                c.explanation = L"The kernel is not currently reporting the saved resolution. The effective value can also change when the service is stopped. This check did not hold a timer request open.";
                c.repair = L"timer";
            }
        }
        push(c);
    }
    if (Stopped(cancelled)) return out;

    auto powerCheck = [&](const wchar_t* id, const wchar_t* name, const GUID& sub, const GUID& setting, DWORD target,
                          const wchar_t* trackedFlag, const wchar_t* repair) {
        HealthCheck c = Make(id, L"Power and Performance", name, repair);
        DWORD value = 0;
        const bool readable = ReadPower(sub, setting, value);
        FinishPolicy(c, readable, FlagOn(trackedFlag), readable && value == target, Format(L"%lu", target),
                     readable ? Format(L"%lu", value) : L"unreadable");
        push(c);
    };
    powerCheck(L"power.perfcheck", L"Performance check interval", kSubProcessor, kPerfCheck, 5000, L"PpmApplied", L"ppm");
    powerCheck(L"power.parking", L"Core parking minimum", kSubProcessor, kParkMin, 100, L"PpmApplied", L"ppm");
    powerCheck(L"power.boost", L"Processor boost mode", kSubProcessor, kBoostMode, 0, L"PpmApplied", L"ppm");
    if (Stopped(cancelled)) return out;

    {
        HealthCheck c = Make(L"gpu.hags", L"Windows Configuration", L"Hardware-accelerated GPU scheduling", L"gpu");
        DWORD v = 0;
        const bool readable = ReadDword(HKEY_LOCAL_MACHINE, kGfx, L"HwSchMode", v);
        FinishPolicy(c, readable, FlagOn(L"GpuApplied"), readable && v == 2, L"HwSchMode = 2",
                     readable ? Format(L"HwSchMode = %lu", v) : L"unreadable");
        if (c.status == HealthStatus::Pass || (readable && v == 2)) {
            c.explanation += L" HAGS takes effect after a restart.";
        }
        push(c);
    }
    {
        HealthCheck c = Make(L"gpu.gamedvr", L"Windows Configuration", L"Game DVR policy", L"gpu");
        DWORD v = 0;
        const bool readable = ReadDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\GameDVR", L"AllowGameDVR", v);
        FinishPolicy(c, readable, FlagOn(L"GpuApplied"), readable && v == 0, L"AllowGameDVR = 0",
                     readable ? Format(L"AllowGameDVR = %lu", v) : L"policy value is not set");
        if (!readable && FlagOn(L"GpuApplied")) {
            c.status = HealthStatus::Attention;
            c.severity = 1;
            c.observed = L"AllowGameDVR is absent";
            c.explanation = L"Synchronus records a GPU pass, but the Game DVR policy value is missing.";
        }
        push(c);
    }
    {
        HealthCheck c = Make(L"gpu.driver", L"Graphics Drivers", L"Installed display driver", L"");
        std::wstring version, desc;
        for (int i = 0; i < 16 && version.empty(); ++i) {
            wchar_t key[160];
            _snwprintf_s(key, _TRUNCATE, L"%s\\%04d", kDisplay, i);
            wchar_t buf[128]{};
            DWORD sz = sizeof(buf);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"DriverVersion", RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) {
                version = buf;
                sz = sizeof(buf);
                buf[0] = 0;
                if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"DriverDesc", RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) desc = buf;
            }
        }
        c.expected = L"a display driver is installed; newest vendor package is not assumed";
        if (version.empty()) {
            c.status = HealthStatus::Attention;
            c.severity = 1;
            c.observed = L"no display driver version was found in the display class";
            c.explanation = L"A missing version here means the adapter record could not be read. It does not by itself prove the newest driver.";
        } else {
            c.status = HealthStatus::Info;
            c.observed = desc.empty() ? version : (desc + L"  " + version);
            c.explanation = L"This is the driver Windows has recorded. Whether a newer compatible package exists is only known after the GPU Clean lookup, which contacts the vendor. A higher number is not treated as automatically better.";
        }
        push(c);
    }
    if (Stopped(cancelled)) return out;

    {
        const std::wstring ab = ProgramFilesX86() + L"\\MSI Afterburner\\MSIAfterburner.exe";
        const std::wstring rt = ProgramFilesX86() + L"\\RivaTuner Statistics Server\\RTSS.exe";
        HealthCheck c = Make(L"util.afterburner", L"Supporting Utilities", L"MSI Afterburner", L"afterburner");
        const bool installed = FileExists(ab);
        const bool running = installed && ProcessRunning(L"MSIAfterburner.exe");
        c.expected = L"installed when you opted into the Afterburner step";
        c.observed = !installed ? L"not installed" : (running ? L"installed and running" : L"installed, not running");
        if (CheckDisabled(c.id)) {
            c.status = HealthStatus::Disabled;
            c.explanation = L"This check is turned off in Settings.";
            c.repair.clear();
        } else if (!installed) {
            c.status = HealthStatus::Info;
            c.explanation = L"Afterburner is optional. Absence is not a failure, and no overclock is inferred.";
            c.repair = L"afterburner";
        } else {
            c.status = HealthStatus::Pass;
            c.explanation = L"The Afterburner executable is present. This does not mean a fan curve, voltage, or overclock profile is active.";
            c.repair.clear();
        }
        push(c);

        HealthCheck r = Make(L"util.rtss", L"Supporting Utilities", L"RivaTuner Statistics Server", L"afterburner");
        const bool rtss = FileExists(rt);
        r.expected = L"installed alongside Afterburner when that step was used";
        r.observed = rtss ? L"RTSS.exe is present" : L"not installed";
        if (CheckDisabled(r.id)) {
            r.status = HealthStatus::Disabled;
            r.explanation = L"This check is turned off in Settings.";
            r.repair.clear();
        } else if (!rtss) {
            r.status = HealthStatus::Info;
            r.explanation = L"RivaTuner is optional and is only expected after the Afterburner step.";
        } else {
            r.status = HealthStatus::Pass;
            r.explanation = L"RTSS is installed. An overlay is not reported as active just because the file exists.";
            r.repair.clear();
        }
        push(r);

        HealthCheck t = Make(L"util.afterburner-task", L"Supporting Utilities", L"Afterburner logon task", L"");
        const bool task = TaskExists(L"Synchronus Afterburner");
        t.expected = L"logon task present only after the Afterburner step saved a profile";
        t.observed = task ? L"scheduled task Synchronus Afterburner exists" : L"no Synchronus Afterburner logon task";
        t.status = HealthStatus::Info;
        t.explanation = task
                            ? L"The task exists. That means Afterburner is scheduled at logon. It does not prove the scanned curve is loaded."
                            : L"No Synchronus logon task was found. That is informational until you run the Afterburner step.";
        push(t);
    }
    if (Stopped(cancelled)) return out;

    {
        HealthCheck c = Make(L"memory.pagefile", L"Memory and CPU", L"Paging file", L"memory");
        wchar_t buf[1024]{};
        DWORD sz = sizeof(buf);
        const LSTATUS rc = RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
                                        L"PagingFiles", RRF_RT_REG_MULTI_SZ | RRF_RT_REG_SZ, nullptr, buf, &sz);
        const bool readable = rc == ERROR_SUCCESS;
        const bool present = readable && buf[0] != 0;
        FinishPolicy(c, readable, FlagOn(L"MemoryApplied"), present, L"a paging file is configured",
                     present ? L"PagingFiles is set" : L"PagingFiles is empty");
        push(c);
    }
    {
        HealthCheck c = Make(L"memory.compression", L"Memory and CPU", L"Memory compression", L"memory");
        const std::wstring ps = L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
        const std::wstring text = RunCapture(ps, L"-NoProfile -NonInteractive -Command \"(Get-MMAgent).MemoryCompression\"", 8000);
        const bool off = text.find(L"False") != std::wstring::npos;
        const bool on = text.find(L"True") != std::wstring::npos;
        if (CheckDisabled(c.id)) {
            c.status = HealthStatus::Disabled;
            c.explanation = L"This check is turned off in Settings.";
        } else if (!off && !on) {
            c.status = HealthStatus::Unknown;
            c.severity = 1;
            c.observed = text.empty() ? L"Get-MMAgent returned nothing" : text.substr(0, 80);
            c.explanation = L"Memory compression could not be read. That is not a pass.";
        } else {
            FinishPolicy(c, true, FlagOn(L"MemoryApplied"), off, L"MemoryCompression = False",
                         off ? L"MemoryCompression = False" : L"MemoryCompression = True");
        }
        push(c);
    }
    if (Stopped(cancelled)) return out;

    auto storage = [&](const wchar_t* id, const wchar_t* name, const GUID& sub, const GUID& setting, DWORD target) {
        HealthCheck c = Make(id, L"Storage", name, L"storage");
        DWORD value = 0;
        const bool readable = ReadPower(sub, setting, value);
        FinishPolicy(c, readable, FlagOn(L"StorageApplied"), readable && value == target, Format(L"%lu", target),
                     readable ? Format(L"%lu", value) : L"unreadable");
        push(c);
    };
    storage(L"storage.aspm", L"PCIe link-state power management", kSubPci, kAspm, 0);
    storage(L"storage.diskidle", L"Disk idle timeout", kSubDisk, kDiskIdle, 0);
    storage(L"storage.usb", L"USB selective suspend", kSubUsb, kUsbSuspend, 0);

    {
        HealthCheck c = Make(L"storage.sysmain", L"Storage", L"SysMain service", L"storage");
        DWORD start = 0;
        const int found = ServiceStartType(L"SysMain", start);
        if (found < 0) {
            c.status = HealthStatus::Inaccessible;
            c.severity = 1;
            c.explanation = L"SysMain's start type could not be read.";
        } else if (found == 0) {
            c.status = HealthStatus::Info;
            c.observed = L"SysMain is not installed";
            c.explanation = L"The service is absent, which already meets the intent of disabling it.";
        } else {
            FinishPolicy(c, true, FlagOn(L"StorageApplied"), start == SERVICE_DISABLED, L"start type disabled",
                         Format(L"start type %lu", start));
        }
        push(c);
    }
    {
        HealthCheck c = Make(L"storage.trim", L"Storage", L"TRIM / delete notification", L"storage");
        const std::wstring text = RunCapture(L"C:\\Windows\\System32\\fsutil.exe", L"behavior query DisableDeleteNotify", 8000);
        const bool on = text.find(L"= 0") != std::wstring::npos || text.find(L"=0") != std::wstring::npos;
        const bool off = text.find(L"= 1") != std::wstring::npos;
        if (!on && !off) {
            c.status = HealthStatus::Unknown;
            c.severity = 1;
            c.observed = text.empty() ? L"fsutil returned nothing" : text.substr(0, 120);
            c.explanation = L"TRIM state could not be read. That is not a pass.";
        } else {
            FinishPolicy(c, true, FlagOn(L"StorageApplied"), on, L"DisableDeleteNotify = 0 (TRIM on)",
                         on ? L"DisableDeleteNotify = 0" : L"DisableDeleteNotify = 1");
        }
        push(c);
    }

    AppendHealthLog(Summarize(out).headline);
    return out;
}

int SelfTestHealth() {
    int failed = 0;
    auto expect = [&](bool cond) {
        if (!cond) ++failed;
    };
    expect(HealthCountsAsPass(HealthStatus::Pass));
    expect(!HealthCountsAsPass(HealthStatus::Attention));
    expect(!HealthCountsAsPass(HealthStatus::Unknown));
    expect(!HealthCountsAsPass(HealthStatus::Error));
    expect(!HealthCountsAsPass(HealthStatus::Inaccessible));
    expect(!HealthCountsAsPass(HealthStatus::Unsupported));
    expect(!HealthCountsAsPass(HealthStatus::Info));
    expect(!HealthCountsAsPass(HealthStatus::Disabled));
    expect(HealthNeedsAttention(HealthStatus::Attention));
    expect(HealthNeedsAttention(HealthStatus::Error));
    expect(!HealthNeedsAttention(HealthStatus::Info));
    expect(!HealthNeedsAttention(HealthStatus::Pass));

    std::vector<HealthCheck> sample(3);
    sample[0].status = HealthStatus::Pass;
    sample[1].status = HealthStatus::Attention;
    sample[2].status = HealthStatus::Error;
    const HealthSummary s = Summarize(sample);
    expect(s.pass == 1);
    expect(s.attention == 2);
    expect(s.headline.find(L"2") != std::wstring::npos);

    sample[1].status = HealthStatus::Unknown;
    sample[2].status = HealthStatus::Inaccessible;
    const HealthSummary u = Summarize(sample);
    expect(u.pass == 1);
    expect(u.attention == 0);
    expect(u.other == 2);
    expect(u.headline.find(L"could not") != std::wstring::npos);
    return failed == 0 ? 0 : 1;
}

}  // namespace gui
