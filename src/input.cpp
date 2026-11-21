#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

struct SystemThread {
  LARGE_INTEGER kernel_time, user_time, create_time;
  ULONG wait_time;
  PVOID start;
  HANDLE pid, tid;
  LONG priority, base_priority;
  ULONG switches, state, wait_reason;
};

struct SystemProcess {
  ULONG next, threads;
  LARGE_INTEGER ws_private;
  ULONG hard_faults, threads_high;
  ULONGLONG cycles;
  LARGE_INTEGER create_time, user_time, kernel_time;
  USHORT name_length, name_max;
  PWSTR name;
  LONG base_priority;
  HANDLE pid, parent;
  ULONG handles, session;
  ULONG_PTR key;
  SIZE_T peak_vsize, vsize;
  ULONG page_faults;
  SIZE_T peak_ws, ws, peak_paged, paged, peak_nonpaged, nonpaged, pagefile, peak_pagefile, private_pages;
  LARGE_INTEGER io[6];
};

static_assert(sizeof(SystemProcess) == 0x100 && sizeof(SystemThread) == 0x50, "process info layout changed");

struct LastSyscall {
  PVOID first_argument;
  USHORT number;
  USHORT pad[3];
};

struct Sender {
  DWORD pid = 0;
  std::wstring name;
  std::wstring path;
  ULONGLONG seen = 0;
  bool keys = false;
  bool mouse = false;
  bool touch = false;
  bool pen = false;
};

enum class Kind { kKeys, kMouse, kTouch, kPen };

constexpr ULONG kProcessInformation = 5;
constexpr ULONG kThreadLastSystemCall = 21;
constexpr ULONG kWaiting = 5;
constexpr ULONG kWrUserRequest = 13;
constexpr LONG kInfoLengthMismatch = LONG(0xC0000004);
constexpr ULONGLONG kScanEveryMs = 200;
constexpr ULONGLONG kForgetAfterMs = 3000;
constexpr size_t kKnownThreads = 8;

using QuerySystemFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using QueryThreadFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

QuerySystemFn g_query_system;
QueryThreadFn g_query_thread;
std::vector<USHORT> g_inject_calls;
HWND g_notify;
HANDLE g_thread;
DWORD g_thread_id;
DWORD g_session;
std::vector<std::pair<DWORD, DWORD>> g_known;
ULONGLONG g_last_scan;
std::vector<uint8_t> g_snapshot;
std::mutex g_lock;
std::vector<Sender> g_senders;

bool syscall_stub(const uint8_t* code) {
  return code[0] == 0x4c && code[1] == 0x8b && code[2] == 0xd1 && code[3] == 0xb8;
}

const uint8_t* stub_called_from(const uint8_t* code, HMODULE module) {
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
  const auto* begin = reinterpret_cast<const uint8_t*>(module);
  const uint8_t* end = begin + nt->OptionalHeader.SizeOfImage;
  for (int i = 0; i < 64 && code + i + 5 <= end; ++i) {
    if (code[i] != 0xe8) continue;
    const uint8_t* target = code + i + 5 + *reinterpret_cast<const int32_t*>(code + i + 1);
    if (target >= begin && target + 8 <= end && syscall_stub(target)) return target;
  }
  return nullptr;
}

constexpr const char* kInjectCalls[] = {"NtUserSendInput",          "NtUserInjectKeyboardInput", "NtUserInjectMouseInput",
                                         "NtUserInjectTouchInput",   "NtUserInjectPointerInput",  "NtUserInjectGenericHidInput",
                                         "NtUserInjectDeviceInput",  "NtUserInjectGesture",       "NtUserInjectTouchpadAction"};

