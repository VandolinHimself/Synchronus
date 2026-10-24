// Update.cpp - check luciuswayne.com for a newer Synchronus build.
//
// The window asks for one document, kUpdateManifestUrl, when it opens.
// That document is the whole announcement: version, notes, and the address
// of the executable. The executable is downloaded only after the user agrees,
// and only from https://media.luciuswayne.com/Synchronus.exe or
// https://media.luciuswayne.com/Synchronus-<release>.exe. It is swapped in
// only when its SHA-256 and its VERSIONINFO both match the document.
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <winver.h>

#include "Gui.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")

namespace gui {
namespace {

constexpr uint64_t kMaxManifest = 64ull * 1024;
constexpr uint64_t kMaxExe = 64ull * 1024 * 1024;

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

bool HostAllowed(const std::wstring& host) {
    const std::wstring h = Lower(host);
    return h == L"luciuswayne.com" || h == L"www.luciuswayne.com" || h == L"media.luciuswayne.com";
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Up to four numeric parts. A missing part is zero, so 1.12.1 and 1.12.1.0 match.
// Each part fits the 16-bit fields of a VERSIONINFO resource.
bool ParseVersion(const std::wstring& text, unsigned out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    if (text.empty() || text.size() > 32) return false;
    unsigned parts[4] = {};
    int n = 0;
    size_t i = 0;
    while (i < text.size()) {
        if (n >= 4 || text[i] < L'0' || text[i] > L'9') return false;
        unsigned v = 0;
        while (i < text.size() && text[i] >= L'0' && text[i] <= L'9') {
            const unsigned d = static_cast<unsigned>(text[i] - L'0');
            if (v > 6553 || (v == 6553 && d > 5)) return false;
            v = v * 10 + d;
            ++i;
        }
        parts[n++] = v;
        if (i == text.size()) break;
        if (text[i] != L'.') return false;
        ++i;
        if (i == text.size()) return false;
    }
    if (n == 0) return false;
    for (int p = 0; p < 4; ++p) out[p] = parts[p];
    return true;
}

int CmpVersion(const unsigned a[4], const unsigned b[4]) {
    for (int i = 0; i < 4; ++i) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

std::wstring VersionText(const unsigned v[4]) {
    if (v[3] != 0) return Format(L"%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
    return Format(L"%u.%u.%u", v[0], v[1], v[2]);
}

void SkipWs(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
}

bool ParseJsonString(const std::string& s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"') return true;
        if (static_cast<unsigned char>(c) < 0x20) return false;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (i >= s.size()) return false;
        const char n = s[i++];
        switch (n) {
            case '"':
            case '\\':
            case '/':
                out.push_back(n);
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u':
                if (i + 4 > s.size()) return false;
                i += 4;
                out.push_back('?');
                break;
            default:
                return false;
        }
    }
    return false;
}

bool SkipContainer(const std::string& s, size_t& i) {
    if (i >= s.size() || (s[i] != '{' && s[i] != '[')) return false;
    int depth = 0;
    bool inStr = false;
    bool esc = false;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') {
            inStr = true;
            continue;
        }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') {
            --depth;
            if (depth == 0) {
                ++i;
                return true;
            }
        }
    }
    return false;
}

struct Manifest {
    std::string version, url, notes, sha256;
    bool hasSize = false;
    uint64_t size = 0;
};

bool ParseManifest(const std::string& body, Manifest& m, std::wstring& err) {
    size_t i = 0;
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF && static_cast<unsigned char>(body[1]) == 0xBB &&
        static_cast<unsigned char>(body[2]) == 0xBF) {
        i = 3;
    }
    SkipWs(body, i);
    if (i >= body.size() || body[i] != '{') {
        err = L"The update list is not a JSON object.";
        return false;
    }
    ++i;
    bool any = false;
    while (true) {
        SkipWs(body, i);
        if (i < body.size() && body[i] == '}') {
            ++i;
            break;
        }
        if (any) {
            if (i >= body.size() || body[i] != ',') {
                err = L"The update list is not valid JSON.";
                return false;
            }
            ++i;
            SkipWs(body, i);
        }
        any = true;
        std::string key;
        if (!ParseJsonString(body, i, key)) {
            err = L"The update list is not valid JSON.";
            return false;
        }
        SkipWs(body, i);
        if (i >= body.size() || body[i] != ':') {
            err = L"The update list is not valid JSON.";
            return false;
        }
        ++i;
        SkipWs(body, i);
        if (i >= body.size()) {
            err = L"The update list is not valid JSON.";
            return false;
        }
        if (body[i] == '"') {
            std::string val;
            if (!ParseJsonString(body, i, val)) {
                err = L"The update list is not valid JSON.";
                return false;
            }
            if (key == "version") m.version = std::move(val);
            else if (key == "url") m.url = std::move(val);
            else if (key == "notes") m.notes = std::move(val);
            else if (key == "sha256") m.sha256 = std::move(val);
        } else if (body[i] == '-' || (body[i] >= '0' && body[i] <= '9')) {
            const bool neg = body[i] == '-';
            if (neg) ++i;
            if (i >= body.size() || body[i] < '0' || body[i] > '9') {
                err = L"The update list is not valid JSON.";
                return false;
            }
            uint64_t v = 0;
            while (i < body.size() && body[i] >= '0' && body[i] <= '9') {
                const unsigned d = static_cast<unsigned>(body[i] - '0');
                if (v > (UINT64_MAX - d) / 10) {
                    err = L"The update list is not valid JSON.";
                    return false;
                }
                v = v * 10 + d;
                ++i;
            }
            if (i < body.size() && body[i] == '.') {
                ++i;
                while (i < body.size() && body[i] >= '0' && body[i] <= '9') ++i;
            }
            if (i < body.size() && (body[i] == 'e' || body[i] == 'E')) {
                err = L"The update list is not valid JSON.";
                return false;
            }
            if (key == "size" && !neg) {
                m.hasSize = true;
                m.size = v;
            }
        } else if (body.compare(i, 4, "true") == 0) {
            i += 4;
        } else if (body.compare(i, 5, "false") == 0) {
            i += 5;
        } else if (body.compare(i, 4, "null") == 0) {
            i += 4;
        } else if (body[i] == '{' || body[i] == '[') {
            if (!SkipContainer(body, i)) {
                err = L"The update list is not valid JSON.";
                return false;
            }
        } else {
            err = L"The update list is not valid JSON.";
            return false;
        }
    }
    SkipWs(body, i);
    if (i != body.size()) {
        err = L"The update list is not valid JSON.";
        return false;
    }
    return true;
}

