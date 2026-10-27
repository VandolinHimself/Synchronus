// Gui.h - interface between the benchmark core and the Direct2D front-end.
//
// The core knows nothing about rendering: it reports into a gui::Feed from
// its own thread (phases, progress, every single Sleep(1) sample, snapshots
// of the sweep grid and the finalists). The GUI implements the Feed, draws
// whatever the latest snapshot says, and drives the core through gui::Host.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gui {

struct StatsView {
    unsigned n = 0;
    double median = 0, mean = 0, stdev = 0, p99 = 0, latePct = 0, score = 0;
};

struct CandidateView {
    unsigned requested = 0;   // 100-ns units
    bool hasData = false;
    int skippedPasses = 0;
    unsigned interferer = 0;
    StatsView loop;           // back-to-back Sleep(1)
    StatsView idle;           // Sleep(1) after a 2-5 ms idle
    double score = 0;
    bool clean = false;
};

struct SweepView {
    std::vector<CandidateView> items;   // the whole grid, fixed positions
    std::vector<double> smooth;         // per item once analysed, else empty
    int bestIdx = -1, boundaryIdx = -1, targetIdx = -1;
    unsigned margin = 0;                // 100-ns units
    int pass = 0;                       // 1-based sweep lap; 0 = none
    int passes = 0;
    int passPos = 0;                    // intervals completed in this lap
};

struct ResultView {
    unsigned winner = 0, optimum = 0, boundary = 0, margin = 0;
    StatsView loop, idle;
    double score = 0;
    std::wstring failure;
    std::wstring csvPath;
    bool stoppedService = false;        // the benchmark stopped the installed service
};

struct SystemView {
    unsigned fastest = 0, slowest = 0, current = 0;
    bool admin = false;
    int serviceState = 0;               // 0 not installed, 1 running, 2 stopped
    unsigned serviceResolution = 0;     // configured DesiredResolution
    std::wstring priority;              // "realtime" etc. once the benchmark raised it
};

// Locked = static frequency (BIOS-locked multiplier, or Windows already holding
// 100% min processor state). Dynamic = turbo/PBO/SpeedStep actually scaling.
struct CpuProbeView {
    enum class Mode { Locked, Dynamic, Unknown };
    Mode mode = Mode::Unknown;
    unsigned idleMhz = 0, busyMhz = 0, maxMhz = 0;
    unsigned nLogical = 0;
    unsigned perfCheckMs = 0;           // current PERFCHECK, 0 if unread
    bool hybrid = false;                // P+E / heterogeneous cores
    bool alreadyApplied = false;
    std::wstring cpuName;
    std::wstring reason;
};

struct GpuProbeView {
    bool nvidia = false, amd = false, intel = false;
    bool hagsOn = false;
    bool alreadyApplied = false;
    std::wstring name;      // first adapter description
    std::wstring detail;    // vendors, HAGS, etc.
};

struct PpmStepView {
    std::wstring alias, title, why, change;
    int status = 0;  // 0 pending, 1 active, 2 done, 3 failed
};

// Implemented by the GUI. Called from the benchmark thread; must be cheap
// and thread-safe. Sample() is called between timed Sleep(1) calls.
class Feed {
public:
    virtual ~Feed() = default;
    virtual void System(const SystemView& s) = 0;
    virtual void Phase(const std::wstring& title, const std::wstring& detail) = 0;
    virtual void Progress(double fraction, double elapsedS, double remainingS) = 0;
    virtual void Log(const std::wstring& line) = 0;
    virtual void Baseline(const StatsView& s, unsigned reportedUnits) = 0;
    virtual void Sample(unsigned requested, double overshootMs, bool idlePattern) = 0;
    virtual void Sweep(const SweepView& view, int activeIdx) = 0;
    virtual void Verify(const std::vector<CandidateView>& finalists, int activeIdx, unsigned target) = 0;
    virtual void Narrate(const std::wstring& title, const std::wstring& body) = 0;
    virtual void Ppm(const std::vector<PpmStepView>& steps) = 0;
    virtual bool Cancelled() const = 0;
};

// Current vendor display driver, trimmed down to the graphics stack.
// Lookup and download only talk to NVIDIA, AMD, and the small AMD version
// index. Nothing is installed until the GUI has asked.
struct GpuDriverOffer {
    int vendor = 0;             // 1 NVIDIA, 2 AMD
    bool ok = false;            // a package URL was found
    bool canDownload = false;   // enough free space, and the package looks real
    bool laptop = false;
    int fail = 0;               // 1 download failed, 2 install failed
    std::wstring gpuName;
    std::wstring installed;     // Windows driver version already on the machine
    std::wstring version;       // vendor package version
    std::wstring channel;       // Game Ready, Adrenalin, ...
    std::wstring url;
    std::wstring sha256;        // lowercase hex when the index published one
    std::wstring file;          // local package after a successful download
    std::wstring summary;       // what is kept and what is left out
    std::wstring note;          // size, battery, the other GPU
    std::wstring otherGpu;
    std::wstring error;
    uint64_t bytes = 0;
};

