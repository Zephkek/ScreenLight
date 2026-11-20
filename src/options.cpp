#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

#include "resource.h"
#include "screenlight.h"

namespace screenlight {
namespace {

constexpr UINT_PTR kServiceTimer = 1;
constexpr DWORD kKeyboardBuild = 17763;

HWND g_dialog;
HWND g_tray;
HICON g_small;
HICON g_big;

DWORD windows_build() {
  using GetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  static const DWORD build = [] {
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof info;
    const auto get = reinterpret_cast<GetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    return get && get(&info) == 0 ? info.dwBuildNumber : 0;
  }();
  return build;
}

void set_check(HWND dialog, int id, bool on) { CheckDlgButton(dialog, id, on ? BST_CHECKED : BST_UNCHECKED); }

bool checked(HWND dialog, int id) { return IsDlgButtonChecked(dialog, id) == BST_CHECKED; }

void show_service(HWND dialog) {
  const bool installed = service_installed();
  const bool running = service_running();
  const wchar_t* text = running     ? L"The service is running."
                        : installed ? L"The service is installed but not answering."
                                    : L"The service is not installed.";
  SetDlgItemTextW(dialog, IDC_SERVICE_TEXT, text);
  SetDlgItemTextW(dialog, IDC_SERVICE, installed ? L"Remove service" : L"Install service");
  Button_SetElevationRequiredState(GetDlgItem(dialog, IDC_SERVICE), TRUE);
  const bool keyboard = windows_build() >= kKeyboardBuild;
  const wchar_t* needs = running    ? L""
                         : keyboard ? L"Screenshots, recorders and keyboard listeners need the service."
                                    : L"Screenshots and recorders need the service.";
  SetDlgItemTextW(dialog, IDC_NEEDS, needs);
}

void show_corner(HWND dialog) {
  const bool badge = ComboBox_GetCurSel(GetDlgItem(dialog, IDC_OVERLAY)) == kOverlayBadge;
  EnableWindow(GetDlgItem(dialog, IDC_CORNER), badge);
  EnableWindow(GetDlgItem(dialog, IDC_CORNER_LABEL), badge);
}

void fit_list(HWND list) {
  HDC dc = GetDC(list);
  HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0)));
  int widest = 0;
  for (int i = 0, n = ListBox_GetCount(list); i < n; ++i) {
    std::vector<wchar_t> text(size_t(ListBox_GetTextLen(list, i)) + 1);
    ListBox_GetText(list, i, text.data());
    SIZE size{};
    GetTextExtentPoint32W(dc, text.data(), int(wcslen(text.data())), &size);
    widest = std::max<int>(widest, size.cx);
  }
  SelectObject(dc, old);
  ReleaseDC(list, dc);
  ListBox_SetHorizontalExtent(list, widest + GetSystemMetrics(SM_CXEDGE) * 4);
}

void show_remove(HWND dialog) {
  EnableWindow(GetDlgItem(dialog, IDC_REMOVE), ListBox_GetCurSel(GetDlgItem(dialog, IDC_IGNORED)) != LB_ERR);
}

void add_program(HWND dialog) {
  wchar_t path[MAX_PATH * 2] = L"";
  OPENFILENAMEW open{};
  open.lStructSize = sizeof open;
  open.hwndOwner = dialog;
  open.lpstrFilter = L"Programs (*.exe)\0*.exe\0All files\0*.*\0";
  open.lpstrFile = path;
  open.nMaxFile = ARRAYSIZE(path);
  open.lpstrTitle = L"Choose a program to ignore";
  open.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_DONTADDTORECENT;
  if (!GetOpenFileNameW(&open)) return;
  HWND list = GetDlgItem(dialog, IDC_IGNORED);
  if (ListBox_FindStringExact(list, -1, path) == LB_ERR) ListBox_SetCurSel(list, ListBox_AddString(list, path));
  fit_list(list);
  show_remove(dialog);
}

