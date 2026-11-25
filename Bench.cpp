// Bench.cpp - the Performance page: live readings and a short benchmark.
//
// The benchmark changes nothing. It times Sleep(1) at whatever timer
// resolution is in force, how long a sleeping thread takes to start running,
// how much processor time goes to deferred procedure calls and interrupts,
// and plain CPU, memory, and disk throughput. Results are indicative: they
// are for comparing this PC with itself before and after a tune.
#include "Util.h"

#include <pdh.h>
#include <pdhmsg.h>
#include <powrprof.h>

#include <cmath>
#include <thread>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "powrprof.lib")

extern "C" NTSYSAPI LONG NTAPI NtQueryTimerResolution(PULONG MinimumResolution, PULONG MaximumResolution, PULONG CurrentResolution);
extern "C" NTSYSAPI LONG NTAPI NtQuerySystemInformation(ULONG SystemInformationClass, PVOID SystemInformation,
                                                        ULONG SystemInformationLength, PULONG ReturnLength);

namespace gui {
namespace {

using namespace util;

struct CpuTimes {
    LARGE_INTEGER idle, kernel, user, dpc, interrupt;
    ULONG interruptCount;
};

struct PowerInfo {
    ULONG number, maxMhz, currentMhz, mhzLimit, maxIdleState, currentIdleState;
};

struct Totals {
    double idle = 0, kernel = 0, user = 0, dpc = 0, isr = 0;
    bool ok = false;
};

Totals ReadTotals() {
    Totals t;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const ULONG n = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
    std::vector<CpuTimes> cpus(n);
    ULONG got = 0;
    // 8 is SystemProcessorPerformanceInformation. Kernel time includes idle, DPC, and interrupt time.
    if (NtQuerySystemInformation(8, cpus.data(), static_cast<ULONG>(cpus.size() * sizeof(CpuTimes)), &got) != 0) return t;
    const size_t have = got / sizeof(CpuTimes);
    for (size_t i = 0; i < have; ++i) {
        t.idle += static_cast<double>(cpus[i].idle.QuadPart);
        t.kernel += static_cast<double>(cpus[i].kernel.QuadPart);
        t.user += static_cast<double>(cpus[i].user.QuadPart);
        t.dpc += static_cast<double>(cpus[i].dpc.QuadPart);
        t.isr += static_cast<double>(cpus[i].interrupt.QuadPart);
    }
    t.ok = have > 0;
    return t;
}

// Shares of all processor time between two readings, in percent.
bool Shares(const Totals& a, const Totals& b, double& cpu, double& dpc, double& isr) {
    if (!a.ok || !b.ok) return false;
    const double total = (b.kernel - a.kernel) + (b.user - a.user);
    if (total <= 0) return false;
    cpu = std::clamp(100.0 * (total - (b.idle - a.idle)) / total, 0.0, 100.0);
    dpc = std::clamp(100.0 * (b.dpc - a.dpc) / total, 0.0, 100.0);
    isr = std::clamp(100.0 * (b.isr - a.isr) / total, 0.0, 100.0);
    return true;
}

double Seconds(const LARGE_INTEGER& a, const LARGE_INTEGER& b, const LARGE_INTEGER& freq) {
    return static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(freq.QuadPart);
}

double Pick(std::vector<double>& sorted, double q) {
    if (sorted.empty()) return 0;
    const size_t i = std::min(sorted.size() - 1, static_cast<size_t>(q * static_cast<double>(sorted.size() - 1) + 0.5));
    return sorted[i];
}

// ---- NVIDIA temperature through the driver's own library ----
struct NvThermal {
    unsigned version;
    unsigned count;
    struct {
        int controller, defaultMin, defaultMax, current, target;
    } sensor[3];
};

class NvTemp {
public:
    NvTemp() {
        mod_ = LoadLibraryW(L"nvapi64.dll");
        if (!mod_) return;
        using Query = void* (*)(unsigned);
        const auto q = reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(mod_, "nvapi_QueryInterface")));
        if (!q) return;
        const auto init = reinterpret_cast<int (*)()>(q(0x0150E828));
        const auto enumGpus = reinterpret_cast<int (*)(void**, unsigned*)>(q(0xE5AC921F));
        thermal_ = reinterpret_cast<int (*)(void*, unsigned, NvThermal*)>(q(0xE3640A56));
        if (!init || !enumGpus || !thermal_ || init() != 0) return;
        void* handles[64]{};
        unsigned count = 0;
        if (enumGpus(handles, &count) != 0 || count == 0) return;
        gpu_ = handles[0];
    }
    ~NvTemp() {
        if (mod_) FreeLibrary(mod_);
    }
    bool Read(double& celsius) const {
        if (!gpu_ || !thermal_) return false;
        NvThermal t{};
        t.version = static_cast<unsigned>(sizeof(NvThermal)) | (2u << 16);
        if (thermal_(gpu_, 15, &t) != 0 || t.count == 0) return false;
        celsius = static_cast<double>(t.sensor[0].current);
        return celsius > 0 && celsius < 150;
    }

private:
    HMODULE mod_ = nullptr;
    void* gpu_ = nullptr;
    int (*thermal_)(void*, unsigned, NvThermal*) = nullptr;
};

