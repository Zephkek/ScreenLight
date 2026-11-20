#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <vsstyle.h>
#include <vssym32.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <map>
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
  std::wstring path;
  SYSTEMTIME started{};
  SYSTEMTIME ended{};
  bool active = true;
  bool ignored = false;
};

constexpr size_t kMaxRows = 200;
constexpr int kNowGroup = 1;
constexpr int kEarlierGroup = 2;
constexpr UINT kOpenLocation = 1;
constexpr UINT kProperties = 2;
constexpr UINT kIgnore = 3;
constexpr UINT kClear = 4;

std::vector<Row> g_rows;
HWND g_window;
HWND g_status;
HWND g_hint;
HWND g_list;
HWND g_settings;
HWND g_tray;
HINSTANCE g_instance;
HFONT g_font;
HFONT g_title_font;
HIMAGELIST g_images;
std::map<std::wstring, int, NoCase> g_image_of;
HICON g_icons[2][2];
COLORREF g_title_color;
bool g_lit;
bool g_service;
bool g_placed;
int g_dpi = 96;

int px(int value) { return MulDiv(value, g_dpi, 96); }

int system_dpi() {
  HDC screen = GetDC(nullptr);
  const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
  ReleaseDC(nullptr, screen);
  return dpi;
}

int window_dpi(HWND window) {
  using DpiFn = UINT(WINAPI*)(HWND);
  static const auto get = reinterpret_cast<DpiFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  const UINT dpi = get && window ? get(window) : 0;
  return dpi ? int(dpi) : system_dpi();
}

int metric(int index) {
  using MetricFn = int(WINAPI*)(int, UINT);
  static const auto get = reinterpret_cast<MetricFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"));
  return get ? get(index, UINT(g_dpi)) : MulDiv(GetSystemMetrics(index), g_dpi, system_dpi());
}

void make_fonts() {
  using InfoFn = BOOL(WINAPI*)(UINT, UINT, void*, UINT, UINT);
  static const auto get = reinterpret_cast<InfoFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi"));
  NONCLIENTMETRICSW metrics{};
  metrics.cbSize = sizeof metrics;
  if (!get || !get(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0, UINT(g_dpi))) {
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0);
    metrics.lfMessageFont.lfHeight = MulDiv(metrics.lfMessageFont.lfHeight, g_dpi, system_dpi());
  }
  if (g_font) DeleteObject(g_font);
  if (g_title_font) DeleteObject(g_title_font);
  g_font = CreateFontIndirectW(&metrics.lfMessageFont);
  LOGFONTW title = metrics.lfMessageFont;
  title.lfHeight = MulDiv(title.lfHeight, 12, 9);
  g_title_font = CreateFontIndirectW(&title);
}

void make_icons() {
  for (int lit = 0; lit < 2; ++lit) {
    for (int big = 0; big < 2; ++big) {
      if (g_icons[lit][big]) DestroyIcon(g_icons[lit][big]);
      g_icons[lit][big] = make_light(lit != 0, metric(big ? SM_CXICON : SM_CXSMICON));
    }
  }
}

std::wstring clock_text(const SYSTEMTIME& time) {
  wchar_t text[64];
  if (!GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &time, nullptr, text, ARRAYSIZE(text))) return std::wstring();
  return text;
}

int active_rows() {
  return int(std::count_if(g_rows.begin(), g_rows.end(), [](const Row& r) { return r.active && !r.ignored; }));
}

std::wstring status_text() {
  const int active = active_rows();
  if (active == 0) return L"Nothing is watching your screen or input right now.";
  if (active == 1) return L"1 program is watching right now.";
  return std::to_wstring(active) + L" programs are watching right now.";
}

bool same_watcher(const Row& row, const Watcher& w) {
  return row.active && row.source == w.source && row.pid == w.pid && row.name == w.name &&
         (w.source == Source::kInput || row.what == w.what);
}

