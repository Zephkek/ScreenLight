#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>

#include <algorithm>
#include <string>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

struct Row {
  Source source = Source::kDuplication;
  DWORD pid = 0;
  std::wstring name;
  std::wstring what;
  SYSTEMTIME started{};
  SYSTEMTIME ended{};
  bool active = true;
};

constexpr size_t kMaxRows = 200;
constexpr COLORREF kInstructionColor = RGB(0x1e, 0x39, 0x5b);

std::vector<Row> g_rows;
HWND g_window;
HWND g_status;
HWND g_hint;
HWND g_list;
HFONT g_font;
HFONT g_title_font;
int g_dpi = 96;

int px(int value) { return MulDiv(value, g_dpi, 96); }

std::wstring clock_text(const SYSTEMTIME& time) {
  wchar_t text[64];
  if (!GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &time, nullptr, text, ARRAYSIZE(text))) return std::wstring();
  return text;
}

std::wstring status_text() {
  const auto active = std::count_if(g_rows.begin(), g_rows.end(), [](const Row& r) { return r.active; });
  if (active == 0) return L"Nothing is watching your screen or input right now.";
  if (active == 1) return L"1 program is watching right now.";
  return std::to_wstring(active) + L" programs are watching right now.";
}

bool same_watcher(const Row& row, const Watcher& w) {
  return row.active && row.source == w.source && row.pid == w.pid && row.name == w.name &&
         (w.source == Source::kInput || row.what == w.what);
}

void layout();

void fill() {
  SetWindowTextW(g_status, status_text().c_str());
  ShowWindow(g_hint, service_running() ? SW_HIDE : SW_SHOWNA);
  layout();
  SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(g_list);
  for (size_t i = 0; i < g_rows.size(); ++i) {
    const Row& row = g_rows[i];
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = int(i);
    item.pszText = const_cast<wchar_t*>(row.name.c_str());
    const int at = ListView_InsertItem(g_list, &item);
    std::wstring started = clock_text(row.started);
    std::wstring ended = row.active ? L"now" : clock_text(row.ended);
    ListView_SetItemText(g_list, at, 1, const_cast<wchar_t*>(row.what.c_str()));
    ListView_SetItemText(g_list, at, 2, started.data());
    ListView_SetItemText(g_list, at, 3, ended.data());
  }
  SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(g_list, nullptr, TRUE);
}

void layout() {
  RECT client;
  GetClientRect(g_window, &client);
  const int margin = px(11), title = px(24);
  const int hint = (GetWindowLongW(g_hint, GWL_STYLE) & WS_VISIBLE) ? px(20) : 0;
  MoveWindow(g_status, margin, margin, client.right - 2 * margin, title, TRUE);
  MoveWindow(g_hint, margin, margin + title, client.right - 2 * margin, hint, TRUE);
  const int top = margin + title + hint + px(7);
  MoveWindow(g_list, margin, top, client.right - 2 * margin, std::max<int>(0, client.bottom - top - margin), TRUE);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_SIZE:
      if (g_list) layout();
      return 0;
    case WM_GETMINMAXINFO:
      reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = {px(380), px(220)};
      return 0;
    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wparam);
      SetTextColor(dc, reinterpret_cast<HWND>(lparam) == g_hint ? GetSysColor(COLOR_WINDOWTEXT) : kInstructionColor);
      SetBkColor(dc, GetSysColor(COLOR_WINDOW));
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_CLOSE:
      ShowWindow(window, SW_HIDE);
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void create(HINSTANCE instance) {
  HDC screen = GetDC(nullptr);
  g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
  ReleaseDC(nullptr, screen);
  NONCLIENTMETRICSW metrics{};
  metrics.cbSize = sizeof metrics;
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0);
  g_font = CreateFontIndirectW(&metrics.lfMessageFont);
  LOGFONTW title = metrics.lfMessageFont;
  title.lfHeight = MulDiv(title.lfHeight, 12, 9);
  g_title_font = CreateFontIndirectW(&title);
  INITCOMMONCONTROLSEX controls{sizeof controls, ICC_LISTVIEW_CLASSES};
  InitCommonControlsEx(&controls);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = window_proc;
  wc.hInstance = instance;
  wc.hIcon = make_light(RGB(140, 140, 140), GetSystemMetrics(SM_CXICON));
  wc.hIconSm = make_light(RGB(140, 140, 140), GetSystemMetrics(SM_CXSMICON));
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  wc.lpszClassName = L"ScreenLightWindow";
  RegisterClassExW(&wc);
  g_window = CreateWindowExW(0, wc.lpszClassName, L"Screen Light", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             px(640), px(360), nullptr, nullptr, instance, nullptr);
  if (!g_window) return;
  g_status = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, g_window,
                             nullptr, instance, nullptr);
  g_hint = CreateWindowExW(0, WC_STATICW,
                           L"Install the service from the tray menu to also catch screenshots, recorders and the magnifier.",
                           WS_CHILD | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, g_window, nullptr, instance, nullptr);
  g_list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                           0, 0, 0, 0, g_window, nullptr, instance, nullptr);
  SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(g_title_font), FALSE);
  SendMessageW(g_hint, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
  SendMessageW(g_list, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
  SetWindowTheme(g_list, L"Explorer", nullptr);
  ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  const struct {
    const wchar_t* title;
    int width;
  } columns[] = {{L"Program", 150}, {L"What", 260}, {L"From", 85}, {L"Until", 85}};
  for (int i = 0; i < int(ARRAYSIZE(columns)); ++i) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = const_cast<wchar_t*>(columns[i].title);
    column.cx = px(columns[i].width);
    ListView_InsertColumn(g_list, i, &column);
  }
  layout();
}

}  // namespace

void track(const std::vector<Watcher>& watchers) {
  SYSTEMTIME now;
  GetLocalTime(&now);
  bool changed = false;
  std::vector<bool> seen(g_rows.size());
  for (const Watcher& w : watchers) {
    const auto it = std::find_if(g_rows.begin(), g_rows.end(), [&w](const Row& r) { return same_watcher(r, w); });
    if (it == g_rows.end()) {
      g_rows.insert(g_rows.begin(), Row{w.source, w.pid, w.name, w.what, now});
      seen.insert(seen.begin(), true);
      changed = true;
      continue;
    }
    seen[it - g_rows.begin()] = true;
    if (it->what != w.what) {
      it->what = w.what;
      changed = true;
    }
  }
  for (size_t i = 0; i < g_rows.size(); ++i) {
    if (!g_rows[i].active || seen[i]) continue;
    g_rows[i].active = false;
    g_rows[i].ended = now;
    changed = true;
  }
  if (g_rows.size() > kMaxRows) g_rows.resize(kMaxRows);
  if (changed && g_window && IsWindowVisible(g_window)) fill();
}

void show_window(HINSTANCE instance) {
  if (!g_window) create(instance);
  if (!g_window) return;
  fill();
  ShowWindow(g_window, IsIconic(g_window) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(g_window);
}

}  // namespace screenlight
