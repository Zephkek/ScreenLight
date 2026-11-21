#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr wchar_t kClass[] = L"ScreenLightFlyout";
constexpr size_t kMaxRows = 6;
constexpr UINT kEndBase = 100;
constexpr UINT kOpenLink = 10;
constexpr UINT kSettingsLink = 11;
constexpr UINT kTurnOnLink = 12;
constexpr DWORD kCornerPreference = 33;
constexpr DWORD kRoundCorners = 2;

HWND g_flyout;
HWND g_owner;
HINSTANCE g_instance;
HFONT g_font;
HFONT g_bold;
HFONT g_title;
int g_dpi = 96;
bool g_busy;
ULONGLONG g_closed_at;
std::vector<Watcher> g_shown;
std::vector<HWND> g_children;
std::vector<HWND> g_quiet;
std::map<std::wstring, HICON, NoCase> g_icons;
HICON g_eye[2];

int px(int value) { return MulDiv(value, g_dpi, 96); }

int window_dpi(HWND window) {
  using DpiFn = UINT(WINAPI*)(HWND);
  static const auto get = reinterpret_cast<DpiFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  if (const UINT dpi = get && window ? get(window) : 0) return int(dpi);
  HDC screen = GetDC(nullptr);
  const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
  ReleaseDC(nullptr, screen);
  return dpi;
}

void make_fonts() {
  using InfoFn = BOOL(WINAPI*)(UINT, UINT, void*, UINT, UINT);
  static const auto get = reinterpret_cast<InfoFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi"));
  NONCLIENTMETRICSW metrics{};
  metrics.cbSize = sizeof metrics;
  if (!get || !get(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0, UINT(g_dpi))) {
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0);
    HDC screen = GetDC(nullptr);
    metrics.lfMessageFont.lfHeight = MulDiv(metrics.lfMessageFont.lfHeight, g_dpi, GetDeviceCaps(screen, LOGPIXELSY));
    ReleaseDC(nullptr, screen);
  }
  for (HFONT* font : {&g_font, &g_bold, &g_title}) {
    if (*font) DeleteObject(*font);
  }
  g_font = CreateFontIndirectW(&metrics.lfMessageFont);
  LOGFONTW bold = metrics.lfMessageFont;
  bold.lfWeight = FW_SEMIBOLD;
  g_bold = CreateFontIndirectW(&bold);
  LOGFONTW title = metrics.lfMessageFont;
  title.lfHeight = MulDiv(title.lfHeight, 12, 9);
  g_title = CreateFontIndirectW(&title);
}

void drop_icons() {
  for (auto& [path, icon] : g_icons) {
    if (icon) DestroyIcon(icon);
  }
  g_icons.clear();
  for (HICON& icon : g_eye) {
    if (icon) DestroyIcon(icon);
    icon = nullptr;
  }
}

HICON program_icon(std::wstring path) {
  if (path.empty()) path = L"?";
  if (const auto it = g_icons.find(path); it != g_icons.end()) return it->second;
  HICON icon = nullptr;
  const int size = px(24);
  if (SHDefExtractIconW(path.c_str(), 0, 0, &icon, nullptr, MAKELONG(size, size)) != S_OK) icon = nullptr;
  if (!icon && FAILED(LoadIconWithScaleDown(nullptr, IDI_APPLICATION, size, size, &icon))) icon = nullptr;
  g_icons.emplace(path, icon);
  return icon;
}

HWND child(const wchar_t* type, const wchar_t* text, DWORD style, int x, int y, int w, int h, UINT id, HFONT font) {
  HWND control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_flyout,
                                 reinterpret_cast<HMENU>(UINT_PTR(id)), g_instance, nullptr);
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
  g_children.push_back(control);
  return control;
}

HWND label(const std::wstring& text, int x, int y, int w, int h, HFONT font, bool quiet) {
  HWND control = child(WC_STATICW, text.c_str(), SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, x, y, w, h, 0, font);
  if (quiet) g_quiet.push_back(control);
  return control;
}

HWND picture(HICON icon, int x, int y, int size) {
  HWND control = child(WC_STATICW, L"", SS_ICON | SS_REALSIZECONTROL | SS_CENTERIMAGE, x, y, size, size, 0, g_font);
  SendMessageW(control, STM_SETICON, reinterpret_cast<WPARAM>(icon), 0);
  return control;
}