void load_title_color() {
  g_title_color = GetSysColor(COLOR_WINDOWTEXT);
  if (HTHEME theme = OpenThemeData(g_window, L"TEXTSTYLE")) {
    COLORREF color;
    if (SUCCEEDED(GetThemeColor(theme, TEXT_MAININSTRUCTION, 0, TMT_TEXTCOLOR, &color))) g_title_color = color;
    CloseThemeData(theme);
  }
}

void show_lit(bool lit) {
  g_lit = lit;
  SendMessageW(g_window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_icons[lit][0]));
  SendMessageW(g_window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_icons[lit][1]));
}

int icon_for(const std::wstring& path) {
  if (path.empty()) return 0;
  if (const auto it = g_image_of.find(path); it != g_image_of.end()) return it->second;
  int index = 0;
  HICON icon = nullptr;
  const int size = metric(SM_CXSMICON);
  if (SHDefExtractIconW(path.c_str(), 0, 0, &icon, nullptr, MAKELONG(size, size)) == S_OK && icon) {
    index = std::max(0, ImageList_AddIcon(g_images, icon));
    DestroyIcon(icon);
  }
  g_image_of.emplace(path, index);
  return index;
}

void make_images() {
  const int size = metric(SM_CXSMICON);
  HIMAGELIST fresh = ImageList_Create(size, size, ILC_COLOR32 | ILC_MASK, 8, 8);
  ImageList_AddIcon(fresh, static_cast<HICON>(LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON, size, size, LR_SHARED)));
  if (HIMAGELIST old = ListView_SetImageList(g_list, fresh, LVSIL_SMALL)) ImageList_Destroy(old);
  g_images = fresh;
  g_image_of.clear();
}

void set_fonts() {
  SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(g_title_font), TRUE);
  for (HWND control : {g_hint, g_settings, g_list}) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
}

const Row* row_at(int item) {
  LVITEMW lv{};
  lv.mask = LVIF_PARAM;
  lv.iItem = item;
  if (item < 0 || !ListView_GetItem(g_list, &lv) || size_t(lv.lParam) >= g_rows.size()) return nullptr;
  return &g_rows[lv.lParam];
}

