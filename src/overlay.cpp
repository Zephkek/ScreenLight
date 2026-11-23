#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr wchar_t kFrameClass[] = L"ScreenLightOverlay";
constexpr wchar_t kBadgeClass[] = L"ScreenLightBadge";
constexpr DWORD kExcludeFromCapture = 0x11;
// before 2004 the badge shows up in screen recordings
constexpr DWORD kExcludeBuild = 19041;
struct Colors {
  COLORREF fill, hot, edge, text;
};

constexpr Colors kDarkBadge = {RGB(32, 32, 32), RGB(50, 50, 50), RGB(92, 92, 92), RGB(255, 255, 255)};
constexpr Colors kLightBadge = {RGB(251, 251, 251), RGB(240, 240, 240), RGB(204, 204, 204), RGB(26, 26, 26)};
constexpr int kMinScale = 60;
constexpr int kMaxScale = 300;
constexpr UINT kMenuOpen = 1;
constexpr UINT kMenuReset = 2;
constexpr UINT kMenuOff = 3;
constexpr int kLeft = 1;
constexpr int kRight = 2;
constexpr int kTop = 4;
constexpr int kBottom = 8;
constexpr int kMove = 16;

struct Badge {
  HWND window = nullptr;
  std::wstring text;
  std::wstring key;
  bool lit = false;
  bool light = false;
  bool hot = false;
  int dpi = 96;
  int scale = 100;
  RECT rect{};
  int drag = 0;
  bool moved = false;
  POINT grab{};
  RECT start{};
  int start_scale = 100;
};

struct Layout {
  int pad, dot, gap, text, width, height;
};

Badge g_badge;
HWND g_owner;
int g_preview = -1;
std::vector<Watcher> g_active;
std::vector<HWND> g_frame;
std::vector<RECT> g_frame_monitors;

DWORD build_number() {
  using GetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  static const DWORD build = [] {
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof info;
    const auto get = reinterpret_cast<GetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    return get && get(&info) == 0 ? info.dwBuildNumber : 0;
  }();
  return build;
}

UINT monitor_dpi(HMONITOR monitor) {
  using GetDpiFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
  static const auto get = [] {
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    return shcore ? reinterpret_cast<GetDpiFn>(GetProcAddress(shcore, "GetDpiForMonitor")) : nullptr;
  }();
  UINT x = 0, y = 0;
  if (get && SUCCEEDED(get(monitor, 0, &x, &y)) && x) return x;
  HDC screen = GetDC(nullptr);
  const UINT dpi = UINT(GetDeviceCaps(screen, LOGPIXELSX));
  ReleaseDC(nullptr, screen);
  return dpi;
}

RECT work_area(HMONITOR monitor) {
  MONITORINFO info{};
  info.cbSize = sizeof info;
  GetMonitorInfoW(monitor, &info);
  return info.rcWork;
}

int scaled(int value, int dpi, int scale) { return std::max(1, MulDiv(MulDiv(value, dpi, 96), scale, 100)); }

LRESULT CALLBACK frame_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCHITTEST) return HTTRANSPARENT;
  return DefWindowProcW(window, message, wparam, lparam);
}

HWND make_frame_window() {
  static bool registered = false;
  HINSTANCE instance = GetModuleHandleW(nullptr);
  if (!registered) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = frame_proc;
    wc.hInstance = instance;
    wc.hbrBackground = CreateSolidBrush(kLitColor);
    wc.lpszClassName = kFrameClass;
    registered = RegisterClassW(&wc) != 0;
  }
  HWND window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                kFrameClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
  if (window && build_number() >= kExcludeBuild) SetWindowDisplayAffinity(window, kExcludeFromCapture);
  return window;
}

BOOL CALLBACK add_monitor(HMONITOR monitor, HDC, RECT*, LPARAM param) {
  MONITORINFO info{};
  info.cbSize = sizeof info;
  if (GetMonitorInfoW(monitor, &info)) reinterpret_cast<std::vector<RECT>*>(param)->push_back(info.rcMonitor);
  return TRUE;
}

std::vector<RECT> monitors() {
  std::vector<RECT> all;
  EnumDisplayMonitors(nullptr, nullptr, add_monitor, reinterpret_cast<LPARAM>(&all));
  return all;
}

