// Health.h - read-only inspection of the settings Synchronus can change.
// A scan never applies a tweak, installs a driver, or holds a timer request.
#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace gui {

enum class HealthStatus {
    Pass,
    Attention,
    Info,
    Disabled,
    Unsupported,
    Unknown,
    Inaccessible,
    Error,
    Reboot
};

struct HealthCheck {
    std::wstring id;
    std::wstring category;
    std::wstring name;
    std::wstring expected;
    std::wstring observed;
    HealthStatus status = HealthStatus::Unknown;
    int severity = 0;  // 0 info, 1 attention, 2 error
    std::wstring explanation;
    std::wstring repair;  // empty, or ppm/gpu/memory/storage/timer/afterburner
    unsigned long long checkedAt = 0;
};

struct HealthSummary {
    int pass = 0;
    int attention = 0;
    int info = 0;
    int other = 0;  // unknown, error, inaccessible, unsupported, reboot, disabled
    std::wstring headline;
};

struct MonitorPrefs {
    bool closeToTray = true;
    bool startMinimized = true;
    bool launchAtLogin = true;
    bool trayPrompted = false;
    bool startupAsked = false;
    bool notify = true;
    bool paused = false;
    bool maintainTiming = false;
    bool maintainAsked = false;
    int scanHours = 6;
};

const wchar_t* HealthStatusText(HealthStatus status);
bool HealthCountsAsPass(HealthStatus status);
bool HealthNeedsAttention(HealthStatus status);
HealthSummary Summarize(const std::vector<HealthCheck>& checks);

// Read-only. cancelled is polled between checks. A failed read is never Pass.
std::vector<HealthCheck> RunHealth(const std::atomic<bool>* cancelled);

// Pure comparisons used by --self-test. Returns 0 when every case holds.
int SelfTestHealth();

int DriftCount(const std::wstring& id);
void SetDriftCount(const std::wstring& id, int count);

MonitorPrefs LoadMonitorPrefs();
void SaveMonitorPrefs(const MonitorPrefs& prefs);
bool CheckDisabled(const std::wstring& id);
void SetCheckDisabled(const std::wstring& id, bool disabled);
void ApplyLaunchAtLogin(bool enabled);
void AppendHealthLog(const std::wstring& line);

struct StepChoices {
    bool prepare = true;
    bool measure = true;
    bool hold = true;
    bool cpu = true;
    bool gpuClean = true;
    bool gpu = true;
    bool afterburner = true;
    bool memory = true;
    bool storage = true;
};

bool SetupComplete();
void MarkSetupComplete();
void SaveStepChoices(const StepChoices& choices);
StepChoices LoadStepChoices();
bool StepChosen(const wchar_t* name);
// Live read of every power, storage, and GPU tweak the apply steps write.
void AppendTuneChecks(std::vector<HealthCheck>& out, const std::atomic<bool>* cancelled);

}  // namespace gui
