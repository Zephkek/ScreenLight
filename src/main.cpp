#include <windows.h>
#include <shellapi.h>

#include <cwchar>

#include "screenlight.h"

namespace {

int run_tray_once(HINSTANCE instance) {
  HANDLE once = CreateMutexW(nullptr, TRUE, L"Local\\ScreenLight");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND running = FindWindowW(L"ScreenLight", nullptr)) {
      DWORD pid = 0;
      GetWindowThreadProcessId(running, &pid);
      AllowSetForegroundWindow(pid);
      PostMessageW(running, screenlight::kShowMessage, 0, 0);
    }
    return 0;
  }
  const int code = screenlight::run_tray(instance);
  if (once) CloseHandle(once);
  return code;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  const wchar_t* verb = argv && argc > 1 ? argv[1] : L"";
  if (wcscmp(verb, L"--service") == 0) return screenlight::run_service();
  if (wcscmp(verb, L"--install") == 0) return screenlight::install_service();
  if (wcscmp(verb, L"--uninstall") == 0) return screenlight::uninstall_service();
  return run_tray_once(instance);
}
