// Watch.cpp - what Synchronus notices while it sits in the tray: a game
// starting, notifications that should wait until the game ends, and hardware
// that is not what it was at the last tune.
#include "Util.h"

#include <setupapi.h>
#include <tlhelp32.h>

#pragma comment(lib, "setupapi.lib")

namespace gui {
namespace {

using namespace util;

constexpr wchar_t kToastPolicyKey[] = L"Software\\Policies\\Microsoft\\Windows\\CurrentVersion\\PushNotifications";
constexpr wchar_t kToastPolicyName[] = L"NoToastApplicationNotification";
constexpr wchar_t kGameBarKey[] = L"Software\\Microsoft\\GameBar";
constexpr wchar_t kGameBarName[] = L"UseNexusForGameBarEnabled";

const GUID kDisplayClass = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool InList(const std::vector<std::wstring>& list, const std::wstring& lowerName) {
    for (const auto& item : list)
        if (Lower(item) == lowerName) return true;
    return false;
}

// Programs that ship inside a game library and are not the game.
bool HelperName(const std::wstring& lowerName) {
    static const wchar_t* kParts[] = {L"launcher",  L"crash",     L"helper",  L"service",   L"updater", L"setup",
                                      L"install",   L"redist",    L"unins",   L"anticheat", L"beservice", L"battleye",
                                      L"reporter",  L"webhelper", L"cefproc", L"bootstrap", L"dxsetup", L"dotnet",
                                      L"overlay",   L"benchmarktool"};
    for (const wchar_t* part : kParts)
        if (lowerName.find(part) != std::wstring::npos) return true;
    return false;
}

bool InLibrary(const std::wstring& lowerPath) {
    static const wchar_t* kNever[] = {L"\\epic games\\launcher\\",   L"\\riot games\\riot client\\", L"\\wallpaper_engine\\",
                                      L"\\steamapps\\common\\steamvr\\", L"\\steamworks shared\\",   L"\\steam controller configs\\"};
    for (const wchar_t* part : kNever)
        if (lowerPath.find(part) != std::wstring::npos) return false;
    static const wchar_t* kLibraries[] = {L"\\steamapps\\common\\", L"\\epic games\\",  L"\\gog galaxy\\games\\",
                                          L"\\gog games\\",         L"\\xboxgames\\",   L"\\riot games\\",
                                          L"\\ubisoft game launcher\\games\\", L"\\ea games\\", L"\\origin games\\"};
    for (const wchar_t* part : kLibraries)
        if (lowerPath.find(part) != std::wstring::npos) return true;
    return false;
}

struct Signature {
    std::wstring cpu, gpu, driver, bios, windows;
};

Signature ReadSignature() {
    Signature s;
    GetSz(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", s.cpu);
    while (!s.cpu.empty() && s.cpu.back() == L' ') s.cpu.pop_back();
    std::wstring version, date;
    GetSz(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"BIOSVersion", version);
    GetSz(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"BIOSReleaseDate", date);
    s.bios = version + (date.empty() ? L"" : L" (" + date + L")");
    GetSz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuild", s.windows);

    const HDEVINFO set = SetupDiGetClassDevsW(&kDisplayClass, nullptr, nullptr, DIGCF_PRESENT);
    if (set != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dev{};
        dev.cbSize = sizeof(dev);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
            wchar_t driver[256]{};
            if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_DRIVER, nullptr, reinterpret_cast<BYTE*>(driver),
                                                   sizeof(driver) - sizeof(wchar_t), nullptr))
                continue;
            const std::wstring key = std::wstring(L"SYSTEM\\CurrentControlSet\\Control\\Class\\") + driver;
            std::wstring name, ver;
            if (!GetSz(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverDesc", name)) continue;
            GetSz(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverVersion", ver);
            if (!s.gpu.empty()) s.gpu += L" + ";
            s.gpu += name;
            if (!s.driver.empty()) s.driver += L" + ";
            s.driver += ver.empty() ? L"unknown" : ver;
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    return s;
}

void SaveSignature(const Signature& s) {
    SetSz(HKEY_LOCAL_MACHINE, kAppKey, L"Hw_cpu", s.cpu);
    SetSz(HKEY_LOCAL_MACHINE, kAppKey, L"Hw_gpu", s.gpu);
    SetSz(HKEY_LOCAL_MACHINE, kAppKey, L"Hw_driver", s.driver);
    SetSz(HKEY_LOCAL_MACHINE, kAppKey, L"Hw_bios", s.bios);
    SetSz(HKEY_LOCAL_MACHINE, kAppKey, L"Hw_windows", s.windows);
}

void NotifyPolicy() {
    SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Policy"));
}

}  // namespace

// ------------------------------------------------------------------ games
GameRules LoadGameRules() {
    GameRules r;
    DWORD v = 0;
    if (GetDword(HKEY_CURRENT_USER, kUserKey, L"GameLibraries", v)) r.libraries = v != 0;
    r.include = GetMulti(HKEY_CURRENT_USER, kUserKey, L"GameInclude");
    r.exclude = GetMulti(HKEY_CURRENT_USER, kUserKey, L"GameExclude");
    return r;
}

void SaveGameRules(const GameRules& rules) {
    SetDword(HKEY_CURRENT_USER, kUserKey, L"GameLibraries", rules.libraries ? 1 : 0);
    SetMulti(HKEY_CURRENT_USER, kUserKey, L"GameInclude", rules.include);
    SetMulti(HKEY_CURRENT_USER, kUserKey, L"GameExclude", rules.exclude);
}

std::vector<std::wstring> WindowsKnownGames() {
    std::vector<std::wstring> out;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"System\\GameConfigStore\\Children", 0, KEY_READ, &root) != ERROR_SUCCESS) return out;
    wchar_t name[128];
    for (DWORD i = 0; i < 4096; ++i) {
        DWORD len = _countof(name);
        if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        const std::wstring key = std::wstring(L"System\\GameConfigStore\\Children\\") + name;
        std::wstring path;
        if (GetSz(HKEY_CURRENT_USER, key.c_str(), L"MatchedExeFullPath", path) && !path.empty()) out.push_back(Lower(path));
    }
    RegCloseKey(root);
    return out;
}

bool LooksLikeGame(const std::wstring& path, const GameRules& rules, const std::vector<std::wstring>& known) {
    const std::wstring lowerPath = Lower(path);
    const std::wstring name = FileNameOf(lowerPath);
    if (name.empty()) return false;
    if (InList(rules.exclude, name)) return false;
    if (InList(rules.include, name)) return true;
    if (!rules.libraries) return false;
    if (HelperName(name)) return false;
    if (InLibrary(lowerPath)) return true;
    for (const auto& k : known)
        if (k == lowerPath) return true;
    return false;
}

bool FindRunningGame(const GameRules& rules, const std::vector<std::wstring>& known,
                     std::vector<std::pair<DWORD, int>>& verdicts, GameHit& hit) {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    std::vector<std::pair<DWORD, int>> next;
    next.reserve(verdicts.size() + 16);
    bool found = false;
    const DWORD self = GetCurrentProcessId();
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            const DWORD pid = pe.th32ProcessID;
            if (pid == 0 || pid == 4 || pid == self) continue;
            int verdict = -1;
            for (const auto& v : verdicts)
                if (v.first == pid) verdict = v.second;
            std::wstring path;
            if (verdict != 0) {
                // A known pid that was a game is read again so the name shown is current.
                DWORD session = 0;
                if (!ProcessIdToSessionId(pid, &session) || session == 0) {
                    verdict = 0;
                } else {
                    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                    if (!process) {
                        verdict = 0;
                    } else {
                        wchar_t buf[MAX_PATH * 2]{};
                        DWORD n = _countof(buf);
                        if (QueryFullProcessImageNameW(process, 0, buf, &n)) path.assign(buf, n);
                        CloseHandle(process);
                        verdict = !path.empty() && LooksLikeGame(path, rules, known) ? 1 : 0;
                    }
                }
            }
            next.emplace_back(pid, verdict);
            if (verdict == 1 && !found) {
                found = true;
                hit.pid = pid;
                hit.path = path;
                hit.exe = FileNameOf(path);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    verdicts.swap(next);
    return found;
}

// ------------------------------------------------- notifications and Game Bar
bool QuietHeld() {
    DWORD v = 0;
    return GetDword(HKEY_CURRENT_USER, kUserKey, L"QuietHeld", v) && v != 0;
}

bool QuietBegin() {
    if (QuietHeld()) return true;
    DWORD prev = 0;
    const bool had = GetDword(HKEY_CURRENT_USER, kToastPolicyKey, kToastPolicyName, prev);
    // Already off by policy: there is nothing to hold, and nothing to put back later.
    if (had && prev == 1) return true;
    if (!SetDword(HKEY_CURRENT_USER, kToastPolicyKey, kToastPolicyName, 1)) return false;
    SetDword(HKEY_CURRENT_USER, kUserKey, L"QuietHad", had ? 1 : 0);
    SetDword(HKEY_CURRENT_USER, kUserKey, L"QuietPrev", prev);
    SetDword(HKEY_CURRENT_USER, kUserKey, L"QuietHeld", 1);
    NotifyPolicy();
    return true;
}

void QuietEnd() {
    if (!QuietHeld()) return;
    DWORD had = 0, prev = 0;
    GetDword(HKEY_CURRENT_USER, kUserKey, L"QuietHad", had);
    GetDword(HKEY_CURRENT_USER, kUserKey, L"QuietPrev", prev);
    if (had) SetDword(HKEY_CURRENT_USER, kToastPolicyKey, kToastPolicyName, prev);
    else DeleteValue(HKEY_CURRENT_USER, kToastPolicyKey, kToastPolicyName);
    DeleteValue(HKEY_CURRENT_USER, kUserKey, L"QuietHeld");
    DeleteValue(HKEY_CURRENT_USER, kUserKey, L"QuietHad");
    DeleteValue(HKEY_CURRENT_USER, kUserKey, L"QuietPrev");
    NotifyPolicy();
}

bool GameBarButtonOff() {
    DWORD v = 1;
    return GetDword(HKEY_CURRENT_USER, kGameBarKey, kGameBarName, v) && v == 0;
}

void SetGameBarButtonOff(bool off) {
    if (off) {
        if (!ValueExists(HKEY_CURRENT_USER, kUserKey, L"GameBarButtonSaved")) {
            DWORD prev = 1;
            const bool had = GetDword(HKEY_CURRENT_USER, kGameBarKey, kGameBarName, prev);
            SetDword(HKEY_CURRENT_USER, kUserKey, L"GameBarButtonSaved", 1);
            SetDword(HKEY_CURRENT_USER, kUserKey, L"GameBarButtonHad", had ? 1 : 0);
            SetDword(HKEY_CURRENT_USER, kUserKey, L"GameBarButtonPrev", prev);
        }
        SetDword(HKEY_CURRENT_USER, kGameBarKey, kGameBarName, 0);
        return;
    }
    // Back on. A value Synchronus created is removed; one that was already there is set to on.
    DWORD had = 0;
    const bool saved = ValueExists(HKEY_CURRENT_USER, kUserKey, L"GameBarButtonSaved");
    GetDword(HKEY_CURRENT_USER, kUserKey, L"GameBarButtonHad", had);
    if (saved && !had) DeleteValue(HKEY_CURRENT_USER, kGameBarKey, kGameBarName);
    else SetDword(HKEY_CURRENT_USER, kGameBarKey, kGameBarName, 1);
    DeleteValue(HKEY_CURRENT_USER, kUserKey, L"GameBarButtonSaved");
    DeleteValue(HKEY_CURRENT_USER, kUserKey, L"GameBarButtonHad");
    DeleteValue(HKEY_CURRENT_USER, kUserKey, L"GameBarButtonPrev");
}

void RestoreGameSettings() {
    QuietEnd();
    if (ValueExists(HKEY_CURRENT_USER, kUserKey, L"GameBarButtonSaved")) SetGameBarButtonOff(false);
}

// --------------------------------------------------------- hardware changes
std::vector<HardwareChange> DetectHardwareChanges() {
    std::vector<HardwareChange> out;
    const Signature now = ReadSignature();
    Signature was;
    const bool have = GetSz(HKEY_LOCAL_MACHINE, kAppKey, L"Hw_cpu", was.cpu);
    if (!have) {
        SaveSignature(now);
        return out;
    }
    GetSz(HKEY_LOCAL_MACHINE, kAppKey, L"Hw_gpu", was.gpu);
    GetSz(HKEY_LOCAL_MACHINE, kAppKey, L"Hw_driver", was.driver);
    GetSz(HKEY_LOCAL_MACHINE, kAppKey, L"Hw_bios", was.bios);
    GetSz(HKEY_LOCAL_MACHINE, kAppKey, L"Hw_windows", was.windows);
    auto add = [&](const wchar_t* part, const std::wstring& what, const wchar_t* advice, const wchar_t* step) {
        HardwareChange c;
        c.part = part;
        c.what = what;
        c.advice = advice;
        c.step = step;
        out.push_back(std::move(c));
    };
    if (!now.gpu.empty() && now.gpu != was.gpu) {
        add(L"gpu", L"Graphics card: " + (was.gpu.empty() ? L"none" : was.gpu) + L"  \u2192  " + now.gpu,
            L"Run GPU driver, GPU settings, and Afterburner again for this card.", L"gpuclean");
    } else if (!now.driver.empty() && now.driver != was.driver) {
        add(L"driver", L"Graphics driver: " + was.driver + L"  \u2192  " + now.driver,
            L"A driver install resets the vendor profile. Run GPU settings again, and Afterburner if you use its curve.",
            L"gpu");
    }
    if (!now.cpu.empty() && now.cpu != was.cpu) {
        add(L"cpu", L"Processor: " + was.cpu + L"  \u2192  " + now.cpu,
            L"The timer and power settings were measured on the old processor. Measure timing and run CPU power again.",
            L"measure");
    }
    if (!now.bios.empty() && now.bios != was.bios) {
        add(L"bios", L"BIOS: " + was.bios + L"  \u2192  " + now.bios,
            L"A BIOS update usually resets its settings. Check the restart notes, then measure timing again.", L"review");
    }
    if (!now.windows.empty() && now.windows != was.windows) {
        add(L"windows", L"Windows build: " + was.windows + L"  \u2192  " + now.windows,
            L"Timer behaviour can change between Windows builds. Measure timing again.", L"measure");
    }
    return out;
}

void AcceptHardware() { SaveSignature(ReadSignature()); }

std::wstring HardwareSummary() {
    const Signature s = ReadSignature();
    std::wstring out = s.cpu;
    if (!s.gpu.empty()) out += (out.empty() ? L"" : L" | ") + s.gpu;
    if (!s.windows.empty()) out += (out.empty() ? L"" : L" | ") + (L"Windows build " + s.windows);
    return out;
}

}  // namespace gui
