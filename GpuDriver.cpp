// GpuDriver.cpp - current NVIDIA or AMD display driver, without the extras.
//
// NVIDIA: the same device-id lookup GeForce Experience uses. The package is
// unpacked and setup.exe is run as "-s -n Display.Driver Display.PhysX".
// -n is IgnoreReboot in setup.cfg. Naming a package that is not installed
// (MSVCRuntime2017) exits 0xE0E00019. Naming MSVCRuntime2019 when a newer
// Visual C++ runtime is already present exits 0xE6000000, so that package
// is left for setup to include only when it is actually needed.
// AMD: the current Adrenalin package from drivers.amd.com (the version index
// is the small public Chocolatey package; the bytes and the hash come from
// that index, and the file itself is only accepted from drivers.amd.com).
// 7-Zip unpacks it. If 7-Zip is not installed, the official 7-Zip installer
// is downloaded and installed first. pnputil installs the display INF only. The Adrenalin
// installer is never launched, because its silent switch installs the suite.
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include "Gui.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cwctype>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace gui {
namespace {

constexpr uint64_t kMinPackage = 32ull << 20;
constexpr uint64_t kMaxPackage = (4ull << 30) - (64ull << 20);
constexpr wchar_t kDisplayClass[] =
    L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";

enum class Allow { NvidiaApi, NvidiaFile, AmdFile, Index, SevenZip, Afterburner };

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buffer[2048];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    return buffer;
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

bool ContainsI(const std::wstring& hay, const wchar_t* needle) {
    return Lower(hay).find(Lower(needle)) != std::wstring::npos;
}

bool EndsWithI(const std::wstring& s, const wchar_t* suf) {
    const size_t n = wcslen(suf);
    if (s.size() < n) return false;
    return _wcsnicmp(s.c_str() + (s.size() - n), suf, n) == 0;
}

std::wstring SizeText(uint64_t n) {
    wchar_t b[32];
    if (n >= (1ull << 30)) swprintf_s(b, L"%.1f GB", static_cast<double>(n) / 1073741824.0);
    else swprintf_s(b, L"%.0f MB", static_cast<double>(n) / 1048576.0);
    return b;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) {
        if (c > 0 && c < 128) s.push_back(static_cast<char>(c));
    }
    return s;
}

std::wstring Widen(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (unsigned char c : s) w.push_back(static_cast<wchar_t>(c));
    return w;
}

std::string PercentEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    o.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            o.push_back(static_cast<char>(c));
        } else {
            o.push_back('%');
            o.push_back(hex[c >> 4]);
            o.push_back(hex[c & 15]);
        }
    }
    return o;
}

bool HexEq(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size() || a.empty()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (towlower(a[i]) != towlower(b[i])) return false;
    }
    return true;
}

std::wstring FullPath(const std::wstring& p) {
    wchar_t buf[1024];
    const DWORD n = GetFullPathNameW(p.c_str(), _countof(buf), buf, nullptr);
    if (!n || n >= _countof(buf)) return p;
    return buf;
}

bool IsUnder(const std::wstring& path, const std::wstring& root) {
    std::wstring p = Lower(FullPath(path));
    std::wstring r = Lower(FullPath(root));
    if (r.empty()) return false;
    if (r.back() != L'\\') r.push_back(L'\\');
    return p.size() > r.size() && p.compare(0, r.size(), r) == 0;
}

std::wstring DriverDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, _countof(buf));
    std::wstring root = (n > 0 && n < _countof(buf)) ? buf : L"C:\\ProgramData";
    return root + L"\\Synchronus\\drivers";
}

void EnsureDir(const std::wstring& path) {
    size_t i = (path.size() >= 2 && path[1] == L':') ? 2 : 0;
    for (; i < path.size(); ++i) {
        if ((path[i] == L'\\' || path[i] == L'/') && i > 2) CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    }
    CreateDirectoryW(path.c_str(), nullptr);
}

void RemoveTree(const std::wstring& dir) {
    if (!IsUnder(dir, DriverDir())) return;
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            const std::wstring full = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                RemoveTree(full);
            } else {
                SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

bool HostAllowed(Allow allow, const std::wstring& host) {
    const std::wstring h = Lower(host);
    switch (allow) {
        case Allow::NvidiaApi:
            return h == L"gfwsl.geforce.com";
        case Allow::NvidiaFile:
            return h == L"download.nvidia.com" || EndsWithI(h, L".download.nvidia.com");
        case Allow::AmdFile:
            return h == L"drivers.amd.com";
        case Allow::Index:
            return h == L"community.chocolatey.org" || h == L"packages.chocolatey.org";
        case Allow::SevenZip:
            return h == L"www.7-zip.org" || h == L"7-zip.org" || h == L"github.com" ||
                   h == L"release-assets.githubusercontent.com";
        case Allow::Afterburner:
            return h == L"ftp.nluug.nl";
    }
    return false;
}

struct HttpOut {
    DWORD status = 0;
    std::wstring finalUrl;
    uint64_t length = 0;
    std::string body;
    bool stopped = false;
};

// sink is called with each chunk. Return false to stop. When sink is empty the
// body is kept, up to maxBytes.
bool Http(Allow allow, const wchar_t* method, const std::wstring& url, bool browser, uint64_t maxBytes,
          const std::function<bool(const BYTE*, DWORD, uint64_t, uint64_t)>& sink, HttpOut& out, std::wstring& err,
          const std::atomic<bool>& stop) {
    out = {};
    if (stop.load()) {
        out.stopped = true;
        return false;
    }
    wchar_t host[256] = {};
    wchar_t path[4096] = {};
    wchar_t extra[2048] = {};
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = _countof(path);
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = _countof(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        err = L"The address is not an https vendor URL.";
        return false;
    }
    if (!HostAllowed(allow, host)) {
        err = L"The address is not on the vendor site.";
        return false;
    }

    HINTERNET ses = WinHttpOpen(L"NvBackend/36.0.0.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        err = Format(L"Could not open the network (%lu).", GetLastError());
        return false;
    }
    WinHttpSetTimeouts(ses, 20000, 20000, 60000, 30000);
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort ? uc.nPort : INTERNET_DEFAULT_HTTPS_PORT, 0);
    std::wstring object = path;
    if (extra[0]) object += extra;
    HINTERNET req = con ? WinHttpOpenRequest(con, method, object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                        : nullptr;
    auto close = [&] {
        if (req) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
    };
    if (!req) {
        err = Format(L"Could not start the request (%lu).", GetLastError());
        close();
        return false;
    }
    DWORD dec = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(req, WINHTTP_OPTION_DECOMPRESSION, &dec, sizeof(dec));
    if (browser) {
        WinHttpAddRequestHeaders(req, L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)\r\n", static_cast<DWORD>(-1),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        if (allow == Allow::AmdFile) {
            WinHttpAddRequestHeaders(req, L"Referer: https://www.amd.com/en/support/download/drivers.html\r\n",
                                     static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }
    }
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        err = Format(L"The vendor site did not answer (%lu).", GetLastError());
        close();
        return false;
    }

    DWORD sz = sizeof(out.status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &out.status, &sz, WINHTTP_NO_HEADER_INDEX);
    wchar_t lenBuf[64] = {};
    DWORD lenSz = sizeof(lenBuf);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenBuf, &lenSz,
                            WINHTTP_NO_HEADER_INDEX)) {
        out.length = _wcstoui64(lenBuf, nullptr, 10);
    }
    DWORD urlSz = 0;
    WinHttpQueryOption(req, WINHTTP_OPTION_URL, nullptr, &urlSz);
    if (urlSz > sizeof(wchar_t) && urlSz < 4096 * sizeof(wchar_t)) {
        std::wstring final(urlSz / sizeof(wchar_t), L'\0');
        if (WinHttpQueryOption(req, WINHTTP_OPTION_URL, final.data(), &urlSz)) {
            while (!final.empty() && final.back() == L'\0') final.pop_back();
            out.finalUrl = final;
            wchar_t fhost[256] = {};
            URL_COMPONENTS fc{};
            fc.dwStructSize = sizeof(fc);
            fc.lpszHostName = fhost;
            fc.dwHostNameLength = _countof(fhost);
            if (!WinHttpCrackUrl(final.c_str(), 0, 0, &fc) || !HostAllowed(allow, fhost)) {
                err = L"The download was redirected away from the vendor site.";
                close();
                return false;
            }
        }
    }
    if (out.status != 200) {
        err = Format(L"The vendor site returned status %lu.", out.status);
        close();
        return false;
    }
    if (_wcsicmp(method, L"HEAD") == 0) {
        close();
        return true;
    }

    uint64_t got = 0;
    const uint64_t total = out.length;
    for (;;) {
        if (stop.load()) {
            out.stopped = true;
            err = L"Stopped.";
            close();
            return false;
        }
        BYTE buf[64 * 1024];
        DWORD read = 0;
        if (!WinHttpReadData(req, buf, sizeof(buf), &read)) {
            err = Format(L"The download stopped early (%lu).", GetLastError());
            close();
            return false;
        }
        if (read == 0) break;
        if (got + read > maxBytes) {
            err = L"The download is larger than a display-driver package should be.";
            close();
            return false;
        }
        got += read;
        if (sink) {
            if (!sink(buf, read, got, total)) {
                if (stop.load()) {
                    out.stopped = true;
                    err = L"Stopped.";
                } else {
                    err = L"Could not save the download.";
                }
                close();
                return false;
            }
        } else {
            out.body.append(reinterpret_cast<const char*>(buf), read);
        }
    }
    if (total && got != total) {
        err = L"The download ended early.";
        close();
        return false;
    }
    out.length = got ? got : out.length;
    close();
    return true;
}

