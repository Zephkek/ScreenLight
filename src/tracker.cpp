#include <windows.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

struct Tracked {
  Source source;
  DWORD pid = 0;
  DWORD session = 0;
  std::wstring name;
  std::wstring what;
  ULONGLONG until = 0;
  int open = 0;
};

std::mutex g_lock;
std::vector<Tracked> g_tracked;

Tracked& entry(Source source, DWORD pid, const std::wstring& what) {
  auto it = std::find_if(g_tracked.begin(), g_tracked.end(),
                         [&](const Tracked& t) { return t.source == source && t.pid == pid; });
  if (it != g_tracked.end()) {
    it->what = what;
    return *it;
  }
  Tracked fresh{source, pid, 0, process_name(pid), what};
  ProcessIdToSessionId(pid, &fresh.session);
  return g_tracked.emplace_back(std::move(fresh));
}

}  // namespace

void track_seen(Source source, DWORD pid, const std::wstring& what, ULONGLONG ttl_ms) {
  std::lock_guard<std::mutex> hold(g_lock);
  Tracked& t = entry(source, pid, what);
  t.until = std::max(t.until, GetTickCount64() + ttl_ms);
}

void track_open(Source source, DWORD pid, const std::wstring& what) {
  std::lock_guard<std::mutex> hold(g_lock);
  ++entry(source, pid, what).open;
}

void track_close(Source source, DWORD pid) {
  std::lock_guard<std::mutex> hold(g_lock);
  for (Tracked& t : g_tracked) {
    if (t.source == source && t.pid == pid && t.open > 0) --t.open;
  }
}

std::vector<Watcher> tracked(DWORD session) {
  const ULONGLONG now = GetTickCount64();
  std::lock_guard<std::mutex> hold(g_lock);
  g_tracked.erase(std::remove_if(g_tracked.begin(), g_tracked.end(),
                                 [now](const Tracked& t) {
                                   return (t.open == 0 && t.until <= now) || process_gone(t.pid);
                                 }),
                  g_tracked.end());
  std::vector<Watcher> found;
  for (const Tracked& t : g_tracked) {
    if (t.session == session) found.push_back({t.source, t.pid, t.name, t.what});
  }
  return found;
}

}  // namespace screenlight
