// Network.cpp - the Network step.
//
// Every value written here is one the adapter's own driver lists under
// Ndi\params, and the value chosen is one the driver names in its list. The
// driver reads these when the adapter starts, so nothing here restarts an
// adapter or drops a connection: the change is live after the next restart.
// The value that was there before is kept the first time it is replaced.
#include "Util.h"

#include <powrprof.h>
#include <setupapi.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "powrprof.lib")

namespace gui {
namespace {

using namespace util;

constexpr wchar_t kClassRoot[] = L"SYSTEM\\CurrentControlSet\\Control\\Class\\";
constexpr wchar_t kBackupRoot[] = L"SOFTWARE\\Synchronus\\Network";
constexpr wchar_t kDeliveryKey[] = L"SOFTWARE\\Policies\\Microsoft\\Windows\\DeliveryOptimization";
constexpr wchar_t kDeliveryName[] = L"DODownloadMode";
constexpr wchar_t kCategory[] = L"Network";

const GUID kNetClass = {0x4d36e972, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
// Wireless Adapter Settings, Power Saving Mode. Index 0 is Maximum Performance.
const GUID kSubWireless = {0x19cbb8fa, 0x5279, 0x450e, {0x9f, 0xac, 0x8a, 0x3d, 0x5f, 0xed, 0xd0, 0xc1}};
const GUID kWirelessPower = {0x12bbebe6, 0x58d6, 0x4636, {0x95, 0xbb, 0x32, 0x17, 0xef, 0x86, 0x7c, 0x1a}};

struct Rule {
    const wchar_t* keyword;
    const wchar_t* target;
    int group;       // 0 power, 1 moderation
    bool turnsOff;   // the target should read as off in the driver's own list
    const wchar_t* title;
    const wchar_t* why;
};

// Keywords that start with * are defined by Windows and mean the same on every
// driver. The rest are the vendor's own, written only where that driver has them.
const Rule kRules[] = {
    {L"*EEE", L"0", 0, true, L"Energy-Efficient Ethernet",
     L"EEE lets the link sleep between packets. Waking it adds delay to the first packet after a quiet moment, "
     L"which is exactly when a game sends input. Off keeps the link awake."},
    {L"EEE", L"0", 0, true, L"Energy-Efficient Ethernet (vendor)",
     L"The same link sleep under the name older drivers use for it."},
    {L"AdvancedEEE", L"0", 0, true, L"Advanced EEE",
     L"A deeper form of the same link sleep. Off for the same reason."},
    {L"EEELinkAdvertisement", L"0", 0, true, L"EEE link advertisement",
     L"Stops the adapter from offering Energy-Efficient Ethernet to the switch, so the link never negotiates it."},
    {L"EnableGreenEthernet", L"0", 0, true, L"Green Ethernet",
     L"Lowers transmit power on short cables and when the link is idle. Off keeps the adapter at full power."},
    {L"PowerSavingMode", L"0", 0, true, L"link power saving",
     L"Powers the adapter down while it thinks the link is idle. Off stops it from sleeping between bursts."},
    {L"*SelectiveSuspend", L"0", 0, true, L"selective suspend",
     L"Lets Windows suspend the adapter when it is idle. Resuming it delays the next packet."},
    {L"*DeviceSleepOnDisconnect", L"0", 0, true, L"sleep on disconnect",
     L"Puts the adapter to sleep when the link drops. Off brings the link back sooner after a reconnect."},
    {L"ULPMode", L"0", 0, true, L"ultra low power mode",
     L"An Intel idle state for the adapter. Off keeps it ready."},
    {L"AutoPowerSaveModeEnabled", L"0", 0, true, L"auto power save",
     L"Lets the driver drop into power save on its own. Off keeps the adapter awake."},
    {L"LowPowerEnable", L"0", 0, true, L"Wi-Fi power saving",
     L"The radio dozes between beacons and the access point holds packets for it until it wakes. "
     L"Off keeps the radio listening, so a packet is delivered when it arrives."},
    {L"MIMOPowerSaveMode", L"3", 0, false, L"MIMO power save",
     L"Turns antenna chains off to save power and back on when traffic picks up. No SMPS keeps every chain on."},
    {L"uAPSDSupport", L"0", 0, true, L"U-APSD",
     L"A Wi-Fi power-save delivery mode. Off avoids the access point buffering packets for a sleeping radio."},
    {L"*InterruptModeration", L"0", 1, true, L"interrupt moderation",
     L"The adapter waits to collect several packets before it interrupts the CPU. That saves CPU time on big "
     L"downloads and delays every packet a little. Off delivers each packet at once. Expect more CPU use while a "
     L"large download is running."},
    {L"*RSS", L"1", 1, false, L"receive side scaling",
     L"Spreads incoming packets across CPU cores so one core is not the bottleneck. On."},
};

struct Live {
    bool exists = false;
    DWORD type = REG_SZ;
    std::wstring text;
};

Live ReadLive(const std::wstring& key, const wchar_t* name) {
    Live v;
    DWORD type = 0, size = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), name, RRF_RT_ANY, &type, nullptr, &size) != ERROR_SUCCESS) return v;
    if (type == REG_DWORD) {
        DWORD data = 0;
        if (!GetDword(HKEY_LOCAL_MACHINE, key.c_str(), name, data)) return v;
        v.exists = true;
        v.type = REG_DWORD;
        v.text = std::to_wstring(data);
    } else if (type == REG_SZ) {
        if (!GetSz(HKEY_LOCAL_MACHINE, key.c_str(), name, v.text)) return v;
        v.exists = true;
        v.type = REG_SZ;
    }
    return v;
}