// One wildcard counter, read as an array of instances.
class PdhArray {
public:
    explicit PdhArray(const wchar_t* path) {
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
            query_ = nullptr;
            return;
        }
        if (PdhAddEnglishCounterW(query_, path, 0, &counter_) != ERROR_SUCCESS) {
            PdhCloseQuery(query_);
            query_ = nullptr;
            return;
        }
        PdhCollectQueryData(query_);
    }
    ~PdhArray() {
        if (query_) PdhCloseQuery(query_);
    }
    bool Read(std::vector<std::pair<std::wstring, double>>& out) {
        out.clear();
        if (!query_) return false;
        if (PdhCollectQueryData(query_) != ERROR_SUCCESS) return false;
        DWORD bytes = 0, count = 0;
        PDH_STATUS rc = PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
        if (rc != static_cast<PDH_STATUS>(PDH_MORE_DATA) || !bytes) return false;
        std::vector<BYTE> buf(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        rc = PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &bytes, &count, items);
        if (rc != ERROR_SUCCESS) return false;
        for (DWORD i = 0; i < count; ++i) {
            if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            out.emplace_back(items[i].szName ? items[i].szName : L"", items[i].FmtValue.doubleValue);
        }
        return true;
    }

private:
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER counter_ = nullptr;
};

volatile unsigned long long g_sink = 0;

// A fixed mix of integer and floating-point work. Returns steps done in the time given.
double Churn(double seconds, const std::atomic<bool>& stop) {
    LARGE_INTEGER freq{}, start{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    unsigned long long x = 0x9E3779B97F4A7C15ull, steps = 0;
    double f = 1.000001;
    for (;;) {
        for (int i = 0; i < 20000; ++i) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            f = f * 1.0000001 + static_cast<double>(x & 0xFF) * 1e-9;
            if (f > 1e6) f = 1.000001;
        }
        steps += 20000;
        QueryPerformanceCounter(&now);
        if (Seconds(start, now, freq) >= seconds || stop.load()) break;
    }
    g_sink = g_sink + x + static_cast<unsigned long long>(f);
    QueryPerformanceCounter(&now);
    const double took = Seconds(start, now, freq);
    return took > 0 ? static_cast<double>(steps) / took : 0;
}

std::wstring HistoryPath(const std::wstring& dir) { return dir + L"\\performance-history.csv"; }

std::wstring CsvSafe(std::wstring text) {
    for (auto& c : text)
        if (c == L',' || c == L'\r' || c == L'\n') c = L' ';
    return text;
}

bool ParseLine(const std::wstring& line, bool& baseline, BenchResult& r) {
    std::vector<std::wstring> f;
    size_t a = 0;
    while (a <= line.size()) {
        const size_t b = line.find(L',', a);
        f.push_back(line.substr(a, (b == std::wstring::npos ? line.size() : b) - a));
        if (b == std::wstring::npos) break;
        a = b + 1;
    }
    if (f.size() < 15 || (f[0] != L"baseline" && f[0] != L"run")) return false;
    baseline = f[0] == L"baseline";
    r.when = f[1];
    r.label = f[2];
    double* slots[] = {&r.sleepMedianMs, &r.sleepP99Ms, &r.wakeMedianUs, &r.wakeP99Us, &r.dpcPct,        &r.isrPct,
                       &r.cpuSingle,     &r.cpuAll,     &r.memGbps,      &r.diskWriteMbps, &r.diskReadMbps, &r.disk4kUs};
    for (size_t i = 0; i < _countof(slots); ++i) *slots[i] = _wtof(f[3 + i].c_str());
    r.ok = true;
    return true;
}

}  // namespace