bool JsonString(const std::string& body, const char* key, std::string& value, size_t from) {
    const std::string pat = std::string("\"") + key + "\"";
    const size_t p = body.find(pat, from);
    if (p == std::string::npos) return false;
    const size_t colon = body.find(':', p + pat.size());
    if (colon == std::string::npos || colon > p + pat.size() + 8) return false;
    size_t i = colon + 1;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\n' || body[i] == '\r' || body[i] == '\t')) ++i;
    if (i + 4 <= body.size() && body.compare(i, 4, "null") == 0) return false;
    if (i >= body.size() || body[i] != '"') return false;
    ++i;
    std::string v;
    while (i < body.size() && body[i] != '"') {
        if (body[i] == '\\' && i + 1 < body.size()) {
            const char n = body[i + 1];
            if (n == '/' || n == '\\' || n == '"') {
                v.push_back(n);
                i += 2;
                continue;
            }
            if (n == 'u' && i + 5 < body.size()) {
                i += 6;
                continue;
            }
        }
        v.push_back(body[i++]);
    }
    if (v.empty()) return false;
    value = std::move(v);
    return true;
}

bool QuotedAfter(const std::string& text, const char* key, std::string& out) {
    const size_t p = text.find(key);
    if (p == std::string::npos) return false;
    const size_t q1 = text.find('\'', p + strlen(key));
    const size_t q2 = text.find('"', p + strlen(key));
    size_t a = std::string::npos;
    char q = 0;
    if (q1 != std::string::npos && (q2 == std::string::npos || q1 < q2)) {
        a = q1;
        q = '\'';
    } else if (q2 != std::string::npos) {
        a = q2;
        q = '"';
    }
    if (a == std::string::npos) return false;
    const size_t b = text.find(q, a + 1);
    if (b == std::string::npos || b <= a + 1) return false;
    out = text.substr(a + 1, b - a - 1);
    return !out.empty();
}

struct Sha256 {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<UCHAR> obj;
    bool ok = false;
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return;
        DWORD len = 0, cb = 0;
        if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&len), sizeof(len), &cb, 0) < 0 || !len)
            return;
        obj.resize(len);
        if (BCryptCreateHash(alg, &hash, obj.data(), len, nullptr, 0, 0) < 0) return;
        ok = true;
    }
    ~Sha256() {
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    void Add(const void* p, DWORD n) {
        if (ok && n) BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<void*>(p)), n, 0);
    }
    std::wstring Hex() {
        if (!ok) return {};
        UCHAR dig[32];
        if (BCryptFinishHash(hash, dig, sizeof(dig), 0) < 0) return {};
        ok = false;
        wchar_t out[65];
        for (int i = 0; i < 32; ++i) swprintf_s(out + static_cast<size_t>(i) * 2, 3, L"%02x", dig[i]);
        out[64] = 0;
        return out;
    }
};

bool GrabHex(const std::wstring& s, const wchar_t* key, size_t n, std::wstring& out) {
    const std::wstring h = Lower(s);
    const std::wstring k = Lower(key);
    const size_t p = h.find(k);
    if (p == std::wstring::npos || p + k.size() + n > h.size()) return false;
    for (size_t i = 0; i < n; ++i) {
        if (!iswxdigit(h[p + k.size() + i])) return false;
    }
    out = h.substr(p + k.size(), n);
    for (wchar_t& c : out) c = static_cast<wchar_t>(towupper(c));
    return true;
}

struct Pci {
    std::wstring dev, ven, ss, sv;
    bool valid() const { return dev.size() == 4 && ven.size() == 4; }
    bool full() const { return valid() && ss.size() == 4 && sv.size() == 4; }
    std::wstring two() const { return dev + L"_" + ven; }
    std::wstring four() const { return dev + L"_" + ven + L"_" + ss + L"_" + sv; }
};

Pci ParsePci(const std::wstring& id) {
    Pci p;
    GrabHex(id, L"VEN_", 4, p.ven);
    GrabHex(id, L"DEV_", 4, p.dev);
    std::wstring sub;
    if (GrabHex(id, L"SUBSYS_", 8, sub)) {
        p.ss = sub.substr(0, 4);
        p.sv = sub.substr(4, 4);
    }
    return p;
}