bool WriteLive(const std::wstring& key, const wchar_t* name, DWORD type, const std::wstring& text) {
    if (type == REG_DWORD) return SetDword(HKEY_LOCAL_MACHINE, key.c_str(), name, wcstoul(text.c_str(), nullptr, 10));
    return SetSz(HKEY_LOCAL_MACHINE, key.c_str(), name, text);
}

// What the driver calls this value, or empty when it has no list for the keyword.
// listed is false when the driver has a list and the value is not in it.
std::wstring EnumLabel(const std::wstring& adapterKey, const wchar_t* keyword, const wchar_t* value, bool& listed) {
    listed = true;
    const std::wstring enumKey = adapterKey + L"\\Ndi\\params\\" + keyword + L"\\enum";
    if (!KeyExists(HKEY_LOCAL_MACHINE, enumKey.c_str())) return {};
    std::wstring label;
    if (!GetSz(HKEY_LOCAL_MACHINE, enumKey.c_str(), value, label)) {
        listed = false;
        return {};
    }
    return label;
}

// A driver can reuse a name with the opposite meaning. When its own label for
// the value plainly says the other thing, the value is left alone.
bool LabelContradicts(const Rule& rule, const std::wstring& label) {
    if (label.empty()) return false;
    const std::wstring l = Lower(label);
    const bool saysOff = l.find(L"disab") != std::wstring::npos || l == L"off" || l.find(L"no ") == 0 ||
                         l.find(L"never") != std::wstring::npos;
    const bool saysOn = (l.find(L"enab") != std::wstring::npos && l.find(L"disab") == std::wstring::npos) || l == L"on" ||
                        l.find(L"auto") != std::wstring::npos;
    if (rule.turnsOff) return saysOn && !saysOff;
    if (wcscmp(rule.keyword, L"*RSS") == 0) return saysOff;
    return l.find(L"auto") != std::wstring::npos || l.find(L"static") != std::wstring::npos ||
           l.find(L"dynamic") != std::wstring::npos;
}

std::wstring Shown(const std::wstring& adapterKey, const wchar_t* keyword, const std::wstring& value) {
    if (value.empty()) return L"(driver default)";
    bool listed = true;
    const std::wstring label = EnumLabel(adapterKey, keyword, value.c_str(), listed);
    return label.empty() ? value : label;
}