std::vector<USHORT> inject_syscalls() {
  std::vector<USHORT> numbers;
  if (HMODULE win32u = LoadLibraryW(L"win32u.dll")) {
    for (const char* name : kInjectCalls) {
      const auto* code = reinterpret_cast<const uint8_t*>(GetProcAddress(win32u, name));
      if (code && syscall_stub(code)) numbers.push_back(*reinterpret_cast<const uint16_t*>(code + 4));
    }
  }
  if (!numbers.empty()) return numbers;
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  const auto* code = reinterpret_cast<const uint8_t*>(GetProcAddress(user32, "SendInput"));
  if (code && !syscall_stub(code)) code = stub_called_from(code, user32);
  if (code) numbers.push_back(*reinterpret_cast<const uint16_t*>(code + 4));
  return numbers;
}

bool load_lookup() {
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  g_query_system = reinterpret_cast<QuerySystemFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
  g_query_thread = reinterpret_cast<QueryThreadFn>(GetProcAddress(ntdll, "NtQueryInformationThread"));
  if (g_query_system && g_query_thread) g_inject_calls = inject_syscalls();
  return !g_inject_calls.empty();
}

bool in_send_input(DWORD tid) {
  HANDLE thread = OpenThread(THREAD_GET_CONTEXT, FALSE, tid);
  if (!thread) return false;
  LastSyscall call{};
  const bool inside = g_query_thread(thread, kThreadLastSystemCall, &call, sizeof call, nullptr) >= 0 &&
                      std::find(g_inject_calls.begin(), g_inject_calls.end(), call.number) != g_inject_calls.end();
  CloseHandle(thread);
  return inside;
}

void record(DWORD pid, const std::wstring& name, const std::wstring& path, Kind kind, ULONGLONG now) {
  bool fresh = false;
  {
    std::lock_guard<std::mutex> hold(g_lock);
    auto it = std::find_if(g_senders.begin(), g_senders.end(), [pid](const Sender& s) { return s.pid == pid; });
    if (it == g_senders.end()) {
      std::wstring label = name;
      if (label.empty()) label = process_name(pid);
      it = g_senders.insert(g_senders.end(), Sender{pid, label, path.empty() && pid ? process_path(pid) : path});
      fresh = true;
    }
    it->seen = now;
    (kind == Kind::kKeys ? it->keys : kind == Kind::kMouse ? it->mouse : kind == Kind::kTouch ? it->touch : it->pen) = true;
  }
  if (fresh && g_notify) PostMessageW(g_notify, kRefreshMessage, 0, 0);
}

void injected(Kind kind) {
  const ULONGLONG now = GetTickCount64();
  for (size_t i = 0; i < g_known.size(); ++i) {
    if (!in_send_input(g_known[i].first)) continue;
    std::rotate(g_known.begin(), g_known.begin() + i, g_known.begin() + i + 1);
    record(g_known[0].second, std::wstring(), std::wstring(), kind, now);
    return;
  }
  if (now - g_last_scan < kScanEveryMs) return;
  g_last_scan = now;
  DWORD pid = 0, tid = 0;
  std::wstring name, path;
  if (find_sender(g_session, &pid, &tid)) {
    g_known.insert(g_known.begin(), {tid, pid});
    if (g_known.size() > kKnownThreads) g_known.pop_back();
  } else {
    pid = service_sender(&name, &path);
  }
  if (pid) {
    record(pid, name, path, kind, now);
  } else if (!service_running()) {
    record(0, L"Full detection needed", std::wstring(), kind, now);
  } else if (kind == Kind::kKeys) {
    record(0, L"Protected program", std::wstring(), kind, now);
  }
}

DWORD g_pointer_pid;
std::wstring g_pointer_name, g_pointer_path;
ULONGLONG g_pointer_at;

void pointer_injected(Kind kind) {
  const ULONGLONG now = GetTickCount64();
  if (!service_running()) {
    record(0, L"Full detection needed", std::wstring(), kind, now);
    return;
  }
  if (!g_pointer_at || now - g_pointer_at > 1000) {
    g_pointer_pid = service_pointer(&g_pointer_name, &g_pointer_path);
    g_pointer_at = now;
  }
  if (g_pointer_pid) record(g_pointer_pid, g_pointer_name, g_pointer_path, kind, now);
}

constexpr ULONG_PTR kPointerSignature = 0xFF515700;
constexpr ULONG_PTR kSignatureMask = 0xFFFFFF00;
constexpr ULONG_PTR kTouchBit = 0x80;