void EnrichSubsys(Pci& p, const wchar_t* service) {
    if (!p.valid() || p.full() || !service) return;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Enum\\PCI", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;
    const std::wstring needle = L"VEN_" + p.ven + L"&DEV_" + p.dev + L"&SUBSYS_";
    std::wstring fallbackSs, fallbackSv;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD n = _countof(name);
        if (RegEnumKeyExW(key, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring upper = name;
        for (wchar_t& c : upper) c = static_cast<wchar_t>(towupper(c));
        const size_t at = upper.find(needle);
        if (at == std::wstring::npos || at + needle.size() + 8 > upper.size()) continue;
        const std::wstring sub = upper.substr(at + needle.size(), 8);
        bool digits = true;
        for (wchar_t c : sub)
            if (!iswxdigit(c)) digits = false;
        if (!digits) continue;
        HKEY devKey = nullptr;
        if (RegOpenKeyExW(key, name, 0, KEY_READ, &devKey) != ERROR_SUCCESS) continue;
        wchar_t inst[256];
            bool matched = false;
            for (DWORD j = 0;; ++j) {
                DWORD in = _countof(inst);
                if (RegEnumKeyExW(devKey, j, inst, &in, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                wchar_t svc[64] = {};
                DWORD sz = sizeof(svc);
                if (RegGetValueW(devKey, inst, L"Service", RRF_RT_REG_SZ, nullptr, svc, &sz) == ERROR_SUCCESS &&
                    _wcsicmp(svc, service) == 0) {
                    matched = true;
                    break;
                }
            }
        RegCloseKey(devKey);
        if (matched) {
            p.ss = sub.substr(0, 4);
            p.sv = sub.substr(4, 4);
            RegCloseKey(key);
            return;
        }
        if (fallbackSs.empty()) {
            fallbackSs = sub.substr(0, 4);
            fallbackSv = sub.substr(4, 4);
        }
    }
    RegCloseKey(key);
    if (!fallbackSs.empty()) {
        p.ss = fallbackSs;
        p.sv = fallbackSv;
    }
}

struct Adapter {
    std::wstring name;
    std::wstring installed;
    Pci pci;
    int vendor = 0;  // 1 nv, 2 amd, 3 intel
    bool discrete = false;
};

bool NameIs(const std::wstring& name, std::initializer_list<const wchar_t*> keys) {
    for (const wchar_t* k : keys)
        if (ContainsI(name, k)) return true;
    return false;
}

std::vector<Adapter> ListAdapters() {
    std::vector<Adapter> out;
    for (int i = 0; i < 32; ++i) {
        wchar_t key[160];
        swprintf_s(key, L"%s\\%04d", kDisplayClass, i);
        wchar_t desc[256] = {};
        DWORD dsz = sizeof(desc);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"DriverDesc", RRF_RT_REG_SZ, nullptr, desc, &dsz) != ERROR_SUCCESS)
            continue;
        if (ContainsI(desc, L"Microsoft Basic")) continue;
        Adapter a;
        a.name = desc;
        wchar_t ver[64] = {};
        DWORD vsz = sizeof(ver);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"DriverVersion", RRF_RT_REG_SZ, nullptr, ver, &vsz) == ERROR_SUCCESS)
            a.installed = ver;
        wchar_t id[256] = {};
        DWORD isz = sizeof(id);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"MatchingDeviceId", RRF_RT_REG_SZ, nullptr, id, &isz) ==
            ERROR_SUCCESS) {
            a.pci = ParsePci(id);
        }
        wchar_t provider[128] = {};
        DWORD psz = sizeof(provider);
        RegGetValueW(HKEY_LOCAL_MACHINE, key, L"ProviderName", RRF_RT_REG_SZ, nullptr, provider, &psz);
        const std::wstring blob = std::wstring(desc) + L" " + provider;
        if (a.pci.ven == L"10DE" || NameIs(blob, {L"NVIDIA", L"GeForce", L"Quadro"})) a.vendor = 1;
        else if (a.pci.ven == L"1002" || NameIs(blob, {L"AMD", L"Radeon", L"ATI"})) a.vendor = 2;
        else if (a.pci.ven == L"8086" || NameIs(blob, {L"Intel", L"Arc"})) a.vendor = 3;
        else continue;
        if (a.vendor == 1) {
            a.discrete = NameIs(a.name, {L"GeForce", L"RTX", L"GTX", L"Quadro", L"Tesla"});
            EnrichSubsys(a.pci, L"nvlddmkm");
        } else if (a.vendor == 2) {
            const bool igpu = ContainsI(a.name, L"Ryzen") || (ContainsI(a.name, L"Graphics") && !ContainsI(a.name, L"RX"));
            a.discrete = !igpu && NameIs(a.name, {L"RX", L"Radeon Pro", L"VII", L"Instinct", L"FirePro"});
            EnrichSubsys(a.pci, L"amdkmdag");
        }
        out.push_back(std::move(a));
    }
    return out;
}

const Adapter* Pick(const std::vector<Adapter>& all, std::wstring& other) {
    const Adapter* nvD = nullptr;
    const Adapter* nv = nullptr;
    const Adapter* amdD = nullptr;
    const Adapter* amd = nullptr;
    for (const Adapter& a : all) {
        if (a.vendor == 1) {
            if (!nv) nv = &a;
            if (a.discrete && !nvD) nvD = &a;
        } else if (a.vendor == 2) {
            if (!amd) amd = &a;
            if (a.discrete && !amdD) amdD = &a;
        }
    }
    const Adapter* c = nvD ? nvD : nv ? nv : amdD ? amdD : amd;
    if (!c) return nullptr;
    const Adapter* rest = c->vendor == 1 ? (amdD ? amdD : amd) : (nvD ? nvD : nv);
    if (rest) other = rest->name;
    return c;
}

bool Laptop() {
    SYSTEM_POWER_STATUS ps{};
    if (!GetSystemPowerStatus(&ps)) return false;
    if (ps.BatteryFlag == 128 || ps.BatteryFlag == 255) return false;
    return (ps.BatteryFlag & (1 | 2 | 4 | 8)) != 0;
}

std::wstring OsBuild() {
    wchar_t b[32] = {};
    DWORD sz = sizeof(b);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber",
                     RRF_RT_REG_SZ, nullptr, b, &sz) != ERROR_SUCCESS) {
        return L"22621";
    }
    std::wstring d;
    for (wchar_t c : std::wstring(b))
        if (c >= L'0' && c <= L'9') d.push_back(c);
    return d.empty() ? L"22621" : d;
}

std::wstring FileNameOf(const std::wstring& url) {
    std::wstring path = url;
    const size_t q = path.find(L'?');
    if (q != std::wstring::npos) path.erase(q);
    const size_t s = path.find_last_of(L'/');
    std::wstring name = s == std::wstring::npos ? path : path.substr(s + 1);
    if (name.empty() || name.size() > 160 || name.find(L"..") != std::wstring::npos) name = L"driver.exe";
    for (wchar_t& c : name) {
        if (c == L'\\' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|')
            c = L'_';
    }
    return name;
}

void AddContext(GpuDriverOffer& o, bool laptop, const std::wstring& other) {
    o.laptop = laptop;
    o.otherGpu = other;
    o.note.clear();
    if (o.bytes) o.note = L"About " + SizeText(o.bytes) + L".";
    else if (o.ok) o.note = L"The vendor did not list the package size.";
    if (laptop) o.note += L" This PC reports a battery. Laptops sometimes need the OEM package instead.";
    if (!other.empty()) o.note += L" " + other + L" stays on its current driver.";
}

void CheckSpace(GpuDriverOffer& o) {
    EnsureDir(DriverDir());
    ULARGE_INTEGER avail{};
    if (!GetDiskFreeSpaceExW(DriverDir().c_str(), &avail, nullptr, nullptr)) {
        o.canDownload = false;
        o.error = L"Could not read free space on this drive.";
        return;
    }
    if (o.bytes && (o.bytes < kMinPackage || o.bytes > kMaxPackage)) {
        o.ok = false;
        o.canDownload = false;
        o.error = L"The published package is not a full display driver.";
        return;
    }
    const uint64_t need = o.bytes ? o.bytes * 2 + (1ull << 30) : (4ull << 30);
    if (avail.QuadPart < need) {
        o.canDownload = false;
        o.error = Format(L"Need about %s free. This drive has %s.", SizeText(need).c_str(), SizeText(avail.QuadPart).c_str());
        return;
    }
    o.canDownload = true;
}

void ProbeSize(GpuDriverOffer& o, Allow allow, bool browser, const std::atomic<bool>& stop) {
    HttpOut head;
    std::wstring err;
    if (Http(allow, L"HEAD", o.url, browser, 0, {}, head, err, stop) && head.length) o.bytes = head.length;
}

std::wstring SevenZip() {
    const wchar_t* roots[] = {L"ProgramW6432", L"ProgramFiles"};
    for (const wchar_t* env : roots) {
        wchar_t buf[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(env, buf, _countof(buf));
        if (!n || n >= _countof(buf)) continue;
        const std::wstring path = std::wstring(buf) + L"\\7-Zip\\7z.exe";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }
    const wchar_t* cands[] = {L"C:\\Program Files\\7-Zip\\7z.exe", L"C:\\Program Files (x86)\\7-Zip\\7z.exe"};
    for (const wchar_t* c : cands) {
        if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) return c;
    }
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"7z.exe", nullptr, _countof(found), found, nullptr)) return found;
    return {};
}

struct Proc {
    bool started = false;
    bool timedOut = false;
    DWORD code = 0xFFFFFFFF;
};