std::wstring NormalizeHash(const std::string& s) {
    std::wstring o;
    o.reserve(s.size());
    for (unsigned char c : s) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        o.push_back(static_cast<wchar_t>(towlower(c)));
    }
    return o;
}

bool IsSha256(const std::wstring& s) {
    if (s.size() != 64) return false;
    for (wchar_t c : s) {
        if (!iswxdigit(c)) return false;
    }
    return true;
}

struct UrlParts {
    std::wstring host;
    std::wstring object;
    INTERNET_PORT port = 0;
};

bool CrackHttps(const std::wstring& url, UrlParts& out, std::wstring& err) {
    if (url.size() > 2048) {
        err = L"The update address is too long.";
        return false;
    }
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    wchar_t extra[2048] = {};
    wchar_t user[8] = {};
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = _countof(path);
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = _countof(extra);
    uc.lpszUserName = user;
    uc.dwUserNameLength = _countof(user);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        err = L"The update address is not https.";
        return false;
    }
    if (uc.dwUserNameLength != 0) {
        err = L"The update address is not on luciuswayne.com.";
        return false;
    }
    if (!HostAllowed(host)) {
        err = L"The update address is not on luciuswayne.com.";
        return false;
    }
    out.host = host;
    out.object = path;
    if (extra[0]) out.object += extra;
    out.port = uc.nPort ? uc.nPort : INTERNET_DEFAULT_HTTPS_PORT;
    return true;
}

