#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>

#include <string>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kShow = 1;
constexpr UINT kExit = 2;
constexpr UINT_PTR kPollTimer = 1;

HINSTANCE g_instance;
UINT g_taskbar_created;
HICON g_off;
HICON g_on;
bool g_lit;
std::wstring g_tip = L"Screen Light - nothing is watching";

NOTIFYICONDATAW tray_data(HWND window) {
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof data;
  data.hWnd = window;
  data.uID = 1;
  return data;
}

void add_icon(HWND window) {
  NOTIFYICONDATAW data = tray_data(window);
  data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  data.uCallbackMessage = kTrayMessage;
  data.hIcon = g_lit ? g_on : g_off;
  lstrcpynW(data.szTip, g_tip.c_str(), ARRAYSIZE(data.szTip));
  Shell_NotifyIconW(NIM_ADD, &data);
  data.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &data);
}

std::wstring describe(const std::vector<Watcher>& watchers) {
  if (watchers.empty()) return L"Screen Light - nothing is watching";
  std::wstring tip = L"Screen Light";
  for (const Watcher& w : watchers) tip += L"\n" + w.name + L" " + w.what;
  if (tip.size() > 127) tip = tip.substr(0, 124) + L"...";
  return tip;
}

void refresh(HWND window) {
  const std::vector<Watcher> watchers = collect_watchers();
  track(watchers);
  const bool lit = !watchers.empty();
  std::wstring tip = describe(watchers);
  if (lit == g_lit && tip == g_tip) return;
  g_lit = lit;
  g_tip = std::move(tip);
  NOTIFYICONDATAW data = tray_data(window);
  data.uFlags = NIF_ICON | NIF_TIP;
  data.hIcon = g_lit ? g_on : g_off;
  lstrcpynW(data.szTip, g_tip.c_str(), ARRAYSIZE(data.szTip));
  Shell_NotifyIconW(NIM_MODIFY, &data);
}

void show_menu(HWND window, int x, int y) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, kShow, L"Show");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kExit, L"Exit");
  SetMenuDefaultItem(menu, kShow, FALSE);
  SetForegroundWindow(window);
  const UINT picked = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, window, nullptr);
  PostMessageW(window, WM_NULL, 0, 0);
  DestroyMenu(menu);
  if (picked == kShow) show_window(g_instance);
  if (picked == kExit) DestroyWindow(window);
}

LRESULT CALLBACK tray_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == g_taskbar_created) {
    add_icon(window);
    return 0;
  }
  switch (message) {
    case kTrayMessage:
      switch (LOWORD(lparam)) {
        case WM_CONTEXTMENU:
          show_menu(window, GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam));
          break;
        case NIN_SELECT:
        case NIN_KEYSELECT:
          show_window(g_instance);
          break;
      }
      return 0;
    case WM_TIMER:
      if (wparam == kPollTimer) refresh(window);
      return 0;
    case kRefreshMessage:
      refresh(window);
      return 0;
    case kShowMessage:
      show_window(g_instance);
      return 0;
    case WM_DESTROY: {
      KillTimer(window, kPollTimer);
      NOTIFYICONDATAW data = tray_data(window);
      Shell_NotifyIconW(NIM_DELETE, &data);
      PostQuitMessage(0);
      return 0;
    }
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int run_tray(HINSTANCE instance) {
  g_instance = instance;
  g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
  const int size = GetSystemMetrics(SM_CXSMICON);
  g_off = make_light(RGB(140, 140, 140), size);
  g_on = make_light(RGB(226, 52, 40), size);
  WNDCLASSW wc{};
  wc.lpfnWndProc = tray_proc;
  wc.hInstance = instance;
  wc.lpszClassName = L"ScreenLight";
  RegisterClassW(&wc);
  HWND window = CreateWindowExW(0, wc.lpszClassName, L"Screen Light", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
  if (!window) return 1;
  ChangeWindowMessageFilterEx(window, g_taskbar_created, MSGFLT_ALLOW, nullptr);
  ChangeWindowMessageFilterEx(window, kShowMessage, MSGFLT_ALLOW, nullptr);
  add_icon(window);
  start_input_watch(window);
  refresh(window);
  SetTimer(window, kPollTimer, 1000, nullptr);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  stop_input_watch();
  DestroyIcon(g_on);
  DestroyIcon(g_off);
  return 0;
}

}  // namespace screenlight
