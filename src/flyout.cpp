#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <string>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr wchar_t kClass[] = L"ScreenLightFlyout";
constexpr size_t kEarlierRows = 3;
constexpr int kVisibleTiles = 5;
constexpr UINT kList = 10;
constexpr UINT kLocation = 11;
constexpr UINT kTrust = 12;
constexpr UINT kEnd = 13;
constexpr UINT kTurnOn = 14;
constexpr UINT kOpen = 15;
constexpr UINT kSettings = 16;
constexpr int kNowGroup = 1;
constexpr int kEarlierGroup = 2;
constexpr DWORD kCornerPreference = 33;
constexpr DWORD kRoundCorners = 2;

struct Item {
  Seen seen;
  std::wstring key;
};

HWND g_flyout;
HWND g_owner;
HWND g_eye_box;
HWND g_title;
HWND g_note;
HWND g_list;
HWND g_location;
HWND g_trust;
HWND g_end;
HWND g_reason;
HWND g_shield;
HWND g_turn_on;
HWND g_open;
HWND g_settings;
HINSTANCE g_instance;
HFONT g_font;
HFONT g_title_font;
HIMAGELIST g_images;
HICON g_eye[2];
HICON g_shield_icon;
std::map<std::wstring, int, NoCase> g_image_of;
int g_dpi = 96;
int g_footer_top;
bool g_busy;
bool g_filling;
ULONGLONG g_closed_at;
std::vector<Item> g_items;
std::wstring g_signature;
std::wstring g_selected;

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

int windows_build() {
  using GetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  static const int build = [] {
    const auto get = reinterpret_cast<GetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof info;
    return get && get(&info) == 0 ? int(info.dwBuildNumber) : 0;
  }();
  return build;
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
  g_font = CreateFontIndirectW(&metrics.lfMessageFont);
  LOGFONTW title = metrics.lfMessageFont;
  title.lfHeight = MulDiv(title.lfHeight, 12, 9);
  g_title_font = CreateFontIndirectW(&title);
}

void drop_resources() {
  for (HICON& icon : g_eye) {
    if (icon) DestroyIcon(icon);
    icon = nullptr;
  }
  if (g_shield_icon) DestroyIcon(g_shield_icon);
  g_shield_icon = nullptr;
  if (g_images) ImageList_Destroy(g_images);
  g_images = nullptr;
  g_image_of.clear();
  for (HFONT* font : {&g_font, &g_title_font}) {
    if (*font) DeleteObject(*font);
    *font = nullptr;
  }
}

int image_for(const std::wstring& path) {
  const std::wstring key = path.empty() ? L"?" : path;
  if (const auto it = g_image_of.find(key); it != g_image_of.end()) return it->second;
  const int size = px(32);
  HICON icon = nullptr;
  if (path.empty() || SHDefExtractIconW(path.c_str(), 0, 0, &icon, nullptr, MAKELONG(size, size)) != S_OK) icon = nullptr;
  if (!icon && FAILED(LoadIconWithScaleDown(nullptr, IDI_APPLICATION, size, size, &icon))) icon = nullptr;
  const int index = icon ? std::max(0, ImageList_AddIcon(g_images, icon)) : 0;
  if (icon) DestroyIcon(icon);
  g_image_of.emplace(key, index);
  return index;
}

std::wstring clock_text(const SYSTEMTIME& time) {
  wchar_t text[64];
  if (!GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &time, nullptr, text, ARRAYSIZE(text))) return std::wstring();
  return text;
}

std::wstring capitalized(std::wstring text) {
  if (!text.empty()) text[0] = wchar_t(towupper(text[0]));
  return text;
}

std::wstring detail(const Seen& s) {
  std::wstring text = capitalized(s.watcher.what);
  text += s.active ? L" - since " + clock_text(s.started) : L" - ended " + clock_text(s.ended);
  if (s.trusted) text += L" - Trusted";
  return text;
}

std::vector<Item> gather() {
  std::vector<Item> now, earlier;
  for (const Seen& s : seen_rows()) {
    if (s.watcher.possible) continue;
    Item item{s, s.watcher.name + L"\t" + s.watcher.what + L"\t" + std::to_wstring(s.watcher.pid)};
    if (s.active) {
      now.push_back(item);
    } else if (earlier.size() < kEarlierRows) {
      earlier.push_back(item);
    }
  }
  now.insert(now.end(), earlier.begin(), earlier.end());
  return now;
}