std::vector<NetAdapter> Adapters(const NetOptions& options) {
    std::vector<NetAdapter> out;
    const HDEVINFO set = SetupDiGetClassDevsW(&kNetClass, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof(dev);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        wchar_t driver[256]{};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_DRIVER, nullptr, reinterpret_cast<BYTE*>(driver),
                                               sizeof(driver) - sizeof(wchar_t), nullptr))
            continue;
        NetAdapter a;
        a.key = std::wstring(kClassRoot) + driver;
        DWORD traits = 0;
        // 0x4 is a physical adapter. Virtual switches, VPN taps, and filters are skipped.
        if (!GetDword(HKEY_LOCAL_MACHINE, a.key.c_str(), L"Characteristics", traits) || !(traits & 0x4)) continue;
        if (!GetSz(HKEY_LOCAL_MACHINE, a.key.c_str(), L"NetCfgInstanceId", a.guid) || a.guid.empty()) continue;
        wchar_t friendly[256]{};
        if (SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_FRIENDLYNAME, nullptr, reinterpret_cast<BYTE*>(friendly),
                                              sizeof(friendly) - sizeof(wchar_t), nullptr) &&
            friendly[0])
            a.name = friendly;
        else if (!GetSz(HKEY_LOCAL_MACHINE, a.key.c_str(), L"DriverDesc", a.name))
            a.name = L"Network adapter";
        const Live media = ReadLive(a.key, L"*MediaType");
        const Live ifType = ReadLive(a.key, L"*IfType");
        a.wifi = media.text == L"16" || ifType.text == L"71";

        for (const Rule& rule : kRules) {
            if (rule.group == 0 && !options.power) continue;
            if (rule.group == 1 && !options.moderation) continue;
            const std::wstring paramKey = a.key + L"\\Ndi\\params\\" + rule.keyword;
            if (!KeyExists(HKEY_LOCAL_MACHINE, paramKey.c_str())) continue;
            bool listed = true;
            const std::wstring label = EnumLabel(a.key, rule.keyword, rule.target, listed);
            if (!listed || LabelContradicts(rule, label)) continue;
            Live live = ReadLive(a.key, rule.keyword);
            std::wstring current = live.text;
            if (!live.exists) GetSz(HKEY_LOCAL_MACHINE, paramKey.c_str(), L"default", current);
            NetChange c;
            c.keyword = rule.keyword;
            c.title = rule.title;
            c.why = rule.why;
            c.group = rule.group;
            c.from = current;
            c.to = rule.target;
            c.already = current == rule.target;
            a.changes.push_back(std::move(c));
        }
        out.push_back(std::move(a));
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

bool ReadWifiPower(bool ac, DWORD& value) {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return false;
    const DWORD rc = ac ? PowerReadACValueIndex(nullptr, scheme, &kSubWireless, &kWirelessPower, &value)
                        : PowerReadDCValueIndex(nullptr, scheme, &kSubWireless, &kWirelessPower, &value);
    LocalFree(scheme);
    return rc == ERROR_SUCCESS;
}

bool WriteWifiPower(DWORD ac, DWORD dc, bool writeAc, bool writeDc) {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return false;
    bool ok = true;
    if (writeAc) ok = PowerWriteACValueIndex(nullptr, scheme, &kSubWireless, &kWirelessPower, ac) == ERROR_SUCCESS && ok;
    if (writeDc) ok = PowerWriteDCValueIndex(nullptr, scheme, &kSubWireless, &kWirelessPower, dc) == ERROR_SUCCESS && ok;
    PowerSetActiveScheme(nullptr, scheme);
    LocalFree(scheme);
    return ok;
}

// Keeps the value a keyword had before the first change. A later run leaves that copy alone,
// so a restore goes back to the adapter's own setting and not to an earlier Synchronus one.
void KeepOriginal(const NetAdapter& a, const wchar_t* keyword) {
    const std::wstring backup = std::wstring(kBackupRoot) + L"\\" + a.guid;
    const std::wstring absent = std::wstring(keyword) + L"|absent";
    if (ValueExists(HKEY_LOCAL_MACHINE, backup.c_str(), keyword) || ValueExists(HKEY_LOCAL_MACHINE, backup.c_str(), absent.c_str()))
        return;
    const Live live = ReadLive(a.key, keyword);
    if (!live.exists) SetDword(HKEY_LOCAL_MACHINE, backup.c_str(), absent.c_str(), 1);
    else if (live.type == REG_DWORD) SetDword(HKEY_LOCAL_MACHINE, backup.c_str(), keyword, wcstoul(live.text.c_str(), nullptr, 10));
    else SetSz(HKEY_LOCAL_MACHINE, backup.c_str(), keyword, live.text);
}

