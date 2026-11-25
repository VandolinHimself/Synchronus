// Profile.cpp - the profile choice, settings export and import, share codes,
// and custom scripts.
#include "Util.h"

#include <bcrypt.h>
#include <cmath>

#pragma comment(lib, "bcrypt.lib")

namespace gui {
namespace {

using namespace util;

constexpr wchar_t kScriptApprovals[] = L"SOFTWARE\\Synchronus\\Scripts";
constexpr char kCodePrefix[] = "SYN1.";
constexpr wchar_t kShareBase[] = L"https://luciuswayne.com/synchronus/profile#";
const wchar_t* const kHooks[] = {L"after-tune", L"performance", L"balanced", L"game-start", L"game-stop"};

// ---- a small JSON reader, enough for the settings file ----
struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    bool flag = false;
    double number = 0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    const Json* Get(const char* name) const {
        for (const auto& f : fields)
            if (f.first == name) return &f.second;
        return nullptr;
    }
    bool Bool(const char* name, bool fallback) const {
        const Json* v = Get(name);
        return v && v->kind == Kind::Bool ? v->flag : fallback;
    }
    int Int(const char* name, int fallback, int lo, int hi) const {
        const Json* v = Get(name);
        if (!v || v->kind != Kind::Number || !std::isfinite(v->number)) return fallback;
        return static_cast<int>(std::clamp(v->number, static_cast<double>(lo), static_cast<double>(hi)));
    }
    std::string Str(const char* name) const {
        const Json* v = Get(name);
        return v && v->kind == Kind::String ? v->text : std::string();
    }
};

struct Reader {
    const std::string& s;
    size_t i = 0;
    int depth = 0;

    void Ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    }
    bool String(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i >= s.size()) return false;
            const char e = s[i++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    if (i + 4 > s.size()) return false;
                    unsigned code = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = s[i++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                        else return false;
                    }
                    const wchar_t w[2] = {static_cast<wchar_t>(code), 0};
                    out += ToUtf8(w);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool Value(Json& out) {
        if (++depth > 12) return false;
        Ws();
        if (i >= s.size()) return false;
        bool ok = false;
        const char c = s[i];
        if (c == '{') {
            ++i;
            out.kind = Json::Kind::Object;
            Ws();
            if (i < s.size() && s[i] == '}') {
                ++i;
                ok = true;
            } else {
                for (;;) {
                    Ws();
                    std::string name;
                    if (!String(name)) break;
                    Ws();
                    if (i >= s.size() || s[i] != ':') break;
                    ++i;
                    Json child;
                    if (!Value(child)) break;
                    out.fields.emplace_back(std::move(name), std::move(child));
                    if (out.fields.size() > 200) break;
                    Ws();
                    if (i < s.size() && s[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < s.size() && s[i] == '}') {
                        ++i;
                        ok = true;
                    }
                    break;
                }
            }
        } else if (c == '[') {
            ++i;
            out.kind = Json::Kind::Array;
            Ws();
            if (i < s.size() && s[i] == ']') {
                ++i;
                ok = true;
            } else {
                for (;;) {
                    Json child;
                    if (!Value(child)) break;
                    out.items.push_back(std::move(child));
                    if (out.items.size() > 500) break;
                    Ws();
                    if (i < s.size() && s[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < s.size() && s[i] == ']') {
                        ++i;
                        ok = true;
                    }
                    break;
                }
            }
        } else if (c == '"') {
            out.kind = Json::Kind::String;
            ok = String(out.text);
        } else if (s.compare(i, 4, "true") == 0) {
            out.kind = Json::Kind::Bool;
            out.flag = true;
            i += 4;
            ok = true;
        } else if (s.compare(i, 5, "false") == 0) {
            out.kind = Json::Kind::Bool;
            i += 5;
            ok = true;
        } else if (s.compare(i, 4, "null") == 0) {
            i += 4;
            ok = true;
        } else {
            const size_t start = i;
            while (i < s.size() && (isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '+' || s[i] == '.' ||
                                    s[i] == 'e' || s[i] == 'E'))
                ++i;
            if (i > start && i - start < 40) {
                out.kind = Json::Kind::Number;
                out.number = atof(s.substr(start, i - start).c_str());
                ok = true;
            }
        }
        --depth;
        return ok;
    }
};

bool ParseJson(const std::string& text, Json& out) {
    if (text.size() > 64 * 1024) return false;
    Reader r{text};
    if (!r.Value(out)) return false;
    r.Ws();
    return r.i == text.size();
}

std::string Quote(const std::wstring& value) {
    std::string out = "\"";
    for (const unsigned char c : ToUtf8(value)) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) out += ' ';
        else out.push_back(static_cast<char>(c));
    }
    out += "\"";
    return out;
}

const char* Flag(bool on) { return on ? "true" : "false"; }

std::string List(const std::vector<std::wstring>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += ",";
        out += Quote(items[i]);
    }
    return out + "]";
}

