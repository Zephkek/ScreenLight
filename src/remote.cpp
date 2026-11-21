#include <windows.h>
#include <tlhelp32.h>
#include <wtsapi32.h>

#include <string>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr int kRemoteControl = 0x2001;

std::wstring session_user(DWORD session) {
  std::wstring name;
  LPWSTR user = nullptr;
  DWORD size = 0;
  if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSUserName, &user, &size) && user) {
    name = user;
    WTSFreeMemory(user);
  }
  return name;
}


constexpr int kMiracast = 15;
constexpr int kVirtualScreen = 17;

void virtual_screens(std::vector<Watcher>* found) {
  UINT32 paths = 0, modes = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS || !paths) return;
  std::vector<DISPLAYCONFIG_PATH_INFO> path(paths);
  std::vector<DISPLAYCONFIG_MODE_INFO> mode(modes);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path.data(), &modes, mode.data(), nullptr) != ERROR_SUCCESS) return;
  for (UINT32 i = 0; i < paths; ++i) {
    const int technology = int(path[i].targetInfo.outputTechnology);
    if (technology != kMiracast && technology != kVirtualScreen) continue;
    DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    name.header.size = sizeof name;
    name.header.adapterId = path[i].targetInfo.adapterId;
    name.header.id = path[i].targetInfo.id;
    std::wstring label = DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS ? name.monitorFriendlyDeviceName : L"";
    if (technology == kMiracast) {
      found->push_back({Source::kRemote, 0, label.empty() ? L"Wireless display" : label, L"casts the screen", {}});
    } else {
      found->push_back({Source::kRemote, 0, label.empty() ? L"Virtual screen" : label,
                        L"virtual display", {}});
    }
  }
}

Watcher controller() {
  DWORD session = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &session);
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
      DWORD owner = 0;
      if (_wcsicmp(entry.szExeFile, L"RdpSa.exe") == 0 && ProcessIdToSessionId(entry.th32ProcessID, &owner) && owner == session) {
        CloseHandle(snapshot);
        return {Source::kRemote, entry.th32ProcessID, L"Remote desktop", L"controls this PC remotely",
                process_path(entry.th32ProcessID)};
      }
    }
    CloseHandle(snapshot);
  }
  return {Source::kRemote, 0, L"Remote desktop", L"controls this PC remotely", {}};
}

std::vector<Watcher> g_slow;
ULONGLONG g_slow_at;
bool g_slow_stale = true;
constexpr ULONGLONG kSlowEveryMs = 30000;

std::vector<Watcher> slow_viewers() {
  std::vector<Watcher> found;
  WTS_SESSION_INFOW* sessions = nullptr;
  DWORD count = 0;
  if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
    for (DWORD i = 0; i < count; ++i) {
      if (sessions[i].State != WTSShadow) continue;
      std::wstring user = session_user(sessions[i].SessionId);
      if (user.empty()) user = L"Session " + std::to_wstring(sessions[i].SessionId);
      found.push_back({Source::kRemote, 0, user, L"watches a session remotely", {}});
    }
    WTSFreeMemory(sessions);
  }
  DISPLAY_DEVICEW device{};
  device.cb = sizeof device;
  const DWORD mirror = DISPLAY_DEVICE_MIRRORING_DRIVER | DISPLAY_DEVICE_ATTACHED_TO_DESKTOP;
  for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &device, 0); ++i, device.cb = sizeof device) {
    if ((device.StateFlags & mirror) == mirror) found.push_back({Source::kRemote, 0, device.DeviceString, L"mirrors the screen", {}});
  }
  virtual_screens(&found);
  return found;
}

}  // namespace

void remote_changed() { g_slow_stale = true; }

std::vector<Watcher> remote_viewers() {
  const ULONGLONG now = GetTickCount64();
  if (g_slow_stale || now - g_slow_at > kSlowEveryMs) {
    g_slow = slow_viewers();
    g_slow_at = now;
    g_slow_stale = false;
  }
  std::vector<Watcher> found = g_slow;
  static Watcher remote;
  static ULONGLONG remote_at;
  if (GetSystemMetrics(kRemoteControl)) {
    if (!remote_at || now - remote_at > kSlowEveryMs) {
      remote = controller();
      remote_at = now;
    }
    found.push_back(remote);
  } else {
    remote_at = 0;
  }
  return found;
}

}  // namespace screenlight
