#include <windows.h>
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
      found->push_back({Source::kRemote, 0, label.empty() ? L"Wireless display" : label, L"shows your screen on another device", {}});
    } else {
      found->push_back({Source::kRemote, 0, label.empty() ? L"Virtual screen" : label,
                        L"is a virtual screen, the program behind it can stream what is on it", {}});
    }
  }
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
  if (GetSystemMetrics(kRemoteControl)) found.push_back({Source::kRemote, 0, L"Someone", L"controls this session remotely", {}});
  return found;
}

}  // namespace screenlight