std::wstring signature(const std::vector<Item>& items) {
  std::wstring text = service_running() ? L"1" : L"0";
  for (const Item& item : items) {
    text += item.key + (item.seen.active ? L"a" : L"e") + (item.seen.trusted ? L"t" : L"-") + clock_text(item.seen.ended) +
            std::to_wstring(item.seen.watcher.pid) + L"\n";
  }
  return text;
}

size_t active_count() {
  return size_t(std::count_if(g_items.begin(), g_items.end(), [](const Item& i) { return i.seen.active && !i.seen.trusted; }));
}

std::wstring headline() {
  const size_t count = active_count();
  if (count == 0) return L"Nothing is watching right now";
  if (count == 1) return L"1 program is watching";
  return std::to_wstring(count) + L" programs are watching";
}

const Item* selected_item() {
  for (const Item& item : g_items) {
    if (item.key == g_selected) return &item;
  }
  return nullptr;
}

HWND child(const wchar_t* type, const wchar_t* text, DWORD style, UINT id, HFONT font) {
  HWND control = CreateWindowExW(0, type, text, WS_CHILD | style, 0, 0, 0, 0, g_flyout,
                                 reinterpret_cast<HMENU>(UINT_PTR(id)), g_instance, nullptr);
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
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

std::wstring fit_path(const std::wstring& path, int width) {
  std::vector<wchar_t> text(path.begin(), path.end());
  text.resize(path.size() + 8, L'\0');
  HDC dc = GetDC(g_flyout);
  HGDIOBJ old = SelectObject(dc, g_font);
  RECT r{0, 0, width, 100};
  DrawTextW(dc, text.data(), -1, &r, DT_CALCRECT | DT_PATH_ELLIPSIS | DT_MODIFYSTRING | DT_NOPREFIX | DT_SINGLELINE);
  SelectObject(dc, old);
  ReleaseDC(g_flyout, dc);
  return text.data();
}

void fill_list() {
  g_filling = true;
  SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(g_list);
  int chosen = -1;
  for (size_t i = 0; i < g_items.size(); ++i) {
    const Seen& s = g_items[i].seen;
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM | LVIF_GROUPID;
    item.iItem = int(i);
    item.iGroupId = s.active ? kNowGroup : kEarlierGroup;
    item.iImage = image_for(s.watcher.path);
    item.lParam = LPARAM(i);
    item.pszText = const_cast<wchar_t*>(s.watcher.name.c_str());
    const int index = ListView_InsertItem(g_list, &item);
    std::wstring second = detail(s);
    ListView_SetItemText(g_list, index, 1, second.data());
    UINT columns[] = {1};
    LVTILEINFO tile{};
    tile.cbSize = sizeof tile;
    tile.iItem = index;
    tile.cColumns = 1;
    tile.puColumns = columns;
    ListView_SetTileInfo(g_list, &tile);
    if (chosen < 0 && g_items[i].key == g_selected) chosen = index;
  }
  if (chosen < 0 && !g_items.empty()) {
    chosen = 0;
    g_selected = g_items[0].key;
  }
  if (chosen >= 0) {
    ListView_SetItemState(g_list, chosen, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g_list, chosen, FALSE);
  }
  SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(g_list, nullptr, TRUE);
  g_filling = false;
}

bool needs_elevation(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
  if (process) {
    CloseHandle(process);
    return false;
  }
  return GetLastError() == ERROR_ACCESS_DENIED;
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

void place(int width, int height) {
  const RECT icon = tray_anchor();
  MONITORINFO monitor{};
  monitor.cbSize = sizeof monitor;
  GetMonitorInfoW(MonitorFromRect(&icon, MONITOR_DEFAULTTONEAREST), &monitor);
  const RECT& work = monitor.rcWork;
  APPBARDATA bar{};
  bar.cbSize = sizeof bar;
  UINT edge = ABE_BOTTOM;
  if (SHAppBarMessage(ABM_GETTASKBARPOS, &bar)) edge = bar.uEdge;
  const int gap = windows_build() >= 22000 ? px(12) : 0;
  const int cx = (icon.left + icon.right) / 2, cy = (icon.top + icon.bottom) / 2;
  int x = std::clamp(cx - width / 2, int(work.left) + gap, std::max(int(work.left), int(work.right) - width - gap));
  int y = std::clamp(cy - height / 2, int(work.top) + gap, std::max(int(work.top), int(work.bottom) - height - gap));
  switch (edge) {
    case ABE_TOP:
      y = std::max(work.top, icon.bottom) + gap;
      break;
    case ABE_LEFT:
      x = std::max(work.left, icon.right) + gap;
      break;
    case ABE_RIGHT:
      x = std::min(work.right, icon.left) - width - gap;
      break;
    default:
      y = std::min(work.bottom, icon.top) - height - gap;
      break;
  }
  x = std::clamp(x, int(work.left), std::max(int(work.left), int(work.right) - width));
  y = std::clamp(y, int(work.top), std::max(int(work.top), int(work.bottom) - height));
  SetWindowPos(g_flyout, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
}

void layout() {
  const int margin = px(11), width = px(340), inner = width - 2 * margin, eye = px(32);
  SendMessageW(g_eye_box, STM_SETICON, reinterpret_cast<WPARAM>(g_eye[active_count() ? 1 : 0]), 0);
  int y = margin;
  const std::wstring title = headline();
  const int text_x = margin + eye + px(10), text_w = width - text_x - margin;
  const int title_h = text_height(g_title_font, title, text_w);
  SetWindowTextW(g_title, title.c_str());
  const bool quiet = active_count() == 0;
  const std::wstring note = L"The eye turns red when a program watches your screen or your input.";
  const int note_h = quiet ? text_height(g_font, note, text_w) : 0;
  SetWindowTextW(g_note, note.c_str());
  const int text_block = title_h + (quiet ? px(2) + note_h : 0);
  const int head = std::max(eye, text_block);
  MoveWindow(g_eye_box, margin, y + (head - eye) / 2, eye, eye, FALSE);
  const int text_top = y + (head - text_block) / 2;
  MoveWindow(g_title, text_x, text_top, text_w, title_h, FALSE);
  MoveWindow(g_note, text_x, text_top + title_h + px(2), text_w, note_h, FALSE);
  ShowWindow(g_note, quiet ? SW_SHOWNA : SW_HIDE);
  y += head + px(11);

  const Item* item = g_items.empty() ? nullptr : selected_item();
  const bool has_path = item && !item->seen.watcher.path.empty();
  const DWORD pid = item ? item->seen.watcher.pid : 0;
  const bool alive = pid && !process_gone(pid);
  const bool endable = alive && can_end(pid);
  const wchar_t* reason = !item || !pid ? L""
                         : !alive       ? L"Already closed"
                         : endable      ? L""
                         : process_path(pid).empty() ? L"Protected by Windows"
                                                     : L"Part of Windows";
  ShowWindow(g_list, g_items.empty() ? SW_HIDE : SW_SHOWNA);
  ShowWindow(g_location, has_path ? SW_SHOWNA : SW_HIDE);
  ShowWindow(g_trust, has_path ? SW_SHOWNA : SW_HIDE);
  ShowWindow(g_end, endable ? SW_SHOWNA : SW_HIDE);
  ShowWindow(g_reason, *reason ? SW_SHOWNA : SW_HIDE);
  SetWindowTextW(g_reason, reason);
  if (!g_items.empty()) {
    const bool now = std::any_of(g_items.begin(), g_items.end(), [](const Item& i) { return i.seen.active; });
    const bool earlier = std::any_of(g_items.begin(), g_items.end(), [](const Item& i) { return !i.seen.active; });
    const int cap = kVisibleTiles * px(46) + (int(now) + int(earlier)) * px(30) + px(6);
    MoveWindow(g_list, margin, y, inner, cap, FALSE);
    LVTILEVIEWINFO info{};
    info.cbSize = sizeof info;
    info.dwMask = LVTVIM_TILESIZE | LVTVIM_COLUMNS;
    info.dwFlags = LVTVIF_FIXEDSIZE;
    info.sizeTile = {inner - GetSystemMetrics(SM_CXVSCROLL) - px(6), px(44)};
    info.cLines = 1;
    ListView_SetTileViewInfo(g_list, &info);
    RECT last{};
    int list_h = cap;
    if (ListView_GetItemRect(g_list, int(g_items.size()) - 1, &last, LVIR_BOUNDS)) {
      POINT origin{};
      ListView_GetOrigin(g_list, &origin);
      list_h = std::min(cap, int(last.bottom + origin.y) + px(4));
    }
    MoveWindow(g_list, margin, y, inner, list_h, FALSE);
    y += list_h + px(7);
  }
  if (has_path) {
    SetWindowTextW(g_location, (L"Location: <a>" + fit_path(item->seen.watcher.path, inner - px(64)) + L"</a>").c_str());
    MoveWindow(g_location, margin, y, inner, px(18), FALSE);
    CheckDlgButton(g_flyout, kTrust, item->seen.trusted ? BST_CHECKED : BST_UNCHECKED);
    y += px(18) + px(7);
  }
  if (has_path || endable || *reason) {
    if (has_path) MoveWindow(g_trust, margin, y + px(4), inner - px(120), px(18), FALSE);
    if (endable) {
      Button_SetElevationRequiredState(g_end, needs_elevation(pid));
      MoveWindow(g_end, width - margin - px(110), y, px(110), px(26), FALSE);
    }
    if (*reason) MoveWindow(g_reason, width - margin - px(150), y + px(5), px(150), px(18), FALSE);
    y += px(26) + px(11);
  }
  const bool off = !service_running();
  ShowWindow(g_shield, off ? SW_SHOWNA : SW_HIDE);
  ShowWindow(g_turn_on, off ? SW_SHOWNA : SW_HIDE);
  if (off) {
    const std::wstring plain = L"Full detection is off, so screen recorders, screenshots and keyboard listeners won't show. Turn on";
    const int shield = px(16), text_left = margin + shield + px(6);
    const int h = text_height(g_font, plain, width - text_left - margin) + px(2);
    MoveWindow(g_shield, margin, y + px(1), shield, shield, FALSE);
    MoveWindow(g_turn_on, text_left, y, width - text_left - margin, h, FALSE);
    y += h + px(11);
  }
  g_footer_top = y;
  const int footer = px(37), link_h = px(18);
  MoveWindow(g_open, margin, y + (footer - link_h) / 2, inner / 2, link_h, FALSE);
  MoveWindow(g_settings, margin + inner / 2, y + (footer - link_h) / 2, inner - inner / 2, link_h, FALSE);
  y += footer;
  RECT outer{0, 0, width, y};
  AdjustWindowRectEx(&outer, DWORD(GetWindowLongW(g_flyout, GWL_STYLE)), FALSE, DWORD(GetWindowLongW(g_flyout, GWL_EXSTYLE)));
  place(outer.right - outer.left, outer.bottom - outer.top);
  RedrawWindow(g_flyout, nullptr, nullptr, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void refresh(bool force) {
  std::vector<Item> items = gather();
  const std::wstring sign = signature(items);
  if (!force && sign == g_signature) return;
  g_signature = sign;
  g_items = std::move(items);
  fill_list();
  layout();
}

void hide() {
  if (g_flyout) DestroyWindow(g_flyout);
}

void end_selected() {
  const Item* item = selected_item();
  if (!item || !item->seen.watcher.pid) return;
  const Watcher w = item->seen.watcher;
  g_busy = true;
  const bool ended = end_program(g_flyout, w.pid, w.name);
  g_busy = false;
  if (ended && g_owner) SendMessageW(g_owner, kRefreshMessage, 0, 0);
  if (g_flyout) {
    refresh(true);
    SetForegroundWindow(g_flyout);
    SetFocus(IsWindowVisible(g_list) ? g_list : g_open);
  }
}

void open_location() {
  const Item* item = selected_item();
  if (!item || item->seen.watcher.path.empty()) return;
  const std::wstring path = item->seen.watcher.path;
  hide();
  ShellExecuteW(nullptr, nullptr, L"explorer.exe", (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

void trust_toggled() {
  const Item* item = selected_item();
  if (!item || item->seen.watcher.path.empty()) return;
  ignore_program(item->seen.watcher.path, IsDlgButtonChecked(g_flyout, kTrust) == BST_CHECKED);
  if (g_owner) SendMessageW(g_owner, kApplyMessage, 0, 0);
  if (g_flyout) refresh(true);
}

void link(UINT id) {
  HWND owner = g_owner;
  HINSTANCE instance = g_instance;
  if (id == kLocation) {
    open_location();
  } else if (id == kTurnOn) {
    g_busy = true;
    run_elevated(g_flyout, L"--install");
    g_busy = false;
    hide();
  } else if (id == kOpen) {
    hide();
    show_window(instance, owner);
  } else if (id == kSettings) {
    hide();
    show_settings(instance, owner);
  }
}

LRESULT list_notify(NMHDR* header) {
  switch (header->code) {
    case LVN_ITEMCHANGED: {
      const auto* change = reinterpret_cast<NMLISTVIEW*>(header);
      if (!g_filling && (change->uNewState & LVIS_SELECTED) && change->lParam >= 0 &&
          size_t(change->lParam) < g_items.size()) {
        g_selected = g_items[change->lParam].key;
        layout();
      }
      return 0;
    }
    case LVN_ITEMACTIVATE:
      open_location();
      return 0;
    case LVN_KEYDOWN:
      if (reinterpret_cast<NMLVKEYDOWN*>(header)->wVKey == VK_DELETE) end_selected();
      return 0;
    case LVN_GETINFOTIPW: {
      auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(header);
      LVITEMW lv{};
      lv.mask = LVIF_PARAM;
      lv.iItem = tip->iItem;
      if (!ListView_GetItem(g_list, &lv) || size_t(lv.lParam) >= g_items.size()) return 0;
      const Watcher& w = g_items[lv.lParam].seen.watcher;
      std::wstring text = w.name + L"\n" + capitalized(w.what);
      if (!w.path.empty()) text += L"\n" + w.path;
      if (w.pid) text += L"\nProcess ID " + std::to_wstring(w.pid);
      lstrcpynW(tip->pszText, text.c_str(), tip->cchTextMax);
      return 0;
    }
  }
  return 0;
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
      } else if (LOWORD(wparam) == kEnd && HIWORD(wparam) == BN_CLICKED) {
        end_selected();
      } else if (LOWORD(wparam) == kTrust && HIWORD(wparam) == BN_CLICKED) {
        trust_toggled();
      }
      return 0;
    case WM_NOTIFY: {
      auto* header = reinterpret_cast<NMHDR*>(lparam);
      if (header->hwndFrom == g_list) return list_notify(header);
      if (header->code == NM_CLICK || header->code == NM_RETURN) link(UINT(header->idFrom));
      return 0;
    }
    case WM_ERASEBKGND: {
      HDC dc = reinterpret_cast<HDC>(wparam);
      RECT client;
      GetClientRect(window, &client);
      RECT top = client, band = client;
      top.bottom = g_footer_top;
      band.top = g_footer_top;
      FillRect(dc, &top, GetSysColorBrush(COLOR_WINDOW));
      FillRect(dc, &band, GetSysColorBrush(COLOR_BTNFACE));
      return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
      HDC dc = reinterpret_cast<HDC>(wparam);
      HWND control = reinterpret_cast<HWND>(lparam);
      const bool band = control == g_open || control == g_settings;
      const bool quiet = control == g_note || control == g_reason || control == g_turn_on;
      SetTextColor(dc, GetSysColor(quiet ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
      SetBkColor(dc, GetSysColor(band ? COLOR_BTNFACE : COLOR_WINDOW));
      return reinterpret_cast<LRESULT>(GetSysColorBrush(band ? COLOR_BTNFACE : COLOR_WINDOW));
    }
    case WM_CLOSE:
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      g_flyout = nullptr;
      g_items.clear();
      g_signature.clear();
      drop_resources();
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

void add_group(int id, const wchar_t* title) {
  LVGROUP group{};
  group.cbSize = sizeof group;
  group.mask = LVGF_HEADER | LVGF_GROUPID;
  group.pszHeader = const_cast<wchar_t*>(title);
  group.iGroupId = id;
  ListView_InsertGroup(g_list, -1, &group);
}

void create_controls() {
  g_eye[0] = make_light(false, px(32));
  g_eye[1] = make_light(true, px(32));
  SHSTOCKICONINFO stock{};
  stock.cbSize = sizeof stock;
  if (SUCCEEDED(SHGetStockIconInfo(SIID_SHIELD, SHGSI_ICON | SHGSI_SMALLICON, &stock))) g_shield_icon = stock.hIcon;
  g_eye_box = child(WC_STATICW, L"", WS_VISIBLE | SS_ICON | SS_REALSIZECONTROL | SS_CENTERIMAGE, 0, g_font);
  g_title = child(WC_STATICW, L"", WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, g_title_font);
  g_note = child(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, 0, g_font);
  g_list = child(WC_LISTVIEWW, L"Programs",
                 WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER, kList, g_font);
  SetWindowTheme(g_list, L"Explorer", nullptr);
  ListView_SetExtendedListViewStyle(g_list, LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP | LVS_EX_FULLROWSELECT);
  g_images = ImageList_Create(px(32), px(32), ILC_COLOR32 | ILC_MASK, 8, 8);
  ListView_SetImageList(g_list, g_images, LVSIL_NORMAL);
  for (int i = 0; i < 2; ++i) {
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    column.cx = px(200);
    ListView_InsertColumn(g_list, i, &column);
  }
  ListView_SetView(g_list, LV_VIEW_TILE);
  ListView_EnableGroupView(g_list, TRUE);
  add_group(kNowGroup, L"Right now");
  add_group(kEarlierGroup, L"Earlier");
  g_location = child(WC_LINK, L"", WS_TABSTOP, kLocation, g_font);
  g_trust = child(WC_BUTTONW, L"&Trust this program", WS_TABSTOP | BS_AUTOCHECKBOX, kTrust, g_font);
  g_end = child(WC_BUTTONW, L"&End program", WS_TABSTOP | BS_PUSHBUTTON, kEnd, g_font);
  g_reason = child(WC_STATICW, L"", SS_RIGHT | SS_NOPREFIX, 0, g_font);
  g_shield = child(WC_STATICW, L"", SS_ICON | SS_REALSIZECONTROL | SS_CENTERIMAGE, 0, g_font);
  SendMessageW(g_shield, STM_SETICON, reinterpret_cast<WPARAM>(g_shield_icon), 0);
  g_turn_on = child(WC_LINK,
                    L"Full detection is off, so screen recorders, screenshots and keyboard listeners won't show. <a>Turn on</a>",
                    WS_TABSTOP, kTurnOn, g_font);
  g_open = child(WC_LINK, L"<a>Open Screen Light</a>", WS_VISIBLE | WS_TABSTOP, kOpen, g_font);
  g_settings = child(WC_LINK, L"<a>Settings</a>", WS_VISIBLE | WS_TABSTOP | LWS_RIGHT, kSettings, g_font);
}

}  // namespace

bool flyout_message(MSG* msg) { return g_flyout && IsDialogMessageW(g_flyout, msg); }

void toggle_flyout(HINSTANCE instance, HWND owner, const std::wstring& select) {
  if (g_flyout) {
    hide();
    return;
  }
  if (select.empty() && GetTickCount64() - g_closed_at < GetDoubleClickTime()) return;
  g_instance = instance;
  g_owner = owner;
  register_class();
  const RECT icon = tray_anchor();
  g_flyout = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_CONTROLPARENT, kClass, L"Screen Light",
                             WS_POPUP | WS_BORDER, icon.left, icon.top, 1, 1, owner, nullptr, instance, nullptr);
  if (!g_flyout) return;
  g_dpi = window_dpi(g_flyout);
  make_fonts();
  round_corners();
  create_controls();
  g_selected = select;
  refresh(true);
  ShowWindow(g_flyout, SW_SHOW);
  SetForegroundWindow(g_flyout);
  SetFocus(IsWindowVisible(g_list) ? g_list : g_open);
}

void update_flyout() {
  if (!g_flyout || g_busy) return;
  refresh(false);
}

bool flyout_visible() { return g_flyout != nullptr; }

}  // namespace screenlight
