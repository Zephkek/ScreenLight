#include <windows.h>
#include <winternl.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr wchar_t kInstancePrefix[] = L"CaptureService_";
constexpr DWORD kUserServiceTypes = SERVICE_WIN32 | 0x40 | 0x80;
constexpr ULONG kObjectTypesInformation = 3;
constexpr ULONG kProcessBasicInformation = 0;
constexpr ULONG kProcessWow64Information = 26;
constexpr ULONG kProcessHandleInformation = 51;
constexpr LONG kInfoLengthMismatch = LONG(0xC0000004);
constexpr ULONGLONG kRelistMs = 60000;
constexpr ULONGLONG kFirstLookMs = 2000;
constexpr ULONGLONG kLookAgainMs = 10000;
constexpr wchar_t kCaptures[] = L"captures the screen or a window";
constexpr wchar_t kMayCapture[] = L"may be capturing the screen or a window";

using QueryObjectFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using QueryProcessFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

struct TypeInfo {
  USHORT length, max_length;
  PWSTR name;
  ULONG counts[12];
  ULONG invalid_attributes;
  GENERIC_MAPPING mapping;
  ULONG valid_access;
  BOOLEAN security_required, maintain_count;
  UCHAR index;
  CHAR reserved;
  ULONG pool_type, paged_charge, nonpaged_charge;
};

struct HandleEntry {
  HANDLE handle;
  ULONG_PTR handle_count, pointer_count;
  ULONG access, type, attributes, reserved;
};

static_assert(sizeof(TypeInfo) == 0x68 && sizeof(HandleEntry) == 40, "ntdll layout changed");

struct Guess {
  ULONGLONG at = 0;
  ULONGLONG since = 0;
  std::vector<Watcher> found;
};

std::mutex g_lock;
QueryObjectFn g_query_object;
QueryProcessFn g_query_process;
SC_HANDLE g_manager;
std::vector<SC_HANDLE> g_instances;
ULONGLONG g_listed_at;
bool g_listed;
bool g_unsupported;
std::map<DWORD, Guess> g_guesses;
std::set<DWORD> g_unexplained;

bool load_nt() {
  if (g_query_object && g_query_process) return true;
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  g_query_object = reinterpret_cast<QueryObjectFn>(GetProcAddress(ntdll, "NtQueryObject"));
  g_query_process = reinterpret_cast<QueryProcessFn>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
  return g_query_object && g_query_process;
}

void list_instances() {
  for (SC_HANDLE instance : g_instances) CloseServiceHandle(instance);
  g_instances.clear();
  g_listed = true;
  g_listed_at = GetTickCount64();
  if (!g_manager) g_manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
  if (!g_manager) return;
  std::vector<BYTE> buffer;
  DWORD needed = 0, count = 0;
  for (int attempt = 0; attempt < 4; ++attempt) {
    DWORD resume = 0;
    if (EnumServicesStatusExW(g_manager, SC_ENUM_PROCESS_INFO, kUserServiceTypes, SERVICE_STATE_ALL, buffer.data(),
                              DWORD(buffer.size()), &needed, &count, &resume, nullptr)) {
      break;
    }
    count = 0;
    if (GetLastError() == ERROR_INVALID_PARAMETER) g_unsupported = true;
    if (GetLastError() != ERROR_MORE_DATA) return;
    buffer.resize(buffer.size() + needed + 4096);
  }
  const auto* services = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
  for (DWORD i = 0; i < count; ++i) {
    if (_wcsnicmp(services[i].lpServiceName, kInstancePrefix, ARRAYSIZE(kInstancePrefix) - 1) != 0) continue;
    if (SC_HANDLE instance = OpenServiceW(g_manager, services[i].lpServiceName, SERVICE_QUERY_STATUS)) {
      g_instances.push_back(instance);
    }
  }
}

