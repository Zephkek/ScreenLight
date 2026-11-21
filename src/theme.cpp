#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>

#include <string>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr DWORD kDarkBuild = 17763;
constexpr DWORD kAppModeBuild = 18362;
constexpr DWORD kNewTitleBuild = 18985;
constexpr DWORD kOldDarkTitle = 19;
constexpr DWORD kDarkTitle = 20;
constexpr UINT_PTR kGroupSubclass = 1;
constexpr UINT_PTR kFrameSubclass = 2;
constexpr UINT_PTR kListSubclass = 3;

enum AppMode { kDefaultMode, kAllowDark, kForceDark, kForceLight };

using SetAppModeFn = int(WINAPI*)(int);
using AllowAppFn = BOOL(WINAPI*)(BOOL);
using AllowWindowFn = BOOL(WINAPI*)(HWND, BOOL);
using FlushMenusFn = void(WINAPI*)();
using SetAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

struct Api {
  SetAppModeFn set_mode = nullptr;
  AllowAppFn allow_app = nullptr;
  AllowWindowFn allow_window = nullptr;
  FlushMenusFn flush_menus = nullptr;
  SetAttributeFn set_attribute = nullptr;
};

bool g_known;
bool g_dark;
Palette g_palette;
HBRUSH g_window_brush;
HBRUSH g_band_brush;

DWORD build() {
  using GetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  static const DWORD number = [] {
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof info;
    const auto get = reinterpret_cast<GetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    return get && get(&info) == 0 ? info.dwBuildNumber : 0;
  }();
  return number;
}

const Api& api() {
  static const Api found = [] {
    Api a;
    if (HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
      a.set_attribute = reinterpret_cast<SetAttributeFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    }
    if (build() < kDarkBuild) return a;
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return a;
    // undocumented uxtheme ordinals, 135 changed meaning in 1903
    FARPROC mode = GetProcAddress(ux, MAKEINTRESOURCEA(135));
    if (build() >= kAppModeBuild) {
      a.set_mode = reinterpret_cast<SetAppModeFn>(mode);
    } else {
      a.allow_app = reinterpret_cast<AllowAppFn>(mode);
    }
    a.allow_window = reinterpret_cast<AllowWindowFn>(GetProcAddress(ux, MAKEINTRESOURCEA(133)));
    a.flush_menus = reinterpret_cast<FlushMenusFn>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    return a;
  }();
  return found;
}

bool system_dark() {
  DWORD light = 1, size = sizeof light;
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                   L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS) {
    return false;
  }
  return light == 0;
}

bool high_contrast() {
  HIGHCONTRASTW contrast{};
  contrast.cbSize = sizeof contrast;
  return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON);
}

COLORREF instruction_color() {
  COLORREF color = GetSysColor(COLOR_WINDOWTEXT);
  if (HTHEME theme = OpenThemeData(nullptr, L"TEXTSTYLE")) {
    COLORREF themed;
    if (SUCCEEDED(GetThemeColor(theme, TEXT_MAININSTRUCTION, 0, TMT_TEXTCOLOR, &themed))) color = themed;
    CloseThemeData(theme);
  }
  return color;
}

void load_palette() {
  if (g_dark) {
    g_palette = {RGB(32, 32, 32),    RGB(24, 24, 24),    RGB(255, 255, 255), RGB(160, 160, 160),
                 RGB(130, 190, 245), RGB(110, 180, 255), RGB(80, 80, 80)};
  } else {
    g_palette = {GetSysColor(COLOR_WINDOW),   GetSysColor(COLOR_BTNFACE),  GetSysColor(COLOR_WINDOWTEXT),
                 GetSysColor(COLOR_GRAYTEXT), instruction_color(),         GetSysColor(COLOR_HOTLIGHT),
                 GetSysColor(COLOR_3DSHADOW)};
  }
  for (HBRUSH* brush : {&g_window_brush, &g_band_brush}) {
    if (*brush) DeleteObject(*brush);
  }
  g_window_brush = CreateSolidBrush(g_palette.window);
  g_band_brush = CreateSolidBrush(g_palette.band);
}

int dpi_of(HWND window) {
  using DpiFn = UINT(WINAPI*)(HWND);
  static const auto get = reinterpret_cast<DpiFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  if (const UINT dpi = get ? get(window) : 0) return int(dpi);
  HDC screen = GetDC(nullptr);
  const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
  ReleaseDC(nullptr, screen);
  return dpi;
}