// An exe name as the game lists store it: a bare file name, nothing that reads as a path.
bool PlainExeName(const std::wstring& name) {
    if (name.size() < 5 || name.size() > 80) return false;
    if (name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return false;
    return Lower(name).compare(name.size() - 4, 4, L".exe") == 0;
}

std::vector<std::wstring> NamesFrom(const Json* array) {
    std::vector<std::wstring> out;
    if (!array || array->kind != Json::Kind::Array) return out;
    for (const auto& item : array->items) {
        if (item.kind != Json::Kind::String) continue;
        const std::wstring name = FromUtf8(item.text);
        if (PlainExeName(name) && out.size() < 64) out.push_back(name);
    }
    return out;
}

std::string Base64Url(const std::string& bytes) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= bytes.size()) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                           static_cast<unsigned char>(bytes[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(kAlphabet[(v >> 6) & 63]);
        out.push_back(kAlphabet[v & 63]);
        i += 3;
    }
    if (i + 1 == bytes.size()) {
        const unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
    } else if (i + 2 == bytes.size()) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8);
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(kAlphabet[(v >> 6) & 63]);
    }
    return out;
}

bool Base64UrlDecode(const std::string& text, std::string& out) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-' || c == '+') return 62;
        if (c == '_' || c == '/') return 63;
        return -1;
    };
    out.clear();
    unsigned acc = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=') break;
        const int v = value(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

bool Sha256Hex(const std::wstring& path, std::wstring& hex) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    BYTE buf[16384];
    DWORD got = 0;
    unsigned long long total = 0;
    while (ok && ReadFile(file, buf, sizeof(buf), &got, nullptr) && got) {
        ok = BCryptHashData(hash, buf, got, 0) == 0;
        total += got;
        if (total > (8ull << 20)) ok = false;  // a script is not eight megabytes
    }
    BYTE digest[32]{};
    ok = ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    if (!ok) return false;
    hex.clear();
    for (const BYTE b : digest) hex += Fmt(L"%02x", b);
    return true;
}

std::wstring ApprovalName(const ScriptEntry& script) { return script.hook + L"\\" + script.name; }

bool ScriptFile(const std::wstring& lowerName) {
    for (const wchar_t* ext : {L".ps1", L".cmd", L".bat"}) {
        const size_t n = wcslen(ext);
        if (lowerName.size() > n && lowerName.compare(lowerName.size() - n, n, ext) == 0) return true;
    }
    return false;
}

}  // namespace

// --------------------------------------------------------------- profiles
const wchar_t* ProfileName(Profile p) { return p == Profile::Balanced ? L"Balanced" : L"Performance"; }

Profile ChosenProfile() {
    DWORD v = 0;
    return GetDword(HKEY_LOCAL_MACHINE, kAppKey, L"ProfileChoice", v) && v == 1 ? Profile::Balanced : Profile::Performance;
}

Profile ActiveProfile() {
    DWORD v = 0;
    return GetDword(HKEY_LOCAL_MACHINE, kAppKey, L"Profile", v) && v == 1 ? Profile::Balanced : Profile::Performance;
}