std::set<DWORD> running_sessions() {
  std::set<DWORD> sessions;
  if (g_unsupported) return sessions;
  if (!g_listed || (g_instances.empty() && GetTickCount64() - g_listed_at > kRelistMs)) list_instances();
  for (SC_HANDLE instance : g_instances) {
    SERVICE_STATUS_PROCESS status{};
    DWORD size = 0;
    if (!QueryServiceStatusEx(instance, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof status, &size)) {
      continue;
    }
    DWORD session = 0;
    if (status.dwCurrentState != SERVICE_STOPPED && status.dwProcessId && ProcessIdToSessionId(status.dwProcessId, &session)) {
      sessions.insert(session);
    }
  }
  return sessions;
}

ULONG shared_resource_type() {
  static const ULONG type = [] {
    ULONG size = 1 << 16;
    std::vector<uint8_t> buffer;
    LONG status = kInfoLengthMismatch;
    for (int attempt = 0; attempt < 4 && status == kInfoLengthMismatch; ++attempt) {
      buffer.assign(size, 0);
      status = g_query_object(nullptr, kObjectTypesInformation, buffer.data(), size, &size);
      size = std::max<ULONG>(size, ULONG(buffer.size()) * 2);
    }
    if (status < 0) return 0ul;
    const ULONG count = *reinterpret_cast<const ULONG*>(buffer.data());
    size_t at = sizeof(void*);
    for (ULONG i = 0; i < count && at + sizeof(TypeInfo) <= buffer.size(); ++i) {
      const auto* info = reinterpret_cast<const TypeInfo*>(buffer.data() + at);
      if (info->name && std::wstring(info->name, info->length / 2) == L"DxgkSharedResource") return ULONG(info->index);
      at += (sizeof(TypeInfo) + info->max_length + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
    }
    return 0ul;
  }();
  return type;
}

bool holds_type(HANDLE process, ULONG type) {
  ULONG size = 16 * 1024;
  std::vector<uint8_t> buffer;
  LONG status = kInfoLengthMismatch;
  for (int attempt = 0; attempt < 5 && status == kInfoLengthMismatch; ++attempt) {
    buffer.assign(size, 0);
    status = g_query_process(process, kProcessHandleInformation, buffer.data(), size, &size);
    size = std::max<ULONG>(size + 4096, ULONG(buffer.size()) * 2);
  }
  if (status < 0) return false;
  const ULONG_PTR count = *reinterpret_cast<const ULONG_PTR*>(buffer.data());
  const auto* entries = reinterpret_cast<const HandleEntry*>(buffer.data() + 2 * sizeof(ULONG_PTR));
  for (ULONG_PTR i = 0; i < count && (i + 1) * sizeof(HandleEntry) + 2 * sizeof(ULONG_PTR) <= buffer.size(); ++i) {
    if (entries[i].type == type) return true;
  }
  return false;
}

template <typename Pointer>
bool read(HANDLE process, ULONGLONG at, Pointer* value) {
  SIZE_T done = 0;
  return ReadProcessMemory(process, reinterpret_cast<const void*>(ULONG_PTR(at)), value, sizeof *value, &done) &&
         done == sizeof *value;
}

template <typename Pointer>
bool module_in_list(HANDLE process, ULONGLONG head, ULONGLONG name_offset, const wchar_t* wanted) {
  Pointer link = 0;
  if (!read(process, head, &link)) return false;
  const size_t wanted_length = wcslen(wanted);
  for (int i = 0; i < 2048 && link && link != head; ++i) {
    USHORT length = 0;
    Pointer buffer = 0;
    if (!read(process, link + name_offset, &length) || !read(process, link + name_offset + sizeof(Pointer), &buffer)) {
      return false;
    }
    if (length == wanted_length * sizeof(wchar_t) && buffer) {
      wchar_t name[64] = {};
      SIZE_T done = 0;
      if (ReadProcessMemory(process, reinterpret_cast<const void*>(ULONG_PTR(buffer)), name, length, &done) &&
          done == length && _wcsnicmp(name, wanted, wanted_length) == 0) {
        return true;
      }
    }
    if (!read(process, link, &link)) return false;
  }
  return false;
}

bool has_module(HANDLE process, const wchar_t* wanted) {
  if (wcslen(wanted) >= 64) return false;
  ULONG_PTR peb32 = 0;
  if (g_query_process(process, kProcessWow64Information, &peb32, sizeof peb32, nullptr) >= 0 && peb32) {
    uint32_t ldr = 0;
    if (read(process, peb32 + 0x0c, &ldr) && ldr && module_in_list<uint32_t>(process, ldr + 0x14, 0x24, wanted)) return true;
  }
  PROCESS_BASIC_INFORMATION basic{};
  if (g_query_process(process, kProcessBasicInformation, &basic, sizeof basic, nullptr) < 0 || !basic.PebBaseAddress) {
    return false;
  }
  uint64_t ldr = 0;
  return read(process, ULONGLONG(ULONG_PTR(basic.PebBaseAddress)) + 0x18, &ldr) && ldr &&
         module_in_list<uint64_t>(process, ldr + 0x20, 0x48, wanted);
}

std::vector<Watcher> look(DWORD session, const std::vector<DWORD>& known) {
  std::vector<Watcher> found;
  const ULONG type = load_nt() ? shared_resource_type() : 0;
  WTS_PROCESS_INFOW* processes = nullptr;
  DWORD count = 0;
  if (type && WTSEnumerateProcessesW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &processes, &count)) {
    const DWORD self = GetCurrentProcessId();
    for (DWORD i = 0; i < count; ++i) {
      const DWORD pid = processes[i].ProcessId;
      if (processes[i].SessionId != session || pid == self || pid <= 4) continue;
      if (std::find(known.begin(), known.end(), pid) != known.end()) continue;
      HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
      if (!process) continue;
      if (holds_type(process, type) && has_module(process, L"GraphicsCapture.dll")) {
        found.push_back({Source::kCapture, pid, process_name(pid), kCaptures, process_path(pid)});
      }
      CloseHandle(process);
    }
    WTSFreeMemory(processes);
  }
  if (found.empty() && known.empty()) found.push_back({Source::kCapture, 0, L"A program", kCaptures, {}});
  if (found.size() > 1) {
    for (Watcher& w : found) w.what = kMayCapture;
  }
  return found;
}