Proc RunProcess(const std::wstring& application, const std::wstring& commandLine, const std::wstring& workDir,
                DWORD timeoutMs, bool killOnTimeout, bool noWindow) {
    Proc p;
    if (application.find(L'"') != std::wstring::npos || workDir.find(L'"') != std::wstring::npos) return p;
    std::vector<wchar_t> cmd(commandLine.begin(), commandLine.end());
    cmd.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    const wchar_t* dir = workDir.empty() ? nullptr : workDir.c_str();
    const wchar_t* app = application.empty() ? nullptr : application.c_str();
    const DWORD flags = noWindow ? CREATE_NO_WINDOW : 0;
    if (!CreateProcessW(app, cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, dir, &si, &pi)) return p;
    p.started = true;
    DWORD waited = 0;
    for (;;) {
        const DWORD w = WaitForSingleObject(pi.hProcess, 500);
        if (w == WAIT_OBJECT_0) break;
        waited += 500;
        if (timeoutMs && waited >= timeoutMs) {
            p.timedOut = true;
            if (killOnTimeout) TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    GetExitCodeProcess(pi.hProcess, &p.code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return p;
}

bool NvidiaOk(DWORD c) { return c == 0 || c == 1 || c == 3010; }

std::wstring CodeText(DWORD c) {
    wchar_t b[32];
    if (c > 255) swprintf_s(b, L"0x%08X", c);
    else swprintf_s(b, L"%lu", c);
    return b;
}

bool JsonUrlFromNvidia(const std::string& body, std::wstring& version, std::wstring& channel, std::wstring& url) {
    const size_t da = body.find("\"DriverAttributes\"");
    const size_t from = da == std::string::npos ? 0 : da;
    std::string v, name, link;
    JsonString(body, "Version", v, from);
    JsonString(body, "Name", name, from);
    if (!JsonString(body, "DownloadURLAdmin", link, from)) JsonString(body, "DownloadURL", link, from);
    if (link.empty()) return false;
    version = Widen(v);
    channel = Widen(name);
    url = Widen(link);
    return url.rfind(L"https://", 0) == 0;
}

bool QueryNvidia(const std::wstring& id, const std::wstring& build, bool laptop, std::wstring& version,
                 std::wstring& channel, std::wstring& url, std::wstring& err, const std::atomic<bool>& stop) {
    const std::string json = std::string("{\"dIDa\":[\"") + Narrow(id) + "\"],\"osC\":\"10.0\",\"osB\":\"" + Narrow(build) +
                             "\",\"is6\":\"1\",\"lg\":\"1033\",\"iLp\":\"" + (laptop ? "1" : "0") +
                             "\",\"prvMd\":\"0\",\"dch\":\"1\",\"upCRD\":\"0\",\"isCRD\":\"0\"}";
    const std::wstring api =
        L"https://gfwsl.geforce.com/nvidia_web_services/controller.gfeclientcontent.NG.php/"
        L"com.nvidia.services.GFEClientContent_NG.getDispDrvrByDevid/" +
        Widen(PercentEncode(json));
    HttpOut res;
    if (!Http(Allow::NvidiaApi, L"GET", api, false, 4ull << 20, {}, res, err, stop)) return false;
    if (!JsonUrlFromNvidia(res.body, version, channel, url)) {
        err = L"NVIDIA did not publish a package for this adapter.";
        return false;
    }
    wchar_t host[256] = {};
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || !HostAllowed(Allow::NvidiaFile, host)) {
        err = L"NVIDIA returned a package that is not on its download site.";
        url.clear();
        return false;
    }
    return true;
}

void FillNvidia(GpuDriverOffer& o, const Adapter& a, bool laptop, const std::wstring& other, Feed* feed,
                const std::atomic<bool>& stop) {
    o.vendor = 1;
    o.gpuName = a.name;
    if (!a.installed.empty()) o.installed = L"Windows driver  " + a.installed;
    o.summary = L"Keeps the display driver and PhysX. Leaves out the NVIDIA App, telemetry, ShadowPlay, FrameView, "
                L"and HD audio. The current NVIDIA driver is replaced. 7-Zip is installed first if it is not already "
                L"there. Nothing is installed until you confirm again.";
    if (!a.pci.valid()) {
        o.error = L"The adapter PCI id could not be read.";
        return;
    }
    if (feed) feed->Log(L"Asking NVIDIA for the current driver.");
    const std::wstring build = OsBuild();
    std::wstring err;
    bool got = false;
    if (a.pci.full()) got = QueryNvidia(a.pci.four(), build, laptop, o.version, o.channel, o.url, err, stop);
    if (!got && !stop.load()) {
        std::wstring err2;
        got = QueryNvidia(a.pci.two(), build, laptop, o.version, o.channel, o.url, err2, stop);
        if (!got) err = err.empty() ? err2 : err;
    }
    if (stop.load()) return;
    if (!got) {
        o.error = err.empty() ? L"NVIDIA did not publish a package for this adapter." : err;
        return;
    }
    if (o.channel.empty()) o.channel = L"GeForce Game Ready Driver";
    ProbeSize(o, Allow::NvidiaFile, false, stop);
    if (stop.load()) return;
    o.ok = true;
    CheckSpace(o);
    AddContext(o, laptop, other);
    if (feed) feed->Log(L"NVIDIA published " + o.version + L".");
}

bool ReadFileText(const std::wstring& path, std::string& out) {
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<char> buf(256 * 1024);
    DWORD read = 0;
    const BOOL ok = ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok || !read) return false;
    out.assign(buf.data(), buf.data() + read);
    return true;
}

bool LookupAmdIndex(std::wstring& url, std::wstring& sha, std::wstring& version, std::wstring& err,
                    const std::atomic<bool>& stop) {
    const std::wstring index = L"https://community.chocolatey.org/api/v2/package/amd-software-adrenalin-edition";
    const std::wstring dir = DriverDir();
    EnsureDir(dir);
    const std::wstring nupkg = dir + L"\\amd-index.nupkg";
    const std::wstring unpack = dir + L"\\amd-index";
    RemoveTree(unpack);
    EnsureDir(unpack);
    DeleteFileW(nupkg.c_str());
    HANDLE file = CreateFileW(nupkg.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        err = L"Could not store the AMD version index.";
        return false;
    }
    HttpOut res;
    const bool httpOk = Http(
        Allow::Index, L"GET", index, true, 8ull << 20,
        [&](const BYTE* p, DWORD n, uint64_t, uint64_t) {
            DWORD wrote = 0;
            return WriteFile(file, p, n, &wrote, nullptr) && wrote == n;
        },
        res, err, stop);
    CloseHandle(file);
    if (!httpOk) {
        DeleteFileW(nupkg.c_str());
        if (err.empty()) err = L"The AMD driver index did not respond.";
        return false;
    }
    const Proc tar = RunProcess(L"C:\\Windows\\System32\\tar.exe",
                                L"\"C:\\Windows\\System32\\tar.exe\" -xf \"" + nupkg + L"\" -C \"" + unpack + L"\"", dir,
                                60000, true, true);
    DeleteFileW(nupkg.c_str());
    if (!tar.started || tar.timedOut || tar.code != 0) {
        RemoveTree(unpack);
        err = L"The AMD driver index could not be read.";
        return false;
    }
    std::wstring script;
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW((unpack + L"\\tools\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (EndsWithI(fd.cFileName, L"chocolateyinstall.ps1")) {
                script = unpack + L"\\tools\\" + fd.cFileName;
                break;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::string text;
    if (script.empty() || !ReadFileText(script, text)) {
        RemoveTree(unpack);
        err = L"The AMD driver index did not name a package.";
        return false;
    }
    RemoveTree(unpack);
    std::string link, hash;
    if (!QuotedAfter(text, "url64", link)) QuotedAfter(text, "url", link);
    QuotedAfter(text, "checksum64", hash);
    url = Widen(link);
    sha = Lower(Widen(hash));
    if (url.rfind(L"https://drivers.amd.com/", 0) != 0) {
        url.clear();
        err = L"The AMD index did not point at an official package.";
        return false;
    }
    const size_t ed = url.find(L"edition-");
    if (ed != std::wstring::npos) {
        const size_t start = ed + 8;
        const size_t end = url.find(L'-', start);
        if (end != std::wstring::npos && end > start) version = url.substr(start, end - start);
    }
    if (sha.size() != 64) sha.clear();
    return true;
}

void FillAmd(GpuDriverOffer& o, const Adapter& a, bool laptop, const std::wstring& other, Feed* feed,
             const std::atomic<bool>& stop) {
    o.vendor = 2;
    o.gpuName = a.name;
    if (!a.installed.empty()) o.installed = L"Windows driver  " + a.installed;
    o.channel = L"AMD Software: Adrenalin Edition";
    o.summary = L"Keeps the AMD display driver only. 7-Zip unpacks the package and Windows installs that display "
                L"driver. Adrenalin, the overlay, Chill, and the rest of the suite are not installed. 7-Zip is "
                L"installed first if it is not already there. If the package cannot be trimmed, nothing is installed.";
    if (feed) feed->Log(L"Asking for the current AMD display driver.");
    std::wstring err;
    if (!LookupAmdIndex(o.url, o.sha256, o.version, err, stop) || stop.load()) {
        if (!stop.load()) o.error = err.empty() ? L"The AMD driver index did not respond." : err;
        return;
    }
    ProbeSize(o, Allow::AmdFile, true, stop);
    if (stop.load()) return;
    o.ok = true;
    CheckSpace(o);
    AddContext(o, laptop, other);
    if (feed) feed->Log(L"AMD published " + (o.version.empty() ? L"the current package" : o.version) + L".");
}

bool LooksLikePe(const std::wstring& path) {
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BYTE mag[2] = {};
    DWORD read = 0;
    const BOOL ok = ReadFile(h, mag, 2, &read, nullptr);
    CloseHandle(h);
    return ok && read == 2 && mag[0] == 'M' && mag[1] == 'Z';
}

bool BlockedDir(const wchar_t* name) {
    return ContainsI(name, L"Audio") || ContainsI(name, L"HDMI") || ContainsI(name, L"ACP") || ContainsI(name, L"SAM") ||
           ContainsI(name, L"Wireless") || ContainsI(name, L"AMDLink") || ContainsI(name, L"WVR");
}

bool FileHas(const std::wstring& path, const char* needle) {
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<BYTE> buf(256 * 1024);
    DWORD read = 0;
    ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr);
    CloseHandle(h);
    const size_t m = strlen(needle);
    if (read < m) return false;
    auto asciiAt = [&](DWORD i) {
        for (size_t j = 0; j < m; ++j) {
            char c = static_cast<char>(buf[i + j]);
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != needle[j]) return false;
        }
        return true;
    };
    for (DWORD i = 0; i + m <= read; ++i)
        if (asciiAt(i)) return true;
    if (read >= m * 2) {
        for (DWORD i = 0; i + m * 2 <= read; ++i) {
            bool ok = true;
            for (size_t j = 0; j < m; ++j) {
                char c = static_cast<char>(buf[i + j * 2]);
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                if (c != needle[j] || buf[i + j * 2 + 1] != 0) {
                    ok = false;
                    break;
                }
            }
            if (ok) return true;
        }
    }
    return false;
}

void CollectInf(const std::wstring& dir, int depth, std::vector<std::wstring>& infs) {
    if (depth > 6) return;
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        const std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if (BlockedDir(fd.cFileName)) continue;
            CollectInf(full, depth + 1, infs);
        } else if (EndsWithI(fd.cFileName, L".inf") && full.find(L'"') == std::wstring::npos) {
            infs.push_back(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void CollectArchives(const std::wstring& dir, std::vector<std::wstring>& archives) {
    if (archives.size() >= 3) return;
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (archives.size() >= 3) break;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        const std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) CollectArchives(full, archives);
        } else if (EndsWithI(fd.cFileName, L".7z") || EndsWithI(fd.cFileName, L".zip")) {
            archives.push_back(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

}  // namespace

GpuDriverOffer LookupGpuDriver(Feed* feed, const std::atomic<bool>& stop) {
    GpuDriverOffer o;
    if (stop.load()) return o;
    const std::vector<Adapter> all = ListAdapters();
    bool anyIntel = false;
    bool anyGpu = false;
    for (const Adapter& a : all) {
        if (a.vendor == 3) anyIntel = true;
        if (a.vendor == 1 || a.vendor == 2) anyGpu = true;
    }
    std::wstring other;
    const Adapter* chosen = Pick(all, other);
    if (!chosen) {
        o.error = anyIntel && !anyGpu ? L"Intel graphics stay on the current driver."
                                      : L"No NVIDIA or AMD display adapter was found.";
        if (feed) feed->Log(o.error);
        return o;
    }
    const bool laptop = Laptop();
    if (chosen->vendor == 1) FillNvidia(o, *chosen, laptop, other, feed, stop);
    else FillAmd(o, *chosen, laptop, other, feed, stop);
    if (!o.ok && feed && !o.error.empty()) feed->Log(o.error);
    return o;
}

int DownloadGpuDriver(Feed& feed, GpuDriverOffer& offer, const std::atomic<bool>& stop) {
    if (stop.load() || feed.Cancelled()) return 2;
    if (!offer.ok || offer.url.empty() || !offer.canDownload) {
        offer.error = offer.error.empty() ? L"There is no package to download." : offer.error;
        return 1;
    }
    const Allow allow = offer.vendor == 2 ? Allow::AmdFile : Allow::NvidiaFile;
    const bool browser = offer.vendor == 2;
    wchar_t host[256] = {};
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    if (!WinHttpCrackUrl(offer.url.c_str(), 0, 0, &uc) || !HostAllowed(allow, host)) {
        offer.error = L"The package address is not on the vendor site.";
        return 1;
    }
    const std::wstring dir = DriverDir();
    EnsureDir(dir);
    if (dir.find(L'"') != std::wstring::npos) {
        offer.error = L"The driver folder path is not usable.";
        return 1;
    }
    const std::wstring name = FileNameOf(offer.url);
    const std::wstring finalPath = dir + L"\\" + name;
    const std::wstring partial = finalPath + L".partial";
    WIN32_FILE_ATTRIBUTE_DATA existing{};
    if (offer.sha256.empty() && GetFileAttributesExW(finalPath.c_str(), GetFileExInfoStandard, &existing)) {
        const uint64_t have = (static_cast<uint64_t>(existing.nFileSizeHigh) << 32) | existing.nFileSizeLow;
        const bool sizeOk = have >= kMinPackage && (offer.bytes == 0 || have == offer.bytes);
        if (sizeOk && LooksLikePe(finalPath)) {
            offer.file = finalPath;
            offer.bytes = have;
            offer.error.clear();
            feed.Log(L"Using the package already downloaded.");
            feed.Progress(1, 0, 0);
            return 0;
        }
    }
    if (finalPath.find(L'"') != std::wstring::npos) {
        offer.error = L"The package name is not usable.";
        return 1;
    }
    HANDLE file = CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        offer.error = L"Could not create the download file.";
        return 1;
    }
    Sha256 sha;
    const ULONGLONG t0 = GetTickCount64();
    feed.Phase(L"GPU CLEAN", L"downloading the driver package");
    feed.Log(L"Downloading " + (offer.channel.empty() ? L"the driver" : offer.channel) +
             (offer.version.empty() ? L"" : L" " + offer.version) + L".");
    feed.Progress(0, 0, 0);
    HttpOut res;
    std::wstring err;
    const bool ok = Http(
        allow, L"GET", offer.url, browser, kMaxPackage,
        [&](const BYTE* p, DWORD n, uint64_t got, uint64_t total) {
            if (stop.load()) return false;
            DWORD wrote = 0;
            if (!WriteFile(file, p, n, &wrote, nullptr) || wrote != n) return false;
            sha.Add(p, n);
            const uint64_t denom = total ? total : offer.bytes;
            const double elapsed = static_cast<double>(GetTickCount64() - t0) / 1000.0;
            const double frac = denom ? std::min(1.0, static_cast<double>(got) / static_cast<double>(denom)) : 0;
            const double remain = frac > 0.02 ? elapsed * (1.0 - frac) / frac : 0;
            feed.Progress(frac, elapsed, remain);
            return true;
        },
        res, err, stop);
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(partial.c_str());
        if (stop.load() || feed.Cancelled() || res.stopped) return 2;
        offer.error = err.empty() ? L"The download failed." : err;
        return 1;
    }
    const std::wstring digest = sha.Hex();
    if (!offer.sha256.empty() && (digest.empty() || !HexEq(digest, offer.sha256))) {
        DeleteFileW(partial.c_str());
        offer.error = digest.empty() ? L"Could not hash the package." : L"The AMD package hash did not match the published hash.";
        return 1;
    }
    if (!MoveFileExW(partial.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(partial.c_str());
        offer.error = L"Could not finish saving the package.";
        return 1;
    }
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(finalPath.c_str(), GetFileExInfoStandard, &info)) {
        offer.error = L"The package did not save.";
        return 1;
    }
    const uint64_t bytes = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (bytes < kMinPackage || !LooksLikePe(finalPath)) {
        DeleteFileW(finalPath.c_str());
        offer.error = L"The download was not an installer package.";
        return 1;
    }
    offer.file = finalPath;
    offer.bytes = bytes;
    offer.error.clear();
    feed.Progress(1, static_cast<double>(GetTickCount64() - t0) / 1000.0, 0);
    feed.Log(L"Download complete.");
    return 0;
}

// Official 7-Zip 26.05 x64 installer. www.7-zip.org redirects to the GitHub
// release, which redirects to release-assets.githubusercontent.com.
constexpr wchar_t kSevenZipUrl[] = L"https://www.7-zip.org/a/7z2605-x64.exe";

std::wstring EnsureSevenZip(Feed* feed, std::wstring& err) {
    const std::wstring have = SevenZip();
    if (!have.empty()) return have;
    if (feed && feed->Cancelled()) {
        err = L"Stopped.";
        return {};
    }
    if (feed) feed->Phase(L"GPU CLEAN", L"installing 7-Zip");
    if (feed) feed->Log(L"7-Zip is not installed. Downloading the official 7-Zip installer.");
    const std::wstring dir = DriverDir();
    EnsureDir(dir);
    const std::wstring setup = dir + L"\\7z2605-x64.exe";
    DeleteFileW(setup.c_str());
    const HANDLE file = CreateFileW(setup.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        err = L"Could not store the 7-Zip installer.";
        return {};
    }
    std::atomic<bool> stop{false};
    HttpOut res;
    const bool saved = Http(
        Allow::SevenZip, L"GET", kSevenZipUrl, true, 16ull << 20,
        [&](const BYTE* p, DWORD n, uint64_t, uint64_t) {
            if (feed && feed->Cancelled()) {
                stop.store(true);
                return false;
            }
            DWORD wrote = 0;
            return WriteFile(file, p, n, &wrote, nullptr) && wrote == n;
        },
        res, err, stop);
    CloseHandle(file);
    if (!saved || !LooksLikePe(setup)) {
        DeleteFileW(setup.c_str());
        if ((feed && feed->Cancelled()) || res.stopped) err = L"Stopped.";
        else if (err.empty()) err = L"The 7-Zip installer could not be downloaded.";
        return {};
    }
    if (feed) feed->Log(L"Installing 7-Zip.");
    const std::wstring cmd = L"\"" + setup + L"\" /S";
    const Proc installed = RunProcess(setup, cmd, dir, 5u * 60u * 1000u, true, true);
    DeleteFileW(setup.c_str());
    if (!installed.started) {
        err = L"The 7-Zip installer did not start.";
        return {};
    }
    if (installed.timedOut || installed.code != 0) {
        err = L"7-Zip did not install (" + CodeText(installed.code) + L").";
        return {};
    }
    const std::wstring ready = SevenZip();
    if (ready.empty()) {
        err = L"7-Zip installed, but 7z.exe was not found.";
        return {};
    }
    if (feed) feed->Log(L"7-Zip is installed.");
    return ready;
}

int InstallTrimmedGpuDriver(Feed& feed, GpuDriverOffer& offer) {
    if (offer.file.empty() || !IsUnder(offer.file, DriverDir()) || !LooksLikePe(offer.file)) {
        offer.error = L"There is no downloaded package to install.";
        return 1;
    }
    const std::wstring dir = DriverDir();
    feed.Phase(L"GPU CLEAN", L"installing the trimmed driver");
    feed.Narrate(L"Installing", L"The display may flicker. This can take several minutes.");

    if (offer.vendor == 1) {
        // Display.Driver and Display.PhysX only. MSVCRuntime2019's own check
        // refuses to install over a newer runtime already on the machine, and
        // an explicitly named package that fails that check aborts the whole
        // install with 0xE6000000. Left unnamed, setup skips it when present
        // and installs it when missing. -n is setup.cfg's IgnoreReboot option.
        const std::wstring keep = L"Display.Driver Display.PhysX";
        std::wstring zipErr;
        const std::wstring seven = EnsureSevenZip(&feed, zipErr);
        if (seven.empty()) {
            offer.error = zipErr.empty() ? L"7-Zip could not be installed, so the NVIDIA package cannot be trimmed." : zipErr;
            return 1;
        }
        const std::wstring unpack = dir + L"\\nvidia-unpack";
        RemoveTree(unpack);
        EnsureDir(unpack);
        feed.Progress(0.05, 0, 0);
        feed.Log(L"Unpacking the NVIDIA package.");
        const std::wstring xcmd = L"\"" + seven + L"\" x -y -o\"" + unpack + L"\" \"" + offer.file + L"\"";
        const Proc unzip = RunProcess(seven, xcmd, dir, 20u * 60u * 1000u, true, true);
        const std::wstring setup = unpack + L"\\setup.exe";
        if (!unzip.started || unzip.timedOut || (unzip.code != 0 && unzip.code != 1) ||
            GetFileAttributesW(setup.c_str()) == INVALID_FILE_ATTRIBUTES) {
            RemoveTree(unpack);
            offer.error = L"The NVIDIA package could not be unpacked.";
            return 1;
        }
        feed.Progress(0.4, 0, 0);
        feed.Log(L"NVIDIA setup, display driver and PhysX only.");
        const std::wstring setupLog = dir + L"\\nvidia-setup.log";
        RemoveTree(setupLog);
        const std::wstring cmd = L"\"" + setup + L"\" -s -n " + keep + L" -log:" + setupLog + L" -loglevel:6";
        const Proc p = RunProcess(setup, cmd, unpack, 25u * 60u * 1000u, false, false);
        if (!p.started) {
            offer.error = Format(L"NVIDIA setup did not start (%lu).", GetLastError());
            return 1;
        }
        if (p.timedOut) {
            offer.error = L"NVIDIA setup is still running after 25 minutes. It was left running.";
            return 1;
        }
        if (NvidiaOk(p.code)) {
            feed.Progress(1, 0, 0);
            feed.Log(L"NVIDIA setup finished (" + CodeText(p.code) + L"). Restart to finish the driver.");
            offer.error.clear();
            RemoveTree(unpack);
            return 0;
        }
        RemoveTree(unpack);
        offer.error = L"NVIDIA setup failed (" + CodeText(p.code) +
                      L"). The NVIDIA App was not installed. The setup log is " + setupLog + L".";
        return 1;
    }

    std::wstring zipErr;
    const std::wstring seven = EnsureSevenZip(&feed, zipErr);
    if (seven.empty()) {
        offer.error = zipErr.empty() ? L"7-Zip could not be installed, so the AMD package was not opened. It is saved at " +
                                       offer.file + L"."
                                 : zipErr + L" The package is saved at " + offer.file + L".";
        return 1;
    }
    const std::wstring unpack = dir + L"\\amd-unpack";
    RemoveTree(unpack);
    EnsureDir(unpack);
    feed.Log(L"Unpacking the AMD package. The Adrenalin installer is not run.");
    const std::wstring cmd = L"\"" + seven + L"\" x -y -o\"" + unpack + L"\" \"" + offer.file + L"\"";
    const Proc unzip = RunProcess(seven, cmd, dir, 15u * 60u * 1000u, true, true);
    if (!unzip.started || unzip.timedOut || (unzip.code != 0 && unzip.code != 1)) {
        RemoveTree(unpack);
        offer.error = L"The AMD package could not be unpacked. It is saved at " + offer.file +
                      L". Run it and choose Driver Only.";
        return 1;
    }
    std::vector<std::wstring> inners;
    CollectArchives(unpack, inners);
    int n = 0;
    for (const std::wstring& arc : inners) {
        const std::wstring sub = unpack + L"\\inner" + std::to_wstring(++n);
        EnsureDir(sub);
        const std::wstring icmd = L"\"" + seven + L"\" x -y -o\"" + sub + L"\" \"" + arc + L"\"";
        RunProcess(seven, icmd, dir, 15u * 60u * 1000u, true, true);
    }
    std::vector<std::wstring> infs;
    CollectInf(unpack, 0, infs);
    std::vector<std::wstring> hits;
    for (const std::wstring& inf : infs) {
        if (!IsUnder(inf, unpack)) continue;
        if (FileHas(inf, "amdkmdag")) hits.push_back(inf);
    }
    std::vector<std::wstring> display;
    for (const std::wstring& inf : hits)
        if (ContainsI(inf, L"\\display\\")) display.push_back(inf);
    const std::vector<std::wstring>& use = display.empty() ? hits : display;
    if (use.empty()) {
        RemoveTree(unpack);
        offer.error = L"The AMD package unpacked, but no display driver was inside. The package is saved at " +
                      offer.file + L".";
        return 1;
    }
    const std::wstring pnputil = L"C:\\Windows\\System32\\pnputil.exe";
    bool any = false;
    DWORD last = 0xFFFFFFFF;
    const size_t limit = std::min<size_t>(use.size(), 4);
    for (size_t i = 0; i < limit; ++i) {
        const std::wstring& inf = use[i];
        feed.Log(L"Installing display driver " + inf.substr(inf.find_last_of(L'\\') + 1) + L".");
        const std::wstring pcmd = L"\"" + pnputil + L"\" /add-driver \"" + inf + L"\" /install";
        const Proc p = RunProcess(pnputil, pcmd, dir, 10u * 60u * 1000u, false, true);
        if (!p.started) continue;
        if (p.timedOut) {
            RemoveTree(unpack);
            offer.error = L"The display-driver install is still running. It was left running.";
            return 1;
        }
        last = p.code;
        if (p.code == 0 || p.code == 3010) any = true;
    }
    RemoveTree(unpack);
    if (!any) {
        offer.error = L"The AMD display driver did not install (" + CodeText(last) + L").";
        return 1;
    }
    feed.Log(L"AMD display driver installed. Restart to finish it. Adrenalin was not installed.");
    offer.error.clear();
    return 0;
}

std::wstring ProgramFilesX86() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"ProgramFiles(x86)", buf, _countof(buf));
    if (n > 0 && n < _countof(buf)) return buf;
    return L"C:\\Program Files (x86)";
}

