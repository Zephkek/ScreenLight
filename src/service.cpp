#include <windows.h>

#include <cwchar>
#include <string>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr wchar_t kDisplayName[] = L"Screen Light";
constexpr wchar_t kDescription[] = L"Tells the Screen Light tray icon about programs that read the screen.";

SERVICE_STATUS_HANDLE g_status_handle;
SERVICE_STATUS g_status{SERVICE_WIN32_OWN_PROCESS, SERVICE_START_PENDING, 0, NO_ERROR, 0, 0, 0};
HANDLE g_stop;

void set_state(DWORD state) {
  g_status.dwCurrentState = state;
  g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
  SetServiceStatus(g_status_handle, &g_status);
}

DWORD WINAPI control(DWORD code, DWORD, void*, void*) {
  if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN) {
    set_state(SERVICE_STOP_PENDING);
    SetEvent(g_stop);
  }
  return NO_ERROR;
}

DWORD WINAPI pipe_thread(void*) {
  serve_pipe(g_stop);
  return 0;
}

void WINAPI service_main(DWORD, LPWSTR*) {
  g_status_handle = RegisterServiceCtrlHandlerExW(kServiceName, control, nullptr);
  if (!g_status_handle) return;
  g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  restore_tracked();
  remember_drivers();
  start_trace();
  HANDLE pipe = CreateThread(nullptr, 0, pipe_thread, nullptr, 0, nullptr);
  set_state(SERVICE_RUNNING);
  const DWORD wait = flush_needed() ? 500 : INFINITE;
  while (WaitForSingleObject(g_stop, wait) == WAIT_TIMEOUT) flush_trace();
  stop_trace();
  if (pipe) {
    WaitForSingleObject(pipe, 5000);
    CloseHandle(pipe);
  }
  set_state(SERVICE_STOPPED);
}

std::wstring install_dir() {
  wchar_t path[MAX_PATH];
  const DWORD length = ExpandEnvironmentStringsW(L"%ProgramFiles%\\Screen Light", path, ARRAYSIZE(path));
  return length && length <= ARRAYSIZE(path) ? std::wstring(path) : std::wstring();
}

void stop_and_wait(SC_HANDLE service) {
  SERVICE_STATUS_PROCESS running{};
  DWORD size = 0;
  HANDLE process = nullptr;
  if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&running), sizeof running, &size) &&
      running.dwProcessId) {
    process = OpenProcess(SYNCHRONIZE, FALSE, running.dwProcessId);
  }
  SERVICE_STATUS status{};
  if (ControlService(service, SERVICE_CONTROL_STOP, &status) && process) WaitForSingleObject(process, 10000);
  if (process) CloseHandle(process);
}

int fail(const std::wstring& text) {
  MessageBoxW(nullptr, text.c_str(), kDisplayName, MB_OK | MB_ICONERROR);
  return 1;
}

}  // namespace

bool service_installed() {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!manager) return false;
  SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
  if (service) CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return service != nullptr;
}

int run_service() {
  SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t*>(kServiceName), service_main}, {nullptr, nullptr}};
  return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}

int install_service() {
  const std::wstring dir = install_dir();
  wchar_t self[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
  if (dir.empty() || !length || length >= ARRAYSIZE(self)) return fail(L"Could not work out where to install the service.");
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
  if (!manager) return fail(L"Installing the service needs admin rights.");
  SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_ALL_ACCESS);
  if (service) stop_and_wait(service);
  const std::wstring exe = dir + L"\\ScreenLight.exe";
  CreateDirectoryW(dir.c_str(), nullptr);
  if (_wcsicmp(self, exe.c_str()) != 0 && !CopyFileW(self, exe.c_str(), FALSE)) {
    if (service) CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return fail(L"Could not copy ScreenLight.exe to " + dir + L". Close any Screen Light running from there and try again.");
  }
  const std::wstring command = L"\"" + exe + L"\" --service";
  if (service) {
    ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, command.c_str(), nullptr,
                         nullptr, nullptr, nullptr, nullptr, kDisplayName);
  } else {
    service = CreateServiceW(manager, kServiceName, kDisplayName, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                             SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr,
                             nullptr, nullptr);
  }
  if (!service) {
    CloseServiceHandle(manager);
    return fail(L"Could not create the Screen Light service.");
  }
  SERVICE_DESCRIPTIONW description{const_cast<wchar_t*>(kDescription)};
  ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);
  const bool started = StartServiceW(service, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return started ? 0 : fail(L"The service was installed but did not start.");
}

int uninstall_service() {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!manager) return fail(L"Removing the service needs admin rights.");
  if (SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE)) {
    stop_and_wait(service);
    DeleteService(service);
    CloseServiceHandle(service);
  }
  CloseServiceHandle(manager);
  const std::wstring dir = install_dir();
  if (!dir.empty()) DeleteFileW((dir + L"\\running.txt").c_str());
  wchar_t self[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
  const std::wstring exe = dir + L"\\ScreenLight.exe";
  const bool installed = GetFileAttributesW((dir + L"\\unins000.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
  if (!dir.empty() && !installed && length && length < ARRAYSIZE(self) && _wcsicmp(self, exe.c_str()) != 0) {
    if (!DeleteFileW(exe.c_str())) MoveFileExW(exe.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    RemoveDirectoryW(dir.c_str());
  }
  return 0;
}

}  // namespace screenlight