std::wstring AdapterKeyFor(const std::wstring& guid) {
    for (int i = 0; i < 256; ++i) {
        const std::wstring key = Fmt(L"%s{4d36e972-e325-11ce-bfc1-08002be10318}\\%04d", kClassRoot, i);
        std::wstring have;
        if (GetSz(HKEY_LOCAL_MACHINE, key.c_str(), L"NetCfgInstanceId", have) && IEquals(have, guid)) return key;
    }
    return {};
}

std::wstring KindName(const std::vector<NetAdapter>& all, size_t index) {
    const bool wifi = all[index].wifi;
    int same = 0, before = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i].wifi != wifi || all[i].changes.empty()) continue;
        ++same;
        if (i < index) ++before;
    }
    const wchar_t* base = wifi ? L"Wi-Fi" : L"Ethernet";
    return same > 1 ? Fmt(L"%s %d", base, before + 1) : std::wstring(base);
}

void Finish(HealthCheck& c, bool readable, bool match, bool stepOn, const std::wstring& expected,
            const std::wstring& observed) {
    c.expected = expected;
    c.observed = observed;
    if (CheckDisabled(c.id)) {
        c.status = HealthStatus::Disabled;
        c.explanation = L"This check is turned off in Settings.";
    } else if (!readable) {
        c.status = HealthStatus::Inaccessible;
        c.severity = 1;
        c.explanation = L"The live value could not be read. That is not a pass.";
    } else if (match) {
        c.status = HealthStatus::Pass;
        c.explanation = L"Live. Adapter settings take effect when the adapter next starts.";
    } else if (!NetworkApplied()) {
        c.status = HealthStatus::Info;
        c.explanation = L"The Network step has not been run on this PC.";
    } else if (!stepOn) {
        c.status = HealthStatus::Info;
        c.explanation = L"This part of the Network step is switched off.";
    } else {
        c.status = HealthStatus::Attention;
        c.severity = 1;
        c.repair = L"network";
        c.explanation = L"Not live. Repair puts the Synchronus value back.";
    }
}

}  // namespace

NetOptions LoadNetOptions() {
    NetOptions o;
    DWORD v = 0;
    if (GetDword(HKEY_CURRENT_USER, kUserKey, L"NetPower", v)) o.power = v != 0;
    if (GetDword(HKEY_CURRENT_USER, kUserKey, L"NetModeration", v)) o.moderation = v != 0;
    if (GetDword(HKEY_CURRENT_USER, kUserKey, L"NetDelivery", v)) o.delivery = v != 0;
    return o;
}

void SaveNetOptions(const NetOptions& options) {
    SetDword(HKEY_CURRENT_USER, kUserKey, L"NetPower", options.power ? 1 : 0);
    SetDword(HKEY_CURRENT_USER, kUserKey, L"NetModeration", options.moderation ? 1 : 0);
    SetDword(HKEY_CURRENT_USER, kUserKey, L"NetDelivery", options.delivery ? 1 : 0);
}

bool NetworkApplied() {
    DWORD v = 0;
    return GetDword(HKEY_LOCAL_MACHINE, kAppKey, L"NetworkApplied", v) && v != 0;
}

NetPlan PlanNetwork(const NetOptions& options) {
    NetPlan plan;
    plan.adapters = Adapters(options);
    for (const auto& a : plan.adapters) {
        if (a.wifi) plan.hasWifi = true;
        for (const auto& c : a.changes)
            if (!c.already) ++plan.pending;
    }
    DWORD value = 0;
    if (ReadWifiPower(true, value)) {
        plan.wifiPowerReadable = true;
        plan.wifiPowerAc = value;
    }
    if (plan.hasWifi && options.power && !options.skipPlan && plan.wifiPowerReadable && plan.wifiPowerAc != 0) ++plan.pending;
    DWORD mode = 0;
    if (GetDword(HKEY_LOCAL_MACHINE, kDeliveryKey, kDeliveryName, mode)) {
        plan.deliveryPolicySet = true;
        plan.deliveryMode = mode;
    }
    if (options.delivery && !(plan.deliveryPolicySet && plan.deliveryMode == 0)) ++plan.pending;
    return plan;
}

