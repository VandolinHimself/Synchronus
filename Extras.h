// Extras.h - the parts added in 3.0: network and audio steps, profiles,
// the game watcher, hardware-change detection, quiet notifications,
// benchmarks, live metrics, export and import, and custom scripts.
//
// Everything that changes Windows keeps the value it replaced, and has a
// restore that --uninstall calls. Reads never change anything.
#pragma once

#include "Gui.h"
#include "Health.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace gui {

// How a step reports. feed may be null in the console. out and err reach the
// console, the log, and the feed.
struct StepIo {
    Feed* feed = nullptr;
    bool dwell = true;
    std::function<void(const std::wstring&)> out;
    std::function<void(const std::wstring&)> err;
};

// ---------------------------------------------------------------- network
struct NetOptions {
    bool power = true;       // adapter power saving off
    bool moderation = true;  // interrupt moderation off, receive side scaling on
    bool delivery = true;    // Windows Update stops sharing with other PCs
    bool skipPlan = false;   // not saved. Set while Balanced: the Wi-Fi level in the power plan is left alone
};

struct NetChange {
    std::wstring keyword;
    std::wstring title;
    std::wstring why;
    std::wstring from;   // live value, or empty when the driver has not stored one
    std::wstring to;
    int group = 0;       // 0 power, 1 moderation
    bool already = false;
};

struct NetAdapter {
    std::wstring name;
    std::wstring key;    // HKLM class key of this adapter
    std::wstring guid;   // NetCfgInstanceId
    bool wifi = false;
    std::vector<NetChange> changes;
};

struct NetPlan {
    std::vector<NetAdapter> adapters;
    bool hasWifi = false;
    bool wifiPowerReadable = false;
    unsigned wifiPowerAc = 0;    // 0 is Maximum Performance
    bool deliveryPolicySet = false;
    unsigned deliveryMode = 0;
    int pending = 0;             // changes that are not already in place
};

NetOptions LoadNetOptions();
void SaveNetOptions(const NetOptions& options);
NetPlan PlanNetwork(const NetOptions& options);  // read-only
bool ApplyNetwork(const StepIo& io, const NetOptions& options);
bool RestoreNetwork(const StepIo* io);
bool NetworkApplied();
void AppendNetworkChecks(std::vector<HealthCheck>& out, const std::atomic<bool>* cancelled);

// ------------------------------------------------------------------ audio
struct AudioDevice {
    std::wstring id;
    std::wstring name;
    bool capture = false;
    bool readable = false;
    bool enhancementsOff = false;
};

std::vector<AudioDevice> ListAudioDevices();  // active playback and recording devices, read-only
bool ApplyAudio(const StepIo& io);
bool RestoreAudio(const StepIo* io);
bool AudioApplied();
void AppendAudioChecks(std::vector<HealthCheck>& out, const std::atomic<bool>* cancelled);

// --------------------------------------------------------------- profiles
// Performance keeps the tuned power plan and the timer hold. Balanced hands
// the power plan back and releases the timer. Nothing else is touched.
enum class Profile : int { Performance = 0, Balanced = 1 };

const wchar_t* ProfileName(Profile p);
Profile ChosenProfile();   // what the user picked
Profile ActiveProfile();   // what is applied now; a running game can lift Balanced
void SetChosenProfile(Profile p);
// Implemented by the core. detail says what happened in plain words.
bool ApplyProfile(Profile p, std::wstring& detail);
// True for the checks Balanced is allowed to leave untuned.
bool ProfileRelaxes(const std::wstring& checkId);
void RelaxForProfile(std::vector<HealthCheck>& checks);

// ------------------------------------------------- export, import, sharing
// Step choices and preferences only. No folders, no user name, no device ids.
std::wstring ExportSettings(const std::wstring& version);
bool ImportSettings(const std::wstring& json, std::wstring& summary, std::wstring& err);
std::wstring MakeShareCode(const std::wstring& json);
// Accepts a bare code or a link that ends in #code.
bool ReadShareCode(const std::wstring& text, std::wstring& json);
std::wstring ShareLink(const std::wstring& code);
bool WriteTextFile(const std::wstring& path, const std::wstring& text);
bool ReadTextFile(const std::wstring& path, std::wstring& text);

