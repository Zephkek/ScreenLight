#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
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
  ULONGLONG seen = 0;
  bool keys = false;
  bool mouse = false;
};

constexpr ULONG kProcessInformation = 5;
constexpr ULONG kThreadLastSystemCall = 21;
constexpr ULONG kWaiting = 5;
constexpr ULONG kWrUserRequest = 13;
constexpr LONG kInfoLengthMismatch = LONG(0xC0000004);
constexpr ULONGLONG kScanEveryMs = 200;
constexpr ULONGLONG kForgetAfterMs = 3000;

using QuerySystemFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using QueryThreadFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

QuerySystemFn g_query_system;
QueryThreadFn g_query_thread;
USHORT g_send_input;
HWND g_notify;
HANDLE g_thread;
DWORD g_thread_id;
DWORD g_session;
DWORD g_last_tid;
DWORD g_last_pid;
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

USHORT send_input_syscall() {
  if (HMODULE win32u = LoadLibraryW(L"win32u.dll")) {
    const auto* code = reinterpret_cast<const uint8_t*>(GetProcAddress(win32u, "NtUserSendInput"));
    if (code && syscall_stub(code)) return *reinterpret_cast<const uint16_t*>(code + 4);
  }
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  const auto* code = reinterpret_cast<const uint8_t*>(GetProcAddress(user32, "SendInput"));
  if (!code) return 0;
  if (!syscall_stub(code)) code = stub_called_from(code, user32);
  return code ? *reinterpret_cast<const uint16_t*>(code + 4) : 0;
}

bool load_lookup() {
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  g_query_system = reinterpret_cast<QuerySystemFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
  g_query_thread = reinterpret_cast<QueryThreadFn>(GetProcAddress(ntdll, "NtQueryInformationThread"));
  if (g_query_system && g_query_thread) g_send_input = send_input_syscall();
  return g_send_input != 0;
}

bool in_send_input(DWORD tid) {
  HANDLE thread = OpenThread(THREAD_GET_CONTEXT, FALSE, tid);
  if (!thread) return false;
  LastSyscall call{};
  const bool inside =
      g_query_thread(thread, kThreadLastSystemCall, &call, sizeof call, nullptr) >= 0 && call.number == g_send_input;
  CloseHandle(thread);
  return inside;
}

void record(DWORD pid, const std::wstring& name, bool keys, ULONGLONG now) {
  bool fresh = false;
  {
    std::lock_guard<std::mutex> hold(g_lock);
    auto it = std::find_if(g_senders.begin(), g_senders.end(), [pid](const Sender& s) { return s.pid == pid; });
    if (it == g_senders.end()) {
      std::wstring label = name;
      if (label.empty()) label = pid ? process_name(pid) : L"Unknown program";
      it = g_senders.insert(g_senders.end(), Sender{pid, label});
      fresh = true;
    }
    it->seen = now;
    (keys ? it->keys : it->mouse) = true;
  }
  if (fresh && g_notify) PostMessageW(g_notify, kRefreshMessage, 0, 0);
}

void injected(bool keys) {
  const ULONGLONG now = GetTickCount64();
  if (g_last_tid && in_send_input(g_last_tid)) {
    record(g_last_pid, std::wstring(), keys, now);
    return;
  }
  if (now - g_last_scan < kScanEveryMs) return;
  g_last_scan = now;
  DWORD pid = 0, tid = 0;
  std::wstring name;
  if (find_sender(g_session, &pid, &tid)) {
    g_last_pid = pid;
    g_last_tid = tid;
  } else {
    pid = service_sender(&name);
  }
  record(pid, name, keys, now);
}

LRESULT CALLBACK mouse_hook(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION && (reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam)->flags & LLMHF_INJECTED)) injected(false);
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK key_hook(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION && (reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam)->flags & LLKHF_INJECTED)) injected(true);
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

DWORD WINAPI hook_thread(void*) {
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
    const wchar_t* what = s.keys && s.mouse ? L"is sending keys and mouse input"
                          : s.keys          ? L"is sending keys"
                                            : L"is sending mouse input";
    found.push_back({Source::kInput, s.pid, s.name, what});
  }
  return found;
}

}  // namespace screenlight
