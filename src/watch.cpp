#include <windows.h>

#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

bool own_window(const Watcher& w) {
  DWORD owner = 0;
  if (!w.window || !GetWindowThreadProcessId(reinterpret_cast<HWND>(ULONG_PTR(w.window)), &owner) || !owner) return false;
  if (owner == w.pid) return true;
  const std::wstring path = process_path(owner);
  return !path.empty() && !w.path.empty() && _wcsicmp(path.c_str(), w.path.c_str()) == 0;
}

void append(std::vector<Watcher>* all, std::vector<Watcher> more) {
  all->insert(all->end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
}

}  // namespace

std::vector<Watcher> collect_watchers() {
  const Settings& s = settings();
  std::vector<Watcher> all;
  std::vector<Watcher> from_service = service_watchers();
  if (s.watch_screen) {
    append(&all, duplicating_processes());
    append(&all, game_captures());
    append(&all, print_screen_watchers());
    static const DWORD session = [] {
      DWORD id = 0;
      ProcessIdToSessionId(GetCurrentProcessId(), &id);
      return id;
    }();
    if (!service_running()) append(&all, running_captures(session));
  }
  if (s.watch_remote) {
    append(&all, hidden_desktop_processes());
    append(&all, remote_viewers());
  }
  if (s.watch_input) append(&all, input_senders());
  for (Watcher& w : possible_watchers()) {
    if (watched(w)) all.push_back(std::move(w));
  }
  const DWORD self = GetCurrentProcessId();
  for (Watcher& w : from_service) {
    if (w.pid != self && watched(w) && !own_window(w)) all.push_back(std::move(w));
  }
  return all;
}

}  // namespace screenlight