// ------------------------------------------------------------- live metrics
struct LiveMeter::Impl {
    Totals last;
    NvTemp nv;
    PdhArray gpuLoad{L"\\GPU Engine(*engtype_3D)\\Utilization Percentage"};
    PdhArray zones{L"\\Thermal Zone Information(*)\\Temperature"};
};

LiveMeter::LiveMeter() : impl_(new Impl) { impl_->last = ReadTotals(); }

LiveMeter::~LiveMeter() { delete impl_; }

bool LiveMeter::Sample(LiveSample& out) {
    out = LiveSample{};
    const Totals now = ReadTotals();
    const bool shares = Shares(impl_->last, now, out.cpu, out.dpc, out.isr);
    impl_->last = now;

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    std::vector<PowerInfo> power(si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1);
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, power.data(),
                               static_cast<ULONG>(power.size() * sizeof(PowerInfo))) == 0) {
        for (const auto& p : power) {
            out.mhz = std::max(out.mhz, static_cast<double>(p.currentMhz));
            out.maxMhz = std::max(out.maxMhz, static_cast<double>(p.maxMhz));
        }
    }
    ULONG slow = 0, fast = 0, current = 0;
    if (NtQueryTimerResolution(&slow, &fast, &current) == 0) out.timerMs = current / 10000.0;
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) out.memory = static_cast<double>(mem.dwMemoryLoad);

    out.hasGpuTemp = impl_->nv.Read(out.gpuTemp);

    std::vector<std::pair<std::wstring, double>> items;
    if (impl_->gpuLoad.Read(items) && !items.empty()) {
        // Instances are per process and per engine. Task Manager shows the busiest engine, summed over processes.
        std::vector<std::pair<std::wstring, double>> engines;
        for (const auto& item : items) {
            const size_t from = item.first.find(L"luid_");
            const size_t to = item.first.find(L"_engtype");
            const std::wstring key = from != std::wstring::npos && to != std::wstring::npos && to > from
                                         ? item.first.substr(from, to - from)
                                         : item.first;
            bool merged = false;
            for (auto& e : engines) {
                if (e.first == key) {
                    e.second += item.second;
                    merged = true;
                }
            }
            if (!merged) engines.emplace_back(key, item.second);
        }
        for (const auto& e : engines) out.gpuLoad = std::max(out.gpuLoad, e.second);
        out.gpuLoad = std::clamp(out.gpuLoad, 0.0, 100.0);
        out.hasGpuLoad = true;
    }
    if (impl_->zones.Read(items)) {
        for (const auto& item : items) {
            const double c = item.second - 273.15;
            if (c > 1 && c < 150) {
                out.zoneTemp = std::max(out.zoneTemp, c);
                out.hasZone = true;
            }
        }
    }
    return shares;
}

