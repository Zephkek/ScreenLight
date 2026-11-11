#include <windows.h>

#include "screenlight.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
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