void SetChosenProfile(Profile p) { SetDword(HKEY_LOCAL_MACHINE, kAppKey, L"ProfileChoice", p == Profile::Balanced ? 1 : 0); }

bool ProfileRelaxes(const std::wstring& id) {
    if (id.compare(0, 6, L"power.") == 0) return true;
    return id == L"storage.ASPM" || id == L"storage.DISKIDLE" || id == L"storage.USBSUSPEND" || id == L"timer.effective" ||
           id == L"network.wifipower";
}

void RelaxForProfile(std::vector<HealthCheck>& checks) {
    if (ActiveProfile() != Profile::Balanced) return;
    for (auto& c : checks) {
        if (!ProfileRelaxes(c.id) || c.status == HealthStatus::Pass || c.status == HealthStatus::Disabled ||
            c.status == HealthStatus::Unsupported)
            continue;
        c.status = HealthStatus::Info;
        c.severity = 0;
        c.relaxed = true;
        c.repair.clear();
        c.explanation = L"The Balanced profile hands this back to Windows. Performance applies it again.";
    }
}

// ------------------------------------------------- export, import, sharing
std::wstring ExportSettings(const std::wstring& version) {
    const StepChoices steps = LoadStepChoices();
    const MonitorPrefs prefs = LoadMonitorPrefs();
    const NetOptions net = LoadNetOptions();
    const GameRules games = LoadGameRules();
    DWORD resolution = 0, pinned = 0;
    GetDword(HKEY_LOCAL_MACHINE, kAppKey, L"DesiredResolution", resolution);
    GetDword(HKEY_LOCAL_MACHINE, kAppKey, L"ResolutionPinned", pinned);
    std::string j = "{\n";
    j += "  \"app\": \"Synchronus\",\n";
    j += "  \"version\": " + Quote(version) + ",\n";
    j += std::string("  \"steps\": {\"prepare\": ") + Flag(steps.prepare) + ", \"measure\": " + Flag(steps.measure) +
         ", \"hold\": " + Flag(steps.hold) + ", \"cpu\": " + Flag(steps.cpu) + ", \"gpuClean\": " + Flag(steps.gpuClean) +
         ", \"gpu\": " + Flag(steps.gpu) + ", \"afterburner\": " + Flag(steps.afterburner) + ", \"memory\": " +
         Flag(steps.memory) + ", \"storage\": " + Flag(steps.storage) + ", \"network\": " + Flag(steps.network) +
         ", \"audio\": " + Flag(steps.audio) + ", \"profile\": " + Flag(steps.profile) + ", \"games\": " + Flag(steps.games) +
         ", \"hardware\": " + Flag(steps.hardware) + ", \"bench\": " + Flag(steps.bench) + ", \"share\": " + Flag(steps.share) +
         ", \"scripts\": " + Flag(steps.scripts) + ", \"metrics\": " + Flag(steps.metrics) + ", \"focus\": " +
         Flag(steps.focus) + "},\n";
    j += std::string("  \"prefs\": {\"closeToTray\": ") + Flag(prefs.closeToTray) + ", \"launchAtLogin\": " +
         Flag(prefs.launchAtLogin) + ", \"notify\": " + Flag(prefs.notify) + ", \"maintain\": " + Flag(prefs.maintainTiming) +
         ", \"scanHours\": " + std::to_string(prefs.scanHours) + ", \"maintainMinutes\": " +
         std::to_string(prefs.maintainMinutes) + ", \"updateMinutes\": " + std::to_string(prefs.updateMinutes) +
         ", \"notifyMinutes\": " + std::to_string(prefs.notifyMinutes) + ", \"hdr\": " + Flag(prefs.hdr) +
         ", \"gameBoost\": " + Flag(prefs.gameBoost) + ", \"gameQuiet\": " + Flag(prefs.gameQuiet) + ", \"watchHardware\": " +
         Flag(prefs.watchHardware) + "},\n";
    j += std::string("  \"profile\": \"") + (ChosenProfile() == Profile::Balanced ? "balanced" : "performance") + "\",\n";
    j += std::string("  \"network\": {\"power\": ") + Flag(net.power) + ", \"moderation\": " + Flag(net.moderation) +
         ", \"delivery\": " + Flag(net.delivery) + "},\n";
    j += std::string("  \"games\": {\"libraries\": ") + Flag(games.libraries) + ", \"include\": " + List(games.include) +
         ", \"exclude\": " + List(games.exclude) + "},\n";
    j += "  \"timer\": {\"resolution\": " + std::to_string(resolution) + ", \"pinned\": " + Flag(pinned != 0) + "},\n";
    j += "  \"pc\": " + Quote(HardwareSummary()) + "\n";
    j += "}\n";
    return FromUtf8(j);
}

