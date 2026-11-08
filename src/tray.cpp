#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kExit = 1;
constexpr UINT_PTR kPollTimer = 1;

UINT g_taskbar_created;
HICON g_off;
HICON g_on;
bool g_lit;
std::wstring g_tip = L"Screen Light - nothing is watching";

HICON make_light(COLORREF color, int size) {
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = size;
  bi.bmiHeader.biHeight = -size;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HBITMAP pixels = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!pixels) return nullptr;
  const float center = (size - 1) / 2.0f, radius = size * 0.34f;
  auto* out = static_cast<uint32_t*>(bits);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float cover = std::clamp(radius - std::hypot(x - center, y - center) + 0.5f, 0.0f, 1.0f);
      out[y * size + x] = uint32_t(cover * 255 + 0.5f) << 24 | (color & 0xff) << 16 | (color & 0xff00) | (color >> 16 & 0xff);
    }
  }
  std::vector<uint8_t> clear((size + 15) / 16 * 2 * size);
  HBITMAP mask = CreateBitmap(size, size, 1, 1, clear.data());
  ICONINFO info{TRUE, 0, 0, mask, pixels};
  HICON icon = CreateIconIndirect(&info);
  DeleteObject(mask);
  DeleteObject(pixels);
  return icon;
}

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
  const std::vector<Watcher> watchers = duplicating_processes();
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
  AppendMenuW(menu, MF_STRING, kExit, L"Exit");
  SetForegroundWindow(window);
  const UINT picked = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, window, nullptr);
  PostMessageW(window, WM_NULL, 0, 0);
  DestroyMenu(menu);
  if (picked == kExit) DestroyWindow(window);
}

LRESULT CALLBACK tray_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == g_taskbar_created) {
    add_icon(window);
    return 0;
  }
  switch (message) {
    case kTrayMessage:
      if (LOWORD(lparam) == WM_CONTEXTMENU) show_menu(window, GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam));
      return 0;
    case WM_TIMER:
      if (wparam == kPollTimer) refresh(window);
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
  add_icon(window);
  refresh(window);
  SetTimer(window, kPollTimer, 1000, nullptr);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  DestroyIcon(g_on);
  DestroyIcon(g_off);
  return 0;
}

}  // namespace screenlight