bool same_rects(const std::vector<RECT>& a, const std::vector<RECT>& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const RECT& x, const RECT& y) { return EqualRect(&x, &y); });
}

void hide_frame() {
  for (HWND window : g_frame) DestroyWindow(window);
  g_frame.clear();
  g_frame_monitors.clear();
}

void show_frame() {
  const std::vector<RECT> now = monitors();
  if (!g_frame.empty() && same_rects(now, g_frame_monitors)) return;
  hide_frame();
  for (const RECT& m : now) {
    const int edge = std::max(2, MulDiv(4, int(monitor_dpi(MonitorFromRect(&m, MONITOR_DEFAULTTONEAREST))), 96));
    const RECT sides[] = {{m.left, m.top, m.right, m.top + edge},
                          {m.left, m.bottom - edge, m.right, m.bottom},
                          {m.left, m.top + edge, m.left + edge, m.bottom - edge},
                          {m.right - edge, m.top + edge, m.right, m.bottom - edge}};
    for (const RECT& side : sides) {
      HWND window = make_frame_window();
      if (!window) continue;
      SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
      SetWindowPos(window, HWND_TOPMOST, side.left, side.top, side.right - side.left, side.bottom - side.top,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
      g_frame.push_back(window);
    }
  }
  g_frame_monitors = now;
}

HFONT badge_font(int dpi, int scale) {
  NONCLIENTMETRICSW metrics{};
  metrics.cbSize = sizeof metrics;
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0);
  HDC screen = GetDC(nullptr);
  const int system_dpi = GetDeviceCaps(screen, LOGPIXELSY);
  ReleaseDC(nullptr, screen);
  LOGFONTW face = metrics.lfMessageFont;
  face.lfHeight = MulDiv(MulDiv(face.lfHeight, dpi, system_dpi), scale, 100);
  face.lfQuality = CLEARTYPE_QUALITY;
  return CreateFontIndirectW(&face);
}

Layout measure(HDC dc, const std::wstring& text, int dpi, int scale) {
  Layout l{};
  l.pad = scaled(12, dpi, scale);
  l.dot = scaled(10, dpi, scale);
  l.gap = scaled(8, dpi, scale);
  const int room = scaled(420, dpi, scale);
  RECT r{0, 0, room, 0};
  DrawTextW(dc, text.c_str(), -1, &r, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX | DT_END_ELLIPSIS);
  l.text = std::min<int>(r.right, room);
  l.height = std::max<int>(r.bottom, l.dot) + scaled(14, dpi, scale);
  l.width = l.pad + l.dot + l.gap + l.text + l.pad;
  return l;
}

SIZE badge_size(const std::wstring& text, int dpi, int scale) {
  HDC screen = GetDC(nullptr);
  HDC memory = CreateCompatibleDC(screen);
  HFONT font = badge_font(dpi, scale);
  HGDIOBJ old = SelectObject(memory, font);
  const Layout l = measure(memory, text, dpi, scale);
  SelectObject(memory, old);
  DeleteObject(font);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
  return {l.width, l.height};
}

float mix(float from, float to, float t) { return from + (to - from) * t; }

