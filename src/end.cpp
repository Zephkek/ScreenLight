#include <windows.h>
#include <commctrl.h>

#include <cwchar>
#include <string>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr int kEndButton = 100;
constexpr const wchar_t* kCore[] = {L"smss.exe",     L"csrss.exe",  L"wininit.exe",     L"winlogon.exe", L"services.exe",
                                    L"lsass.exe",    L"lsaiso.exe", L"svchost.exe",     L"dwm.exe",      L"fontdrvhost.exe",
                                    L"sihost.exe",   L"ctfmon.exe", L"taskhostw.exe",   L"consent.exe",  L"LogonUI.exe",
                                    L"spoolsv.exe",  L"audiodg.exe", L"WUDFHost.exe",   L"dllhost.exe",  L"RuntimeBroker.exe"};
constexpr ULONG kBreakOnTermination = 29;
constexpr DWORD kWaitForExitMs = 3000;

using QueryProcessFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

using IsCriticalFn = BOOL(WINAPI*)(HANDLE, PBOOL);

bool critical(HANDLE process) {
  static const auto is_critical =
      reinterpret_cast<IsCriticalFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsProcessCritical"));
  BOOL yes = FALSE;
  if (is_critical && is_critical(process, &yes)) return yes != FALSE;
  static const auto query =
      reinterpret_cast<QueryProcessFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
  ULONG flag = 0;
  return query && query(process, kBreakOnTermination, &flag, sizeof flag, nullptr) >= 0 && flag;
}

bool critical(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
  if (!process) return false;
  const bool yes = critical(process);
  CloseHandle(process);
  return yes;
}

ULONGLONG started_at(HANDLE process) {
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
  return (ULONGLONG(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

ULONGLONG started_at(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return 0;
  const ULONGLONG started = started_at(process);
  CloseHandle(process);
  return started;
}

HRESULT CALLBACK dialog_events(HWND dialog, UINT message, WPARAM, LPARAM, LONG_PTR elevate) {
  if (message == TDN_CREATED && elevate) SendMessageW(dialog, TDM_SET_BUTTON_ELEVATION_REQUIRED_STATE, kEndButton, TRUE);
  return S_OK;
}

bool confirm(HWND owner, const std::wstring& name, bool elevate) {
  const std::wstring title = L"End " + name + L"?";
  const std::wstring body = name + L" closes right away. Anything it hasn't saved is lost.";
  TASKDIALOG_BUTTON buttons[] = {{kEndButton, L"End program"}};
  TASKDIALOGCONFIG config{};
  config.cbSize = sizeof config;
  config.hwndParent = owner;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
  config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  config.pszWindowTitle = L"Screen Light";
  config.pszMainIcon = TD_WARNING_ICON;
  config.pszMainInstruction = title.c_str();
  config.pszContent = body.c_str();
  config.cButtons = ARRAYSIZE(buttons);
  config.pButtons = buttons;
  config.nDefaultButton = IDCANCEL;
  config.pfCallback = dialog_events;
  config.lpCallbackData = elevate;
  int picked = 0;
  return SUCCEEDED(TaskDialogIndirect(&config, &picked, nullptr, nullptr)) && picked == kEndButton;
}

void refused(HWND owner, const std::wstring& name) {
  const std::wstring title = L"Screen Light couldn't end " + name;
  TaskDialog(owner, nullptr, L"Screen Light", title.c_str(), L"Windows protects this program, so it can only be ended from its own settings or by uninstalling it.",
             TDCBF_CLOSE_BUTTON, TD_ERROR_ICON, nullptr);
}

bool in_folder(const std::wstring& path, const wchar_t* folder, UINT length) {
  return length && path.size() > length + 1 && _wcsnicmp(path.c_str(), folder, length) == 0 && path[length] == L'\\' &&
         path.find(L'\\', length + 1) == std::wstring::npos;
}

bool core_windows(const std::wstring& path) {
  wchar_t system[MAX_PATH], wow[MAX_PATH];
  const UINT system_length = GetSystemDirectoryW(system, ARRAYSIZE(system));
  const UINT wow_length = GetSystemWow64DirectoryW(wow, ARRAYSIZE(wow));
  if (!in_folder(path, system, system_length < ARRAYSIZE(system) ? system_length : 0) &&
      !in_folder(path, wow, wow_length < ARRAYSIZE(wow) ? wow_length : 0)) {
    return false;
  }
  const std::wstring name = path.substr(path.find_last_of(L'\\') + 1);
  for (const wchar_t* core : kCore) {
    if (_wcsicmp(name.c_str(), core) == 0) return true;
  }
  return false;
}

}  // namespace

bool can_end(DWORD pid) {
  if (pid <= 4 || pid == GetCurrentProcessId()) return false;
  const std::wstring path = process_path(pid);
  return !path.empty() && !core_windows(path) && !critical(pid);
}

bool end_program(HWND owner, DWORD pid, const std::wstring& name) {
  if (!can_end(pid)) return false;
  HANDLE process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
  const bool elevate = !process && GetLastError() == ERROR_ACCESS_DENIED;
  if (!process && !elevate) return false;
  const ULONGLONG started = process ? started_at(process) : started_at(pid);
  if (!confirm(owner, name, elevate)) {
    if (process) CloseHandle(process);
    return false;
  }
  if (process) {
    const bool ended = TerminateProcess(process, 1) != FALSE;
    if (ended) WaitForSingleObject(process, kWaitForExitMs);
    CloseHandle(process);
    if (!ended && !process_gone(pid)) refused(owner, name);
    return ended;
  }
  if (!started) {
    refused(owner, name);
    return false;
  }
  wchar_t verb[64];
  swprintf(verb, ARRAYSIZE(verb), L"--end %lu %llu", pid, started);
  run_elevated(owner, verb);
  return true;
}

int end_elevated(const wchar_t* pid_text, const wchar_t* started_text) {
  const DWORD pid = wcstoul(pid_text, nullptr, 10);
  const ULONGLONG started = wcstoull(started_text, nullptr, 10);
  if (pid <= 4 || !started) return 1;
  HANDLE process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
  if (!process) {
    if (GetLastError() != ERROR_INVALID_PARAMETER) refused(nullptr, process_name(pid));
    return 1;
  }
  if (started_at(process) != started || critical(process) || core_windows(process_path(pid))) {
    CloseHandle(process);
    return 1;
  }
  const bool ended = TerminateProcess(process, 1) != FALSE;
  if (ended) WaitForSingleObject(process, kWaitForExitMs);
  CloseHandle(process);
  if (!ended) refused(nullptr, process_name(pid));
  return ended ? 0 : 1;
}

}  // namespace screenlight
