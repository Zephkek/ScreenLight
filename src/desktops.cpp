#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

BOOL CALLBACK add_desktop(LPWSTR name, LPARAM param) {
  reinterpret_cast<std::vector<std::wstring>*>(param)->emplace_back(name);
  return TRUE;
}

BOOL CALLBACK add_owner(HWND window, LPARAM param) {
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  auto* pids = reinterpret_cast<std::vector<DWORD>*>(param);
  if (pid && std::find(pids->begin(), pids->end(), pid) == pids->end()) pids->push_back(pid);
  return TRUE;
}

bool system_desktop(const std::wstring& name) {
  for (const wchar_t* known : {L"Default", L"Winlogon", L"Disconnect", L"Screen-saver"}) {
    if (_wcsicmp(name.c_str(), known) == 0) return true;
  }
  return false;
}

bool desktop_helper(DWORD pid) {
  wchar_t system[MAX_PATH];
  const UINT length = GetSystemDirectoryW(system, ARRAYSIZE(system));
  if (!length || length >= ARRAYSIZE(system)) return false;
  const std::wstring path = process_path(pid);
  for (const wchar_t* helper : {L"\\ctfmon.exe", L"\\conhost.exe"}) {
    if (_wcsicmp(path.c_str(), (std::wstring(system, length) + helper).c_str()) == 0) return true;
  }
  return false;
}

}  // namespace

std::vector<Watcher> hidden_desktop_processes() {
  std::vector<std::wstring> names;
  EnumDesktopsW(GetProcessWindowStation(), add_desktop, reinterpret_cast<LPARAM>(&names));
  std::vector<Watcher> found;
  for (const std::wstring& name : names) {
    if (system_desktop(name)) continue;
    std::vector<DWORD> pids;
    if (HDESK desktop = OpenDesktopW(name.c_str(), 0, FALSE, DESKTOP_READOBJECTS | DESKTOP_ENUMERATE)) {
      EnumDesktopWindows(desktop, add_owner, reinterpret_cast<LPARAM>(&pids));
      CloseDesktop(desktop);
    }
    for (DWORD pid : pids) {
      if (pid == GetCurrentProcessId() || desktop_helper(pid) || process_gone(pid)) continue;
      found.push_back({Source::kHiddenDesktop, pid, process_name(pid), L"runs on hidden desktop " + name});
    }
  }
  return found;
}

}  // namespace screenlight