HBITMAP draw_badge(const std::wstring& text, bool lit, bool light, bool hot, int dpi, int scale, SIZE* size) {
  HDC screen = GetDC(nullptr);
  HDC memory = CreateCompatibleDC(screen);
  HFONT font = badge_font(dpi, scale);
  HGDIOBJ old_font = SelectObject(memory, font);
  const Layout l = measure(memory, text, dpi, scale);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = l.width;
  bi.bmiHeader.biHeight = -l.height;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(memory, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bitmap) {
    HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
    auto* pixel = static_cast<uint32_t*>(bits);
    const Colors& colors = light ? kLightBadge : kDarkBadge;
    const COLORREF fill = hot ? colors.hot : colors.fill;
    const COLORREF dot = lit ? kLitColor : kIdleColor;
    const float radius = l.height / 2.0f, stroke = std::max(1.0f, dpi / 96.0f);
    const float dot_x = l.pad + l.dot / 2.0f - 0.5f, dot_y = l.height / 2.0f - 0.5f, dot_r = l.dot / 2.0f;
    for (int y = 0; y < l.height; ++y) {
      for (int x = 0; x < l.width; ++x) {
        const float cx = std::clamp(x + 0.5f, radius, l.width - radius);
        const float inside = radius - std::hypot(x + 0.5f - cx, y + 0.5f - radius);
        const float edge = std::clamp(stroke + 0.5f - inside, 0.0f, 1.0f);
        const float spot = std::clamp(dot_r - std::hypot(x - dot_x, y - dot_y) + 0.5f, 0.0f, 1.0f);
        const auto channel = [&](BYTE base, BYTE line, BYTE color) {
          return uint32_t(mix(mix(base, line, edge), color, spot) + 0.5f);
        };
        const uint32_t r = channel(GetRValue(fill), GetRValue(colors.edge), GetRValue(dot));
        const uint32_t g = channel(GetGValue(fill), GetGValue(colors.edge), GetGValue(dot));
        const uint32_t bl = channel(GetBValue(fill), GetBValue(colors.edge), GetBValue(dot));
        pixel[y * l.width + x] = 0xff000000u | r << 16 | g << 8 | bl;
      }
    }
    SetBkMode(memory, TRANSPARENT);
    SetTextColor(memory, colors.text);
    RECT place{l.pad + l.dot + l.gap, 0, l.pad + l.dot + l.gap + l.text, l.height};
    DrawTextW(memory, text.c_str(), -1, &place, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    GdiFlush();
    for (int y = 0; y < l.height; ++y) {
      for (int x = 0; x < l.width; ++x) {
        const float cx = std::clamp(x + 0.5f, radius, l.width - radius);
        const float a = std::clamp(radius - std::hypot(x + 0.5f - cx, y + 0.5f - radius) + 0.5f, 0.0f, 1.0f);
        uint32_t& p = pixel[y * l.width + x];
        const uint32_t r = uint32_t(((p >> 16) & 0xff) * a), g = uint32_t(((p >> 8) & 0xff) * a), bl = uint32_t((p & 0xff) * a);
        p = uint32_t(a * 255 + 0.5f) << 24 | r << 16 | g << 8 | bl;
      }
    }
    SelectObject(memory, old_bitmap);
  }
  SelectObject(memory, old_font);
  DeleteObject(font);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
  *size = {l.width, l.height};
  return bitmap;
}

void paint() {
  Badge& b = g_badge;
  if (!b.window) return;
  SIZE size{};
  HBITMAP bitmap = draw_badge(b.text, b.lit, b.light, b.hot || b.drag, b.dpi, b.scale, &size);
  if (!bitmap) return;
  HDC screen = GetDC(nullptr);
  HDC memory = CreateCompatibleDC(screen);
  HGDIOBJ old = SelectObject(memory, bitmap);
  POINT at{b.rect.left, b.rect.top};
  POINT origin{0, 0};
  BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(b.window, screen, &at, &size, memory, &origin, 0, &blend, ULW_ALPHA);
  b.rect.right = b.rect.left + size.cx;
  b.rect.bottom = b.rect.top + size.cy;
  SelectObject(memory, old);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
  DeleteObject(bitmap);
}

RECT rect_at(POINT p, int anchor, SIZE size) {
  const int across = anchor % 3, down = anchor / 3;
  const int left = across == 0 ? p.x : across == 1 ? p.x - size.cx / 2 : p.x - size.cx;
  const int top = down == 0 ? p.y : p.y - size.cy;
  return {left, top, left + size.cx, top + size.cy};
}

POINT anchor_of(const RECT& r, int anchor) {
  const int across = anchor % 3, down = anchor / 3;
  return {across == 0 ? r.left : across == 1 ? (r.left + r.right) / 2 : r.right, down == 0 ? r.top : r.bottom};
}

int anchor_for(const RECT& r, const RECT& work) {
  const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, third = (work.right - work.left) / 3;
  const int across = cx < work.left + third ? 0 : cx >= work.right - third ? 2 : 1;
  return (cy < (work.top + work.bottom) / 2 ? 0 : 3) + across;
}

RECT clamp_into(const RECT& r, const RECT& work) {
  const int w = r.right - r.left, h = r.bottom - r.top;
  const int left = std::clamp(int(r.left), int(work.left), std::max(int(work.left), int(work.right) - w));
  const int top = std::clamp(int(r.top), int(work.top), std::max(int(work.top), int(work.bottom) - h));
  return {left, top, left + w, top + h};
}

RECT snap(const RECT& r, const RECT& work, int dpi) {
  const int margin = scaled(12, dpi, 100), reach = scaled(10, dpi, 100);
  const int w = r.right - r.left, h = r.bottom - r.top, middle = (work.left + work.right) / 2;
  int left = r.left, top = r.top;
  if (std::abs(left - int(work.left + margin)) < reach) {
    left = work.left + margin;
  } else if (std::abs(left + w - int(work.right - margin)) < reach) {
    left = work.right - margin - w;
  } else if (std::abs(left + w / 2 - middle) < reach) {
    left = middle - w / 2;
  }
  if (std::abs(top - int(work.top + margin)) < reach) {
    top = work.top + margin;
  } else if (std::abs(top + h - int(work.bottom - margin)) < reach) {
    top = work.bottom - margin - h;
  }
  return {left, top, left + w, top + h};
}

RECT saved_rect(const std::wstring& text, int scale, int* dpi) {
  const Settings& s = settings();
  POINT at;
  int anchor;
  HMONITOR monitor;
  if (s.badge_anchor < 0) {
    monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    const RECT work = work_area(monitor);
    const int margin = scaled(12, int(monitor_dpi(monitor)), 100);
    const bool left = s.corner == 1 || s.corner == 3, bottom = s.corner >= 2;
    anchor = (bottom ? 3 : 0) + (left ? 0 : 2);
    at = {left ? work.left + margin : work.right - margin, bottom ? work.bottom - margin : work.top + margin};
  } else {
    anchor = s.badge_anchor;
    at = {s.badge_x, s.badge_y};
    monitor = MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST);
  }
  *dpi = int(monitor_dpi(monitor));
  return clamp_into(rect_at(at, anchor, badge_size(text, *dpi, scale)), work_area(monitor));
}

