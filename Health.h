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
    bool startMinimized = false;
    bool launchAtLogin = false;
    bool notify = true;
    bool paused = false;
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

MonitorPrefs LoadMonitorPrefs();
void SaveMonitorPrefs(const MonitorPrefs& prefs);
bool CheckDisabled(const std::wstring& id);
void SetCheckDisabled(const std::wstring& id, bool disabled);
void ApplyLaunchAtLogin(bool enabled);
void AppendHealthLog(const std::wstring& line);

}  // namespace gui
