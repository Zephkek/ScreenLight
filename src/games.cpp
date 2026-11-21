#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <cwchar>
#include <string>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr ULONGLONG kLookEveryMs = 5000;
constexpr ULONGLONG kRecorderEveryMs = 10000;
constexpr ACCESS_MASK kDirectoryQuery = 1;
constexpr LONG kMoreEntries = 0x105;
constexpr wchar_t kKeepAlive[] = L"CaptureHook_KeepAlive";
constexpr const wchar_t* kRecorders[] = {L"obs64.exe", L"obs32.exe", L"obs.exe", L"Streamlabs OBS.exe",
                                         L"Streamlabs Desktop.exe"};

using OpenDirectoryFn = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
using QueryDirectoryFn = LONG(NTAPI*)(HANDLE, PVOID, ULONG, BOOLEAN, BOOLEAN, PULONG, PULONG);

struct DirectoryEntry {
  UNICODE_STRING name, type;
};

ULONGLONG g_looked_at;
ULONGLONG g_recorder_at;
DWORD g_recorder;
std::vector<Watcher> g_found;
std::vector<BYTE> g_listing;

std::vector<DWORD> hooked_games() {
  static const auto open_directory = reinterpret_cast<OpenDirectoryFn>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtOpenDirectoryObject"));
  static const auto query_directory = reinterpret_cast<QueryDirectoryFn>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryDirectoryObject"));
  std::vector<DWORD> games;
  if (!open_directory || !query_directory) return games;
  DWORD session = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &session);
  wchar_t path[64];
  swprintf(path, ARRAYSIZE(path), L"\\Sessions\\%lu\\BaseNamedObjects", session);
  UNICODE_STRING name{USHORT(wcslen(path) * sizeof(wchar_t)), USHORT(sizeof path), path};
  OBJECT_ATTRIBUTES attributes{};
  attributes.Length = sizeof attributes;
  attributes.ObjectName = &name;
  attributes.Attributes = OBJ_CASE_INSENSITIVE;
  HANDLE directory = nullptr;
  if (open_directory(&directory, kDirectoryQuery, &attributes) < 0) return games;
  g_listing.resize(64 * 1024);
  ULONG context = 0, size = 0;
  constexpr size_t prefix = ARRAYSIZE(kKeepAlive) - 1;
  for (BOOLEAN first = TRUE;; first = FALSE) {
    const LONG status = query_directory(directory, g_listing.data(), ULONG(g_listing.size()), FALSE, first, &context, &size);
    if (status < 0) break;
    for (const auto* entry = reinterpret_cast<const DirectoryEntry*>(g_listing.data()); entry->name.Buffer; ++entry) {
      const size_t length = entry->name.Length / sizeof(wchar_t);
      if (length <= prefix || length > prefix + 10 || wcsncmp(entry->name.Buffer, kKeepAlive, prefix) != 0) continue;
      games.push_back(wcstoul(std::wstring(entry->name.Buffer + prefix, length - prefix).c_str(), nullptr, 10));
    }
    if (status != kMoreEntries) break;
  }
  CloseHandle(directory);
  return games;
}

DWORD find_recorder() {
  DWORD found = 0;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return found;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof entry;
  for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry)) {
    for (const wchar_t* name : kRecorders) {
      if (_wcsicmp(entry.szExeFile, name) == 0) found = entry.th32ProcessID;
    }
  }
  CloseHandle(snapshot);
  return found;
}

}  // namespace

std::vector<Watcher> game_captures() {
  const ULONGLONG now = GetTickCount64();
  if (g_looked_at && now - g_looked_at < kLookEveryMs) return g_found;
  g_looked_at = now;
  g_found.clear();
  const std::vector<DWORD> games = hooked_games();
  if (games.empty()) return g_found;
  if (!g_recorder_at || now - g_recorder_at > kRecorderEveryMs || (g_recorder && process_gone(g_recorder))) {
    g_recorder = find_recorder();
    g_recorder_at = now;
  }
  for (DWORD game : games) {
    const std::wstring name = process_name(game);
    if (g_recorder) {
      g_found.push_back({Source::kCapture, g_recorder, process_name(g_recorder), L"records the game " + name, process_path(g_recorder)});
    } else {
      g_found.push_back({Source::kCapture, game, name, L"is being recorded by OBS", process_path(game)});
    }
  }
  return g_found;
}

}  // namespace screenlight