void save_place() {
  Badge& b = g_badge;
  Settings s = settings();
  s.badge_anchor = anchor_for(b.rect, work_area(MonitorFromRect(&b.rect, MONITOR_DEFAULTTONEAREST)));
  const POINT p = anchor_of(b.rect, s.badge_anchor);
  s.badge_x = p.x;
  s.badge_y = p.y;
  s.badge_scale = b.scale;
  save_settings(s);
  b.key.clear();
}

int zone_at(POINT local) {
  const Badge& b = g_badge;
  const int w = b.rect.right - b.rect.left, h = b.rect.bottom - b.rect.top;
  const int grip = std::max(4, std::min(scaled(6, b.dpi, 100), h / 4));
  int zone = 0;
  if (local.x < grip) {
    zone |= kLeft;
  } else if (local.x >= w - grip) {
    zone |= kRight;
  }
  if (local.y < grip) {
    zone |= kTop;
  } else if (local.y >= h - grip) {
    zone |= kBottom;
  }
  return zone ? zone : kMove;
}

const wchar_t* cursor_for(int zone) {
  if (zone == kMove) return IDC_SIZEALL;
  if (zone == (kLeft | kTop) || zone == (kRight | kBottom)) return IDC_SIZENWSE;
  if (zone == (kRight | kTop) || zone == (kLeft | kBottom)) return IDC_SIZENESW;
  return zone & (kLeft | kRight) ? IDC_SIZEWE : IDC_SIZENS;
}