// /Synchronus.exe, or /Synchronus-<release>.exe where the release token is
// 1..64 of [A-Za-z0-9._-] and does not contain "..". Another host is refused,
// including after a redirect.
bool IsPublishedExePath(const std::wstring& path) {
    if (_wcsicmp(path.c_str(), L"/Synchronus.exe") == 0) return true;
    const std::wstring prefix = L"/Synchronus-";
    const std::wstring suffix = L".exe";
    if (path.size() <= prefix.size() + suffix.size()) return false;
    if (_wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) != 0) return false;
    if (_wcsicmp(path.c_str() + (path.size() - suffix.size()), suffix.c_str()) != 0) return false;
    const std::wstring token = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
    if (token.empty() || token.size() > 64 || token.find(L"..") != std::wstring::npos) return false;
    for (wchar_t c : token) {
        const bool ok = (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || c == L'.' ||
                        c == L'_' || c == L'-';
        if (!ok) return false;
    }
    return true;
}

bool ExeUrl(const std::wstring& url, std::wstring& err) {
    UrlParts parts;
    if (!CrackHttps(url, parts, err)) return false;
    if (Lower(parts.host) != L"media.luciuswayne.com" || parts.port != INTERNET_DEFAULT_HTTPS_PORT) {
        err = L"The update file is not on media.luciuswayne.com.";
        return false;
    }
    const size_t q = parts.object.find(L'?');
    const std::wstring path = q == std::wstring::npos ? parts.object : parts.object.substr(0, q);
    if (!IsPublishedExePath(path)) {
        err = L"The update file must be https://media.luciuswayne.com/Synchronus.exe or Synchronus-<release>.exe.";
        return false;
    }
    return true;
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

struct HttpOut {
    DWORD status = 0;
    std::string body;
    std::wstring sha256;
    uint64_t bytes = 0;
};

// savePath empty: keep the body, up to maxBytes. Otherwise write the body
// there and return its SHA-256. The final URL after redirects must still be
// https on luciuswayne.com. When exe is set, that final URL must be a
// published Synchronus executable on media.luciuswayne.com.
bool HttpGet(const std::wstring& url, const std::wstring& agent, bool exe, uint64_t maxBytes, uint64_t expectedSize,
             const std::wstring& savePath, const std::function<void(uint64_t, uint64_t)>& progress,
             const std::atomic<bool>& stop, HttpOut& out, std::wstring& err) {
    out = {};
    if (stop.load()) {
        err = L"Stopped.";
        return false;
    }
    UrlParts parts;
    if (!CrackHttps(url, parts, err)) return false;
    if (exe && !ExeUrl(url, err)) return false;

    HINTERNET ses = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        err = L"Could not reach luciuswayne.com.";
        return false;
    }
    WinHttpSetTimeouts(ses, 8000, 8000, 20000, exe ? 30000 : 10000);
    HINTERNET con = WinHttpConnect(ses, parts.host.c_str(), parts.port, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", parts.object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                        : nullptr;
    auto close = [&] {
        if (req) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
    };
    if (!req) {
        err = L"Could not reach luciuswayne.com.";
        close();
        return false;
    }
    DWORD dec = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(req, WINHTTP_OPTION_DECOMPRESSION, &dec, sizeof(dec));
    const std::wstring headers = exe ? L"Accept: application/octet-stream\r\nCache-Control: no-cache\r\n"
                                     : L"Accept: application/json\r\nCache-Control: no-cache\r\n";
    WinHttpAddRequestHeaders(req, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        err = GetLastError() == ERROR_WINHTTP_SECURE_FAILURE
                  ? L"luciuswayne.com did not present a trusted certificate."
                  : L"Could not reach luciuswayne.com.";
        close();
        return false;
    }

    DWORD sz = sizeof(out.status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &out.status, &sz, WINHTTP_NO_HEADER_INDEX);
    DWORD urlSz = 0;
    WinHttpQueryOption(req, WINHTTP_OPTION_URL, nullptr, &urlSz);
    if (urlSz > sizeof(wchar_t) && urlSz < 4096 * sizeof(wchar_t)) {
        std::wstring final(urlSz / sizeof(wchar_t), L'\0');
        if (WinHttpQueryOption(req, WINHTTP_OPTION_URL, final.data(), &urlSz)) {
            while (!final.empty() && final.back() == L'\0') final.pop_back();
            std::wstring why;
            if (!CrackHttps(final, parts, why) || (exe && !ExeUrl(final, why))) {
                err = exe ? L"The download was redirected away from media.luciuswayne.com."
                          : L"The update list was redirected away from luciuswayne.com.";
                close();
                return false;
            }
        }
    }
    if (out.status == 404 && !exe) {
        err = L"No update list is published at /synchronus/latest.json yet.";
        close();
        return false;
    }
    if (out.status != 200) {
        err = Format(L"The update server returned status %lu.", out.status);
        close();
        return false;
    }

    uint64_t announced = 0;
    wchar_t lenBuf[64] = {};
    DWORD lenSz = sizeof(lenBuf);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenBuf, &lenSz,
                            WINHTTP_NO_HEADER_INDEX)) {
        announced = _wcstoui64(lenBuf, nullptr, 10);
    }
    if (announced > maxBytes || (expectedSize && announced && announced != expectedSize)) {
        err = L"The download size does not match the update list.";
        close();
        return false;
    }
    const uint64_t total = announced ? announced : expectedSize;

    HANDLE file = INVALID_HANDLE_VALUE;
    if (!savePath.empty()) {
        file = CreateFileW(savePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            err = L"Could not store the download.";
            close();
            return false;
        }
    }
    Sha256 sha;
    uint64_t got = 0;
    bool failed = false;
    for (;;) {
        if (stop.load()) {
            err = L"Stopped.";
            failed = true;
            break;
        }
        BYTE buf[64 * 1024];
        DWORD read = 0;
        if (!WinHttpReadData(req, buf, sizeof(buf), &read)) {
            err = L"The download stopped early.";
            failed = true;
            break;
        }
        if (read == 0) break;
        if (got + read > maxBytes) {
            err = L"The download is larger than expected.";
            failed = true;
            break;
        }
        got += read;
        if (file != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            if (!WriteFile(file, buf, read, &wrote, nullptr) || wrote != read) {
                err = L"Could not store the download.";
                failed = true;
                break;
            }
            sha.Add(buf, read);
        } else {
            out.body.append(reinterpret_cast<const char*>(buf), read);
        }
        if (progress) progress(got, total);
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    close();
    if (failed) {
        if (!savePath.empty()) DeleteFileW(savePath.c_str());
        return false;
    }
    if ((announced && got != announced) || (expectedSize && got != expectedSize)) {
        if (!savePath.empty()) DeleteFileW(savePath.c_str());
        err = L"The download size does not match the update list.";
        return false;
    }
    out.bytes = got;
    if (!savePath.empty()) {
        out.sha256 = sha.Hex();
        if (out.sha256.empty()) {
            DeleteFileW(savePath.c_str());
            err = L"Could not verify the download.";
            return false;
        }
    }
    return true;
}