bool ImportSettings(const std::wstring& json, std::wstring& summary, std::wstring& err) {
    Json root;
    if (!ParseJson(ToUtf8(json), root) || root.kind != Json::Kind::Object) {
        err = L"That is not a Synchronus settings file.";
        return false;
    }
    if (root.Str("app") != "Synchronus") {
        err = L"That file was not written by Synchronus.";
        return false;
    }
    const Json* steps = root.Get("steps");
    const Json* prefs = root.Get("prefs");
    if (!steps || steps->kind != Json::Kind::Object || !prefs || prefs->kind != Json::Kind::Object) {
        err = L"The settings file is missing its step choices or preferences.";
        return false;
    }
    StepChoices s = LoadStepChoices();
    s.prepare = steps->Bool("prepare", s.prepare);
    s.measure = steps->Bool("measure", s.measure);
    s.hold = steps->Bool("hold", s.hold);
    s.cpu = steps->Bool("cpu", s.cpu);
    s.gpuClean = steps->Bool("gpuClean", s.gpuClean);
    s.gpu = steps->Bool("gpu", s.gpu);
    s.afterburner = steps->Bool("afterburner", s.afterburner);
    s.memory = steps->Bool("memory", s.memory);
    s.storage = steps->Bool("storage", s.storage);
    s.network = steps->Bool("network", s.network);
    s.audio = steps->Bool("audio", s.audio);
    s.profile = steps->Bool("profile", s.profile);
    s.games = steps->Bool("games", s.games);
    s.hardware = steps->Bool("hardware", s.hardware);
    s.bench = steps->Bool("bench", s.bench);
    s.share = steps->Bool("share", s.share);
    s.scripts = steps->Bool("scripts", s.scripts);
    s.metrics = steps->Bool("metrics", s.metrics);
    s.focus = steps->Bool("focus", s.focus);
    SaveStepChoices(s);

    MonitorPrefs p = LoadMonitorPrefs();
    p.closeToTray = prefs->Bool("closeToTray", p.closeToTray);
    p.launchAtLogin = prefs->Bool("launchAtLogin", p.launchAtLogin);
    p.startMinimized = p.launchAtLogin;
    p.notify = prefs->Bool("notify", p.notify);
    p.maintainTiming = prefs->Bool("maintain", p.maintainTiming);
    p.maintainAsked = true;
    p.scanHours = prefs->Int("scanHours", p.scanHours, 1, 168);
    p.maintainMinutes = prefs->Int("maintainMinutes", p.maintainMinutes, 1, 180);
    p.updateMinutes = prefs->Int("updateMinutes", p.updateMinutes, 1, 1440);
    p.notifyMinutes = prefs->Int("notifyMinutes", p.notifyMinutes, 1, 1440);
    p.hdr = prefs->Bool("hdr", p.hdr);
    p.gameBoost = prefs->Bool("gameBoost", p.gameBoost);
    p.gameQuiet = prefs->Bool("gameQuiet", p.gameQuiet);
    p.watchHardware = prefs->Bool("watchHardware", p.watchHardware);
    SaveMonitorPrefs(p);

    if (const Json* net = root.Get("network"); net && net->kind == Json::Kind::Object) {
        NetOptions o = LoadNetOptions();
        o.power = net->Bool("power", o.power);
        o.moderation = net->Bool("moderation", o.moderation);
        o.delivery = net->Bool("delivery", o.delivery);
        SaveNetOptions(o);
    }
    if (const Json* games = root.Get("games"); games && games->kind == Json::Kind::Object) {
        GameRules r = LoadGameRules();
        r.libraries = games->Bool("libraries", r.libraries);
        r.include = NamesFrom(games->Get("include"));
        r.exclude = NamesFrom(games->Get("exclude"));
        SaveGameRules(r);
    }
    const std::string profile = root.Str("profile");
    if (profile == "balanced") SetChosenProfile(Profile::Balanced);
    else if (profile == "performance") SetChosenProfile(Profile::Performance);

    summary = L"Step choices and preferences are in place.";
    const std::wstring from = FromUtf8(root.Str("version"));
    if (!from.empty() && from.size() < 24) summary += L" They came from Synchronus " + from + L".";
    const Json* timer = root.Get("timer");
    const int resolution = timer && timer->kind == Json::Kind::Object ? timer->Int("resolution", 0, 0, 200000) : 0;
    std::wstring pc = FromUtf8(root.Str("pc"));
    if (pc.size() > 160) pc.resize(160);
    if (resolution > 0) {
        summary += Fmt(L" That PC held %.4f ms", resolution / 10000.0);
        summary += pc.empty() ? L"." : L" on " + pc + L".";
        summary += L" The timer is measured again on this PC, so that value is not copied.";
    }
    summary += L" Run Full tune to apply the steps here.";
    return true;
}