int text_height(HFONT font, const std::wstring& text, int width) {
  HDC dc = GetDC(g_flyout);
  HGDIOBJ old = SelectObject(dc, font);
  RECT r{0, 0, width, 0};
  DrawTextW(dc, text.c_str(), int(text.size()), &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  SelectObject(dc, old);
  ReleaseDC(g_flyout, dc);
  return r.bottom;
}

std::wstring headline(size_t count) {
  if (count == 0) return L"Nothing is watching right now";
  if (count == 1) return L"1 program is watching right now";
  return std::to_wstring(count) + L" programs are watching right now";
}

SIZE build() {
  SendMessageW(g_flyout, WM_SETREDRAW, FALSE, 0);
  for (HWND control : g_children) DestroyWindow(control);
  g_children.clear();
  g_quiet.clear();
  const int pad = px(14), width = px(340), inner = width - 2 * pad;
  const int eye = px(32);
  if (!g_eye[0]) g_eye[0] = make_light(false, eye);
  if (!g_eye[1]) g_eye[1] = make_light(true, eye);
  int y = pad;
  picture(g_eye[g_shown.empty() ? 0 : 1], pad, y, eye);
  const int text_x = pad + eye + px(10), text_w = width - text_x - pad;
  const std::wstring title = headline(g_shown.size());
  const int title_h = text_height(g_title, title, text_w);
  const std::wstring sub = g_shown.empty() ? L"Screen Light lights up when a program reads your screen or your input."
                                           : L"End a program if you don't know why it is watching.";
  const int sub_h = text_height(g_font, sub, text_w);
  const int head = std::max(eye, title_h + px(2) + sub_h);
  label(title, text_x, y + (head - title_h - px(2) - sub_h) / 2, text_w, title_h, g_title, false);
  HWND note = child(WC_STATICW, sub.c_str(), SS_LEFT | SS_NOPREFIX, text_x, y + (head + title_h + px(2) - sub_h) / 2, text_w,
                    sub_h, 0, g_font);
  g_quiet.push_back(note);
  y += head + px(12);
  const int row_h = px(40), icon = px(24), end_w = px(64), end_h = px(26);
  for (size_t i = 0; i < g_shown.size() && i < kMaxRows; ++i) {
    const Watcher& w = g_shown[i];
    const int top = y + int(i) * row_h;
    if (HICON program = program_icon(w.path)) picture(program, pad, top + (row_h - icon) / 2, icon);
    const bool endable = w.pid && can_end(w.pid);
    const int text_left = pad + icon + px(10), text_right = width - pad - (endable ? end_w + px(8) : 0);
    const int line = px(17);
    label(w.name, text_left, top + px(3), text_right - text_left, line, g_bold, false);
    label(w.what, text_left, top + px(3) + line, text_right - text_left, line, g_font, true);
    if (endable) {
      child(WC_BUTTONW, L"End", WS_TABSTOP | BS_PUSHBUTTON, width - pad - end_w, top + (row_h - end_h) / 2, end_w, end_h,
            kEndBase + UINT(i), g_font);
    }
  }
  y += int(std::min(g_shown.size(), kMaxRows)) * row_h;
  if (g_shown.size() > kMaxRows) {
    label(L"And " + std::to_wstring(g_shown.size() - kMaxRows) + L" more in Screen Light", pad, y, inner, px(20), g_font, true);
    y += px(24);
  }
  if (!service_running()) {
    const std::wstring text = L"Full detection is off, so screen recorders and keyboard listeners won't show. <a>Turn on</a>";
    const int h = text_height(g_font, L"Full detection is off, so screen recorders and keyboard listeners won't show. Turn on", inner) + px(2);
    child(WC_LINK, text.c_str(), WS_TABSTOP, pad, y + px(4), inner, h, kTurnOnLink, g_font);
    y += h + px(10);
  }
  y += px(6);
  const int footer = px(20);
  child(WC_LINK, L"<a>Open Screen Light</a>", WS_TABSTOP, pad, y, inner / 2, footer, kOpenLink, g_font);
  child(WC_LINK, L"<a>Settings</a>", WS_TABSTOP | LWS_RIGHT, pad + inner / 2, y, inner - inner / 2, footer, kSettingsLink, g_font);
  y += footer + pad;
  SendMessageW(g_flyout, WM_SETREDRAW, TRUE, 0);
  RedrawWindow(g_flyout, nullptr, nullptr, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
  return {width, y};
}

RECT tray_anchor() {
  RECT icon{};
  NOTIFYICONIDENTIFIER id{};
  id.cbSize = sizeof id;
  id.hWnd = g_owner;
  id.uID = 1;
  if (SUCCEEDED(Shell_NotifyIconGetRect(&id, &icon))) return icon;
  POINT at;
  GetCursorPos(&at);
  return {at.x, at.y, at.x + 1, at.y + 1};
}

void place(SIZE size) {
  const RECT icon = tray_anchor();
  RECT window{0, 0, size.cx, size.cy};
  AdjustWindowRectEx(&window, DWORD(GetWindowLongW(g_flyout, GWL_STYLE)), FALSE, DWORD(GetWindowLongW(g_flyout, GWL_EXSTYLE)));
  SIZE outer{window.right - window.left, window.bottom - window.top};
  POINT anchor{(icon.left + icon.right) / 2, (icon.top + icon.bottom) / 2};
  RECT out{};
  if (!CalculatePopupWindowPosition(&anchor, &outer, TPM_CENTERALIGN | TPM_BOTTOMALIGN | TPM_VERTICAL | TPM_WORKAREA,
                                    const_cast<RECT*>(&icon), &out)) {
    out = {anchor.x - outer.cx, anchor.y - outer.cy, anchor.x, anchor.y};
  }
  SetWindowPos(g_flyout, HWND_TOPMOST, out.left, out.top, outer.cx, outer.cy, SWP_NOACTIVATE);
}

void hide() {
  if (!g_flyout) return;
  DestroyWindow(g_flyout);
}

void clicked(UINT id) {
  if (id >= kEndBase && id < kEndBase + kMaxRows && id - kEndBase < g_shown.size()) {
    const Watcher w = g_shown[id - kEndBase];
    g_busy = true;
    const bool ended = end_program(g_flyout, w.pid, w.name);
    g_busy = false;
    if (ended && g_owner) PostMessageW(g_owner, kRefreshMessage, 0, 0);
    if (g_flyout) SetForegroundWindow(g_flyout);
  }
}

void link(UINT id) {
  HWND owner = g_owner;
  HINSTANCE instance = g_instance;
  if (id == kTurnOnLink) {
    g_busy = true;
    run_elevated(g_flyout, L"--install");
    g_busy = false;
    hide();
  } else if (id == kOpenLink) {
    hide();
    show_window(instance, owner);
  } else if (id == kSettingsLink) {
    hide();
    show_settings(instance, owner);
  }
}

LRESULT CALLBACK flyout_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_ACTIVATE:
      if (LOWORD(wparam) == WA_INACTIVE && !g_busy) {
        g_closed_at = GetTickCount64();
        PostMessageW(window, WM_CLOSE, 0, 0);
      }
      return 0;
    case WM_COMMAND:
      if (LOWORD(wparam) == IDCANCEL) {
        PostMessageW(window, WM_CLOSE, 0, 0);
      } else if (HIWORD(wparam) == BN_CLICKED) {
        clicked(LOWORD(wparam));
      }
      return 0;
    case WM_NOTIFY: {
      const auto* header = reinterpret_cast<const NMHDR*>(lparam);
      if (header->code == NM_CLICK || header->code == NM_RETURN) link(UINT(header->idFrom));
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wparam);
      const bool quiet = std::find(g_quiet.begin(), g_quiet.end(), reinterpret_cast<HWND>(lparam)) != g_quiet.end();
      SetTextColor(dc, GetSysColor(quiet ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
      SetBkColor(dc, GetSysColor(COLOR_WINDOW));
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_KEYDOWN:
      if (wparam == VK_ESCAPE) PostMessageW(window, WM_CLOSE, 0, 0);
      return 0;
    case WM_CLOSE:
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      g_children.clear();
      g_quiet.clear();
      drop_icons();
      g_flyout = nullptr;
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void register_class() {
  static bool done = false;
  if (done) return;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.style = CS_DROPSHADOW;
  wc.lpfnWndProc = flyout_proc;
  wc.hInstance = g_instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);
  done = true;
}

void round_corners() {
  using SetAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
  HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
  const auto set = dwm ? reinterpret_cast<SetAttributeFn>(GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr;
  if (set) set(g_flyout, kCornerPreference, &kRoundCorners, sizeof kRoundCorners);
}

}  // namespace

bool flyout_message(MSG* msg) { return g_flyout && IsDialogMessageW(g_flyout, msg); }

void toggle_flyout(HINSTANCE instance, HWND owner, const std::vector<Watcher>& active) {
  if (g_flyout) {
    hide();
    return;
  }
  if (GetTickCount64() - g_closed_at < 400) return;
  g_instance = instance;
  g_owner = owner;
  g_shown = active;
  register_class();
  const RECT icon = tray_anchor();
  g_flyout = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_CONTROLPARENT, kClass, L"Screen Light",
                             WS_POPUP | WS_BORDER, icon.left, icon.top, 1, 1, owner, nullptr, instance, nullptr);
  if (!g_flyout) return;
  g_dpi = window_dpi(g_flyout);
  make_fonts();
  round_corners();
  place(build());
  ShowWindow(g_flyout, SW_SHOW);
  SetForegroundWindow(g_flyout);
  if (!g_children.empty()) {
    HWND first = GetNextDlgTabItem(g_flyout, nullptr, FALSE);
    if (first) SetFocus(first);
  }
}

void update_flyout(const std::vector<Watcher>& active) {
  if (!g_flyout || g_busy) return;
  const auto same = [](const Watcher& a, const Watcher& b) {
    return a.source == b.source && a.pid == b.pid && a.name == b.name && a.what == b.what;
  };
  if (active.size() == g_shown.size() && std::equal(active.begin(), active.end(), g_shown.begin(), same)) return;
  g_shown = active;
  HWND focus = GetFocus();
  const int focused = focus ? GetDlgCtrlID(focus) : 0;
  place(build());
  if (focused) {
    if (HWND again = GetDlgItem(g_flyout, focused)) SetFocus(again);
  }
}

bool flyout_visible() { return g_flyout != nullptr; }

}  // namespace screenlight