void open_location(const std::wstring& path) {
  ShellExecuteW(g_window, nullptr, L"explorer.exe", (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

void show_properties(const std::wstring& path) {
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof info;
  info.fMask = SEE_MASK_INVOKEIDLIST;
  info.hwnd = g_window;
  info.lpVerb = L"properties";
  info.lpFile = path.c_str();
  info.nShow = SW_SHOWNORMAL;
  ShellExecuteExW(&info);
}

void fill();

void clear_finished() {
  g_rows.erase(std::remove_if(g_rows.begin(), g_rows.end(), [](const Row& r) { return !r.active; }), g_rows.end());
  fill();
}

void row_menu(LPARAM where) {
  const int item = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
  const Row* row = row_at(item);
  const std::wstring path = row ? row->path : std::wstring();
  const bool was_ignored = row && row->ignored;
  POINT at{GET_X_LPARAM(where), GET_Y_LPARAM(where)};
  if (where == -1) {
    RECT r{};
    if (item >= 0) ListView_GetItemRect(g_list, item, &r, LVIR_LABEL);
    at = {r.left, r.bottom};
    ClientToScreen(g_list, &at);
  }
  HMENU menu = CreatePopupMenu();
  if (!path.empty()) {
    AppendMenuW(menu, MF_STRING, kOpenLocation, L"Open file location");
    AppendMenuW(menu, MF_STRING, kProperties, L"Properties");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (was_ignored ? MF_CHECKED : MF_UNCHECKED), kIgnore, L"Ignore this program");
    SetMenuDefaultItem(menu, kOpenLocation, FALSE);
  }
  if (std::any_of(g_rows.begin(), g_rows.end(), [](const Row& r) { return !r.active; })) {
    if (!path.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kClear, L"Clear earlier entries");
  }
  if (GetMenuItemCount(menu) == 0) {
    DestroyMenu(menu);
    return;
  }
  const UINT picked = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, at.x, at.y, 0, g_window, nullptr);
  DestroyMenu(menu);
  if (picked == kOpenLocation) open_location(path);
  if (picked == kProperties) show_properties(path);
  if (picked == kIgnore) {
    ignore_program(path, !was_ignored);
    if (g_tray) PostMessageW(g_tray, kApplyMessage, 0, 0);
  }
  if (picked == kClear) clear_finished();
}

void info_tip(NMLVGETINFOTIPW* tip) {
  const Row* row = row_at(tip->iItem);
  if (!row) return;
  std::wstring text = row->path;
  if (row->pid) text += (text.empty() ? L"" : L"\n") + std::wstring(L"Process ID ") + std::to_wstring(row->pid);
  if (text.empty()) return;
  if (tip->dwFlags == 0 && tip->pszText[0]) text = std::wstring(tip->pszText) + L"\n" + text;
  lstrcpynW(tip->pszText, text.c_str(), tip->cchTextMax);
}

void fit_columns() {
  RECT client;
  GetClientRect(g_list, &client);
  int fixed = 0;
  for (int column : {0, 2, 3}) fixed += ListView_GetColumnWidth(g_list, column);
  ListView_SetColumnWidth(g_list, 1, std::max(px(120), int(client.right) - fixed));
}

void layout() {
  RECT client;
  GetClientRect(g_window, &client);
  const int margin = px(11), title = px(24), link = px(60);
  const int width = client.right - 2 * margin;
  MoveWindow(g_settings, client.right - margin - link, margin + px(4), link, px(18), TRUE);
  const int hint = (GetWindowLongW(g_hint, GWL_STYLE) & WS_VISIBLE) ? int(SendMessageW(g_hint, LM_GETIDEALHEIGHT, width, 0)) : 0;
  MoveWindow(g_status, margin, margin, width - link, title, TRUE);
  MoveWindow(g_hint, margin, margin + title, width, hint, TRUE);
  const int top = margin + title + hint + px(7);
  MoveWindow(g_list, margin, top, client.right - 2 * margin, std::max<int>(0, client.bottom - top - margin), TRUE);
  fit_columns();
}

void fill() {
  SetWindowTextW(g_status, status_text().c_str());
  ShowWindow(g_hint, g_service ? SW_HIDE : SW_SHOWNA);
  layout();
  SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(g_list);
  for (size_t i = 0; i < g_rows.size(); ++i) {
    const Row& row = g_rows[i];
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_GROUPID | LVIF_IMAGE | LVIF_PARAM;
    item.iItem = int(i);
    item.iGroupId = row.active ? kNowGroup : kEarlierGroup;
    item.iImage = icon_for(row.path);
    item.lParam = LPARAM(i);
    item.pszText = const_cast<wchar_t*>(row.name.c_str());
    const int at = ListView_InsertItem(g_list, &item);
    std::wstring started = clock_text(row.started);
    std::wstring ended = row.active ? L"now" : clock_text(row.ended);
    ListView_SetItemText(g_list, at, 1, const_cast<wchar_t*>(row.what.c_str()));
    ListView_SetItemText(g_list, at, 2, started.data());
    ListView_SetItemText(g_list, at, 3, ended.data());
  }
  fit_columns();
  SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(g_list, nullptr, TRUE);
}

void place_near_tray() {
  POINT tray{0, 0};
  if (HWND bar = FindWindowW(L"Shell_TrayWnd", nullptr)) {
    HWND notify = FindWindowExW(bar, nullptr, L"TrayNotifyWnd", nullptr);
    RECT r;
    if (GetWindowRect(notify ? notify : bar, &r)) tray = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
  }
  MONITORINFO monitor{};
  monitor.cbSize = sizeof monitor;
  if (!GetMonitorInfoW(MonitorFromPoint(tray, MONITOR_DEFAULTTOPRIMARY), &monitor)) return;
  const RECT& area = monitor.rcWork;
  RECT window;
  GetWindowRect(g_window, &window);
  const int width = window.right - window.left, height = window.bottom - window.top, gap = px(12);
  const int x = tray.x < (area.left + area.right) / 2 ? area.left + gap : area.right - width - gap;
  const int y = tray.y < (area.top + area.bottom) / 2 ? area.top + gap : area.bottom - height - gap;
  SetWindowPos(g_window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT notify(HWND window, NMHDR* header) {
  if (header->hwndFrom == g_hint && (header->code == NM_CLICK || header->code == NM_RETURN)) {
    run_elevated(window, L"--install");
    return 0;
  }
  if (header->hwndFrom == g_settings && (header->code == NM_CLICK || header->code == NM_RETURN)) {
    show_settings(g_instance, g_tray);
    return 0;
  }
  if (header->hwndFrom != g_list) return 0;
  switch (header->code) {
    case LVN_KEYDOWN:
      if (reinterpret_cast<NMLVKEYDOWN*>(header)->wVKey == VK_ESCAPE) ShowWindow(window, SW_HIDE);
      return 0;
    case LVN_GETINFOTIPW:
      info_tip(reinterpret_cast<NMLVGETINFOTIPW*>(header));
      return 0;
    case NM_CUSTOMDRAW: {
      auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(header);
      if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
      if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        const Row* row = row_at(int(draw->nmcd.dwItemSpec));
        if (row && row->ignored) draw->clrText = GetSysColor(COLOR_GRAYTEXT);
      }
      return CDRF_DODEFAULT;
    }
    case NM_DBLCLK:
      if (const Row* row = row_at(reinterpret_cast<NMITEMACTIVATE*>(header)->iItem); row && !row->path.empty()) {
        open_location(row->path);
      }
      return 0;
    case LVN_GETEMPTYMARKUP: {
      auto* markup = reinterpret_cast<NMLVEMPTYMARKUP*>(header);
      markup->dwFlags = EMF_CENTERED;
      lstrcpynW(markup->szMarkup, L"Programs that watch your screen or input will show up here.",
                ARRAYSIZE(markup->szMarkup));
      return TRUE;
    }
  }
  return 0;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_SIZE:
      if (g_list) layout();
      return 0;
    case WM_GETMINMAXINFO:
      reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = {px(420), px(240)};
      return 0;
    case WM_DPICHANGED: {
      const int old = g_dpi;
      g_dpi = HIWORD(wparam);
      make_fonts();
      set_fonts();
      make_icons();
      show_lit(g_lit);
      make_images();
      for (int column : {0, 2, 3}) {
        ListView_SetColumnWidth(g_list, column, MulDiv(ListView_GetColumnWidth(g_list, column), g_dpi, old));
      }
      const RECT* r = reinterpret_cast<const RECT*>(lparam);
      SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      fill();
      return 0;
    }
    case WM_THEMECHANGED:
      load_title_color();
      InvalidateRect(g_status, nullptr, TRUE);
      break;
    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wparam);
      SetTextColor(dc, reinterpret_cast<HWND>(lparam) == g_status ? g_title_color : GetSysColor(COLOR_WINDOWTEXT));
      SetBkColor(dc, GetSysColor(COLOR_WINDOW));
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_NOTIFY:
      return notify(window, reinterpret_cast<NMHDR*>(lparam));
    case WM_CONTEXTMENU:
      if (reinterpret_cast<HWND>(wparam) == g_list) row_menu(lparam);
      return 0;
    case WM_SETFOCUS:
      SetFocus(g_list);
      return 0;
    case WM_CLOSE:
      ShowWindow(window, SW_HIDE);
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void add_group(int id, const wchar_t* title) {
  LVGROUP group{};
  group.cbSize = sizeof group;
  group.mask = LVGF_HEADER | LVGF_GROUPID;
  group.pszHeader = const_cast<wchar_t*>(title);
  group.iGroupId = id;
  ListView_InsertGroup(g_list, -1, &group);
}

int time_column_width() {
  SYSTEMTIME widest{};
  GetLocalTime(&widest);
  widest.wHour = 23;
  widest.wMinute = 58;
  widest.wSecond = 58;
  const int text = std::max(ListView_GetStringWidth(g_list, clock_text(widest).c_str()),
                            ListView_GetStringWidth(g_list, L"Until"));
  return text + px(24);
}

void create(HINSTANCE instance) {
  g_dpi = system_dpi();
  INITCOMMONCONTROLSEX controls{sizeof controls, ICC_LISTVIEW_CLASSES | ICC_LINK_CLASS};
  InitCommonControlsEx(&controls);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = window_proc;
  wc.hInstance = instance;
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  wc.lpszClassName = L"ScreenLightWindow";
  RegisterClassExW(&wc);
  g_window = CreateWindowExW(0, wc.lpszClassName, L"Screen Light", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             px(660), px(380), nullptr, nullptr, instance, nullptr);
  if (!g_window) return;
  g_dpi = window_dpi(g_window);
  make_fonts();
  make_icons();
  load_title_color();
  g_status = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, g_window,
                             nullptr, instance, nullptr);
  g_hint = CreateWindowExW(0, WC_LINK,
                           L"<a>Install the service</a> to also see screenshots, screen recorders and the magnifier.",
                           WS_CHILD | WS_TABSTOP, 0, 0, 0, 0, g_window, nullptr, instance, nullptr);
  g_settings = CreateWindowExW(0, WC_LINK, L"<a>Settings</a>", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LWS_RIGHT, 0, 0, 0, 0,
                               g_window, nullptr, instance, nullptr);
  g_list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                           0, 0, 0, 0, g_window, nullptr, instance, nullptr);
  set_fonts();
  SetWindowTheme(g_list, L"Explorer", nullptr);
  ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
  make_images();
  const int time = time_column_width();
  const struct {
    const wchar_t* title;
    int width;
  } columns[] = {{L"Program", px(170)}, {L"What", px(200)}, {L"From", time}, {L"Until", time}};
  for (int i = 0; i < int(ARRAYSIZE(columns)); ++i) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = const_cast<wchar_t*>(columns[i].title);
    column.cx = columns[i].width;
    ListView_InsertColumn(g_list, i, &column);
  }
  ListView_EnableGroupView(g_list, TRUE);
  add_group(kNowGroup, L"Right now");
  add_group(kEarlierGroup, L"Earlier");
  layout();
}

}  // namespace

