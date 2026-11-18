#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>

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
constexpr UINT_PTR kPollTimer = 1;
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Screen Light";

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
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, kOpen, L"Open Screen Light");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
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
  return 0;
}

}  // namespace screenlight
