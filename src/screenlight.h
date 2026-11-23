#pragma once

#include <windows.h>
#include <commctrl.h>

#include <cwchar>
#include <functional>
#include <string>
#include <vector>

namespace screenlight {

constexpr UINT kRefreshMessage = WM_APP + 2;
constexpr UINT kShowMessage = WM_APP + 3;
constexpr UINT kApplyMessage = WM_APP + 4;
constexpr wchar_t kServiceName[] = L"ScreenLight";
constexpr COLORREF kIdleColor = RGB(140, 140, 140);
constexpr COLORREF kLitColor = RGB(226, 52, 40);

enum class Source {
  kDuplication,
  kHiddenDesktop,
  kRemote,
  kInput,
  kScreenRead,
  kMagnifier,
  kThumbnail,
  kCapture,
  kKeyboard,
  kPrintScreen
};

struct NoCase {
  bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
};

struct Watcher {
  Source source = Source::kDuplication;
  DWORD pid = 0;
  std::wstring name;
  std::wstring what;
  std::wstring path;
  ULONG window = 0;
  bool possible = false;
};

enum { kOverlayOff, kOverlayBadge, kOverlayBorder };

struct Settings {
  bool notifications = true;
  bool watch_screen = true;
  bool watch_input = true;
  bool watch_keyboard = true;
  bool watch_remote = true;
  bool asked_service = false;
  int overlay = kOverlayOff;
  int corner = 0;
  int badge_anchor = -1;
  int badge_x = 0;
  int badge_y = 0;
  int badge_scale = 100;
  int theme = 0;
  std::vector<std::wstring> ignored;
  std::vector<std::wstring> distrusted;
};

const Settings& settings();
void save_settings(const Settings& s);
bool portable();
bool watched(const Watcher& w);
bool ignored(const Watcher& w);
bool trusted(Source source, const std::wstring& path);
bool known_publisher(const std::wstring& path);
void watch_publishers(HWND notify);
bool ignored_path(const std::wstring& path);
void ignore_program(const std::wstring& path, bool on);
bool starts_with_windows();
void start_with_windows(bool on);
void show_settings(HINSTANCE instance, HWND tray);
bool settings_message(MSG* msg);

std::vector<Watcher> duplicating_processes();
std::vector<Watcher> hidden_desktop_processes();
std::vector<Watcher> remote_viewers();
void remote_changed();
void displays_changed();
std::vector<Watcher> input_senders();
std::vector<Watcher> print_screen_watchers();
bool note_print_screen();
ULONGLONG print_screen_seq();
std::vector<Watcher> game_captures();
std::vector<Watcher> possible_watchers();
std::vector<Watcher> system_watchers(DWORD session);
void remember_drivers();
void devices_changed();
std::vector<Watcher> running_captures(DWORD session);
void remember_running_captures();
void forget_running_captures(DWORD session);
std::vector<Watcher> earlier_captures(DWORD session, const std::vector<DWORD>& known);
std::vector<Watcher> collect_watchers();

void start_input_watch(HWND notify);
void stop_input_watch();
bool find_sender(DWORD session, DWORD* pid, DWORD* tid);

void track_seen(Source source, DWORD pid, const std::wstring& what, ULONGLONG ttl_ms, ULONG window = 0);
void track_open(Source source, DWORD pid, const std::wstring& what);
void track_close(Source source, DWORD pid, ULONGLONG linger_ms = 0);
bool tracking(Source source, DWORD pid);
std::vector<Watcher> tracked(DWORD session);
void restore_tracked();

bool start_trace();
bool flush_needed();
void flush_trace();
void stop_trace();
void WINAPI handle_trace_event(struct _EVENT_RECORD* record);
void begin_calibration(DWORD pid);
DWORD pointer_maker(DWORD session);
DWORD service_pointer(std::wstring* name, std::wstring* path);
ULONG read_code();

void serve_pipe(HANDLE stop);
std::wstring ask_service(const wchar_t* request);
std::vector<Watcher> service_watchers();
DWORD service_sender(std::wstring* name, std::wstring* path);
bool service_running();

bool find_module(HANDLE process, const std::function<bool(const std::wstring&)>& visit);
std::wstring file_part(const std::wstring& path);
std::wstring process_path(DWORD pid);
std::wstring process_name(DWORD pid, const char* fallback = nullptr);
bool process_gone(DWORD pid);
bool script_host(const std::wstring& path);
std::vector<char> file_bytes(const std::wstring& path);

HICON make_light(bool lit, int size);
void show_overlay(const std::vector<Watcher>& active);
void set_overlay_owner(HWND owner);
void preview_overlay(int kind);
void reset_badge();

struct Palette {
  COLORREF window, band, text, gray, title, link, line;
};

bool dark_supported();
bool dark_mode();
bool light_badge();
bool update_theme();
const Palette& palette();
HBRUSH window_brush();
HBRUSH band_brush();
void theme_window(HWND window);
void theme_control(HWND control, const wchar_t* dark_class, const wchar_t* light_class = nullptr);
void theme_link(HWND link);
void theme_group(HWND group);
void theme_frame(HWND control);
void theme_list(HWND list);
LRESULT draw_check(const NMCUSTOMDRAW* draw);
void retheme_window();
void retheme_settings();

struct Seen {
  Watcher watcher;
  SYSTEMTIME started{};
  SYSTEMTIME ended{};
  bool active = true;
  bool trusted = false;
};

std::vector<Watcher> track(const std::vector<Watcher>& watchers);
std::vector<Seen> seen_rows();
void show_window(HINSTANCE instance, HWND tray);
void toggle_flyout(HINSTANCE instance, HWND owner, const std::wstring& select = std::wstring());
void update_flyout();
bool flyout_message(MSG* msg);
bool flyout_visible();
void close_flyout();

int run_tray(HINSTANCE instance, bool show);
void run_elevated(HWND window, const wchar_t* verb);
int run_unelevated(const wchar_t* args);
void offer_service();
bool can_end(DWORD pid);
bool end_program(HWND owner, DWORD pid, const std::wstring& name);
int end_elevated(const wchar_t* pid, const wchar_t* started);

int run_service();
int install_service();
int uninstall_service();
bool service_installed();

}  // namespace screenlight