std::vector<Watcher> track(const std::vector<Watcher>& watchers) {
  SYSTEMTIME now;
  GetLocalTime(&now);
  bool changed = false;
  std::vector<Watcher> fresh;
  std::vector<bool> seen(g_rows.size());
  for (const Watcher& w : watchers) {
    const auto it = std::find_if(g_rows.begin(), g_rows.end(), [&w](const Row& r) { return same_watcher(r, w); });
    if (it == g_rows.end()) {
      g_rows.insert(g_rows.begin(), Row{w.source, w.pid, w.name, w.what, w.path, now, {}, true, ignored(w)});
      seen.insert(seen.begin(), true);
      fresh.push_back(w);
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
  for (Row& row : g_rows) {
    const bool skip = ignored_path(row.path);
    if (row.ignored != skip) {
      row.ignored = skip;
      changed = true;
    }
  }
  if (g_rows.size() > kMaxRows) g_rows.resize(kMaxRows);
  if (service_running() != g_service) {
    g_service = !g_service;
    changed = true;
  }
  if (g_window && (active_rows() > 0) != g_lit) show_lit(!g_lit);
  if (changed && g_window && IsWindowVisible(g_window)) fill();
  return fresh;
}

void show_window(HINSTANCE instance, HWND tray) {
  g_instance = instance;
  g_tray = tray;
  if (!g_window) create(instance);
  if (!g_window) return;
  show_lit(active_rows() > 0);
  fill();
  if (!g_placed) {
    place_near_tray();
    g_placed = true;
  }
  ShowWindow(g_window, IsIconic(g_window) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(g_window);
}

}  // namespace screenlight
