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

}  // namespace

std::vector<Watcher> remote_viewers() {
  std::vector<Watcher> found;
  if (GetSystemMetrics(kRemoteControl)) found.push_back({Source::kRemote, 0, L"Someone", L"controls this session remotely", {}});
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
  return found;
}

}  // namespace screenlight