bool IsX64Exe(const std::wstring& path, std::wstring& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = L"The download could not be read.";
        return false;
    }
    BYTE hdr[4096];
    DWORD n = 0;
    const BOOL read = ReadFile(h, hdr, sizeof(hdr), &n, nullptr);
    if (!read || n < 0x40 || hdr[0] != 'M' || hdr[1] != 'Z') {
        CloseHandle(h);
        err = L"The download is not a Windows executable.";
        return false;
    }
    DWORD e = 0;
    memcpy(&e, hdr + 0x3C, sizeof(e));
    BYTE sig[6] = {};
    bool pe = false;
    if (e <= n - 6) {
        pe = hdr[e] == 'P' && hdr[e + 1] == 'E' && hdr[e + 2] == 0 && hdr[e + 3] == 0;
        sig[4] = hdr[e + 4];
        sig[5] = hdr[e + 5];
    } else {
        LARGE_INTEGER dist{};
        dist.QuadPart = e;
        if (SetFilePointerEx(h, dist, nullptr, FILE_BEGIN)) {
            DWORD got = 0;
            ReadFile(h, sig, sizeof(sig), &got, nullptr);
            pe = got == sizeof(sig) && sig[0] == 'P' && sig[1] == 'E' && sig[2] == 0 && sig[3] == 0;
        }
    }
    CloseHandle(h);
    if (!pe) {
        err = L"The download is not a Windows executable.";
        return false;
    }
    const WORD machine = static_cast<WORD>(sig[4] | (sig[5] << 8));
    if (machine != 0x8664) {
        err = L"The download is not the 64-bit Synchronus executable.";
        return false;
    }
    return true;
}