std::wstring AfterburnerExe() {
    const std::wstring path = ProgramFilesX86() + L"\\MSI Afterburner\\MSIAfterburner.exe";
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES ? path : L"";
}

std::wstring RtssExe() {
    const std::wstring path = ProgramFilesX86() + L"\\RivaTuner Statistics Server\\RTSS.exe";
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES ? path : L"";
}

bool WriteTextFile(const std::wstring& path, const std::string& text) {
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
    CloseHandle(h);
    return ok && wrote == text.size();
}

// Replaces the first line in [Settings] that begins with key. Afterburner's
// shipped file uses "Key = value".
bool SetCfgKey(std::string& text, const char* key, const char* value) {
    const std::string name = key;
    size_t at = text.find(std::string("\n") + name);
    if (at == std::string::npos && text.compare(0, name.size(), name) == 0) at = 0;
    else if (at != std::string::npos) ++at;
    else return false;
    const size_t end = text.find('\n', at);
    const size_t count = (end == std::string::npos ? text.size() : end) - at;
    text.replace(at, count, name + " = " + value);
    return true;
}

void ConfigureAfterburner(const std::wstring& exe) {
    const size_t slash = exe.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    const std::wstring cfg = exe.substr(0, slash) + L"\\MSIAfterburner.cfg";
    std::string text;
    if (!ReadFileText(cfg, text)) return;
    SetCfgKey(text, "StartWithWindows", "1");
    SetCfgKey(text, "StartMinimized", "1");
    SetCfgKey(text, "RememberSettings", "1");
    SetCfgKey(text, "StartupViaTaskScheduler", "1");
    SetCfgKey(text, "StartupDelay", "0");
    SetCfgKey(text, "UnlockVoltageControl", "1");
    SetCfgKey(text, "UnlockVoltageMonitoring", "1");
    SetCfgKey(text, "FirstRun", "0");
    WriteTextFile(cfg, text);
}

