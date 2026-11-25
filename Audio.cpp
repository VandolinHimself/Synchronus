// Audio.cpp - the Audio step.
//
// Turns off audio enhancements on the playback and recording devices that are
// in use. Enhancements are effects the driver inserts into the audio path
// (loudness, virtual surround, noise suppression). Each one adds buffering,
// and a slow one causes dropouts. Windows already runs the audio engine at a
// real-time priority, so there is nothing to raise there.
//
// The switch is the one Sound settings writes, set through the endpoint's
// property store. What it replaced is kept the first time.
#include "Util.h"

#include <mmdeviceapi.h>
#include <objbase.h>
#include <propsys.h>

#pragma comment(lib, "ole32.lib")

namespace gui {
namespace {

using namespace util;

constexpr wchar_t kBackupKey[] = L"SOFTWARE\\Synchronus\\Audio";
constexpr wchar_t kCategory[] = L"Audio";
constexpr DWORD kWasAbsent = 0xFFFFFFFFu;

// PKEY_AudioEndpoint_Disable_SysFx and PKEY_Device_FriendlyName.
const PROPERTYKEY kDisableSysFx = {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 5};
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (hr == S_OK || hr == S_FALSE) CoUninitialize();
    }
};

template <typename T>
struct Ptr {
    T* p = nullptr;
    ~Ptr() {
        if (p) p->Release();
    }
    T* operator->() const { return p; }
    T** operator&() { return &p; }
    explicit operator bool() const { return p != nullptr; }
};

// 1 off, 0 on, -1 not stored (which Windows treats as on), -2 unreadable.
int ReadSwitch(IMMDevice* device) {
    Ptr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || !store) return -2;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (FAILED(store->GetValue(kDisableSysFx, &v))) return -2;
    int state = -1;
    if (v.vt == VT_UI4) state = v.ulVal ? 1 : 0;
    else if (v.vt == VT_I4) state = v.lVal ? 1 : 0;
    else if (v.vt == VT_BOOL) state = v.boolVal ? 1 : 0;
    PropVariantClear(&v);
    return state;
}

bool WriteSwitch(IMMDevice* device, bool off) {
    Ptr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READWRITE, &store)) || !store) return false;
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = off ? 1u : 0u;
    if (FAILED(store->SetValue(kDisableSysFx, v))) return false;
    return SUCCEEDED(store->Commit());
}

std::wstring NameOf(IMMDevice* device) {
    Ptr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || !store) return L"Audio device";
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring name = L"Audio device";
    if (SUCCEEDED(store->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) name = v.pwszVal;
    PropVariantClear(&v);
    return name;
}

struct Found {
    AudioDevice info;
    IMMDevice* device = nullptr;
};

// Devices are released by the caller.
std::vector<Found> Enumerate() {
    std::vector<Found> out;
    Ptr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator))) ||
        !enumerator)
        return out;
    Ptr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &devices)) || !devices) return out;
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        if (FAILED(devices->Item(i, &device)) || !device) continue;
        Found f;
        f.device = device;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            f.info.id = id;
            CoTaskMemFree(id);
        }
        Ptr<IMMEndpoint> endpoint;
        EDataFlow flow = eRender;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IMMEndpoint), reinterpret_cast<void**>(&endpoint))) && endpoint)
            endpoint->GetDataFlow(&flow);
        f.info.capture = flow == eCapture;
        f.info.name = NameOf(device);
        const int state = ReadSwitch(device);
        f.info.readable = state != -2;
        f.info.enhancementsOff = state == 1;
        if (f.info.id.empty()) {
            device->Release();
            continue;
        }
        out.push_back(std::move(f));
    }
    return out;
}

void ReleaseAll(std::vector<Found>& found) {
    for (auto& f : found) {
        if (f.device) f.device->Release();
        f.device = nullptr;
    }
}

}  // namespace

bool AudioApplied() {
    DWORD v = 0;
    return GetDword(HKEY_LOCAL_MACHINE, kAppKey, L"AudioApplied", v) && v != 0;
}

std::vector<AudioDevice> ListAudioDevices() {
    ComScope com;
    std::vector<Found> found = Enumerate();
    std::vector<AudioDevice> out;
    out.reserve(found.size());
    for (const auto& f : found) out.push_back(f.info);
    ReleaseAll(found);
    return out;
}