// --------------------------------------------------------------- benchmarks
bool RunBench(BenchResult& out, const std::wstring& dataDir, const std::atomic<bool>& stop,
              const std::function<void(const wchar_t* phase, double fraction)>& progress) {
    out = BenchResult{};
    out.when = LocalStamp();
    auto say = [&](const wchar_t* phase, double fraction) {
        if (progress) progress(phase, fraction);
    };
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    const int oldPriority = GetThreadPriority(GetCurrentThread());

    // 1. Sleep(1) at the timer resolution in force. Nothing requests a resolution here.
    say(L"Sleep(1) wake-up", 0.02);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    {
        std::vector<double> over;
        for (int i = 0; i < 130 && !stop.load(); ++i) {
            LARGE_INTEGER a{}, b{};
            QueryPerformanceCounter(&a);
            Sleep(1);
            QueryPerformanceCounter(&b);
            if (i >= 10) over.push_back(Seconds(a, b, freq) * 1000.0 - 1.0);
        }
        std::sort(over.begin(), over.end());
        out.sleepMedianMs = Pick(over, 0.5);
        out.sleepP99Ms = Pick(over, 0.99);
    }
    if (stop.load()) {
        SetThreadPriority(GetCurrentThread(), oldPriority);
        return false;
    }

    // 2. A thread that has been asleep for a couple of milliseconds is signalled. The time until it runs
    //    includes waking its core from idle, which is what core parking and deep idle states add to.
    say(L"Thread wake-up", 0.18);
    {
        const HANDLE go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::atomic<bool> quit{false};
        LARGE_INTEGER woke{};
        std::thread sleeper([&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            for (;;) {
                WaitForSingleObject(go, INFINITE);
                QueryPerformanceCounter(&woke);
                if (quit.load()) break;
                SetEvent(done);
            }
        });
        std::vector<double> lat;
        for (int i = 0; i < 260 && !stop.load(); ++i) {
            Sleep(2);
            LARGE_INTEGER sent{};
            QueryPerformanceCounter(&sent);
            SetEvent(go);
            WaitForSingleObject(done, 1000);
            if (i >= 10) lat.push_back(Seconds(sent, woke, freq) * 1e6);
        }
        quit.store(true);
        SetEvent(go);
        sleeper.join();
        CloseHandle(go);
        CloseHandle(done);
        std::sort(lat.begin(), lat.end());
        out.wakeMedianUs = Pick(lat, 0.5);
        out.wakeP99Us = Pick(lat, 0.99);
    }
    SetThreadPriority(GetCurrentThread(), oldPriority);
    if (stop.load()) return false;

    // 3. Share of processor time spent in DPCs and interrupts while the machine does what it was doing.
    say(L"DPC and interrupt time", 0.36);
    {
        const Totals a = ReadTotals();
        for (int i = 0; i < 30 && !stop.load(); ++i) Sleep(50);
        const Totals b = ReadTotals();
        double cpu = 0;
        Shares(a, b, cpu, out.dpcPct, out.isrPct);
    }
    if (stop.load()) return false;

    // 4. Processor throughput, one core and then every core.
    say(L"Processor, one core", 0.50);
    out.cpuSingle = Churn(0.7, stop) / 1e6;
    if (stop.load()) return false;
    say(L"Processor, all cores", 0.60);
    {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const unsigned n = std::max<unsigned>(1, si.dwNumberOfProcessors);
        std::vector<double> each(n, 0.0);
        std::vector<std::thread> pool;
        for (unsigned i = 0; i < n; ++i) pool.emplace_back([&, i] { each[i] = Churn(0.7, stop); });
        for (auto& t : pool) t.join();
        for (double v : each) out.cpuAll += v / 1e6;
    }
    if (stop.load()) return false;

    // 5. Memory copy.
    say(L"Memory copy", 0.70);
    {
        const size_t size = 64u << 20;
        void* src = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        void* dst = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (src && dst) {
            memset(src, 0x5A, size);
            memset(dst, 0, size);
            LARGE_INTEGER a{}, b{};
            QueryPerformanceCounter(&a);
            int passes = 0;
            do {
                memcpy(dst, src, size);
                ++passes;
                QueryPerformanceCounter(&b);
            } while (Seconds(a, b, freq) < 0.5 && !stop.load());
            const double took = Seconds(a, b, freq);
            if (took > 0) out.memGbps = static_cast<double>(passes) * static_cast<double>(size) / took / 1e9;
            g_sink = g_sink + static_cast<unsigned char*>(dst)[size / 2];
        }
        if (src) VirtualFree(src, 0, MEM_RELEASE);
        if (dst) VirtualFree(dst, 0, MEM_RELEASE);
    }
    if (stop.load()) return false;

    // 6. Disk, on the drive that holds the Synchronus data folder. Unbuffered, so the file cache is not what is timed.
    say(L"Disk", 0.80);
    {
        const std::wstring path = dataDir + L"\\performance-test.tmp";
        const DWORD chunk = 1u << 20;
        const int chunks = 64;
        void* buf = VirtualAlloc(nullptr, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        CreateDirectoryW(dataDir.c_str(), nullptr);
        HANDLE file = buf ? CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr)
                          : INVALID_HANDLE_VALUE;
        if (file != INVALID_HANDLE_VALUE) {
            for (DWORD i = 0; i < chunk; ++i) static_cast<unsigned char*>(buf)[i] = static_cast<unsigned char>(i * 131u + (i >> 9));
            LARGE_INTEGER a{}, b{};
            bool ok = true;
            QueryPerformanceCounter(&a);
            for (int i = 0; i < chunks && ok && !stop.load(); ++i) {
                DWORD wrote = 0;
                ok = WriteFile(file, buf, chunk, &wrote, nullptr) && wrote == chunk;
            }
            QueryPerformanceCounter(&b);
            if (ok && !stop.load()) out.diskWriteMbps = static_cast<double>(chunks) / Seconds(a, b, freq);
            say(L"Disk", 0.90);
            if (ok && !stop.load()) {
                SetFilePointer(file, 0, nullptr, FILE_BEGIN);
                QueryPerformanceCounter(&a);
                for (int i = 0; i < chunks && ok && !stop.load(); ++i) {
                    DWORD got = 0;
                    ok = ReadFile(file, buf, chunk, &got, nullptr) && got == chunk;
                }
                QueryPerformanceCounter(&b);
                if (ok && !stop.load()) out.diskReadMbps = static_cast<double>(chunks) / Seconds(a, b, freq);
            }
            if (ok && !stop.load()) {
                unsigned long long x = 0x2545F4914F6CDD1Dull;
                const int reads = 400;
                QueryPerformanceCounter(&a);
                int done = 0;
                for (; done < reads && ok && !stop.load(); ++done) {
                    x ^= x << 13;
                    x ^= x >> 7;
                    x ^= x << 17;
                    LARGE_INTEGER pos{};
                    pos.QuadPart = static_cast<LONGLONG>((x % (static_cast<unsigned long long>(chunks) * 256ull)) * 4096ull);
                    DWORD got = 0;
                    ok = SetFilePointerEx(file, pos, nullptr, FILE_BEGIN) && ReadFile(file, buf, 4096, &got, nullptr) && got == 4096;
                }
                QueryPerformanceCounter(&b);
                if (ok && done > 0) out.disk4kUs = Seconds(a, b, freq) * 1e6 / static_cast<double>(done);
            }
            CloseHandle(file);
            DeleteFileW(path.c_str());
        }
        if (buf) VirtualFree(buf, 0, MEM_RELEASE);
    }
    if (stop.load()) return false;
    say(L"Finished", 1.0);
    out.ok = true;
    return true;
}