std::wstring window_text(HWND window) {
  std::wstring text(size_t(GetWindowTextLengthW(window)) + 1, L'\0');
  text.resize(size_t(GetWindowTextW(window, text.data(), int(text.size()))));
  return text;
}

LRESULT CALLBACK group_proc(HWND box, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
  if (message != WM_PAINT || !g_dark) return DefSubclassProc(box, message, wparam, lparam);
  PAINTSTRUCT paint;
  HDC dc = BeginPaint(box, &paint);
  RECT client;
  GetClientRect(box, &client);
  HGDIOBJ old_font = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(box, WM_GETFONT, 0, 0)));
  TEXTMETRICW metrics{};
  GetTextMetricsW(dc, &metrics);
  const int dpi = dpi_of(box), inset = MulDiv(6, dpi, 96), pad = MulDiv(2, dpi, 96);
  HPEN pen = CreatePen(PS_SOLID, 1, g_palette.line);
  HGDIOBJ old_pen = SelectObject(dc, pen);
  HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  RoundRect(dc, client.left, client.top + metrics.tmHeight / 2, client.right, client.bottom, MulDiv(4, dpi, 96),
            MulDiv(4, dpi, 96));
  SelectObject(dc, old_brush);
  SelectObject(dc, old_pen);
  DeleteObject(pen);
  const std::wstring text = window_text(box);
  RECT label{client.left + inset, client.top, client.right - inset, client.top + metrics.tmHeight};
  DrawTextW(dc, text.c_str(), int(text.size()), &label, DT_SINGLELINE | DT_CALCRECT | DT_HIDEPREFIX);
  RECT back{label.left - pad, label.top, label.right + pad, label.bottom};
  FillRect(dc, &back, g_window_brush);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, IsWindowEnabled(box) ? g_palette.text : g_palette.gray);
  DrawTextW(dc, text.c_str(), int(text.size()), &label, DT_SINGLELINE | DT_HIDEPREFIX);
  SelectObject(dc, old_font);
  EndPaint(box, &paint);
  return 0;
}

LRESULT CALLBACK frame_proc(HWND control, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
  const LRESULT result = DefSubclassProc(control, message, wparam, lparam);
  if (message == WM_NCPAINT && g_dark) {
    RECT window, client;
    GetWindowRect(control, &window);
    GetClientRect(control, &client);
    MapWindowPoints(control, nullptr, reinterpret_cast<POINT*>(&client), 2);
    const int edge = client.left > window.left ? int(client.left - window.left) : 1;
    HDC dc = GetWindowDC(control);
    RECT r{0, 0, window.right - window.left, window.bottom - window.top};
    HBRUSH line = CreateSolidBrush(g_palette.line);
    FrameRect(dc, &r, line);
    DeleteObject(line);
    // the client edge can be 2px, paint every ring or the light one shows
    for (int ring = 1; ring < edge; ++ring) {
      InflateRect(&r, -1, -1);
      FrameRect(dc, &r, g_window_brush);
    }
    ReleaseDC(control, dc);
  }
  return result;
}

LRESULT CALLBACK list_proc(HWND list, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
  if (message == WM_NOTIFY && g_dark) {
    const auto* header = reinterpret_cast<const NMHDR*>(lparam);
    if (header->code == NM_CUSTOMDRAW && header->hwndFrom == ListView_GetHeader(list)) {
      const auto* draw = reinterpret_cast<const NMCUSTOMDRAW*>(lparam);
      if (draw->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
      if (draw->dwDrawStage == CDDS_ITEMPREPAINT) {
        SetTextColor(draw->hdc, g_palette.text);
        return CDRF_DODEFAULT;
      }
    }
  }
  return DefSubclassProc(list, message, wparam, lparam);
}

}  // namespace

bool dark_supported() { return build() >= kDarkBuild; }

bool update_theme() {
  const int choice = settings().theme;
  const bool dark = dark_supported() && !high_contrast() && (choice == 2 || (choice == 0 && system_dark()));
  const bool changed = !g_known || dark != g_dark;
  g_dark = dark;
  g_known = true;
  load_palette();
  if (changed) {
    if (api().set_mode) {
      api().set_mode(dark ? kForceDark : kForceLight);
    } else if (api().allow_app) {
      api().allow_app(dark);
    }
    if (api().flush_menus) api().flush_menus();
  }
  return changed;
}

bool dark_mode() {
  if (!g_known) update_theme();
  return g_dark;
}

bool light_badge() { return dark_supported() ? !dark_mode() : settings().theme != 2; }

const Palette& palette() {
  if (!g_known) update_theme();
  return g_palette;
}