std::vector<Watcher> guess(DWORD session, const std::vector<DWORD>& known) {
  const ULONGLONG now = GetTickCount64();
  Guess& g = g_guesses[session];
  if (!g.since) g.since = now;
  const ULONGLONG every = now - g.since < kLookAgainMs ? kFirstLookMs : kLookAgainMs;
  if (!g.at || now - g.at >= every) {
    g.found = look(session, known);
    g.at = now;
  }
  return g.found;
}

}  // namespace

std::vector<Watcher> running_captures(DWORD session) {
  std::lock_guard<std::mutex> hold(g_lock);
  if (!running_sessions().count(session)) {
    g_guesses.erase(session);
    return {};
  }
  return guess(session, {});
}

void remember_running_captures() {
  std::lock_guard<std::mutex> hold(g_lock);
  g_unexplained = running_sessions();
}

void forget_running_captures(DWORD session) {
  std::lock_guard<std::mutex> hold(g_lock);
  g_unexplained.erase(session);
  g_guesses.erase(session);
}

std::vector<Watcher> earlier_captures(DWORD session, const std::vector<DWORD>& known) {
  std::lock_guard<std::mutex> hold(g_lock);
  if (!g_unexplained.count(session)) return {};
  if (!running_sessions().count(session)) {
    g_unexplained.erase(session);
    g_guesses.erase(session);
    return {};
  }
  return guess(session, known);
}

}  // namespace screenlight