std::wstring MakeShareCode(const std::wstring& json) {
    // The code carries the same settings without the layout whitespace.
    std::string compact;
    bool quoted = false;
    const std::string utf8 = ToUtf8(json);
    for (size_t i = 0; i < utf8.size(); ++i) {
        const char c = utf8[i];
        if (c == '"' && (i == 0 || utf8[i - 1] != '\\')) quoted = !quoted;
        if (!quoted && (c == ' ' || c == '\n' || c == '\r' || c == '\t')) continue;
        compact.push_back(c);
    }
    return FromUtf8(std::string(kCodePrefix) + Base64Url(compact));
}

bool ReadShareCode(const std::wstring& text, std::wstring& json) {
    std::string s = ToUtf8(text);
    const size_t hash = s.rfind('#');
    if (hash != std::string::npos) s = s.substr(hash + 1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n' || s.back() == '\t')) s.pop_back();
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\r' || s[start] == '\n' || s[start] == '\t')) ++start;
    s = s.substr(start);
    const size_t n = sizeof(kCodePrefix) - 1;
    if (s.size() <= n || s.size() > 12000 || s.compare(0, n, kCodePrefix) != 0) return false;
    std::string decoded;
    if (!Base64UrlDecode(s.substr(n), decoded) || decoded.empty()) return false;
    Json probe;
    if (!ParseJson(decoded, probe) || probe.kind != Json::Kind::Object) return false;
    json = FromUtf8(decoded);
    return true;
}

std::wstring ShareLink(const std::wstring& code) { return std::wstring(kShareBase) + code; }

bool WriteTextFile(const std::wstring& path, const std::wstring& text) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const std::string bytes = ToUtf8(text);
    DWORD wrote = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr) && wrote == bytes.size();
    CloseHandle(file);
    return ok;
}

bool ReadTextFile(const std::wstring& path, std::wstring& text) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > (4ll << 20)) {
        CloseHandle(file);
        return false;
    }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    const bool ok = bytes.empty() || ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr);
    CloseHandle(file);
    if (!ok) return false;
    bytes.resize(got);
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
        bytes.erase(0, 3);
    text = FromUtf8(bytes);
    return true;
}