bool ScheduleLogon(const wchar_t* task, const std::wstring& command) {
    const std::wstring sch = L"C:\\Windows\\System32\\schtasks.exe";
    const std::wstring args = L"\"" + sch + L"\" /Create /F /SC ONLOGON /TN \"" + std::wstring(task) +
                              L"\" /TR \"" + command + L"\"";
    const Proc p = RunProcess(sch, args, L"", 20000, true, true);
    return p.started && !p.timedOut && p.code == 0;
}

void DeleteLogonTask(const wchar_t* task) {
    const std::wstring sch = L"C:\\Windows\\System32\\schtasks.exe";
    const std::wstring args = L"\"" + sch + L"\" /Delete /F /TN \"" + std::wstring(task) + L"\"";
    RunProcess(sch, args, L"", 15000, true, true);
}

struct Started {
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    DWORD pid = 0;
    bool ok = false;
};

Started StartShown(const std::wstring& exe, const std::wstring& args) {
    Started s;
    std::wstring cmd = L"\"" + exe + L"\"";
    if (!args.empty()) cmd += L" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION pi{};
    const size_t slash = exe.find_last_of(L"\\/");
    const std::wstring dir = slash == std::wstring::npos ? L"" : exe.substr(0, slash);
    if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
        return s;
    }
    s.process = pi.hProcess;
    s.thread = pi.hThread;
    s.pid = pi.dwProcessId;
    s.ok = true;
    return s;
}