// ---------------------------------------------------------------- scripts
// PowerShell and batch files under Program Files\Synchronus\Scripts\<hook>.
// A script runs only after it is approved, and again only while its contents
// still match what was approved.
struct ScriptEntry {
    std::wstring hook;   // after-tune, performance, balanced, game-start, game-stop
    std::wstring name;
    std::wstring path;
    bool approved = false;
};

std::wstring ScriptsDir();
void EnsureScriptFolders();
std::vector<ScriptEntry> ListScripts();
bool ApproveScript(const ScriptEntry& script, bool on);
// Runs the approved scripts of one hook in name order and waits for each.
int RunHook(const wchar_t* hook, const std::wstring& game, const std::function<void(const std::wstring&)>& log);

// ------------------------------------------------------------------ games
struct GameRules {
    bool libraries = true;                 // Windows' own list and launcher library folders
    std::vector<std::wstring> include;     // exe names the user added
    std::vector<std::wstring> exclude;     // exe names that are never a game
};

GameRules LoadGameRules();
void SaveGameRules(const GameRules& rules);
std::vector<std::wstring> WindowsKnownGames();  // lowercase full paths
// Pure. known is WindowsKnownGames().
bool LooksLikeGame(const std::wstring& path, const GameRules& rules, const std::vector<std::wstring>& known);

struct GameHit {
    DWORD pid = 0;
    std::wstring exe;
    std::wstring path;
};

// One pass over the process list. verdicts remembers what each pid was.
bool FindRunningGame(const GameRules& rules, const std::vector<std::wstring>& known,
                     std::vector<std::pair<DWORD, int>>& verdicts, GameHit& hit);

// ------------------------------------------------- notifications and Game Bar
bool QuietBegin();   // app notification banners off; remembers the previous state
void QuietEnd();
bool QuietHeld();
bool GameBarButtonOff();
void SetGameBarButtonOff(bool off);
// For uninstall: notifications and the Game Bar button go back to what they were.
void RestoreGameSettings();

// --------------------------------------------------------- hardware changes
struct HardwareChange {
    std::wstring part;    // gpu, driver, cpu, bios, windows
    std::wstring what;    // one line for the user
    std::wstring advice;  // which steps to run again
    std::wstring step;    // gpu, afterburner, ppm, measure
};

std::vector<HardwareChange> DetectHardwareChanges();  // saves the first signature it sees
void AcceptHardware();
std::wstring HardwareSummary();

// ------------------------------------------------------------- live metrics
struct LiveSample {
    double cpu = 0, dpc = 0, isr = 0;     // percent of all processors
    double mhz = 0, maxMhz = 0;
    double timerMs = 0;
    double memory = 0;                    // percent in use
    double gpuTemp = 0, gpuLoad = 0, zoneTemp = 0;
    bool hasGpuTemp = false, hasGpuLoad = false, hasZone = false;
};

class LiveMeter {
public:
    LiveMeter();
    ~LiveMeter();
    LiveMeter(const LiveMeter&) = delete;
    LiveMeter& operator=(const LiveMeter&) = delete;
    // Shares since the previous call. The first call only primes the counters.
    bool Sample(LiveSample& out);

private:
    struct Impl;
    Impl* impl_;
};

// --------------------------------------------------------------- benchmarks
struct BenchResult {
    std::wstring when;
    std::wstring label;
    double sleepMedianMs = 0, sleepP99Ms = 0;   // Sleep(1) overshoot at the current timer
    double wakeMedianUs = 0, wakeP99Us = 0;     // event signal to thread running
    double dpcPct = 0, isrPct = 0;
    double cpuSingle = 0, cpuAll = 0;           // million steps per second
    double memGbps = 0;
    double diskWriteMbps = 0, diskReadMbps = 0, disk4kUs = 0;
    bool ok = false;
};

struct BenchHistory {
    bool hasBaseline = false;
    BenchResult baseline;
    std::vector<BenchResult> runs;  // newest first
};

bool RunBench(BenchResult& out, const std::wstring& dataDir, const std::atomic<bool>& stop,
              const std::function<void(const wchar_t* phase, double fraction)>& progress);
void SaveBench(const std::wstring& dataDir, const BenchResult& result, bool asBaseline);
BenchHistory LoadBench(const std::wstring& dataDir);

// 0 when the pure rules in these modules hold. Changes nothing.
int SelfTestExtras();

}  // namespace gui