bool ApplyNetwork(const StepIo& io, const NetOptions& options) {
    Steps steps(io);
    steps.Narrate(L"Network",
                  L"Turns off the power saving and packet batching that delay a packet, on each adapter that has "
                  L"those settings. Nothing is restarted, so the connection stays up. The adapters pick the "
                  L"changes up at the next restart.");
    steps.Phase(L"NETWORK", L"reading the adapters");
    steps.Out(L"Applying network settings. Adapter changes take effect at the next restart.");

    const NetPlan plan = PlanNetwork(options);
    struct Row {
        size_t adapter = 0;
        size_t change = 0;
        int kind = 0;  // 0 adapter keyword, 1 Wi-Fi power plan, 2 delivery
    };
    std::vector<Row> rows;
    for (size_t ai = 0; ai < plan.adapters.size(); ++ai) {
        const NetAdapter& a = plan.adapters[ai];
        const std::wstring kind = KindName(plan.adapters, ai);
        for (size_t ci = 0; ci < a.changes.size(); ++ci) {
            const NetChange& c = a.changes[ci];
            steps.Add(c.keyword, kind + L": " + c.title, c.why + L" Adapter: " + a.name + L".",
                      Shown(a.key, c.keyword.c_str(), c.from) + L"  \u2192  " + Shown(a.key, c.keyword.c_str(), c.to));
            rows.push_back({ai, ci, 0});
        }
    }
    if (plan.hasWifi && options.power && !options.skipPlan && plan.wifiPowerReadable) {
        steps.Add(L"WIFIPOWER", L"Wi-Fi power plan setting",
                  L"The power plan has its own Wi-Fi power saving level. Maximum Performance tells the radio not "
                  L"to doze. This one applies at once.",
                  Fmt(L"%lu  \u2192  0 (Maximum Performance)", plan.wifiPowerAc));
        rows.push_back({0, 0, 1});
    }
    if (options.delivery) {
        steps.Add(L"DELIVERY", L"Windows Update sharing with other PCs",
                  L"Delivery Optimization uploads pieces of updates to other PCs and downloads from them. That "
                  L"traffic competes with a game for the connection. Updates still download from Microsoft.",
                  plan.deliveryPolicySet ? Fmt(L"%lu  \u2192  0 (Microsoft only)", plan.deliveryMode)
                                         : L"(not set)  \u2192  0 (Microsoft only)");
        rows.push_back({0, 0, 2});
    }
    if (rows.empty()) {
        steps.Narrate(L"Network", L"No adapter on this PC has these settings, and nothing else was selected.");
        steps.Out(L"No network setting applies on this PC.");
        return true;
    }
    steps.Publish();
    steps.Dwell(1200);

    int wrote = 0, failed = 0, kept = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (steps.Cancelled()) return false;
        const Row& row = rows[i];
        steps.Begin(i, L"NETWORK", 1400);
        if (steps.Cancelled()) return false;
        bool ok = false;
        if (row.kind == 0) {
            const NetAdapter& a = plan.adapters[row.adapter];
            const NetChange& c = a.changes[row.change];
            if (c.already) {
                ok = true;
                ++kept;
            } else {
                KeepOriginal(a, c.keyword.c_str());
                const Live live = ReadLive(a.key, c.keyword.c_str());
                ok = WriteLive(a.key, c.keyword.c_str(), live.exists ? live.type : REG_SZ, c.to);
                if (ok) ++wrote;
            }
        } else if (row.kind == 1) {
            DWORD ac = 0, dc = 0;
            const bool hadAc = ReadWifiPower(true, ac);
            const bool hadDc = ReadWifiPower(false, dc);
            if (!ValueExists(HKEY_LOCAL_MACHINE, kAppKey, L"Net_WifiPower_saved")) {
                SetDword(HKEY_LOCAL_MACHINE, kAppKey, L"Net_WifiPower_saved", 1);
                if (hadAc) SetDword(HKEY_LOCAL_MACHINE, kAppKey, L"Net_WifiPower_ac", ac);
                if (hadDc) SetDword(HKEY_LOCAL_MACHINE, kAppKey, L"Net_WifiPower_dc", dc);
            }
            ok = WriteWifiPower(0, 0, true, true);
            DWORD confirm = 1;
            ok = ok && ReadWifiPower(true, confirm) && confirm == 0;
            if (ok) ++wrote;
        } else {
            if (!ValueExists(HKEY_LOCAL_MACHINE, kAppKey, L"Net_Delivery_saved")) {
                DWORD prev = 0;
                const bool had = GetDword(HKEY_LOCAL_MACHINE, kDeliveryKey, kDeliveryName, prev);
                SetDword(HKEY_LOCAL_MACHINE, kAppKey, L"Net_Delivery_saved", 1);
                SetDword(HKEY_LOCAL_MACHINE, kAppKey, L"Net_Delivery_had", had ? 1 : 0);
                if (had) SetDword(HKEY_LOCAL_MACHINE, kAppKey, L"Net_Delivery_prev", prev);
            }
            ok = SetDword(HKEY_LOCAL_MACHINE, kDeliveryKey, kDeliveryName, 0);
            if (ok) ++wrote;
        }
        if (!ok) {
            ++failed;
            steps.Err(L"    write failed. Continuing with the rest.");
        } else {
            steps.Out(L"    written.");
        }
        steps.End(i, ok);
    }
    if (!wrote && !kept) {
        steps.Err(L"No network setting could be written.");
        return false;
    }
    SetDword(HKEY_LOCAL_MACHINE, kAppKey, L"NetworkApplied", 1);
    steps.Narrate(L"Network settings applied",
                  Fmt(L"Wrote %d setting(s)%s%s. Adapter settings are picked up at the next restart.", wrote,
                      kept ? Fmt(L", %d were already in place", kept).c_str() : L"",
                      failed ? Fmt(L", %d could not be written", failed).c_str() : L""));
    steps.Out(Fmt(L"Applied %d network setting(s)%s. Restart for the adapter settings to take effect.", wrote,
                  failed ? Fmt(L", %d failed", failed).c_str() : L""));
    steps.Dwell(1000);
    return true;
}