void CloseStarted(Started& s) {
    if (s.thread) CloseHandle(s.thread);
    if (s.process) CloseHandle(s.process);
    s = {};
}

BOOL CALLBACK ClosePidWindow(HWND hwnd, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == static_cast<DWORD>(lp)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

bool InstallAfterburner(Feed* feed, std::wstring& err) {
    auto stopped = [&] { return feed && feed->Cancelled(); };
    auto log = [&](const std::wstring& s) { if (feed) feed->Log(s); };
    auto phase = [&](const wchar_t* d) { if (feed) feed->Phase(L"AFTERBURNER", d); };
    if (stopped()) {
        err = L"Stopped.";
        return false;
    }

    std::wstring ab = AfterburnerExe();
    if (ab.empty()) {
        phase(L"downloading MSI Afterburner");
        log(L"Downloading MSI Afterburner 4.6.6 from the Guru3D archive.");
        std::wstring zipErr;
        const std::wstring seven = EnsureSevenZip(feed, zipErr);
        if (seven.empty()) {
            err = zipErr.empty() ? L"7-Zip is required to open the Afterburner package." : zipErr;
            return false;
        }
        const std::wstring dir = DriverDir();
        EnsureDir(dir);
        const std::wstring zip = dir + L"\\MSIAfterburnerSetup466.zip";
        DeleteFileW(zip.c_str());
        const HANDLE file = CreateFileW(zip.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            err = L"Could not store the Afterburner download.";
            return false;
        }
        std::atomic<bool> stop{false};
        Sha256 sha;
        HttpOut res;
        const wchar_t* url =
            L"https://ftp.nluug.nl/pub/games/PC/guru3d/afterburner/%5BGuru3D%5D-MSIAfterburnerSetup466Build16757.zip";
        const bool saved = Http(
            Allow::Afterburner, L"GET", url, true, 64ull << 20,
            [&](const BYTE* p, DWORD n, uint64_t got, uint64_t total) {
                if (stopped()) {
                    stop.store(true);
                    return false;
                }
                DWORD wrote = 0;
                if (!WriteFile(file, p, n, &wrote, nullptr) || wrote != n) return false;
                sha.Add(p, n);
                if (feed && total) feed->Progress(std::min(0.25, 0.25 * static_cast<double>(got) / static_cast<double>(total)), 0, 0);
                return true;
            },
            res, err, stop);
        CloseHandle(file);
        const std::wstring digest = sha.Hex();
        constexpr wchar_t kHash[] = L"348e9690a638d84e30e32e9c7c2dd7aad9fd82d71eced1942c04c4c5620b0e1e";
        if (!saved || !HexEq(digest, kHash)) {
            DeleteFileW(zip.c_str());
            if (stopped() || res.stopped) err = L"Stopped.";
            else if (!saved && err.empty()) err = L"The Afterburner download failed.";
            else err = L"The Afterburner download did not match the published checksum.";
            return false;
        }
        const std::wstring unpack = dir + L"\\afterburner-setup";
        RemoveTree(unpack);
        EnsureDir(unpack);
        log(L"Unpacking the Afterburner installer.");
        const std::wstring xcmd = L"\"" + seven + L"\" x -y -o\"" + unpack + L"\" \"" + zip + L"\"";
        const Proc unzip = RunProcess(seven, xcmd, dir, 5u * 60u * 1000u, true, true);
        const std::wstring setup = unpack + L"\\MSIAfterburnerSetup466.exe";
        if (!unzip.started || unzip.timedOut || unzip.code != 0 || !LooksLikePe(setup)) {
            RemoveTree(unpack);
            DeleteFileW(zip.c_str());
            err = L"The Afterburner package could not be unpacked.";
            return false;
        }
        phase(L"installing MSI Afterburner and RivaTuner");
        log(L"Installing MSI Afterburner.");
        const std::wstring scmd = L"\"" + setup + L"\" /S";
        const Proc inst = RunProcess(setup, scmd, unpack, 10u * 60u * 1000u, true, true);
        if (!inst.started || inst.timedOut || inst.code != 0) {
            RemoveTree(unpack);
            DeleteFileW(zip.c_str());
            err = L"MSI Afterburner did not install (" + CodeText(inst.code) + L").";
            return false;
        }
        if (RtssExe().empty()) {
            log(L"Installing RivaTuner Statistics Server.");
            const std::wstring rtssSetup = unpack + L"\\RTSSSetup.exe";
            const std::wstring ecmd = L"\"" + seven + L"\" e \"" + setup + L"\" -o\"" + unpack + L"\" \"Redist\\RTSSSetup.exe\" -y";
            RunProcess(seven, ecmd, unpack, 2u * 60u * 1000u, true, true);
            if (LooksLikePe(rtssSetup)) {
                const std::wstring rcmd = L"\"" + rtssSetup + L"\" /S";
                RunProcess(rtssSetup, rcmd, unpack, 10u * 60u * 1000u, true, true);
            }
        }
        RemoveTree(unpack);
        DeleteFileW(zip.c_str());
        ab = AfterburnerExe();
        if (ab.empty()) {
            err = L"MSI Afterburner installed, but MSIAfterburner.exe was not found.";
            return false;
        }
        log(L"MSI Afterburner is installed.");
    } else {
        log(L"MSI Afterburner is already installed.");
    }

    ConfigureAfterburner(ab);
    const std::wstring rtss = RtssExe();
    if (!rtss.empty()) log(L"RivaTuner Statistics Server is installed.");
    else log(L"RivaTuner Statistics Server was not found next to Afterburner.");

    const bool nvidia = GetFileAttributesW(L"C:\\Windows\\System32\\nvapi64.dll") != INVALID_FILE_ATTRIBUTES;
    const size_t slash = ab.find_last_of(L"\\/");
    const std::wstring scanner = slash == std::wstring::npos
                                     ? L""
                                     : ab.substr(0, slash) + L"\\Bundle\\OCScanner\\MSIOCScanner_x64.exe";
    bool scanned = false;
    if (!nvidia) {
        log(L"The OC scanner targets NVIDIA. Afterburner will still apply its saved settings at logon.");
    } else if (GetFileAttributesW(scanner.c_str()) == INVALID_FILE_ATTRIBUTES) {
        log(L"The OC scanner was not in this Afterburner install. Startup apply is still configured.");
    } else if (!stopped()) {
        phase(L"scanning a stable overclock");
        if (feed) {
            feed->Narrate(L"OC scanner",
                          L"MSI's scanner is testing this NVIDIA GPU. It usually takes about 30 minutes. "
                          L"Leave the scanner window open. The result is applied at every logon.");
        }
        log(L"Starting MSI Afterburner, then the OC scanner.");
        Started burner = StartShown(ab, L"");
        if (!burner.ok) {
            log(L"MSI Afterburner did not start, so the OC scanner did not run.");
        } else {
            for (int i = 0; i < 16 && !stopped(); ++i) Sleep(500);
            Started scan = StartShown(scanner, L"");
            if (!scan.ok) {
            CloseStarted(burner);
            log(L"The OC scanner did not start. Afterburner will still apply its saved settings at logon.");
        } else {
            const ULONGLONG t0 = GetTickCount64();
            constexpr DWORD kScanMs = 45u * 60u * 1000u;
            bool done = false;
            while (!stopped()) {
                const DWORD w = WaitForSingleObject(scan.process, 1000);
                const DWORD elapsed = static_cast<DWORD>(GetTickCount64() - t0);
                if (feed) {
                    feed->Progress(0.30 + 0.65 * std::min(1.0, static_cast<double>(elapsed) / (30.0 * 60.0 * 1000.0)),
                                   elapsed / 1000.0, 0);
                }
                if (w == WAIT_OBJECT_0) {
                    done = true;
                    break;
                }
                if (elapsed >= kScanMs) break;
            }
            DWORD code = 1;
            if (done) GetExitCodeProcess(scan.process, &code);
            if (stopped()) {
                TerminateProcess(scan.process, 1);
                err = L"Stopped.";
                CloseStarted(scan);
                EnumWindows(ClosePidWindow, static_cast<LPARAM>(burner.pid));
                WaitForSingleObject(burner.process, 5000);
                TerminateProcess(burner.process, 1);
                CloseStarted(burner);
                return false;
            }
            if (!done) {
                log(L"The OC scanner is still running after 45 minutes. It was left open.");
            } else if (code == 0) {
                scanned = true;
                log(L"The OC scanner finished and sent the curve to Afterburner.");
            } else {
                log(L"The OC scanner finished with code " + CodeText(code) +
                    L". Afterburner will still apply its saved settings at logon.");
            }
            CloseStarted(scan);
            EnumWindows(ClosePidWindow, static_cast<LPARAM>(burner.pid));
            if (WaitForSingleObject(burner.process, 15000) != WAIT_OBJECT_0) TerminateProcess(burner.process, 0);
            CloseStarted(burner);
            }
        }
    }

    const std::wstring abTr = L"\\\"" + ab + L"\\\" /s";
    if (!ScheduleLogon(L"Synchronus Afterburner", abTr)) {
        log(L"Could not create the Afterburner logon task. Start with Windows is still set in its config.");
    } else {
        log(L"Afterburner will start at logon and apply the saved overclock.");
    }
    if (!rtss.empty()) {
        const std::wstring rtTr = L"\\\"" + rtss + L"\\\"";
        if (ScheduleLogon(L"Synchronus RivaTuner", rtTr)) log(L"RivaTuner will start at logon.");
    }
    if (feed) feed->Progress(1, 0, 0);
    err.clear();
    return true;
}

void RemoveAfterburnerStartup() {
    DeleteLogonTask(L"Synchronus Afterburner");
    DeleteLogonTask(L"Synchronus RivaTuner");
}

}  // namespace gui