void move_to(POINT cursor) {
  Badge& b = g_badge;
  const int w0 = b.start.right - b.start.left, h0 = b.start.bottom - b.start.top;
  const double fx = double(b.grab.x - b.start.left) / std::max(1, w0), fy = double(b.grab.y - b.start.top) / std::max(1, h0);
  HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
  const int dpi = int(monitor_dpi(monitor));
  const bool rescale = dpi != b.dpi;
  b.dpi = dpi;
  const SIZE size = rescale ? badge_size(b.text, b.dpi, b.scale) : SIZE{b.rect.right - b.rect.left, b.rect.bottom - b.rect.top};
  const int left = cursor.x - int(std::lround(fx * size.cx)), top = cursor.y - int(std::lround(fy * size.cy));
  const RECT work = work_area(monitor);
  b.rect = clamp_into(snap({left, top, left + size.cx, top + size.cy}, work, dpi), work);
  if (rescale) {
    paint();
  } else {
    SetWindowPos(b.window, nullptr, b.rect.left, b.rect.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

void resize_by(int dx, int dy) {
  Badge& b = g_badge;
  const int w0 = std::max(1, int(b.start.right - b.start.left)), h0 = std::max(1, int(b.start.bottom - b.start.top));
  const bool across = b.drag & (kLeft | kRight), down = b.drag & (kTop | kBottom);
  const double fw = double(w0 + (b.drag & kLeft ? -dx : dx)) / w0, fh = double(h0 + (b.drag & kTop ? -dy : dy)) / h0;
  const double f = across && down ? (fw + fh) / 2 : across ? fw : fh;
  const int scale = std::clamp(int(std::lround(b.start_scale * f)), kMinScale, kMaxScale);
  if (scale == b.scale) return;
  b.scale = scale;
  const SIZE size = badge_size(b.text, b.dpi, scale);
  const int left = b.drag & kLeft    ? b.start.right - size.cx
                   : b.drag & kRight ? b.start.left
                                     : (b.start.left + b.start.right) / 2 - size.cx / 2;
  const int top = b.drag & kTop      ? b.start.bottom - size.cy
                  : b.drag & kBottom ? b.start.top
                                     : (b.start.top + b.start.bottom) / 2 - size.cy / 2;
  b.rect = clamp_into({left, top, left + size.cx, top + size.cy}, work_area(MonitorFromRect(&b.start, MONITOR_DEFAULTTONEAREST)));
  paint();
}

void end_drag(bool release) {
  Badge& b = g_badge;
  if (!b.drag) return;
  const bool moved = b.moved;
  b.drag = 0;
  if (release && GetCapture() == b.window) ReleaseCapture();
  POINT cursor;
  GetCursorPos(&cursor);
  b.hot = PtInRect(&b.rect, cursor) != FALSE;
  paint();
  if (moved) save_place();
}

void reset_place() {
  Settings s = settings();
  s.badge_anchor = -1;
  s.badge_scale = 100;
  save_settings(s);
  g_badge.key.clear();
  show_overlay(g_active);
}

void badge_menu(HWND window) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, kMenuOpen, L"Open Screen Light");
  AppendMenuW(menu, MF_STRING, kMenuReset, L"Reset position and size");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuOff, L"Turn off the badge");
  POINT at;
  GetCursorPos(&at);
  HWND owner = g_owner ? g_owner : window;
  SetForegroundWindow(owner);
  const UINT picked = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, at.x, at.y, 0, owner, nullptr);
  PostMessageW(owner, WM_NULL, 0, 0);
  DestroyMenu(menu);
  if (picked == kMenuOpen && g_owner) PostMessageW(g_owner, kShowMessage, 0, 0);
  if (picked == kMenuReset) reset_place();
  if (picked == kMenuOff) {
    Settings s = settings();
    s.overlay = kOverlayOff;
    save_settings(s);
    if (g_owner) PostMessageW(g_owner, kApplyMessage, 0, 0);
  }
}