bool ApplyAudio(const StepIo& io) {
    ComScope com;
    Steps steps(io);
    steps.Narrate(L"Audio",
                  L"Turns off audio enhancements on the playback and recording devices that are connected. "
                  L"Enhancements are effects the driver adds, such as loudness, virtual surround, and noise "
                  L"suppression. They add delay. A device that relies on one will sound plainer afterwards.");
    steps.Phase(L"AUDIO", L"reading the audio devices");
    steps.Out(L"Applying audio settings: enhancements off on each active device.");

    std::vector<Found> found = Enumerate();
    if (found.empty()) {
        steps.Narrate(L"Audio", L"No active playback or recording device was found.");
        steps.Out(L"No active audio device.");
        return true;
    }
    for (const auto& f : found) {
        steps.Add(L"FX", (f.info.capture ? L"Recording: " : L"Playback: ") + f.info.name,
                  L"Enhancements run inside the audio engine on every buffer for this device. Off sends the "
                  L"sound straight through. It takes effect the next time the device starts playing or recording.",
                  f.info.enhancementsOff ? L"already off" : L"enhancements on  \u2192  off");
    }
    steps.Publish();
    steps.Dwell(1200);

    int wrote = 0, kept = 0, failed = 0;
    for (size_t i = 0; i < found.size(); ++i) {
        if (steps.Cancelled()) {
            ReleaseAll(found);
            return false;
        }
        steps.Begin(i, L"AUDIO", 1300);
        if (steps.Cancelled()) {
            ReleaseAll(found);
            return false;
        }
        Found& f = found[i];
        bool ok = false;
        if (f.info.enhancementsOff) {
            ok = true;
            ++kept;
        } else {
            if (!ValueExists(HKEY_LOCAL_MACHINE, kBackupKey, f.info.id.c_str())) {
                const int before = ReadSwitch(f.device);
                SetDword(HKEY_LOCAL_MACHINE, kBackupKey, f.info.id.c_str(), before < 0 ? kWasAbsent : static_cast<DWORD>(before));
            }
            ok = WriteSwitch(f.device, true) && ReadSwitch(f.device) == 1;
            if (ok) ++wrote;
        }
        if (!ok) {
            ++failed;
            steps.Err(L"    Windows did not accept the change for this device.");
        } else {
            steps.Out(L"    off.");
        }
        steps.End(i, ok, ok ? L"enhancements off" : L"could not be changed");
    }
    ReleaseAll(found);
    if (!wrote && !kept) {
        steps.Err(L"No audio device could be changed.");
        return false;
    }
    SetDword(HKEY_LOCAL_MACHINE, kAppKey, L"AudioApplied", 1);
    steps.Narrate(L"Audio settings applied",
                  Fmt(L"Enhancements are off on %d device(s)%s. Undo from the Audio screen, or with --restore-audio.",
                      wrote + kept, failed ? Fmt(L"; %d could not be changed", failed).c_str() : L""));
    steps.Out(Fmt(L"Audio enhancements are off on %d device(s)%s.", wrote + kept,
                  failed ? Fmt(L", %d failed", failed).c_str() : L""));
    steps.Dwell(1000);
    return true;
}

bool RestoreAudio(const StepIo* io) {
    ComScope com;
    int restored = 0;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kBackupKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        Ptr<IMMDeviceEnumerator> enumerator;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                         reinterpret_cast<void**>(&enumerator));
        wchar_t name[512];
        for (DWORD i = 0;; ++i) {
            DWORD len = _countof(name);
            DWORD type = 0, data = 0, size = sizeof(data);
            if (RegEnumValueW(key, i, name, &len, nullptr, &type, reinterpret_cast<BYTE*>(&data), &size) != ERROR_SUCCESS) break;
            if (type != REG_DWORD || !enumerator) continue;
            IMMDevice* device = nullptr;
            if (FAILED(enumerator->GetDevice(name, &device)) || !device) continue;
            // A device that never stored the switch is back to its default with enhancements on.
            if (WriteSwitch(device, data != kWasAbsent && data != 0)) ++restored;
            device->Release();
        }
        RegCloseKey(key);
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, kBackupKey);
    }
    DeleteValue(HKEY_LOCAL_MACHINE, kAppKey, L"AudioApplied");
    if (restored && io && io->out) io->out(Fmt(L"Restored audio enhancements on %d device(s).", restored));
    return true;
}

void AppendAudioChecks(std::vector<HealthCheck>& out, const std::atomic<bool>* cancelled) {
    if (cancelled && cancelled->load()) return;
    const std::vector<AudioDevice> devices = ListAudioDevices();
    int total = 0, off = 0, unreadable = 0;
    std::wstring on;
    for (const auto& d : devices) {
        ++total;
        if (!d.readable) ++unreadable;
        else if (d.enhancementsOff) ++off;
        else on += (on.empty() ? L"" : L", ") + d.name;
    }
    HealthCheck c = MakeCheck(L"audio.enhancements", kCategory, L"Audio enhancements off");
    c.expected = L"off on every active device";
    c.observed = total == 0 ? L"no active audio device"
                            : (off == total ? Fmt(L"off on %d of %d", off, total)
                                            : Fmt(L"off on %d of %d; on: %s", off, total, on.empty() ? L"unreadable" : on.c_str()));
    if (CheckDisabled(c.id)) {
        c.status = HealthStatus::Disabled;
        c.explanation = L"This check is turned off in Settings.";
    } else if (total == 0) {
        c.status = HealthStatus::Info;
        c.explanation = L"There is no active audio device to check.";
    } else if (unreadable == total) {
        c.status = HealthStatus::Inaccessible;
        c.severity = 1;
        c.explanation = L"The devices could not be read. That is not a pass.";
    } else if (off == total) {
        c.status = HealthStatus::Pass;
        c.explanation = L"Live. No device is running driver effects.";
    } else if (!AudioApplied()) {
        c.status = HealthStatus::Info;
        c.explanation = L"The Audio step has not been run on this PC.";
    } else if (!StepChosen(L"audio")) {
        c.status = HealthStatus::Info;
        c.explanation = L"You left this step off. It is not currently applied.";
    } else {
        c.status = HealthStatus::Attention;
        c.severity = 1;
        c.repair = L"audio";
        c.explanation = L"A device has enhancements on again, or a new device was connected. Repair turns them off.";
    }
    out.push_back(std::move(c));
}

}  // namespace gui