bool RestoreNetwork(const StepIo* io) {
    auto say = [&](const std::wstring& line) {
        if (io && io->out) io->out(line);
    };
    int restored = 0;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kBackupRoot, 0, KEY_READ, &root) == ERROR_SUCCESS) {
        std::vector<std::wstring> guids;
        wchar_t name[128];
        for (DWORD i = 0;; ++i) {
            DWORD len = _countof(name);
            if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            guids.emplace_back(name);
        }
        RegCloseKey(root);
        for (const auto& guid : guids) {
            const std::wstring backup = std::wstring(kBackupRoot) + L"\\" + guid;
            const std::wstring adapterKey = AdapterKeyFor(guid);
            HKEY bk = nullptr;
            if (!adapterKey.empty() && RegOpenKeyExW(HKEY_LOCAL_MACHINE, backup.c_str(), 0, KEY_READ, &bk) == ERROR_SUCCESS) {
                wchar_t value[256];
                for (DWORD i = 0;; ++i) {
                    DWORD len = _countof(value);
                    DWORD type = 0;
                    if (RegEnumValueW(bk, i, value, &len, nullptr, &type, nullptr, nullptr) != ERROR_SUCCESS) break;
                    std::wstring keyword = value;
                    const size_t bar = keyword.find(L"|absent");
                    if (bar != std::wstring::npos) {
                        keyword.resize(bar);
                        if (DeleteValue(HKEY_LOCAL_MACHINE, adapterKey.c_str(), keyword.c_str())) ++restored;
                        continue;
                    }
                    const Live saved = ReadLive(backup, value);
                    if (saved.exists && WriteLive(adapterKey, value, saved.type, saved.text)) ++restored;
                }
                RegCloseKey(bk);
            }
            RegDeleteTreeW(HKEY_LOCAL_MACHINE, backup.c_str());
        }
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, kBackupRoot);
    }
    if (ValueExists(HKEY_LOCAL_MACHINE, kAppKey, L"Net_WifiPower_saved")) {
        DWORD ac = 0, dc = 0;
        const bool hadAc = GetDword(HKEY_LOCAL_MACHINE, kAppKey, L"Net_WifiPower_ac", ac);
        const bool hadDc = GetDword(HKEY_LOCAL_MACHINE, kAppKey, L"Net_WifiPower_dc", dc);
        if ((hadAc || hadDc) && WriteWifiPower(ac, dc, hadAc, hadDc)) ++restored;
        DeleteValue(HKEY_LOCAL_MACHINE, kAppKey, L"Net_WifiPower_saved");
        DeleteValue(HKEY_LOCAL_MACHINE, kAppKey, L"Net_WifiPower_ac");
        DeleteValue(HKEY_LOCAL_MACHINE, kAppKey, L"Net_WifiPower_dc");
    }
    if (ValueExists(HKEY_LOCAL_MACHINE, kAppKey, L"Net_Delivery_saved")) {
        DWORD had = 0, prev = 0;
        GetDword(HKEY_LOCAL_MACHINE, kAppKey, L"Net_Delivery_had", had);
        if (had && GetDword(HKEY_LOCAL_MACHINE, kAppKey, L"Net_Delivery_prev", prev)) {
            if (SetDword(HKEY_LOCAL_MACHINE, kDeliveryKey, kDeliveryName, prev)) ++restored;
        } else if (DeleteValue(HKEY_LOCAL_MACHINE, kDeliveryKey, kDeliveryName)) {
            ++restored;
        }
        DeleteValue(HKEY_LOCAL_MACHINE, kAppKey, L"Net_Delivery_saved");
        DeleteValue(HKEY_LOCAL_MACHINE, kAppKey, L"Net_Delivery_had");
        DeleteValue(HKEY_LOCAL_MACHINE, kAppKey, L"Net_Delivery_prev");
    }
    DeleteValue(HKEY_LOCAL_MACHINE, kAppKey, L"NetworkApplied");
    if (restored) say(Fmt(L"Restored %d network setting(s). Adapter settings return at the next restart.", restored));
    return true;
}