// stop is checked between network reads. A running installer is not killed.
GpuDriverOffer LookupGpuDriver(Feed* feed, const std::atomic<bool>& stop);
int DownloadGpuDriver(Feed& feed, GpuDriverOffer& offer, const std::atomic<bool>& stop);
int InstallTrimmedGpuDriver(Feed& feed, GpuDriverOffer& offer);
// Downloads MSI Afterburner 4.6.6 when it is missing, installs RivaTuner with
// it, runs the NVIDIA OC scanner, and applies the saved settings at every logon.
bool InstallAfterburner(Feed* feed, std::wstring& err);
void RemoveAfterburnerStartup();

// Stable address of the update list. The file there changes when a release
// is published; the address does not. The executable is
// https://media.luciuswayne.com/Synchronus.exe or
// https://media.luciuswayne.com/Synchronus-<release>.exe.
// Shape of the file: see the readme.
inline constexpr wchar_t kUpdateManifestUrl[] = L"https://luciuswayne.com/synchronus/latest.json";
// Same counter the website download button increments. Failure is ignored.
inline constexpr wchar_t kDownloadCountUrl[] = L"https://luciuswayne.com/api/synchronus/downloads";

struct AppUpdate {
    std::wstring version;
    std::wstring notes;
    std::wstring url;
    std::wstring sha256;
    uint64_t size = 0;
};

// True when `offer` is a newer build than currentVersion. A network or
// manifest problem returns false and explains it in err; callers stay quiet.
bool CheckAppUpdate(const std::wstring& currentVersion, AppUpdate& offer, const std::atomic<bool>& stop,
                    std::wstring& err);
// Saves the published exe to destPath after the checksum, size, and version
// resource all match the manifest. progress may be empty. stop aborts the
// download between reads.
bool DownloadAppUpdate(const AppUpdate& offer, const std::wstring& destPath,
                       const std::function<void(uint64_t got, uint64_t total)>& progress,
                       const std::atomic<bool>& stop, std::wstring& err);
// Tells the website a copy was downloaded. False only means the counter
// was not reached; the caller still installs the file.
bool ReportAppDownload(const std::wstring& version);
// Renames targetPath aside and copies sourcePath into its place. The running
// executable can be renamed; the caller relaunches after this returns.
bool ReplaceExecutable(const std::wstring& targetPath, const std::wstring& sourcePath, std::wstring& err);

// 0 low, 1 medium (default), 2 high. High is 150 back-to-back samples per
// interval per pass. Medium is that count divided by 8. Low is a short single pass.
struct MeasurePlan {
    int samples = 18;
    int passes = 3;
    int verifySamples = 187;
    double seconds = 0;
};

MeasurePlan MeasurePlanFor(int fidelity);

// Provided by the core. The blocking calls run on a GUI worker thread.
struct Host {
    std::wstring version;
    std::wstring logPath;
    std::wstring dataDir;
    bool autoInstall = false;                                   // --yes
    bool ppmForce = false;                                      // --ppm: apply even on turbo CPUs
    bool ppmSkip = false;                                       // --no-ppm
    bool gpuForce = false;                                      // --gpu
    bool gpuSkip = false;                                       // --no-gpu
    bool afterburnerSkip = false;                               // --no-afterburner
    bool prepareSkip = false;
    bool memorySkip = false;
    bool storageSkip = false;
    unsigned pinnedResolution = 0;                              // skip the benchmark, install this (100-ns units)
    int fidelity = 1;                                           // 0 low, 1 medium, 2 high
    std::function<SystemView()> system;
    std::function<ResultView(Feed&, int fidelity)> benchmark;   // whole benchmark, reports into feed
    // 0 ok, 1 fail, 2 relaunched elevated (caller should quit)
    std::function<int(unsigned units, Feed&)> install;
    std::function<CpuProbeView()> probeCpu;
    std::function<GpuProbeView()> probeGpu;
    std::function<int(Feed&)> applyPpm;                         // 0 ok, 1 fail
    std::function<int(Feed&)> applyGpu;                         // 0 ok, 1 fail
    std::function<int(Feed&)> applyAfterburner;                 // 0 ok, 1 fail
    std::function<int(Feed&)> applyPrepare;
    std::function<int(Feed&)> applyMemory;
    std::function<int(Feed&)> applyStorage;
    std::function<void()> restartService;                       // undo a benchmark's service stop
    // Stronger, repeatable protection after the same setting drifts again.
    // Does not install a driver or run the Afterburner scan.
    std::function<void(const std::wstring& kind)> harden;
    // Removes leftover services from older builds. Leaves ProgramData and preferences.
    std::function<void()> tidyServices;
    // Both empty when this launch should not look for an update.
    // checkUpdate is true only when a newer build should be offered.
    // applyUpdate returns 0 after the files are replaced (caller relaunches),
    // or 1 and sets err. stop aborts the download.
    std::function<bool(AppUpdate& offer, const std::atomic<bool>& stop, std::wstring& err)> checkUpdate;
    std::function<int(const AppUpdate& offer, const std::function<void(const std::wstring&)>& status,
                      const std::atomic<bool>& stop, std::wstring& err)>
        applyUpdate;
};

// Creates the window, runs until it is closed. Returns the process exit code.
int Run(const Host& host);

}  // namespace gui
