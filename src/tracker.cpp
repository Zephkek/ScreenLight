#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
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
  std::wstring path;
  ULONG window = 0;
  ULONGLONG until = 0;
  int open = 0;
};

std::mutex g_lock;
std::vector<Tracked> g_tracked;
bool g_saving;

Tracked& entry(Source source, DWORD pid, const std::wstring& what) {
  auto it = std::find_if(g_tracked.begin(), g_tracked.end(),
                         [&](const Tracked& t) { return t.source == source && t.pid == pid; });
  if (it != g_tracked.end()) {
    it->what = what;
    return *it;
  }
  Tracked fresh{source, pid, 0, process_name(pid), what, process_path(pid)};
  ProcessIdToSessionId(pid, &fresh.session);
  return g_tracked.emplace_back(std::move(fresh));
}

ULONGLONG started_at(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return 0;
  FILETIME created{}, exited, kernel, user;
  const bool ok = GetProcessTimes(process, &created, &exited, &kernel, &user);
  CloseHandle(process);
  return ok ? (ULONGLONG(created.dwHighDateTime) << 32) | created.dwLowDateTime : 0;
}

std::wstring state_path() {
  wchar_t path[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
  if (!length || length >= ARRAYSIZE(path)) return std::wstring();
  std::wstring dir(path, length);
  return dir.substr(0, dir.find_last_of(L'\\') + 1) + L"running.txt";
}

void save_locked() {
  if (!g_saving) return;
  std::wstring text;
  for (const Tracked& t : g_tracked) {
    if (t.open <= 0) continue;
    const ULONGLONG started = started_at(t.pid);
    if (!started) continue;
    wchar_t line[64];
    swprintf(line, ARRAYSIZE(line), L"%d\t%lu\t%llu\t%d\t%lu\t", int(t.source), t.pid, started, t.open, t.window);
    text += line + t.what + L"\n";
  }
  const std::wstring path = state_path(), temp = path + L".new";
  if (path.empty()) return;
  if (text.empty()) {
    DeleteFileW(path.c_str());
    return;
  }
  HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  const DWORD bytes = DWORD(text.size() * sizeof(wchar_t));
  const bool ok = WriteFile(file, text.data(), bytes, &written, nullptr) && written == bytes;
  CloseHandle(file);
  if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(temp.c_str());
}

}  // namespace

void track_seen(Source source, DWORD pid, const std::wstring& what, ULONGLONG ttl_ms, ULONG window) {
  std::lock_guard<std::mutex> hold(g_lock);
  Tracked& t = entry(source, pid, what);
  t.until = std::max(t.until, GetTickCount64() + ttl_ms);
  if (window) t.window = window;
}

void track_open(Source source, DWORD pid, const std::wstring& what) {
  std::lock_guard<std::mutex> hold(g_lock);
  ++entry(source, pid, what).open;
  save_locked();
}

void track_close(Source source, DWORD pid, ULONGLONG linger_ms) {
  std::lock_guard<std::mutex> hold(g_lock);
  const ULONGLONG now = GetTickCount64();
  bool changed = false;
  for (Tracked& t : g_tracked) {
    if (t.source == source && t.pid == pid && t.open > 0) {
      --t.open;
      t.until = std::max(t.until, now + linger_ms);
      changed = true;
    }
  }
  if (changed) save_locked();
}

bool tracking(Source source, DWORD pid) {
  const ULONGLONG now = GetTickCount64();
  std::lock_guard<std::mutex> hold(g_lock);
  return std::any_of(g_tracked.begin(), g_tracked.end(), [&](const Tracked& t) {
    return t.source == source && t.pid == pid && (t.open > 0 || t.until > now);
  });
}

std::vector<Watcher> tracked(DWORD session) {
  const ULONGLONG now = GetTickCount64();
  std::lock_guard<std::mutex> hold(g_lock);
  const size_t before = g_tracked.size();
  bool dropped_open = false;
  g_tracked.erase(std::remove_if(g_tracked.begin(), g_tracked.end(),
                                 [now, &dropped_open](const Tracked& t) {
                                   const bool gone = t.open == 0 ? t.until <= now : process_gone(t.pid);
                                   if (gone && t.open > 0) dropped_open = true;
                                   return gone;
                                 }),
                  g_tracked.end());
  if (dropped_open && g_tracked.size() != before) save_locked();
  std::vector<Watcher> found;
  for (const Tracked& t : g_tracked) {
    if (t.session == session) found.push_back({t.source, t.pid, t.name, t.what, t.path, t.window});
  }
  return found;
}

void restore_tracked() {
  std::lock_guard<std::mutex> hold(g_lock);
  g_saving = true;
  const std::wstring path = state_path();
  const std::vector<char> bytes = file_bytes(path);
  if (bytes.size() < sizeof(wchar_t)) return;
  const std::wstring text(reinterpret_cast<const wchar_t*>(bytes.data()), bytes.size() / sizeof(wchar_t));
  for (size_t start = 0, end; start < text.size(); start = end + 1) {
    end = text.find(L'\n', start);
    if (end == std::wstring::npos) end = text.size();
    const std::wstring line = text.substr(start, end - start);
    int source = 0, open = 0;
    unsigned long pid = 0, window = 0;
    unsigned long long started = 0;
    int consumed = 0;
    if (swscanf(line.c_str(), L"%d\t%lu\t%llu\t%d\t%lu\t%n", &source, &pid, &started, &open, &window, &consumed) < 5 ||
        consumed <= 0 || open <= 0 || source < 0 || source > int(Source::kPrintScreen)) {
      continue;
    }
    if (!pid || started_at(pid) != started) continue;
    Tracked& t = entry(Source(source), pid, line.substr(size_t(consumed)));
    t.open = std::max(t.open, open);
    t.window = window;
  }
  save_locked();
}

}  // namespace screenlight
