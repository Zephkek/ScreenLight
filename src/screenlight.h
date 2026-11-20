#pragma once

#include <windows.h>

#include <cwchar>
#include <string>
#include <vector>

namespace screenlight {

constexpr UINT kRefreshMessage = WM_APP + 2;
constexpr UINT kShowMessage = WM_APP + 3;
constexpr UINT kApplyMessage = WM_APP + 4;
constexpr wchar_t kServiceName[] = L"ScreenLight";
constexpr COLORREF kIdleColor = RGB(140, 140, 140);
constexpr COLORREF kLitColor = RGB(226, 52, 40);

enum class Source { kDuplication, kHiddenDesktop, kRemote, kInput, kScreenRead, kMagnifier, kThumbnail, kCapture, kKeyboard };

struct NoCase {
  bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
};

struct Watcher {
  Source source = Source::kDuplication;
  DWORD pid = 0;
  std::wstring name;
  std::wstring what;
  std::wstring path;
};

enum { kOverlayOff, kOverlayBadge, kOverlayBorder };

struct Settings {
  bool notifications = true;
  bool watch_screen = true;
  bool watch_input = true;
  bool watch_keyboard = true;
  bool watch_remote = true;
  int overlay = kOverlayOff;
  int corner = 0;
  std::vector<std::wstring> ignored;
};

const Settings& settings();
void save_settings(const Settings& s);
bool portable();
bool watched(const Watcher& w);
bool ignored(const Watcher& w);
bool ignored_path(const std::wstring& path);
void ignore_program(const std::wstring& path, bool on);
bool starts_with_windows();
void start_with_windows(bool on);
void show_settings(HINSTANCE instance, HWND tray);
bool settings_message(MSG* msg);

std::vector<Watcher> duplicating_processes();
std::vector<Watcher> hidden_desktop_processes();
std::vector<Watcher> remote_viewers();
std::vector<Watcher> input_senders();
std::vector<Watcher> collect_watchers();

void start_input_watch(HWND notify);
void stop_input_watch();
bool find_sender(DWORD session, DWORD* pid, DWORD* tid);

void track_seen(Source source, DWORD pid, const std::wstring& what, ULONGLONG ttl_ms);
void track_open(Source source, DWORD pid, const std::wstring& what);
void track_close(Source source, DWORD pid);
bool tracking(Source source, DWORD pid);
std::vector<Watcher> tracked(DWORD session);

bool start_trace();
bool flush_needed();
void flush_trace();
void stop_trace();
void WINAPI handle_trace_event(struct _EVENT_RECORD* record);
void begin_calibration(DWORD pid);
ULONG read_code();

void serve_pipe(HANDLE stop);
std::wstring ask_service(const wchar_t* request);
std::vector<Watcher> service_watchers();
DWORD service_sender(std::wstring* name, std::wstring* path);
bool service_running();

std::wstring process_path(DWORD pid);
std::wstring process_name(DWORD pid, const char* fallback = nullptr);
bool process_gone(DWORD pid);
std::vector<char> file_bytes(const std::wstring& path);

HICON make_light(COLORREF color, int size);
void show_overlay(const std::vector<Watcher>& active);

std::vector<Watcher> track(const std::vector<Watcher>& watchers);
void show_window(HINSTANCE instance, HWND tray);

int run_tray(HINSTANCE instance, bool show);
void run_elevated(HWND window, const wchar_t* verb);

int run_service();
int install_service();
int uninstall_service();
bool service_installed();

}  // namespace screenlight