// ---------------------------------------------------------------- scripts
std::wstring ScriptsDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"ProgramFiles", buf, _countof(buf));
    return (n > 0 && n < _countof(buf) ? std::wstring(buf) : L"C:\\Program Files") + L"\\Synchronus\\Scripts";
}

void EnsureScriptFolders() {
    const std::wstring dir = ScriptsDir();
    const size_t slash = dir.find_last_of(L'\\');
    CreateDirectoryW(dir.substr(0, slash).c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    for (const wchar_t* hook : kHooks) CreateDirectoryW((dir + L"\\" + hook).c_str(), nullptr);
}

std::vector<ScriptEntry> ListScripts() {
    std::vector<ScriptEntry> out;
    const std::wstring dir = ScriptsDir();
    for (const wchar_t* hook : kHooks) {
        WIN32_FIND_DATAW fd{};
        const HANDLE find = FindFirstFileW((dir + L"\\" + hook + L"\\*").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) continue;
        std::vector<std::wstring> names;
        do {
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            if (ScriptFile(Lower(fd.cFileName))) names.emplace_back(fd.cFileName);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
        std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return Lower(a) < Lower(b); });
        for (const auto& name : names) {
            ScriptEntry s;
            s.hook = hook;
            s.name = name;
            s.path = dir + L"\\" + hook + L"\\" + name;
            std::wstring saved, now;
            s.approved = GetSz(HKEY_LOCAL_MACHINE, kScriptApprovals, ApprovalName(s).c_str(), saved) && Sha256Hex(s.path, now) &&
                         saved == now;
            out.push_back(std::move(s));
            if (out.size() >= 40) return out;
        }
    }
    return out;
}

bool ApproveScript(const ScriptEntry& script, bool on) {
    if (!on) return DeleteValue(HKEY_LOCAL_MACHINE, kScriptApprovals, ApprovalName(script).c_str());
    std::wstring hash;
    if (!Sha256Hex(script.path, hash)) return false;
    return SetSz(HKEY_LOCAL_MACHINE, kScriptApprovals, ApprovalName(script).c_str(), hash);
}

int RunHook(const wchar_t* hook, const std::wstring& game, const std::function<void(const std::wstring&)>& log) {
    int ran = 0;
    for (const ScriptEntry& script : ListScripts()) {
        if (script.hook != hook || !script.approved) continue;
        const bool ps = Lower(script.name).rfind(L".ps1") == script.name.size() - 4;
        wchar_t sys[MAX_PATH]{};
        GetSystemDirectoryW(sys, _countof(sys));
        const std::wstring exe = ps ? std::wstring(sys) + L"\\WindowsPowerShell\\v1.0\\powershell.exe" : std::wstring(sys) + L"\\cmd.exe";
        std::wstring cmd = ps ? L"\"" + exe + L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + script.path + L"\""
                              : L"\"" + exe + L"\" /d /c \"\"" + script.path + L"\"\"";
        // The script sees what happened through three variables added to a copy of the environment.
        std::vector<wchar_t> env;
        if (wchar_t* block = GetEnvironmentStringsW()) {
            const wchar_t* p = block;
            while (*p) {
                const size_t n = wcslen(p) + 1;
                env.insert(env.end(), p, p + n);
                p += n;
            }
            FreeEnvironmentStringsW(block);
        }
        auto addVar = [&](const std::wstring& pair) {
            env.insert(env.end(), pair.begin(), pair.end());
            env.push_back(L'\0');
        };
        addVar(std::wstring(L"SYNCHRONUS_HOOK=") + hook);
        addVar(std::wstring(L"SYNCHRONUS_PROFILE=") + ProfileName(ActiveProfile()));
        addVar(L"SYNCHRONUS_GAME=" + game);
        env.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        const size_t slash = script.path.find_last_of(L'\\');
        const std::wstring folder = script.path.substr(0, slash);
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                            env.data(), folder.c_str(), &si, &pi)) {
            if (log) log(Fmt(L"Script %s\\%s did not start (error %lu).", script.hook.c_str(), script.name.c_str(), GetLastError()));
            continue;
        }
        DWORD code = 0;
        if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 1);
            if (log) log(Fmt(L"Script %s\\%s was stopped after two minutes.", script.hook.c_str(), script.name.c_str()));
        } else {
            GetExitCodeProcess(pi.hProcess, &code);
            if (log) log(Fmt(L"Script %s\\%s finished with exit code %lu.", script.hook.c_str(), script.name.c_str(), code));
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ++ran;
    }
    return ran;
}

