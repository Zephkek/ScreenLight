#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>

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
constexpr UINT kService = 3;
constexpr UINT kStartup = 4;
constexpr UINT kNotify = 5;
constexpr UINT_PTR kPollTimer = 1;
constexpr ULONGLONG kNotifyAgainMs = 30 * 60 * 1000;
constexpr wchar_t kSettingsKey[] = L"Software\\Screen Light";
constexpr wchar_t kNotifyValue[] = L"Notifications";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Screen Light";

HINSTANCE g_instance;
UINT g_taskbar_created;
HICON g_off;
HICON g_on;
HICON g_balloon;
bool g_lit;
bool g_started;
std::map<std::wstring, ULONGLONG, NoCase> g_last_seen;
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

bool notifications_on() {
  DWORD value = 1, size = sizeof value;
  RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kNotifyValue, RRF_RT_REG_DWORD, nullptr, &value, &size);
  return value != 0;
}

void show_notifications(bool on) {
  HKEY key;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
  const DWORD value = on;
  RegSetValueExW(key, kNotifyValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof value);
  RegCloseKey(key);
}

std::wstring key_of(const Watcher& w) { return w.path.empty() ? w.name : w.path; }

std::wstring fit(std::wstring text, size_t room) {
  if (text.size() >= room) text = text.substr(0, room - 4) + L"...";
  return text;
}

void notify(HWND window, const std::vector<Watcher>& news) {
  std::wstring title, body;
  if (news.size() == 1) {
    title = news[0].name + L" " + news[0].what;
    if (!news[0].path.empty()) body = L"Location: " + news[0].path + L"\n";
  } else {
    title = std::to_wstring(news.size()) + L" programs started watching";
    for (size_t i = 0; i < news.size(); ++i) body += (i ? L", " : L"") + news[i].name;
    body += L".\n";
  }
  body += L"Click for more information.";
  NOTIFYICONDATAW data = tray_data(window);
  data.uFlags = NIF_INFO;
  data.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | NIIF_RESPECT_QUIET_TIME;
  data.hBalloonIcon = g_balloon;
  lstrcpynW(data.szInfoTitle, fit(title, ARRAYSIZE(data.szInfoTitle)).c_str(), ARRAYSIZE(data.szInfoTitle));
  lstrcpynW(data.szInfo, fit(body, ARRAYSIZE(data.szInfo)).c_str(), ARRAYSIZE(data.szInfo));
  Shell_NotifyIconW(NIM_MODIFY, &data);
}

void notify_new(HWND window, const std::vector<Watcher>& watchers, const std::vector<Watcher>& fresh) {
  const ULONGLONG now = GetTickCount64();
  std::vector<Watcher> news;
  for (const Watcher& w : fresh) {
    const auto last = g_last_seen.find(key_of(w));
    const bool quiet = last == g_last_seen.end() || now - last->second > kNotifyAgainMs;
    const bool listed = std::any_of(news.begin(), news.end(),
                                    [&](const Watcher& n) { return _wcsicmp(key_of(n).c_str(), key_of(w).c_str()) == 0; });
    if (quiet && !listed) news.push_back(w);
  }
  for (const Watcher& w : watchers) g_last_seen[key_of(w)] = now;
  if (g_started && !news.empty() && notifications_on()) notify(window, news);
  g_started = true;
}

void refresh(HWND window) {
  const std::vector<Watcher> watchers = collect_watchers();
  notify_new(window, watchers, track(watchers));
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

bool service_installed() {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!manager) return false;
  SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
  if (service) CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return service != nullptr;
}

std::wstring startup_command() {
  wchar_t self[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
  if (!length || length >= ARRAYSIZE(self)) return std::wstring();
  return L"\"" + std::wstring(self, length) + L"\" --tray";
}

bool starts_with_windows() {
  wchar_t value[MAX_PATH * 2];
  DWORD size = sizeof value;
  if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return false;
  return _wcsicmp(value, startup_command().c_str()) == 0;
}

void start_with_windows(bool on) {
  HKEY key;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
  const std::wstring command = startup_command();
  if (on && !command.empty()) {
    RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                   DWORD((command.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(key, kRunValue);
  }
  RegCloseKey(key);
}

HBITMAP shield_bitmap() {
  SHSTOCKICONINFO stock{};
  stock.cbSize = sizeof stock;
  if (FAILED(SHGetStockIconInfo(SIID_SHIELD, SHGSI_ICON | SHGSI_SMALLICON, &stock))) return nullptr;
  const int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = cx;
  bi.bmiHeader.biHeight = -cy;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HDC dc = CreateCompatibleDC(nullptr);
  HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bitmap) {
    HGDIOBJ old = SelectObject(dc, bitmap);
    DrawIconEx(dc, 0, 0, stock.hIcon, cx, cy, 0, nullptr, DI_NORMAL);
    SelectObject(dc, old);
  }
  DeleteDC(dc);
  DestroyIcon(stock.hIcon);
  return bitmap;
}

void show_menu(HWND window, int x, int y) {
  const bool installed = service_installed();
  const bool startup = starts_with_windows();
  const bool notifications = notifications_on();
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, kOpen, L"Open Screen Light");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (notifications ? MF_CHECKED : MF_UNCHECKED), kNotify, L"Show notifications");
  AppendMenuW(menu, MF_STRING | (startup ? MF_CHECKED : MF_UNCHECKED), kStartup, L"Start with Windows");
  AppendMenuW(menu, MF_STRING | (installed ? MF_CHECKED : MF_UNCHECKED), kService, L"Detect screenshots and recorders");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kExit, L"Exit");
  SetMenuDefaultItem(menu, kOpen, FALSE);
  HBITMAP shield = shield_bitmap();
  if (shield) {
    MENUITEMINFOW item{};
    item.cbSize = sizeof item;
    item.fMask = MIIM_BITMAP;
    item.hbmpItem = shield;
    SetMenuItemInfoW(menu, kService, FALSE, &item);
  }
  SetForegroundWindow(window);
  const UINT picked = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, window, nullptr);
  PostMessageW(window, WM_NULL, 0, 0);
  DestroyMenu(menu);
  if (shield) DeleteObject(shield);
  if (picked == kOpen) show_window(g_instance);
  if (picked == kStartup) start_with_windows(!startup);
  if (picked == kNotify) show_notifications(!notifications);
  if (picked == kService) run_elevated(window, installed ? L"--uninstall" : L"--install");
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
        case NIN_BALLOONUSERCLICK:
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
  g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
  const int size = GetSystemMetrics(SM_CXSMICON);
  g_off = make_light(kIdleColor, size);
  g_on = make_light(kLitColor, size);
  g_balloon = make_light(kLitColor, GetSystemMetrics(SM_CXICON));
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
  if (show) show_window(instance);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
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