HBRUSH window_brush() {
  if (!g_known) update_theme();
  return g_window_brush;
}

HBRUSH band_brush() {
  if (!g_known) update_theme();
  return g_band_brush;
}

void theme_window(HWND window) {
  const BOOL dark = dark_mode();
  if (api().allow_window) api().allow_window(window, dark);
  if (dark_supported() && api().set_attribute) {
    api().set_attribute(window, build() >= kNewTitleBuild ? kDarkTitle : kOldDarkTitle, &dark, sizeof dark);
  }
}

void theme_control(HWND control, const wchar_t* dark_class, const wchar_t* light_class) {
  if (!control) return;
  const bool dark = dark_mode();
  if (api().allow_window) api().allow_window(control, dark);
  if (dark_supported() || light_class) SetWindowTheme(control, dark ? dark_class : light_class, nullptr);
}

void theme_link(HWND link) {
  if (!link) return;
  LITEM item{};
  item.mask = LIF_ITEMINDEX | LIF_STATE;
  item.iLink = 0;
  item.stateMask = LIS_DEFAULTCOLORS;
  item.state = dark_mode() ? LIS_DEFAULTCOLORS : 0;
  SendMessageW(link, LM_SETITEM, 0, reinterpret_cast<LPARAM>(&item));
}

void theme_group(HWND group) {
  if (group) SetWindowSubclass(group, group_proc, kGroupSubclass, 0);
}

void theme_list(HWND list) {
  if (list) SetWindowSubclass(list, list_proc, kListSubclass, 0);
}

void theme_frame(HWND control) {
  if (control) SetWindowSubclass(control, frame_proc, kFrameSubclass, 0);
}

LRESULT draw_check(const NMCUSTOMDRAW* draw) {
  if (!dark_mode() || draw->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
  HWND button = draw->hdr.hwndFrom;
  HDC dc = draw->hdc;
  RECT area = draw->rc;
  FillRect(dc, &area, g_window_brush);
  const bool on = SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED;
  const bool disabled = (draw->uItemState & CDIS_DISABLED) || !IsWindowEnabled(button);
  const int state = disabled                             ? (on ? CBS_CHECKEDDISABLED : CBS_UNCHECKEDDISABLED)
                    : (draw->uItemState & CDIS_SELECTED) ? (on ? CBS_CHECKEDPRESSED : CBS_UNCHECKEDPRESSED)
                    : (draw->uItemState & CDIS_HOT)      ? (on ? CBS_CHECKEDHOT : CBS_UNCHECKEDHOT)
                                                         : (on ? CBS_CHECKEDNORMAL : CBS_UNCHECKEDNORMAL);
  const int dpi = dpi_of(button);
  SIZE box{MulDiv(13, dpi, 96), MulDiv(13, dpi, 96)};
  if (HTHEME theme = OpenThemeData(button, L"Button")) {
    GetThemePartSize(theme, dc, BP_CHECKBOX, state, nullptr, TS_DRAW, &box);
    RECT glyph{area.left, area.top + (area.bottom - area.top - box.cy) / 2, area.left + box.cx,
               area.top + (area.bottom - area.top - box.cy) / 2 + box.cy};
    DrawThemeBackground(theme, dc, BP_CHECKBOX, state, &glyph, nullptr);
    CloseThemeData(theme);
  }
  HGDIOBJ old_font = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0)));
  const std::wstring text = window_text(button);
  const UINT cues = (draw->uItemState & CDIS_SHOWKEYBOARDCUES) ? 0 : DT_HIDEPREFIX;
  RECT label{area.left + box.cx + MulDiv(4, dpi, 96), area.top, area.right, area.bottom};
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, disabled ? g_palette.gray : g_palette.text);
  DrawTextW(dc, text.c_str(), int(text.size()), &label, DT_SINGLELINE | DT_VCENTER | cues);
  if ((draw->uItemState & CDIS_FOCUS) && (draw->uItemState & CDIS_SHOWKEYBOARDCUES)) {
    RECT focus = label;
    DrawTextW(dc, text.c_str(), int(text.size()), &focus, DT_SINGLELINE | DT_CALCRECT | cues);
    OffsetRect(&focus, 0, (label.bottom - label.top - (focus.bottom - focus.top)) / 2);
    InflateRect(&focus, 1, 0);
    DrawFocusRect(dc, &focus);
  }
  SelectObject(dc, old_font);
  return CDRF_SKIPDEFAULT;
}

}  // namespace screenlight