// ---------------------------------------------------------------- self-test
int SelfTestExtras() {
    int failed = 0;
    auto expect = [&](bool cond) {
        if (!cond) ++failed;
    };
    GameRules rules;
    const std::vector<std::wstring> known = {L"d:\\games\\odd place\\thing.exe"};
    expect(LooksLikeGame(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Portal 2\\portal2.exe", rules, known));
    expect(LooksLikeGame(L"D:\\Games\\Odd Place\\Thing.exe", rules, known));
    expect(!LooksLikeGame(L"C:\\Windows\\System32\\notepad.exe", rules, known));
    expect(!LooksLikeGame(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\wallpaper_engine\\wallpaper32.exe", rules, known));
    expect(!LooksLikeGame(L"C:\\Program Files\\Epic Games\\Launcher\\Portal\\Binaries\\Win64\\EpicGamesLauncher.exe", rules, known));
    expect(!LooksLikeGame(L"C:\\Riot Games\\Riot Client\\RiotClientServices.exe", rules, known));
    expect(!LooksLikeGame(L"E:\\SteamLibrary\\steamapps\\common\\Game\\UnityCrashHandler64.exe", rules, known));
    expect(LooksLikeGame(L"C:\\Riot Games\\VALORANT\\live\\ShooterGame\\Binaries\\Win64\\VALORANT-Win64-Shipping.exe", rules, known));
    rules.exclude = {L"portal2.exe"};
    expect(!LooksLikeGame(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Portal 2\\Portal2.exe", rules, known));
    rules.include = {L"notepad.exe"};
    expect(LooksLikeGame(L"C:\\Windows\\System32\\notepad.exe", rules, known));
    rules.libraries = false;
    expect(!LooksLikeGame(L"D:\\Games\\Odd Place\\Thing.exe", rules, known));
    expect(LooksLikeGame(L"C:\\Windows\\System32\\Notepad.exe", rules, known));

    const std::wstring sample = L"{\n  \"app\": \"Synchronus\",\n  \"note\": \"a \\\"quoted\\\" word\",\n  \"n\": 12\n}\n";
    const std::wstring code = MakeShareCode(sample);
    std::wstring back;
    expect(code.compare(0, 5, L"SYN1.") == 0);
    expect(ReadShareCode(code, back));
    expect(ReadShareCode(L"  " + ShareLink(code) + L"\r\n", back));
    Json parsed;
    expect(ParseJson(ToUtf8(back), parsed) && parsed.Str("app") == "Synchronus" && parsed.Int("n", 0, 0, 100) == 12 &&
           parsed.Str("note") == "a \"quoted\" word");
    expect(!ReadShareCode(L"SYN1.!!!!", back));
    expect(!ReadShareCode(L"hello", back));
    Json bad;
    expect(!ParseJson("{\"a\": [1, 2,", bad));
    expect(!ParseJson("{\"a\": 1} trailing", bad));

    expect(ProfileRelaxes(L"power.PERFCHECK"));
    expect(ProfileRelaxes(L"storage.ASPM"));
    expect(ProfileRelaxes(L"timer.effective"));
    expect(!ProfileRelaxes(L"storage.trim"));
    expect(!ProfileRelaxes(L"gpu.HAGS"));
    expect(!ProfileRelaxes(L"timer.service"));
    expect(PlainExeName(L"game.exe") && !PlainExeName(L"..\\game.exe") && !PlainExeName(L"game.txt"));
    return failed == 0 ? 0 : 1;
}

}  // namespace gui
