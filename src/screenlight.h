#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace screenlight {

constexpr UINT kRefreshMessage = WM_APP + 2;
constexpr UINT kShowMessage = WM_APP + 3;

enum class Source { kDuplication, kHiddenDesktop, kRemote, kInput };

struct Watcher {
  Source source = Source::kDuplication;
  DWORD pid = 0;
  std::wstring name;
  std::wstring what;
};

std::vector<Watcher> duplicating_processes();
std::vector<Watcher> hidden_desktop_processes();
std::vector<Watcher> remote_viewers();
std::vector<Watcher> input_senders();
std::vector<Watcher> collect_watchers();

void start_input_watch(HWND notify);
void stop_input_watch();

std::wstring process_path(DWORD pid);
std::wstring process_name(DWORD pid, const char* fallback = nullptr);
bool process_gone(DWORD pid);

HICON make_light(COLORREF color, int size);

void track(const std::vector<Watcher>& watchers);
void show_window(HINSTANCE instance);

int run_tray(HINSTANCE instance);

}  // namespace screenlight
