#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace screenlight {

struct Watcher {
  DWORD pid = 0;
  std::wstring name;
  std::wstring what;
};

std::vector<Watcher> duplicating_processes();
std::vector<Watcher> hidden_desktop_processes();
std::vector<Watcher> remote_viewers();
std::vector<Watcher> collect_watchers();

std::wstring process_path(DWORD pid);
std::wstring process_name(DWORD pid, const char* fallback = nullptr);
bool process_gone(DWORD pid);

int run_tray(HINSTANCE instance);

}  // namespace screenlight
