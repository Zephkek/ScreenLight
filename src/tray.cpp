#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <dbt.h>
#include <windowsx.h>
#include <wtsapi32.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kOpen = 1;
constexpr UINT kExit = 2;
constexpr UINT kNotify = 5;
constexpr UINT kSettings = 6;
constexpr UINT_PTR kPollTimer = 1;
constexpr ULONGLONG kNotifyAgainMs = 30 * 60 * 1000;


HINSTANCE g_instance;
UINT g_taskbar_created;
HICON g_off;
HICON g_on;
HICON g_balloon;
bool g_lit;
bool g_started;
std::map<std::wstring, ULONGLONG, NoCase> g_last_seen;
std::wstring g_tip = L"Screen Light - Nothing is watching";
std::wstring g_notified;
ULONGLONG g_snapshot_seq;

NOTIFYICONDATAW tray_data(HWND window) {
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof data;
  data.hWnd = window;
  data.uID = 1;
  return data;
}

void add_icon(HWND window) {
  NOTIFYICONDATAW data = tray_data(window);
  data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  data.uCallbackMessage = kTrayMessage;
  data.hIcon = g_lit ? g_on : g_off;
  lstrcpynW(data.szTip, g_tip.c_str(), ARRAYSIZE(data.szTip));
  Shell_NotifyIconW(NIM_ADD, &data);
  data.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &data);
}

std::wstring describe(const std::vector<Watcher>& watchers) {
  if (watchers.empty()) return L"Screen Light - Nothing is watching";
  std::wstring tip = L"Screen Light";
  for (const Watcher& w : watchers) tip += L"\n" + w.name + L" " + w.what;
  if (tip.size() > 127) tip = tip.substr(0, 124) + L"...";
  return tip;
}

std::wstring key_of(const Watcher& w) { return w.path.empty() ? w.name : w.path; }

std::wstring fit(std::wstring text, size_t room) {
  if (text.size() >= room) text = text.substr(0, room - 4) + L"...";
  return text;
}

void notify(HWND window, const std::vector<Watcher>& news) {
  g_notified = news.empty() ? std::wstring() : news[0].name + L"\t" + news[0].what + L"\t" + std::to_wstring(news[0].pid);
  std::wstring title, body;
  if (news.size() == 1) {
    title = news[0].name + L" " + news[0].what;
    body = news[0].path.empty() ? news[0].name : news[0].path;
  } else {
    title = std::to_wstring(news.size()) + L" programs started watching";
    for (size_t i = 0; i < news.size(); ++i) body += (i ? L", " : L"") + news[i].name;
  }
  NOTIFYICONDATAW data = tray_data(window);
  data.uFlags = NIF_INFO | NIF_REALTIME;
  data.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | NIIF_RESPECT_QUIET_TIME | NIIF_NOSOUND;
  data.hBalloonIcon = g_balloon;
  lstrcpynW(data.szInfoTitle, fit(title, ARRAYSIZE(data.szInfoTitle)).c_str(), ARRAYSIZE(data.szInfoTitle));
  lstrcpynW(data.szInfo, fit(body, ARRAYSIZE(data.szInfo)).c_str(), ARRAYSIZE(data.szInfo));
  Shell_NotifyIconW(NIM_MODIFY, &data);
}

void notify_new(HWND window, const std::vector<Watcher>& watchers) {
  const ULONGLONG now = GetTickCount64();
  const bool snapshot = std::any_of(watchers.begin(), watchers.end(),
                                    [](const Watcher& w) { return w.source == Source::kPrintScreen; });
  std::vector<Watcher> news;
  for (const Watcher& w : watchers) {
    if (snapshot && w.source == Source::kCapture) continue;
    const auto last = g_last_seen.find(key_of(w));
    const bool quiet = w.source == Source::kPrintScreen
                           ? print_screen_seq() != g_snapshot_seq
                           : (last == g_last_seen.end() || now - last->second > kNotifyAgainMs);
    const bool listed = std::any_of(news.begin(), news.end(),
                                    [&](const Watcher& n) { return _wcsicmp(key_of(n).c_str(), key_of(w).c_str()) == 0; });
    if (quiet && !listed) news.push_back(w);
  }
  for (const Watcher& w : watchers) g_last_seen[key_of(w)] = now;
  if (g_started && !news.empty() && settings().notifications) notify(window, news);
  if (snapshot) g_snapshot_seq = print_screen_seq();
  g_started = true;
}

void refresh(HWND window) {
  const std::vector<Watcher> watchers = collect_watchers();
  track(watchers);
  std::vector<Watcher> active;
  for (const Watcher& w : watchers) {
    if (!ignored(w) && !w.possible) active.push_back(w);
  }
  notify_new(window, active);
  show_overlay(active);
  update_flyout();
  const bool lit = !active.empty();
  std::wstring tip = describe(active);
  if (lit == g_lit && tip == g_tip) return;
  g_lit = lit;
  g_tip = std::move(tip);
  NOTIFYICONDATAW data = tray_data(window);
  data.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  data.hIcon = g_lit ? g_on : g_off;
  lstrcpynW(data.szTip, g_tip.c_str(), ARRAYSIZE(data.szTip));
  Shell_NotifyIconW(NIM_MODIFY, &data);
}

