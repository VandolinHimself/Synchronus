// Util.h - small helpers shared by the modules added in 3.0.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "Extras.h"

#include <algorithm>
#include <cstdarg>
#include <cwctype>
#include <string>
#include <vector>

namespace gui::util {

inline constexpr wchar_t kAppKey[] = L"SOFTWARE\\Synchronus";            // HKLM
inline constexpr wchar_t kUserKey[] = L"Software\\Synchronus\\Monitor";  // HKCU

inline std::wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t buffer[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    return buffer;
}

inline std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

inline bool IEquals(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

inline bool IContains(const std::wstring& hay, const wchar_t* needle) {
    return Lower(hay).find(Lower(needle)) != std::wstring::npos;
}

inline std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(0, n)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

inline std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(0, n)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

inline bool GetDword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD& value) {
    DWORD data = 0, size = sizeof(data);
    if (RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &data, &size) != ERROR_SUCCESS) return false;
    value = data;
    return true;
}

inline bool SetDword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD value) {
    HKEY handle = nullptr;
    if (RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS) return false;
    const LONG rc = RegSetValueExW(handle, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(handle);
    return rc == ERROR_SUCCESS;
}

inline bool GetSz(HKEY root, const wchar_t* key, const wchar_t* name, std::wstring& value) {
    DWORD size = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t))
        return false;
    std::wstring buf(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS) return false;
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    value = std::move(buf);
    return true;
}

inline bool SetSz(HKEY root, const wchar_t* key, const wchar_t* name, const std::wstring& value) {
    HKEY handle = nullptr;
    if (RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS) return false;
    const LONG rc = RegSetValueExW(handle, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(handle);
    return rc == ERROR_SUCCESS;
}

inline bool DeleteValue(HKEY root, const wchar_t* key, const wchar_t* name) {
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key, 0, KEY_SET_VALUE, &handle) != ERROR_SUCCESS) return false;
    const LONG rc = RegDeleteValueW(handle, name);
    RegCloseKey(handle);
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

inline bool ValueExists(HKEY root, const wchar_t* key, const wchar_t* name) {
    return RegGetValueW(root, key, name, RRF_RT_ANY, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

inline bool KeyExists(HKEY root, const wchar_t* key) {
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE, &handle) != ERROR_SUCCESS) return false;
    RegCloseKey(handle);
    return true;
}

// REG_MULTI_SZ as a list. Missing is an empty list.
inline std::vector<std::wstring> GetMulti(HKEY root, const wchar_t* key, const wchar_t* name) {
    std::vector<std::wstring> out;
    DWORD size = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t))
        return out;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, L'\0');
    if (RegGetValueW(root, key, name, RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS) return out;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) out.emplace_back(p);
    return out;
}

inline bool SetMulti(HKEY root, const wchar_t* key, const wchar_t* name, const std::vector<std::wstring>& list) {
    std::vector<wchar_t> buf;
    for (const auto& item : list) {
        if (item.empty()) continue;
        buf.insert(buf.end(), item.begin(), item.end());
        buf.push_back(L'\0');
    }
    buf.push_back(L'\0');
    HKEY handle = nullptr;
    if (RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS) return false;
    const LONG rc = RegSetValueExW(handle, name, 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(buf.data()),
                                   static_cast<DWORD>(buf.size() * sizeof(wchar_t)));
    RegCloseKey(handle);
    return rc == ERROR_SUCCESS;
}

inline unsigned long long NowFileTime() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

inline std::wstring LocalStamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return Fmt(L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

// The list a step shows while it works: the same rows the older steps publish.
class Steps {
public:
    explicit Steps(const StepIo& io) : io_(io) {}

    size_t Add(const std::wstring& alias, const std::wstring& title, const std::wstring& why, const std::wstring& change) {
        PpmStepView row;
        row.alias = alias;
        row.title = title;
        row.why = why;
        row.change = change;
        rows_.push_back(std::move(row));
        return rows_.size() - 1;
    }

    void Publish() const {
        if (!io_.feed) return;
        io_.feed->Ppm(rows_);
        if (rows_.empty()) return;
        int done = 0;
        bool active = false;
        for (const auto& row : rows_) {
            if (row.status == 2 || row.status == 3) ++done;
            else if (row.status == 1) active = true;
        }
        double frac = static_cast<double>(done) / static_cast<double>(rows_.size());
        if (active) frac = (static_cast<double>(done) + 0.35) / static_cast<double>(rows_.size());
        io_.feed->Progress(frac, 0, 0);
    }

    // Marks one row active, explains it, and holds it on screen before the write.
    void Begin(size_t i, const wchar_t* phase, DWORD dwellMs) {
        rows_[i].status = 1;
        Publish();
        if (io_.feed) {
            io_.feed->Narrate(rows_[i].title, rows_[i].why);
            io_.feed->Phase(phase, rows_[i].title + (rows_[i].change.empty() ? L"" : L"  " + rows_[i].change));
        }
        Out(L"");
        Out(L"  " + rows_[i].title);
        if (!rows_[i].why.empty()) Out(L"    " + rows_[i].why);
        if (!rows_[i].change.empty()) Out(L"    " + rows_[i].change);
        Dwell(dwellMs);
    }

    void End(size_t i, bool ok, const std::wstring& change = {}) {
        rows_[i].status = ok ? 2 : 3;
        if (!change.empty()) rows_[i].change = change;
        Publish();
        Dwell(350);
    }

    bool Cancelled() const { return io_.feed && io_.feed->Cancelled(); }

    void Dwell(DWORD ms) const {
        if (!ms || !io_.dwell) return;
        const ULONGLONG until = GetTickCount64() + ms;
        while (GetTickCount64() < until) {
            if (Cancelled()) return;
            Sleep(40);
        }
    }

    void Out(const std::wstring& line) const {
        if (io_.out) io_.out(line);
        else if (io_.feed) io_.feed->Log(line);
    }

    void Err(const std::wstring& line) const {
        if (io_.err) io_.err(line);
        else if (io_.feed) io_.feed->Log(L"ERROR: " + line);
    }

    void Narrate(const std::wstring& title, const std::wstring& body) const {
        if (io_.feed) io_.feed->Narrate(title, body);
    }

    void Phase(const std::wstring& title, const std::wstring& detail) const {
        if (io_.feed) io_.feed->Phase(title, detail);
    }

    size_t size() const { return rows_.size(); }
    PpmStepView& operator[](size_t i) { return rows_[i]; }

private:
    const StepIo& io_;
    std::vector<PpmStepView> rows_;
};

// A health row in the shape the older checks use.
inline HealthCheck MakeCheck(const std::wstring& id, const wchar_t* category, const std::wstring& name) {
    HealthCheck c;
    c.id = id;
    c.category = category;
    c.name = name;
    c.checkedAt = NowFileTime();
    return c;
}

}  // namespace gui::util
