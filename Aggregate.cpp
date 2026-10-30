#include "Aggregate.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string>

namespace {

std::mutex g_aggMu;

bool Finite(double v) { return std::isfinite(v); }

double MeanOf(double sum, double weight) { return weight > 0 ? sum / weight : 0; }

std::wstring MasterPath(const std::wstring& dir) { return dir + L"\\aggregate-master.csv"; }
std::wstring HistoryPath(const std::wstring& dir) { return dir + L"\\tune-history.csv"; }

void EnsureDir(const std::wstring& dir) { CreateDirectoryW(dir.c_str(), nullptr); }

std::string ReadWhole(const std::wstring& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    const DWORD size = GetFileSize(file, nullptr);
    std::string text(size ? size : 1, '\0');
    DWORD got = 0;
    ::ReadFile(file, text.data(), size, &got, nullptr);
    CloseHandle(file);
    text.resize(got);
    return text;
}

bool WriteFileText(const std::wstring& path, const std::string& text) {
    const std::wstring tmp = path + L".tmp";
    const HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
    CloseHandle(file);
    if (!ok || wrote != text.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::vector<std::string> Split(const std::string& line, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

std::vector<std::string> LinesOf(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            if (!cur.empty()) lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty() && cur.back() == '\r') cur.pop_back();
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

double At(const std::vector<std::string>& f, size_t i) {
    if (i >= f.size() || f[i].empty()) return 0;
    return std::strtod(f[i].c_str(), nullptr);
}

struct Acc {
    unsigned requested = 0;
    unsigned runs = 0;
    double loopW = 0, idleW = 0, scoreW = 0;
    double loopMedian = 0, loopMean = 0, loopStdev = 0, loopLate = 0, loopScore = 0;
    double idleMedian = 0, idleMean = 0, idleStdev = 0, idleLate = 0, idleScore = 0;
    double score = 0;
};

std::map<unsigned, Acc> ReadMaster(const std::wstring& path) {
    std::map<unsigned, Acc> rows;
    const auto lines = LinesOf(ReadWhole(path));
    for (const auto& line : lines) {
        if (line.empty() || line[0] == 'r') continue;
        const auto f = Split(line, ',');
        if (f.size() < 16) continue;
        Acc a;
        a.requested = static_cast<unsigned>(std::strtoul(f[0].c_str(), nullptr, 10));
        if (!a.requested) continue;
        a.runs = static_cast<unsigned>(std::strtoul(f[1].c_str(), nullptr, 10));
        a.loopW = At(f, 2);
        a.loopMedian = At(f, 3);
        a.loopMean = At(f, 4);
        a.loopStdev = At(f, 5);
        a.loopLate = At(f, 6);
        a.loopScore = At(f, 7);
        a.idleW = At(f, 8);
        a.idleMedian = At(f, 9);
        a.idleMean = At(f, 10);
        a.idleStdev = At(f, 11);
        a.idleLate = At(f, 12);
        a.idleScore = At(f, 13);
        a.scoreW = At(f, 14);
        a.score = At(f, 15);
        rows[a.requested] = a;
    }
    return rows;
}

std::string FormatNum(double v) {
    char buf[64];
    sprintf_s(buf, "%.17g", v);
    return buf;
}

bool WriteMaster(const std::wstring& path, const std::map<unsigned, Acc>& rows) {
    std::string text =
        "requested,runs,loop_w,loop_median_s,loop_mean_s,loop_stdev_s,loop_late_s,loop_score_s,"
        "idle_w,idle_median_s,idle_mean_s,idle_stdev_s,idle_late_s,idle_score_s,score_w,score_s\n";
    for (const auto& kv : rows) {
        const Acc& a = kv.second;
        text += std::to_string(a.requested) + "," + std::to_string(a.runs) + "," + FormatNum(a.loopW) + "," +
                FormatNum(a.loopMedian) + "," + FormatNum(a.loopMean) + "," + FormatNum(a.loopStdev) + "," +
                FormatNum(a.loopLate) + "," + FormatNum(a.loopScore) + "," + FormatNum(a.idleW) + "," +
                FormatNum(a.idleMedian) + "," + FormatNum(a.idleMean) + "," + FormatNum(a.idleStdev) + "," +
                FormatNum(a.idleLate) + "," + FormatNum(a.idleScore) + "," + FormatNum(a.scoreW) + "," +
                FormatNum(a.score) + "\n";
    }
    return WriteFileText(path, text);
}

void FoldMetric(double& sum, double runValue, double runWeight) {
    if (!(runWeight > 0) || !Finite(runValue)) return;
    sum += runValue * runWeight;
}

void FoldRun(Acc& master, const Acc& run) {
    if (run.loopW > 0) {
        FoldMetric(master.loopMedian, MeanOf(run.loopMedian, run.loopW), run.loopW);
        FoldMetric(master.loopMean, MeanOf(run.loopMean, run.loopW), run.loopW);
        FoldMetric(master.loopStdev, MeanOf(run.loopStdev, run.loopW), run.loopW);
        FoldMetric(master.loopLate, MeanOf(run.loopLate, run.loopW), run.loopW);
        FoldMetric(master.loopScore, MeanOf(run.loopScore, run.loopW), run.loopW);
        master.loopW += run.loopW;
    }
    if (run.idleW > 0) {
        FoldMetric(master.idleMedian, MeanOf(run.idleMedian, run.idleW), run.idleW);
        FoldMetric(master.idleMean, MeanOf(run.idleMean, run.idleW), run.idleW);
        FoldMetric(master.idleStdev, MeanOf(run.idleStdev, run.idleW), run.idleW);
        FoldMetric(master.idleLate, MeanOf(run.idleLate, run.idleW), run.idleW);
        FoldMetric(master.idleScore, MeanOf(run.idleScore, run.idleW), run.idleW);
        master.idleW += run.idleW;
    }
    if (run.scoreW > 0) {
        FoldMetric(master.score, MeanOf(run.score, run.scoreW), run.scoreW);
        master.scoreW += run.scoreW;
    }
    if (run.loopW > 0 || run.idleW > 0) master.runs += 1;
}

Acc CombineOne(const std::vector<TimingSample>& samples, unsigned requested) {
    Acc run;
    run.requested = requested;
    for (const auto& s : samples) {
        if (s.requested != requested) continue;
        if (s.loopN > 0) {
            const double n = s.loopN;
            run.loopMedian += s.loopMedian * n;
            run.loopMean += s.loopMean * n;
            run.loopStdev += s.loopStdev * n;
            run.loopLate += s.loopLate * n;
            run.loopScore += s.loopScore * n;
            run.loopW += n;
        }
        if (s.idleN > 0) {
            const double n = s.idleN;
            run.idleMedian += s.idleMedian * n;
            run.idleMean += s.idleMean * n;
            run.idleStdev += s.idleStdev * n;
            run.idleLate += s.idleLate * n;
            run.idleScore += s.idleScore * n;
            run.idleW += n;
        }
        const double sn = static_cast<double>(s.loopN) + static_cast<double>(s.idleN);
        if (sn > 0 && Finite(s.score)) {
            run.score += s.score * sn;
            run.scoreW += sn;
        }
    }
    return run;
}

std::wstring Stamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[40];
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring Sanitize(std::wstring text) {
    for (wchar_t& c : text) {
        if (c == L',' || c == L'\r' || c == L'\n') c = L' ';
    }
    if (text.size() > 240) text.resize(240);
    return text;
}

void AppendHistory(const std::wstring& path, const std::wstring& when, const std::wstring& kind, const std::wstring& detail) {
    std::string prior = ReadWhole(path);
    prior += WideToUtf8(when + L"," + Sanitize(kind) + L"," + Sanitize(detail) + L"\n");
    const auto lines = LinesOf(prior);
    const size_t keep = 500;
    std::string text;
    const size_t begin = lines.size() > keep ? lines.size() - keep : 0;
    for (size_t i = begin; i < lines.size(); ++i) text += lines[i] + "\n";
    WriteFileText(path, text);
}

bool ParseSweepLine(const std::string& line, TimingSample& s) {
    if (line.rfind("sweep,", 0) != 0 && line.rfind("verify,", 0) != 0) return false;
    const auto f = Split(line, ',');
    if (f.size() < 27) return false;
    s = {};
    s.requested = static_cast<unsigned>(std::strtoul(f[1].c_str(), nullptr, 10));
    s.loopN = static_cast<unsigned>(std::strtoul(f[4].c_str(), nullptr, 10));
    s.loopMedian = At(f, 5);
    s.loopMean = At(f, 6);
    s.loopStdev = At(f, 7);
    s.loopLate = At(f, 12);
    s.loopScore = At(f, 13);
    s.idleN = static_cast<unsigned>(std::strtoul(f[15].c_str(), nullptr, 10));
    s.idleMedian = At(f, 16);
    s.idleMean = At(f, 17);
    s.idleStdev = At(f, 18);
    s.idleLate = At(f, 23);
    s.idleScore = At(f, 24);
    s.score = At(f, 26);
    return s.requested != 0 && (s.loopN > 1 || s.idleN > 1);
}

void ImportBenchmarks(const std::wstring& dir, std::map<unsigned, Acc>& rows) {
    WIN32_FIND_DATAW fd{};
    const HANDLE find = FindFirstFileW((dir + L"\\benchmark-*.csv").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> names;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        names.push_back(fd.cFileName);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    std::sort(names.begin(), names.end());
    unsigned imported = 0;
    for (const auto& name : names) {
        const auto lines = LinesOf(ReadWhole(dir + L"\\" + name));
        std::vector<TimingSample> samples;
        for (const auto& line : lines) {
            TimingSample s;
            if (ParseSweepLine(line, s)) samples.push_back(s);
        }
        if (samples.empty()) continue;
        std::map<unsigned, int> seen;
        for (const auto& s : samples) seen[s.requested] = 1;
        for (const auto& kv : seen) {
            Acc& master = rows[kv.first];
            master.requested = kv.first;
            FoldRun(master, CombineOne(samples, kv.first));
        }
        ++imported;
    }
    if (imported) {
        AppendHistory(HistoryPath(dir), Stamp(), L"History",
                      std::to_wstring(imported) + L" earlier timer scans folded into the aggregate");
    }
}

AggregatePoint ToPoint(const Acc& a) {
    AggregatePoint p;
    p.requested = a.requested;
    p.runs = a.runs;
    p.loopWeight = a.loopW;
    p.idleWeight = a.idleW;
    p.scoreWeight = a.scoreW;
    p.loopMedian = MeanOf(a.loopMedian, a.loopW);
    p.loopMean = MeanOf(a.loopMean, a.loopW);
    p.loopStdev = MeanOf(a.loopStdev, a.loopW);
    p.loopLate = MeanOf(a.loopLate, a.loopW);
    p.loopScore = MeanOf(a.loopScore, a.loopW);
    p.idleMedian = MeanOf(a.idleMedian, a.idleW);
    p.idleMean = MeanOf(a.idleMean, a.idleW);
    p.idleStdev = MeanOf(a.idleStdev, a.idleW);
    p.idleLate = MeanOf(a.idleLate, a.idleW);
    p.idleScore = MeanOf(a.idleScore, a.idleW);
    p.score = MeanOf(a.score, a.scoreW);
    return p;
}

void LoadLatest(const std::wstring& dir, std::vector<LatestPoint>& latest) {
    WIN32_FIND_DATAW fd{};
    const HANDLE find = FindFirstFileW((dir + L"\\benchmark-*.csv").c_str(), &fd);
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
    for (const auto& line : LinesOf(ReadWhole(dir + L"\\" + best))) {
        TimingSample s;
        if (!ParseSweepLine(line, s) || line.rfind("sweep,", 0) != 0) continue;
        latest.push_back({s.requested, s.score});
    }
}

}  // namespace

void MergeTimingRun(const std::wstring& dataDir, const std::vector<TimingSample>& samples) {
    if (dataDir.empty() || samples.empty()) return;
    std::lock_guard<std::mutex> lock(g_aggMu);
    EnsureDir(dataDir);
    const std::wstring path = MasterPath(dataDir);
    const bool existed = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    auto rows = ReadMaster(path);
    if (!existed) ImportBenchmarks(dataDir, rows);
    std::map<unsigned, int> seen;
    for (const auto& s : samples) {
        if (s.requested) seen[s.requested] = 1;
    }
    for (const auto& kv : seen) {
        Acc& master = rows[kv.first];
        master.requested = kv.first;
        FoldRun(master, CombineOne(samples, kv.first));
    }
    WriteMaster(path, rows);
}

void NoteTuneEvent(const std::wstring& dataDir, const std::wstring& kind, const std::wstring& detail) {
    if (dataDir.empty() || kind.empty()) return;
    std::lock_guard<std::mutex> lock(g_aggMu);
    EnsureDir(dataDir);
    AppendHistory(HistoryPath(dataDir), Stamp(), kind, detail);
}

AggregateView LoadAggregate(const std::wstring& dataDir) {
    AggregateView view;
    if (dataDir.empty()) return view;
    std::lock_guard<std::mutex> lock(g_aggMu);
    const std::wstring path = MasterPath(dataDir);
    auto rows = ReadMaster(path);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ImportBenchmarks(dataDir, rows);
        if (!rows.empty()) WriteMaster(path, rows);
    }
    for (const auto& kv : rows) {
        const AggregatePoint p = ToPoint(kv.second);
        view.points.push_back(p);
        view.runs = std::max(view.runs, p.runs);
        view.samples += p.scoreWeight;
    }
    const auto lines = LinesOf(ReadWhole(HistoryPath(dataDir)));
    for (auto it = lines.rbegin(); it != lines.rend() && view.events.size() < 80; ++it) {
        const auto f = Split(*it, ',');
        if (f.size() < 3) continue;
        TuneEvent e;
        e.when = Utf8ToWide(f[0]);
        e.kind = Utf8ToWide(f[1]);
        e.detail = Utf8ToWide(f[2]);
        view.events.push_back(std::move(e));
    }
    LoadLatest(dataDir, view.latest);
    return view;
}

int SelfTestAggregate() {
    wchar_t tmp[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (!n || n >= MAX_PATH) return 1;
    const std::wstring dir = std::wstring(tmp) + L"SynchronusAggTest";
    CreateDirectoryW(dir.c_str(), nullptr);
    DeleteFileW(MasterPath(dir).c_str());
    DeleteFileW(HistoryPath(dir).c_str());
    auto sample = [](unsigned units, unsigned count, double median, double score) {
        TimingSample s;
        s.requested = units;
        s.loopN = count;
        s.loopMedian = median;
        s.loopMean = median;
        s.score = score;
        return s;
    };
    MergeTimingRun(dir, {sample(5600, 10, 1.0, 1.0)});
    MergeTimingRun(dir, {sample(5600, 10, 3.0, 3.0)});
    {
        const AggregateView mid = LoadAggregate(dir);
        if (mid.points.size() != 1 || mid.points[0].runs != 2) return 2;
        if (std::fabs(mid.points[0].loopMedian - 2.0) > 1e-9) return 3;
        if (std::fabs(mid.points[0].loopWeight - 20.0) > 1e-6) return 4;
    }
    MergeTimingRun(dir, {sample(5600, 10, 5.0, 5.0)});
    {
        const AggregateView next = LoadAggregate(dir);
        if (std::fabs(next.points[0].loopMedian - 3.0) > 1e-9) return 5;
        if (std::fabs(next.points[0].score - 3.0) > 1e-9) return 6;
        if (std::fabs(next.points[0].scoreWeight - 30.0) > 1e-6) return 7;
    }
    MergeTimingRun(dir, {sample(5600, 10000, 1.0, 1.0)});
    MergeTimingRun(dir, {sample(5600, 10, 11.0, 11.0)});
    {
        const AggregateView heavy = LoadAggregate(dir);
        const double expect = (30.0 * 3.0 + 10000.0 * 1.0 + 10.0 * 11.0) / 10040.0;
        if (std::fabs(heavy.points[0].loopMedian - expect) > 1e-6) return 8;
        if (!(heavy.points[0].loopWeight > 10000.0)) return 9;
    }
    NoteTuneEvent(dir, L"CPU", L"power applied");
    const AggregateView noted = LoadAggregate(dir);
    if (noted.events.empty() || noted.events[0].kind != L"CPU") return 10;
    DeleteFileW(MasterPath(dir).c_str());
    DeleteFileW(HistoryPath(dir).c_str());
    RemoveDirectoryW(dir.c_str());
    return 0;
}