bool FileVersion(const std::wstring& path, unsigned out[4], std::wstring& err) {
    DWORD handle = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!bytes) {
        err = L"The download has no version information.";
        return false;
    }
    std::vector<BYTE> block(bytes);
    if (!GetFileVersionInfoW(path.c_str(), 0, bytes, block.data())) {
        err = L"The download's version could not be read.";
        return false;
    }
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(block.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info ||
        len < sizeof(VS_FIXEDFILEINFO)) {
        err = L"The download's version could not be read.";
        return false;
    }
    out[0] = HIWORD(info->dwFileVersionMS);
    out[1] = LOWORD(info->dwFileVersionMS);
    out[2] = HIWORD(info->dwFileVersionLS);
    out[3] = LOWORD(info->dwFileVersionLS);
    return true;
}

}  // namespace

bool CheckAppUpdate(const std::wstring& currentVersion, AppUpdate& offer, const std::atomic<bool>& stop, std::wstring& err) {
    offer = {};
    err.clear();
    unsigned current[4] = {};
    if (!ParseVersion(currentVersion, current)) {
        err = L"This copy's version could not be read.";
        return false;
    }
    // The resource on the exe is what a published build claims to be. Prefer it
    // when it is ahead of the display string, so a replaced copy does not ask
    // for the same update again.
    wchar_t selfPath[MAX_PATH * 2] = {};
    const DWORD selfLen = GetModuleFileNameW(nullptr, selfPath, _countof(selfPath));
    unsigned selfVer[4] = {};
    std::wstring ignored;
    if (selfLen > 0 && selfLen < _countof(selfPath) && FileVersion(selfPath, selfVer, ignored) &&
        CmpVersion(selfVer, current) > 0) {
        current[0] = selfVer[0];
        current[1] = selfVer[1];
        current[2] = selfVer[2];
        current[3] = selfVer[3];
    }
    const std::wstring agent = L"Synchronus/" + currentVersion;
    const std::wstring url = std::wstring(kUpdateManifestUrl) + L"?current=" + currentVersion;
    HttpOut res;
    if (!HttpGet(url, agent, false, kMaxManifest, 0, L"", {}, stop, res, err)) return false;
    if (res.body.size() > kMaxManifest) {
        err = L"The update list is too large.";
        return false;
    }
    Manifest m;
    if (!ParseManifest(res.body, m, err)) return false;
    const std::wstring version = Utf8ToWide(m.version);
    unsigned remote[4] = {};
    if (!ParseVersion(version, remote)) {
        err = L"The update list's version is not a number like 1.12.1.";
        return false;
    }
    if (CmpVersion(remote, current) <= 0) return false;
    const std::wstring fileUrl = Utf8ToWide(m.url);
    if (!ExeUrl(fileUrl, err)) return false;
    const std::wstring hash = NormalizeHash(m.sha256);
    if (!IsSha256(hash)) {
        err = L"The update list's sha256 is not 64 hex digits.";
        return false;
    }
    if (m.hasSize && (m.size == 0 || m.size > kMaxExe)) {
        err = L"The update list's size is not a Synchronus executable.";
        return false;
    }
    std::wstring notes = Utf8ToWide(m.notes);
    if (notes.size() > 2000) notes.resize(2000);
    offer.version = version;
    offer.notes = std::move(notes);
    offer.url = fileUrl;
    offer.sha256 = hash;
    offer.size = m.hasSize ? m.size : 0;
    return true;
}