void show_menu(HWND window, int x, int y) {
  const bool notifications = settings().notifications;
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, kOpen, L"Open Screen Light");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (notifications ? MF_CHECKED : MF_UNCHECKED), kNotify, L"Show notifications");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kSettings, L"Settings");
  AppendMenuW(menu, MF_STRING, kExit, L"Exit");
  SetMenuDefaultItem(menu, kOpen, FALSE);
  SetForegroundWindow(window);
  const UINT picked = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, window, nullptr);
  PostMessageW(window, WM_NULL, 0, 0);
  DestroyMenu(menu);
  if (picked == kOpen) show_window(g_instance, window);
  if (picked == kNotify) {
    Settings s = settings();
    s.notifications = !notifications;
    save_settings(s);
  }
  if (picked == kSettings) show_settings(g_instance, window);
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
        case WM_LBUTTONDBLCLK:
          show_window(g_instance, window);
          break;
        case NIN_SELECT:
        case NIN_KEYSELECT:
          toggle_flyout(g_instance, window);
          break;
        case NIN_BALLOONUSERCLICK:
          toggle_flyout(g_instance, window, g_notified);
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
      show_window(g_instance, window);
      return 0;
    case WM_SETTINGCHANGE:
      if (wparam == SPI_SETHIGHCONTRAST ||
          (lparam && lstrcmpiW(reinterpret_cast<const wchar_t*>(lparam), L"ImmersiveColorSet") == 0)) {
        if (update_theme()) {
          close_flyout();
          retheme_window();
          retheme_settings();
        }
      }
      return 0;
    case kApplyMessage:
      if (update_theme()) {
        close_flyout();
        retheme_window();
        retheme_settings();
      }
      if (settings().watch_input || settings().watch_screen) {
        start_input_watch(window, settings().watch_input);
      } else {
        stop_input_watch();
      }
      refresh(window);
      return 0;
    case WM_ENDSESSION:
      if (wparam) DestroyWindow(window);
      return 0;
    case WM_DISPLAYCHANGE:
      displays_changed();
      refresh(window);
      return 0;
    case WM_DEVICECHANGE:
      if (wparam == DBT_DEVNODES_CHANGED) {
        devices_changed();
        refresh(window);
      }
      return TRUE;
    case WM_WTSSESSION_CHANGE:
      remote_changed();
      refresh(window);
      return 0;
    case WM_DESTROY: {
      KillTimer(window, kPollTimer);
      show_overlay({});
      NOTIFYICONDATAW data = tray_data(window);
      Shell_NotifyIconW(NIM_DELETE, &data);
      PostQuitMessage(0);
      return 0;
    }
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

void run_elevated(HWND window, const wchar_t* verb) {
  wchar_t self[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
  if (!length || length >= ARRAYSIZE(self)) return;
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof info;
  info.hwnd = window;
  info.lpVerb = L"runas";
  info.lpFile = self;
  info.lpParameters = verb;
  info.nShow = SW_SHOWNORMAL;
  ShellExecuteExW(&info);
}

int run_tray(HINSTANCE instance, bool show) {
  g_instance = instance;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  update_theme();
  g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
  RegisterApplicationRestart(L"--tray", RESTART_NO_CRASH | RESTART_NO_HANG | RESTART_NO_REBOOT);
  const int size = GetSystemMetrics(SM_CXSMICON);
  g_off = make_light(false, size);
  g_on = make_light(true, size);
  g_balloon = make_light(true, GetSystemMetrics(SM_CXICON));
  WNDCLASSW wc{};
  wc.lpfnWndProc = tray_proc;
  wc.hInstance = instance;
  wc.lpszClassName = L"ScreenLight";
  RegisterClassW(&wc);
  HWND window = CreateWindowExW(0, wc.lpszClassName, L"Screen Light", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
  if (!window) return 1;
  ChangeWindowMessageFilterEx(window, g_taskbar_created, MSGFLT_ALLOW, nullptr);
  ChangeWindowMessageFilterEx(window, kShowMessage, MSGFLT_ALLOW, nullptr);
  set_overlay_owner(window);
  add_icon(window);
  WTSRegisterSessionNotification(window, NOTIFY_FOR_ALL_SESSIONS);
  if (settings().watch_input || settings().watch_screen) start_input_watch(window, settings().watch_input);
  watch_publishers(window);
  refresh(window);
  SetTimer(window, kPollTimer, 1000, nullptr);
  if (show) {
    show_window(instance, window);
    offer_service();
  }
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (settings_message(&msg) || flyout_message(&msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  stop_input_watch();
  CoUninitialize();
  DestroyIcon(g_on);
  DestroyIcon(g_off);
  DestroyIcon(g_balloon);
  return 0;
}

}  // namespace screenlight