void AppendNetworkChecks(std::vector<HealthCheck>& out, const std::atomic<bool>* cancelled) {
    auto stopped = [&] { return cancelled && cancelled->load(); };
    const NetOptions options = LoadNetOptions();
    NetOptions all;
    const NetPlan plan = PlanNetwork(all);
    for (size_t ai = 0; ai < plan.adapters.size(); ++ai) {
        if (stopped()) return;
        const NetAdapter& a = plan.adapters[ai];
        if (a.changes.empty()) continue;
        const std::wstring kind = KindName(plan.adapters, ai);
        for (int group = 0; group < 2; ++group) {
            int total = 0, match = 0;
            std::wstring off;
            for (const auto& c : a.changes) {
                if (c.group != group) continue;
                ++total;
                if (c.already) ++match;
                else off += (off.empty() ? L"" : L", ") + c.title;
            }
            if (!total) continue;
            HealthCheck c = MakeCheck(L"network." + a.guid + (group == 0 ? L".power" : L".queue"), kCategory,
                                      kind + (group == 0 ? L" power saving off" : L" packet delivery"));
            Finish(c, true, match == total, group == 0 ? options.power : options.moderation,
                   group == 0 ? L"every power-saving setting off" : L"interrupt moderation off, receive side scaling on",
                   match == total ? Fmt(L"%d of %d set", match, total) : Fmt(L"%d of %d set; not set: %s", match, total, off.c_str()));
            c.explanation += L" Adapter: " + a.name + L".";
            out.push_back(std::move(c));
        }
    }
    if (stopped()) return;
    if (plan.hasWifi) {
        HealthCheck c = MakeCheck(L"network.wifipower", kCategory, L"Wi-Fi power plan setting");
        Finish(c, plan.wifiPowerReadable, plan.wifiPowerReadable && plan.wifiPowerAc == 0, options.power,
               L"0 (Maximum Performance)", plan.wifiPowerReadable ? Fmt(L"%u", plan.wifiPowerAc) : L"unreadable");
        out.push_back(std::move(c));
    }
    HealthCheck d = MakeCheck(L"network.delivery", kCategory, L"Update sharing with other PCs");
    Finish(d, true, plan.deliveryPolicySet && plan.deliveryMode == 0, options.delivery, L"off (Microsoft only)",
           plan.deliveryPolicySet ? Fmt(L"download mode %u", plan.deliveryMode) : L"not set");
    out.push_back(std::move(d));
}

}  // namespace gui