LRESULT CALLBACK mouse_hook(int code, WPARAM wparam, LPARAM lparam) {
  const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
  if (code == HC_ACTION && (info->flags & LLMHF_INJECTED)) {
    if ((info->dwExtraInfo & kSignatureMask) == kPointerSignature) {
      pointer_injected((info->dwExtraInfo & kTouchBit) ? Kind::kTouch : Kind::kPen);
    } else {
      injected(Kind::kMouse);
    }
  }
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK key_hook(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION && (reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam)->flags & LLKHF_INJECTED)) injected(Kind::kKeys);
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

DWORD WINAPI hook_thread(void*) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  HINSTANCE self = GetModuleHandleW(nullptr);
  HHOOK mouse = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook, self, 0);
  HHOOK keys = SetWindowsHookExW(WH_KEYBOARD_LL, key_hook, self, 0);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
  if (mouse) UnhookWindowsHookEx(mouse);
  if (keys) UnhookWindowsHookEx(keys);
  return 0;
}

}  // namespace

bool find_sender(DWORD session, DWORD* pid, DWORD* tid) {
  static const bool ready = load_lookup();
  if (!ready) return false;
  ULONG need = 0;
  LONG status;
  while ((status = g_query_system(kProcessInformation, g_snapshot.data(), ULONG(g_snapshot.size()), &need)) ==
         kInfoLengthMismatch) {
    g_snapshot.resize(need + 0x10000);
  }
  if (status < 0) return false;
  const DWORD self = GetCurrentProcessId();
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t offset = 0;;) {
      const auto* process = reinterpret_cast<const SystemProcess*>(g_snapshot.data() + offset);
      const auto* threads = reinterpret_cast<const SystemThread*>(process + 1);
      const DWORD owner = DWORD(ULONG_PTR(process->pid));
      const bool candidate = owner > 4 && owner != self && process->session == session;
      for (ULONG i = 0; candidate && i < process->threads; ++i) {
        if (threads[i].state != kWaiting || (threads[i].wait_reason == kWrUserRequest) != (pass == 0)) continue;
        const DWORD id = DWORD(ULONG_PTR(threads[i].tid));
        if (!in_send_input(id)) continue;
        *pid = owner;
        *tid = id;
        return true;
      }
      if (!process->next) break;
      offset += process->next;
    }
  }
  return false;
}

void start_input_watch(HWND notify) {
  if (g_thread) return;
  ProcessIdToSessionId(GetCurrentProcessId(), &g_session);
  g_notify = notify;
  g_thread = CreateThread(nullptr, 0, hook_thread, nullptr, 0, &g_thread_id);
}

void stop_input_watch() {
  if (!g_thread) return;
  PostThreadMessageW(g_thread_id, WM_QUIT, 0, 0);
  WaitForSingleObject(g_thread, 2000);
  CloseHandle(g_thread);
  g_thread = nullptr;
}

std::vector<Watcher> input_senders() {
  const ULONGLONG now = GetTickCount64();
  std::vector<Watcher> found;
  std::lock_guard<std::mutex> hold(g_lock);
  g_senders.erase(std::remove_if(g_senders.begin(), g_senders.end(),
                                 [now](const Sender& s) { return now - s.seen > kForgetAfterMs; }),
                  g_senders.end());
  for (const Sender& s : g_senders) {
    std::vector<std::wstring> parts;
    if (s.keys) parts.push_back(L"key presses");
    if (s.mouse) parts.push_back(L"mouse input");
    if (s.touch) parts.push_back(L"touch input");
    if (s.pen) parts.push_back(L"pen input");
    std::wstring what = L"simulates";
    for (size_t i = 0; i < parts.size(); ++i) what += (i == 0 ? L" " : i + 1 == parts.size() ? L" and " : L", ") + parts[i];
    found.push_back({Source::kInput, s.pid, s.name, what, s.path});
  }
  return found;
}

}  // namespace screenlight
