#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr wchar_t kClass[] = L"ScreenLightOverlay";
constexpr DWORD kExcludeFromCapture = 0x11;
constexpr DWORD kExcludeBuild = 19041;
constexpr COLORREF kBadgeColor = RGB(32, 32, 32);

HWND g_badge;
std::wstring g_badge_key;
std::vector<HWND> g_frame;
std::vector<RECT> g_frame_monitors;

LRESULT CALLBACK overlay_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCHITTEST) return HTTRANSPARENT;
  return DefWindowProcW(window, message, wparam, lparam);
}

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

HWND make_window() {
  static bool registered = false;
  HINSTANCE instance = GetModuleHandleW(nullptr);
  if (!registered) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = overlay_proc;
    wc.hInstance = instance;
    wc.hbrBackground = CreateSolidBrush(kLitColor);
    wc.lpszClassName = kClass;
    registered = RegisterClassW(&wc) != 0;
  }
  HWND window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                kClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
  if (window && build_number() >= kExcludeBuild) SetWindowDisplayAffinity(window, kExcludeFromCapture);
  return window;
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
      HWND window = make_window();
      if (!window) continue;
      SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
      SetWindowPos(window, HWND_TOPMOST, side.left, side.top, side.right - side.left, side.bottom - side.top,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
      g_frame.push_back(window);
    }
  }
  g_frame_monitors = now;
}

void hide_badge() {
  if (g_badge) DestroyWindow(g_badge);
  g_badge = nullptr;
  g_badge_key.clear();
}

float pill_cover(float x, float y, float width, float height) {
  const float radius = height / 2;
  const float cx = std::clamp(x, radius, width - radius), cy = radius;
  return std::clamp(radius - std::hypot(x - cx, y - cy) + 0.5f, 0.0f, 1.0f);
}

void show_badge(const std::wstring& text, int corner) {
  HMONITOR monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info{};
  info.cbSize = sizeof info;
  if (!GetMonitorInfoW(monitor, &info)) return;
  const int dpi = int(monitor_dpi(monitor));
  const RECT& work = info.rcWork;
  const std::wstring key = text + L"|" + std::to_wstring(corner) + L"|" + std::to_wstring(dpi) + L"|" +
                           std::to_wstring(work.left) + L"," + std::to_wstring(work.top) + L"," +
                           std::to_wstring(work.right) + L"," + std::to_wstring(work.bottom);
  if (g_badge && key == g_badge_key) return;
  if (!g_badge) g_badge = make_window();
  if (!g_badge) return;
  g_badge_key = key;

  NONCLIENTMETRICSW metrics{};
  metrics.cbSize = sizeof metrics;
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0);
  HDC screen = GetDC(nullptr);
  const int system_dpi = GetDeviceCaps(screen, LOGPIXELSX);
  LOGFONTW face = metrics.lfMessageFont;
  face.lfHeight = MulDiv(face.lfHeight, dpi, system_dpi);
  face.lfQuality = CLEARTYPE_QUALITY;
  HFONT font = CreateFontIndirectW(&face);
  HDC memory = CreateCompatibleDC(screen);
  HGDIOBJ old_font = SelectObject(memory, font);
  const int pad = MulDiv(12, dpi, 96), dot = MulDiv(10, dpi, 96), gap = MulDiv(8, dpi, 96);
  const int room = MulDiv(420, dpi, 96);
  RECT measure{0, 0, room, 0};
  DrawTextW(memory, text.c_str(), -1, &measure, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX | DT_END_ELLIPSIS);
  const int text_width = std::min<int>(measure.right, room);
  const int height = std::max<int>(measure.bottom, dot) + MulDiv(14, dpi, 96);
  const int width = pad + dot + gap + text_width + pad;

  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = width;
  bi.bmiHeader.biHeight = -height;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(memory, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap) {
    SelectObject(memory, old_font);
    DeleteObject(font);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return;
  }
  HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
  auto* pixel = static_cast<uint32_t*>(bits);
  const float dot_x = pad + dot / 2.0f - 0.5f, dot_y = height / 2.0f - 0.5f, dot_r = dot / 2.0f;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float a = std::clamp(dot_r - std::hypot(x - dot_x, y - dot_y) + 0.5f, 0.0f, 1.0f);
      const int r = int(GetRValue(kBadgeColor) * (1 - a) + GetRValue(kLitColor) * a);
      const int g = int(GetGValue(kBadgeColor) * (1 - a) + GetGValue(kLitColor) * a);
      const int b = int(GetBValue(kBadgeColor) * (1 - a) + GetBValue(kLitColor) * a);
      pixel[y * width + x] = 0xff000000u | uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b);
    }
  }
  SetBkMode(memory, TRANSPARENT);
  SetTextColor(memory, RGB(255, 255, 255));
  RECT place{pad + dot + gap, 0, pad + dot + gap + text_width, height};
  DrawTextW(memory, text.c_str(), -1, &place, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
  GdiFlush();
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float a = pill_cover(float(x), float(y), float(width), float(height));
      uint32_t& p = pixel[y * width + x];
      const uint32_t r = uint32_t(((p >> 16) & 0xff) * a), g = uint32_t(((p >> 8) & 0xff) * a), b = uint32_t((p & 0xff) * a);
      p = uint32_t(a * 255 + 0.5f) << 24 | r << 16 | g << 8 | b;
    }
  }

  const RECT& area = info.rcWork;
  const int margin = MulDiv(12, dpi, 96);
  POINT at{corner == 1 || corner == 3 ? area.left + margin : area.right - width - margin,
           corner >= 2 ? area.bottom - height - margin : area.top + margin};
  SIZE size{width, height};
  POINT origin{0, 0};
  BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(g_badge, screen, &at, &size, memory, &origin, 0, &blend, ULW_ALPHA);
  SetWindowPos(g_badge, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);

  SelectObject(memory, old_bitmap);
  SelectObject(memory, old_font);
  DeleteObject(bitmap);
  DeleteObject(font);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
}

}  // namespace

void show_overlay(const std::vector<Watcher>& active) {
  const Settings& s = settings();
  if (active.empty() || s.overlay == kOverlayOff) {
    hide_badge();
    hide_frame();
    return;
  }
  if (s.overlay == kOverlayBorder) {
    hide_badge();
    show_frame();
    return;
  }
  hide_frame();
  const std::wstring text = active.size() == 1 ? active[0].name + L" " + active[0].what
                                               : std::to_wstring(active.size()) + L" programs are watching";
  show_badge(text, s.corner);
}

}  // namespace screenlight