void fill(HWND dialog) {
  const Settings& s = settings();
  set_check(dialog, IDC_STARTUP, starts_with_windows());
  set_check(dialog, IDC_NOTIFY, s.notifications);
  set_check(dialog, IDC_SCREEN, s.watch_screen);
  set_check(dialog, IDC_INPUT, s.watch_input);
  set_check(dialog, IDC_KEYBOARD, s.watch_keyboard);
  set_check(dialog, IDC_REMOTE, s.watch_remote);
  if (windows_build() < kKeyboardBuild) {
    SetDlgItemTextW(dialog, IDC_KEYBOARD, L"Programs listening to the keyboard (Windows 10 1809 or later)");
    set_check(dialog, IDC_KEYBOARD, false);
    EnableWindow(GetDlgItem(dialog, IDC_KEYBOARD), FALSE);
  }
  HWND overlay = GetDlgItem(dialog, IDC_OVERLAY);
  for (const wchar_t* text : {L"Nothing", L"A badge in the corner", L"A red frame around the screen"}) {
    ComboBox_AddString(overlay, text);
  }
  ComboBox_SetCurSel(overlay, s.overlay);
  HWND corner = GetDlgItem(dialog, IDC_CORNER);
  for (const wchar_t* text : {L"Top right", L"Top left", L"Bottom right", L"Bottom left"}) ComboBox_AddString(corner, text);
  ComboBox_SetCurSel(corner, s.corner);
  HWND list = GetDlgItem(dialog, IDC_IGNORED);
  for (const std::wstring& path : s.ignored) ListBox_AddString(list, path.c_str());
  fit_list(list);
  std::wstring about = L"Screen Light 0.1.0";
  if (portable()) about += L" (portable)";
  SetDlgItemTextW(dialog, IDC_ABOUT, about.c_str());
  show_corner(dialog);
  show_remove(dialog);
  show_service(dialog);
}

void save(HWND dialog) {
  Settings s = settings();
  s.notifications = checked(dialog, IDC_NOTIFY);
  s.watch_screen = checked(dialog, IDC_SCREEN);
  s.watch_input = checked(dialog, IDC_INPUT);
  if (IsWindowEnabled(GetDlgItem(dialog, IDC_KEYBOARD))) s.watch_keyboard = checked(dialog, IDC_KEYBOARD);
  s.watch_remote = checked(dialog, IDC_REMOTE);
  s.overlay = std::max(0, ComboBox_GetCurSel(GetDlgItem(dialog, IDC_OVERLAY)));
  s.corner = std::max(0, ComboBox_GetCurSel(GetDlgItem(dialog, IDC_CORNER)));
  s.ignored.clear();
  HWND list = GetDlgItem(dialog, IDC_IGNORED);
  for (int i = 0, n = ListBox_GetCount(list); i < n; ++i) {
    std::vector<wchar_t> text(size_t(ListBox_GetTextLen(list, i)) + 1);
    ListBox_GetText(list, i, text.data());
    s.ignored.emplace_back(text.data());
  }
  save_settings(s);
  if (checked(dialog, IDC_STARTUP) != starts_with_windows()) start_with_windows(checked(dialog, IDC_STARTUP));
  if (g_tray) PostMessageW(g_tray, kApplyMessage, 0, 0);
}

INT_PTR CALLBACK options_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM) {
  switch (message) {
    case WM_INITDIALOG:
      g_dialog = dialog;
      SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_small));
      SendMessageW(dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_big));
      fill(dialog);
      SetTimer(dialog, kServiceTimer, 1000, nullptr);
      return TRUE;
    case WM_TIMER:
      if (wparam == kServiceTimer) show_service(dialog);
      return TRUE;
    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case IDC_OVERLAY:
          if (HIWORD(wparam) == CBN_SELCHANGE) show_corner(dialog);
          return TRUE;
        case IDC_IGNORED:
          if (HIWORD(wparam) == LBN_SELCHANGE) show_remove(dialog);
          return TRUE;
        case IDC_ADD:
          add_program(dialog);
          return TRUE;
        case IDC_REMOVE: {
          HWND list = GetDlgItem(dialog, IDC_IGNORED);
          const int at = ListBox_GetCurSel(list);
          if (at != LB_ERR) ListBox_DeleteString(list, at);
          ListBox_SetCurSel(list, std::min(at, ListBox_GetCount(list) - 1));
          fit_list(list);
          show_remove(dialog);
          return TRUE;
        }
        case IDC_SERVICE:
          run_elevated(dialog, service_installed() ? L"--uninstall" : L"--install");
          return TRUE;
        case IDOK:
          save(dialog);
          DestroyWindow(dialog);
          return TRUE;
        case IDCANCEL:
          DestroyWindow(dialog);
          return TRUE;
      }
      break;
    case WM_DESTROY:
      KillTimer(dialog, kServiceTimer);
      g_dialog = nullptr;
      return TRUE;
  }
  return FALSE;
}

}  // namespace

bool settings_message(MSG* msg) { return g_dialog && IsDialogMessageW(g_dialog, msg); }

void show_settings(HINSTANCE instance, HWND tray) {
  g_tray = tray;
  if (g_dialog) {
    ShowWindow(g_dialog, SW_RESTORE);
    SetForegroundWindow(g_dialog);
    return;
  }
  if (!g_small) {
    g_small = make_light(kIdleColor, GetSystemMetrics(SM_CXSMICON));
    g_big = make_light(kIdleColor, GetSystemMetrics(SM_CXICON));
  }
  if (HWND dialog = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr, options_proc, 0)) {
    ShowWindow(dialog, SW_SHOW);
    SetForegroundWindow(dialog);
  }
}

}  // namespace screenlight
