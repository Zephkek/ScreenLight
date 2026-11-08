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

std::wstring process_name(DWORD pid, const char* fallback = nullptr);

int run_tray(HINSTANCE instance);

}  // namespace screenlight