bool DownloadAppUpdate(const AppUpdate& offer, const std::wstring& destPath,
                       const std::function<void(uint64_t got, uint64_t total)>& progress, const std::atomic<bool>& stop,
                       std::wstring& err) {
    err.clear();
    unsigned want[4] = {};
    if (!ParseVersion(offer.version, want) || !ExeUrl(offer.url, err) || !IsSha256(offer.sha256)) {
        err = L"The update list is incomplete.";
        return false;
    }
    if (offer.size > kMaxExe) {
        err = L"The update list's size is not a Synchronus executable.";
        return false;
    }
    HttpOut res;
    const std::wstring agent = L"Synchronus update";
    if (!HttpGet(offer.url, agent, true, kMaxExe, offer.size, destPath, progress, stop, res, err)) return false;
    if (_wcsicmp(res.sha256.c_str(), offer.sha256.c_str()) != 0) {
        DeleteFileW(destPath.c_str());
        err = L"The download did not match the published checksum.";
        return false;
    }
    if (!IsX64Exe(destPath, err)) {
        DeleteFileW(destPath.c_str());
        return false;
    }
    unsigned got[4] = {};
    if (!FileVersion(destPath, got, err)) {
        DeleteFileW(destPath.c_str());
        return false;
    }
    if (CmpVersion(got, want) != 0) {
        DeleteFileW(destPath.c_str());
        err = Format(L"The download says it is %s, not %s.", VersionText(got).c_str(), offer.version.c_str());
        return false;
    }
    return true;
}

bool ReportAppDownload(const std::wstring& version) {
    if (version.empty() || version.size() > 32) return false;
    std::string body = "{\"source\":\"app\",\"version\":\"";
    for (wchar_t c : version) {
        if (!((c >= L'0' && c <= L'9') || c == L'.')) return false;
        body.push_back(static_cast<char>(c));
    }
    body += "\"}";

    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = _countof(path);
    if (!WinHttpCrackUrl(kDownloadCountUrl, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
    if (_wcsicmp(host, L"luciuswayne.com") != 0) return false;
    const INTERNET_PORT port = uc.nPort ? uc.nPort : INTERNET_DEFAULT_HTTPS_PORT;
    if (port != INTERNET_DEFAULT_HTTPS_PORT) return false;

    HINTERNET ses = WinHttpOpen(L"Synchronus update", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, 3000, 3000, 4000, 4000);
    HINTERNET con = WinHttpConnect(ses, host, port, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE)
                        : nullptr;
    auto close = [&] {
        if (req) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
    };
    if (!req) {
        close();
        return false;
    }
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    const wchar_t* headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    const BOOL sent = WinHttpSendRequest(req, headers, static_cast<DWORD>(-1), const_cast<char*>(body.data()),
                                         static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!sent || !WinHttpReceiveResponse(req, nullptr)) {
        close();
        return false;
    }
    DWORD status = 0;
    DWORD sz = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                        &sz, WINHTTP_NO_HEADER_INDEX);
    close();
    return status >= 200 && status < 300;
}

bool ReplaceExecutable(const std::wstring& targetPath, const std::wstring& sourcePath, std::wstring& err) {
    const std::wstring backup = targetPath + L".old";
    DeleteFileW(backup.c_str());
    const DWORD attr = GetFileAttributesW(targetPath.c_str());
    const bool exists = attr != INVALID_FILE_ATTRIBUTES;
    if (exists) {
        SetFileAttributesW(targetPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!MoveFileExW(targetPath.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            err = Format(L"Could not move the current executable aside (error %lu).", GetLastError());
            return false;
        }
    }
    if (!CopyFileW(sourcePath.c_str(), targetPath.c_str(), FALSE)) {
        const DWORD rc = GetLastError();
        if (exists) MoveFileExW(backup.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        err = Format(L"Could not replace %s (error %lu).", targetPath.c_str(), rc);
        return false;
    }
    SetFileAttributesW(targetPath.c_str(), FILE_ATTRIBUTE_NORMAL);
    return true;
}

}  // namespace gui