void SaveBench(const std::wstring& dataDir, const BenchResult& r, bool asBaseline) {
    const BenchHistory have = LoadBench(dataDir);
    const bool baseline = asBaseline || !have.hasBaseline;
    CreateDirectoryW(dataDir.c_str(), nullptr);
    const HANDLE file = CreateFileW(HistoryPath(dataDir).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const std::wstring line =
        Fmt(L"%s,%s,%s,%.4f,%.4f,%.1f,%.1f,%.3f,%.3f,%.1f,%.1f,%.2f,%.0f,%.0f,%.0f\r\n", baseline ? L"baseline" : L"run",
            CsvSafe(r.when).c_str(), CsvSafe(r.label).c_str(), r.sleepMedianMs, r.sleepP99Ms, r.wakeMedianUs, r.wakeP99Us,
            r.dpcPct, r.isrPct, r.cpuSingle, r.cpuAll, r.memGbps, r.diskWriteMbps, r.diskReadMbps, r.disk4kUs);
    const std::string bytes = ToUtf8(line);
    DWORD wrote = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
    CloseHandle(file);
}

BenchHistory LoadBench(const std::wstring& dataDir) {
    BenchHistory h;
    std::wstring text;
    if (!ReadTextFile(HistoryPath(dataDir), text)) return h;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;
        bool baseline = false;
        BenchResult r;
        if (!ParseLine(line, baseline, r)) continue;
        if (baseline) {
            h.baseline = r;
            h.hasBaseline = true;
        }
        h.runs.insert(h.runs.begin(), r);
    }
    if (h.runs.size() > 40) h.runs.resize(40);
    return h;
}

}  // namespace gui