LRESULT CALLBACK badge_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  Badge& b = g_badge;
  switch (message) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT) {
        POINT cursor;
        GetCursorPos(&cursor);
        const int zone = b.drag ? b.drag : zone_at({cursor.x - b.rect.left, cursor.y - b.rect.top});
        SetCursor(LoadCursorW(nullptr, cursor_for(zone)));
        return TRUE;
      }
      break;
    case WM_LBUTTONDOWN:
      b.drag = zone_at({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
      b.moved = false;
      GetCursorPos(&b.grab);
      b.start = b.rect;
      b.start_scale = b.scale;
      SetCapture(window);
      return 0;
    case WM_MOUSEMOVE: {
      if (!b.drag) {
        if (!b.hot) {
          b.hot = true;
          TRACKMOUSEEVENT track{sizeof track, TME_LEAVE, window, 0};
          TrackMouseEvent(&track);
          paint();
        }
        return 0;
      }
      POINT cursor;
      GetCursorPos(&cursor);
      const int dx = cursor.x - b.grab.x, dy = cursor.y - b.grab.y;
      if (!b.moved && std::abs(dx) < GetSystemMetrics(SM_CXDRAG) && std::abs(dy) < GetSystemMetrics(SM_CYDRAG)) return 0;
      b.moved = true;
      if (b.drag == kMove) {
        move_to(cursor);
      } else {
        resize_by(dx, dy);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      if (b.hot && !b.drag) {
        b.hot = false;
        paint();
      }
      return 0;
    case WM_LBUTTONUP:
      end_drag(true);
      return 0;
    case WM_CAPTURECHANGED:
      if (reinterpret_cast<HWND>(lparam) != window) end_drag(false);
      return 0;
    case WM_LBUTTONDBLCLK:
      if (g_owner) PostMessageW(g_owner, kShowMessage, 0, 0);
      return 0;
    case WM_RBUTTONUP:
      badge_menu(window);
      return 0;
    case WM_DPICHANGED:
      return 0;
    case WM_DISPLAYCHANGE:
      b.key.clear();
      show_overlay(g_active);
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

HWND make_badge_window() {
  static bool registered = false;
  HINSTANCE instance = GetModuleHandleW(nullptr);
  if (!registered) {
    WNDCLASSW wc{};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = badge_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    wc.lpszClassName = kBadgeClass;
    registered = RegisterClassW(&wc) != 0;
  }
  HWND window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kBadgeClass,
                                L"Screen Light", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
  if (window && build_number() >= kExcludeBuild) SetWindowDisplayAffinity(window, kExcludeFromCapture);
  return window;
}

void hide_badge() {
  Badge& b = g_badge;
  if (b.drag) end_drag(true);
  if (b.window) DestroyWindow(b.window);
  b = Badge{};
}

void show_badge(const std::wstring& text, bool lit) {
  Badge& b = g_badge;
  if (b.drag) return;
  const int scale = std::clamp(settings().badge_scale, kMinScale, kMaxScale);
  int dpi = 96;
  const RECT r = saved_rect(text, scale, &dpi);
  const bool light = light_badge();
  const std::wstring key = text + (lit ? L"|1|" : L"|0|") + (light ? L"w|" : L"b|") + std::to_wstring(dpi) + L"|" +
                           std::to_wstring(scale) + L"|" +
                           std::to_wstring(r.left) + L"," + std::to_wstring(r.top) + L"," + std::to_wstring(r.right) + L"," +
                           std::to_wstring(r.bottom);
  if (b.window && key == b.key) return;
  if (!b.window) b.window = make_badge_window();
  if (!b.window) return;
  b.key = key;
  b.text = text;
  b.lit = lit;
  b.light = light;
  b.dpi = dpi;
  b.scale = scale;
  b.rect = r;
  paint();
  SetWindowPos(b.window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

}  // namespace

void set_overlay_owner(HWND owner) { g_owner = owner; }

void show_overlay(const std::vector<Watcher>& active) {
  g_active = active;
  const Watcher* snapshot = nullptr;
  const Watcher* capture = nullptr;
  for (const Watcher& w : active) {
    if (w.source == Source::kPrintScreen) snapshot = &w;
    if (w.source == Source::kCapture) capture = &w;
  }
  int kind = g_preview >= 0 ? g_preview : active.empty() ? int(kOverlayOff) : settings().overlay;
  if (kind == kOverlayOff && snapshot) kind = kOverlayBadge;
  if (kind == kOverlayOff) {
    hide_badge();
    hide_frame();
    return;
  }
  if (kind == kOverlayBorder) {
    hide_badge();
    show_frame();
    return;
  }
  hide_frame();
  std::wstring text;
  if (snapshot) {
    const Watcher* shown = capture ? capture : snapshot;
    text = shown->name + L" " + shown->what;
  } else if (active.empty()) {
    text = L"Nothing is watching";
  } else if (active.size() == 1) {
    text = active[0].name + L" " + active[0].what;
  } else {
    text = std::to_wstring(active.size()) + L" programs are watching";
  }
  show_badge(text, !active.empty());
}

void preview_overlay(int kind) {
  g_preview = kind;
  show_overlay(g_active);
}

void reset_badge() { reset_place(); }

}  // namespace screenlight
