#include <windows.h>
#include <evntcons.h>
#include <versionhelpers.h>
#include <evntrace.h>
#include <tdh.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr wchar_t kSessionName[] = L"ScreenLight";
constexpr GUID kWin32k = {0x8c416c79, 0xd49b, 0x4f01, {0xa4, 0x67, 0xe5, 0x6d, 0x3a, 0xa8, 0x23, 0x4c}};
constexpr GUID kWin32k7 = {0xe7ef96be, 0x969f, 0x414f, {0x97, 0xd7, 0x3d, 0xdb, 0x7b, 0x55, 0x8c, 0xcc}};
constexpr GUID kMagnification = {0xc882ff1d, 0x7585, 0x4b33, {0xb1, 0x35, 0x95, 0xc5, 0x77, 0x17, 0x91, 0x37}};
constexpr GUID kDwmApi = {0x292a52c4, 0xfa27, 0x4461, {0xb5, 0x26, 0x54, 0xa4, 0x64, 0x30, 0xbd, 0x54}};
constexpr GUID kDwmApi7 = {0x92ae46d7, 0x6d9c, 0x4727, {0x9e, 0xd5, 0xe4, 0x9a, 0xf9, 0xc2, 0x4c, 0xbf}};
constexpr GUID kGraphicsCapture = {0x7d0cbd25, 0x390e, 0x524d, {0x8c, 0x1e, 0x2a, 0x8e, 0x84, 0x60, 0x55, 0xc0}};
constexpr USHORT kCaptureStarted = 1;
constexpr USHORT kCaptureStopped = 2;
constexpr ULONGLONG kWindowUpdateKeyword = 0x1000;
constexpr ULONGLONG kAuditKeyword = 0x400;
constexpr ULONGLONG kEverything = ~0ULL;
constexpr UCHAR kStopOpcode = 2;
constexpr USHORT kUpdateEvent = 1;
constexpr USHORT kRawInputEvent = 1001;
constexpr USHORT kHookEvent = 1002;
constexpr USHORT kKeyStateEvent = 1003;
constexpr USHORT kInjectMouseEvent = 81;
constexpr USHORT kTouchInjectionEvent = 129;
constexpr USHORT kPointerDeviceEvent = 196;
constexpr USHORT kWin32kEvents[] = {kUpdateEvent, kRawInputEvent, kHookEvent, kKeyStateEvent,
                                    kInjectMouseEvent, kTouchInjectionEvent, kPointerDeviceEvent};
constexpr ULONGLONG kInjectionKeyword = 0x800000;
constexpr ULONGLONG kInjectShowsForMs = 3000;
constexpr ULONG kKeyboardPage = 1;
constexpr ULONG kKeyboardUsage = 6;
constexpr ULONG kKeypadUsage = 7;
constexpr ULONG kBackgroundPolls = 32;
// win32k logs from 1 call/ms and games poll that fast too
constexpr ULONGLONG kPollsPerMs = 4;
constexpr ULONGLONG kPollShowsForMs = 10000;
constexpr wchar_t kListens[] = L"listens to the keyboard";
constexpr int kJournalPlayback = 1;
constexpr ULONG kEventIdFilter = 0x80000200;

struct EventIdFilter {
  BOOLEAN filter_in;
  UCHAR reserved;
  USHORT count;
  USHORT events[ARRAYSIZE(kWin32kEvents)];
};
constexpr ULONG kSyncRequest = 0x80000000;
constexpr ULONGLONG kCalibrateMs = 5000;
constexpr ULONGLONG kReadShowsForMs = 5000;
constexpr ULONGLONG kCaptureLingerMs = 5000;

std::atomic<bool> g_stopping{false};
std::atomic<DWORD> g_calibrating{0};
std::atomic<ULONGLONG> g_calibrate_until{0};
std::atomic<ULONG> g_read_code{0};
std::atomic<bool> g_surface_names_window{false};
std::atomic<DWORD> g_calibrated{0};
std::map<DWORD, std::pair<ULONGLONG, bool>> g_copies;

struct Injector {
  ULONGLONG mouse = 0;
  ULONGLONG touch = 0;
};

std::map<DWORD, Injector> g_injectors;
std::mutex g_pointer_lock;
std::map<DWORD, std::pair<DWORD, ULONGLONG>> g_pointer_makers;
std::vector<BYTE> g_properties;
TRACEHANDLE g_session;
TRACEHANDLE g_consumer = INVALID_PROCESSTRACE_HANDLE;
HANDLE g_thread;

bool uint_property(PEVENT_RECORD record, const wchar_t* name, ULONG* value) {
  PROPERTY_DATA_DESCRIPTOR descriptor{reinterpret_cast<ULONGLONG>(name), ULONG_MAX, 0};
  ULONG size = 0;
  if (TdhGetPropertySize(record, 0, nullptr, 1, &descriptor, &size) != ERROR_SUCCESS || size > sizeof(ULONGLONG)) return false;
  ULONGLONG raw = 0;
  if (TdhGetProperty(record, 0, nullptr, 1, &descriptor, size, reinterpret_cast<BYTE*>(&raw)) != ERROR_SUCCESS) return false;
  *value = ULONG(raw);
  return true;
}

bool text_property(PEVENT_RECORD record, const wchar_t* name, std::wstring* value) {
  PROPERTY_DATA_DESCRIPTOR descriptor{reinterpret_cast<ULONGLONG>(name), ULONG_MAX, 0};
  ULONG size = 0;
  if (TdhGetPropertySize(record, 0, nullptr, 1, &descriptor, &size) != ERROR_SUCCESS || size > 4096) return false;
  if (size < sizeof(wchar_t)) {
    value->clear();
    return true;
  }
  std::vector<wchar_t> text(size / sizeof(wchar_t) + 1);
  if (TdhGetProperty(record, 0, nullptr, 1, &descriptor, size, reinterpret_cast<BYTE*>(text.data())) != ERROR_SUCCESS) return false;
  value->assign(text.data());
  return true;
}

bool windows_file(DWORD pid, const wchar_t* file) {
  wchar_t windows[MAX_PATH];
  const UINT length = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
  if (!length || length >= ARRAYSIZE(windows)) return false;
  return _wcsicmp(process_path(pid).c_str(), (std::wstring(windows, length) + file).c_str()) == 0;
}

ULONGLONG process_started(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return 0;
  FILETIME created{}, exited, kernel, user;
  const bool ok = GetProcessTimes(process, &created, &exited, &kernel, &user);
  CloseHandle(process);
  return ok ? (ULONGLONG(created.dwHighDateTime) << 32) | created.dwLowDateTime : 0;
}

bool own_copy(DWORD pid) {
  static const std::vector<char> self = [] {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return length && length < ARRAYSIZE(path) ? file_bytes(path) : std::vector<char>();
  }();
  const ULONGLONG started = process_started(pid);
  if (!started) return false;
  if (const auto it = g_copies.find(pid); it != g_copies.end() && it->second.first == started) return it->second.second;
  const std::wstring path = process_path(pid);
  const bool same = !self.empty() && !path.empty() && file_bytes(path) == self;
  if (g_copies.size() > 256) g_copies.clear();
  g_copies[pid] = {started, same};
  return same;
}

void screen_update(PEVENT_RECORD record) {
  ULONG type = 0;
  if (!uint_property(record, L"Type", &type) || !(type & kSyncRequest)) return;
  const DWORD pid = record->EventHeader.ProcessId;
  const ULONGLONG now = GetTickCount64();
  if (pid == g_calibrating && now < g_calibrate_until) {
    g_read_code = type;
    g_calibrating = 0;
    g_calibrated = pid;
    return;
  }
  if (type != g_read_code || (pid == g_calibrated && now < g_calibrate_until) || own_copy(pid)) return;
  if (!windows_file(pid, L"\\System32\\consent.exe")) track_seen(Source::kScreenRead, pid, L"reads the screen", kReadShowsForMs);
}

bool windows_component(DWORD pid) {
  wchar_t windows[MAX_PATH];
  const UINT length = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
  if (!length || length >= ARRAYSIZE(windows)) return false;
  const std::wstring folder = std::wstring(windows, length) + L"\\";
  const std::wstring path = process_path(pid);
  return path.size() > folder.size() && _wcsnicmp(path.c_str(), folder.c_str(), folder.size()) == 0 && !script_host(path);
}

bool trusted_listener(DWORD pid) { return windows_component(pid) || own_copy(pid); }

void injection(DWORD pid, USHORT id) {
  if (pid <= 4 || trusted_listener(pid)) return;
  if (id == kPointerDeviceEvent) {
    DWORD session = 0;
    if (!ProcessIdToSessionId(pid, &session)) return;
    std::lock_guard<std::mutex> hold(g_pointer_lock);
    g_pointer_makers[session] = {pid, process_started(pid)};
    return;
  }
  const ULONGLONG now = GetTickCount64();
  Injector& seen = g_injectors[pid];
  (id == kInjectMouseEvent ? seen.mouse : seen.touch) = now;
  std::vector<std::wstring> parts;
  if (now - seen.mouse < kInjectShowsForMs) parts.push_back(L"mouse input");
  if (now - seen.touch < kInjectShowsForMs) parts.push_back(L"touch input");
  std::wstring what = L"simulates";
  for (size_t i = 0; i < parts.size(); ++i) what += (i == 0 ? L" " : L" and ") + parts[i];
  if (!parts.empty()) track_seen(Source::kInput, pid, what, kInjectShowsForMs);
  if (g_injectors.size() > 64) {
    for (auto it = g_injectors.begin(); it != g_injectors.end();) {
      const ULONGLONG last = std::max(it->second.mouse, it->second.touch);
      it = now - last > kInjectShowsForMs ? g_injectors.erase(it) : std::next(it);
    }
  }
}

void keyboard_listener(PEVENT_RECORD record) {
  ULONG pid = record->EventHeader.ProcessId;
  switch (record->EventHeader.EventDescriptor.Id) {
    case kInjectMouseEvent:
    case kTouchInjectionEvent:
    case kPointerDeviceEvent:
      injection(pid, record->EventHeader.EventDescriptor.Id);
      return;
    case kHookEvent: {
      // TODO: hooks set before the trace started never show up here
      ULONG type = 0, result = 0;
      if (!uint_property(record, L"FilterType", &type) || !uint_property(record, L"ReturnValue", &result) || !result) return;
      if (type == kJournalPlayback) {
        if (!trusted_listener(pid)) track_open(Source::kInput, pid, L"replays recorded input");
        return;
      }
      if (type != WH_KEYBOARD && type != WH_KEYBOARD_LL && type != WH_JOURNALRECORD) return;
      std::wstring library;
      if (type == WH_KEYBOARD && text_property(record, L"pstrLib", &library) && library.empty()) return;
      if (!trusted_listener(pid)) track_open(Source::kKeyboard, pid, kListens);
      return;
    }
    case kRawInputEvent: {
      ULONG page = 0, usage = 0, flags = 0, result = 0;
      if (!uint_property(record, L"UsagePage", &page) || !uint_property(record, L"Usage", &usage) ||
          !uint_property(record, L"Flags", &flags) || !uint_property(record, L"ReturnValue", &result) ||
          !uint_property(record, L"ProcessId", &pid) || !result) {
        return;
      }
      if (page != kKeyboardPage || (usage != kKeyboardUsage && usage != kKeypadUsage)) return;
      if (flags & RIDEV_REMOVE) {
        track_close(Source::kKeyboard, pid);
      } else if ((flags & (RIDEV_INPUTSINK | RIDEV_EXINPUTSINK)) && !trusted_listener(pid)) {
        track_open(Source::kKeyboard, pid, kListens);
      }
      return;
    }
    case kKeyStateEvent: {
      ULONG calls = 0, since = 0;
      if (!uint_property(record, L"PID", &pid) || !uint_property(record, L"BackgroundCallCount", &calls) ||
          !uint_property(record, L"MsSinceLastKeyEvent", &since)) {
        return;
      }
      const bool sweeping = calls >= kBackgroundPolls && calls >= since * kPollsPerMs;
      if (sweeping && !trusted_listener(pid)) track_seen(Source::kKeyboard, pid, kListens, kPollShowsForMs);
      return;
    }
  }
}

enum class DwmCall { kOther, kRegisterThumbnail, kUnregisterThumbnail, kSharedSurface };

DwmCall dwm_call(PEVENT_RECORD record) {
  static std::map<std::pair<USHORT, UCHAR>, DwmCall> known;
  const EVENT_DESCRIPTOR& event = record->EventHeader.EventDescriptor;
  const auto key = std::make_pair(event.Id, event.Version);
  if (const auto it = known.find(key); it != known.end()) return it->second;
  DwmCall call = DwmCall::kOther;
  ULONG size = 0;
  if (TdhGetEventInformation(record, 0, nullptr, nullptr, &size) == ERROR_INSUFFICIENT_BUFFER) {
    std::vector<BYTE> buffer(size);
    auto* info = reinterpret_cast<TRACE_EVENT_INFO*>(buffer.data());
    if (TdhGetEventInformation(record, 0, nullptr, info, &size) == ERROR_SUCCESS && info->TaskNameOffset) {
      const std::wstring task = reinterpret_cast<const wchar_t*>(buffer.data() + info->TaskNameOffset);
      if (task.rfind(L"ApiRegisterThumbnail", 0) == 0) call = DwmCall::kRegisterThumbnail;
      if (task.rfind(L"ApiUnregisterThumbnail", 0) == 0) call = DwmCall::kUnregisterThumbnail;
      if (task.rfind(L"ApipDxGetWindowSharedSurface", 0) == 0) call = DwmCall::kSharedSurface;
    }
  }
  known.emplace(key, call);
  return call;
}

void dwm_api(PEVENT_RECORD record) {
  const bool stop = record->EventHeader.EventDescriptor.Opcode == kStopOpcode;
  const DWORD pid = record->EventHeader.ProcessId;
  switch (dwm_call(record)) {
    case DwmCall::kRegisterThumbnail:
      if (!stop && !windows_file(pid, L"\\explorer.exe")) track_open(Source::kThumbnail, pid, L"mirrors a window");
      break;
    case DwmCall::kUnregisterThumbnail:
      if (!stop) track_close(Source::kThumbnail, pid);
      break;
    case DwmCall::kSharedSurface: {
      ULONG window = 0;
      const bool named = uint_property(record, L"Hwnd", &window) && window;
      if (named) g_surface_names_window = true;
      if ((!named && (stop || g_surface_names_window)) || tracking(Source::kMagnifier, pid)) break;
      track_seen(Source::kThumbnail, pid, L"copies a window", kReadShowsForMs, window);
      break;
    }
    case DwmCall::kOther:
      break;
  }
}

void graphics_capture(PEVENT_RECORD record) {
  ULONG pid = 0;
  if (!uint_property(record, L"processId", &pid) || !pid) return;
  const USHORT id = record->EventHeader.EventDescriptor.Id;
  if (id == kCaptureStarted) track_open(Source::kCapture, pid, L"captures the screen");
  if (id != kCaptureStopped) return;
  DWORD session = 0;
  if (!tracking(Source::kCapture, pid) && ProcessIdToSessionId(pid, &session)) forget_running_captures(session);
  track_close(Source::kCapture, pid, kCaptureLingerMs);
}

EVENT_TRACE_PROPERTIES* properties() {
  g_properties.assign(sizeof(EVENT_TRACE_PROPERTIES) + sizeof kSessionName, 0);
  auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(g_properties.data());
  props->Wnode.BufferSize = ULONG(g_properties.size());
  props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
  props->Wnode.ClientContext = 1;
  props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
  props->BufferSize = 64;
  props->MinimumBuffers = 4;
  props->MaximumBuffers = 32;
  props->FlushTimer = 1;
  props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
  return props;
}

void enable_win32k() {
  const ULONGLONG keywords = kWindowUpdateKeyword | kAuditKeyword;
  if (IsWindows8Point1OrGreater()) {
    EventIdFilter ids{TRUE, 0, USHORT(ARRAYSIZE(kWin32kEvents)), {}};
    for (size_t i = 0; i < ARRAYSIZE(kWin32kEvents); ++i) ids.events[i] = kWin32kEvents[i];
    EVENT_FILTER_DESCRIPTOR descriptor{reinterpret_cast<ULONGLONG>(&ids), sizeof ids, kEventIdFilter};
    ENABLE_TRACE_PARAMETERS parameters{};
    parameters.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    parameters.EnableFilterDesc = &descriptor;
    parameters.FilterDescCount = 1;
    if (EnableTraceEx2(g_session, &kWin32k, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE,
                       keywords | kInjectionKeyword, 0, 0, &parameters) == ERROR_SUCCESS) {
      return;
    }
  }
  EnableTraceEx2(g_session, &kWin32k, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, keywords, 0, 0, nullptr);
}

DWORD WINAPI consume(void*) {
  ProcessTrace(&g_consumer, 1, nullptr, nullptr);
  return 0;
}

}  // namespace

void WINAPI handle_trace_event(PEVENT_RECORD record) {
  const EVENT_HEADER& header = record->EventHeader;
  if ((header.ProviderId == kWin32k || header.ProviderId == kWin32k7) && header.EventDescriptor.Id == kUpdateEvent) {
    screen_update(record);
  } else if (header.ProviderId == kWin32k) {
    keyboard_listener(record);
  } else if (header.ProviderId == kMagnification) {
    track_seen(Source::kMagnifier, header.ProcessId, L"magnifies the screen", kReadShowsForMs);
  } else if (header.ProviderId == kDwmApi || header.ProviderId == kDwmApi7) {
    dwm_api(record);
  } else if (header.ProviderId == kGraphicsCapture) {
    graphics_capture(record);
  }
}

DWORD pointer_maker(DWORD session) {
  std::lock_guard<std::mutex> hold(g_pointer_lock);
  const auto it = g_pointer_makers.find(session);
  if (it == g_pointer_makers.end()) return 0;
  if (process_started(it->second.first) != it->second.second) {
    g_pointer_makers.erase(it);
    return 0;
  }
  return it->second.first;
}

void begin_calibration(DWORD pid) {
  g_calibrate_until = GetTickCount64() + kCalibrateMs;
  g_calibrating = pid;
}

ULONG read_code() { return g_read_code; }

bool start_trace() {
  g_stopping = false;
  ControlTraceW(0, kSessionName, properties(), EVENT_TRACE_CONTROL_STOP);
  if (StartTraceW(&g_session, kSessionName, properties()) != ERROR_SUCCESS) return false;
  enable_win32k();
  EnableTraceEx2(g_session, &kWin32k7, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, kWindowUpdateKeyword,
                 0, 0, nullptr);
  for (const GUID& provider : {kMagnification, kDwmApi, kDwmApi7, kGraphicsCapture}) {
    EnableTraceEx2(g_session, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE, kEverything, 0, 0,
                   nullptr);
  }
  EVENT_TRACE_LOGFILEW log{};
  log.LoggerName = const_cast<wchar_t*>(kSessionName);
  log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
  log.EventRecordCallback = handle_trace_event;
  g_consumer = OpenTraceW(&log);
  if (g_consumer == INVALID_PROCESSTRACE_HANDLE) return false;
  g_thread = CreateThread(nullptr, 0, consume, nullptr, 0, nullptr);
  remember_running_captures();
  return g_thread != nullptr;
}

bool trace_alive() { return g_thread && WaitForSingleObject(g_thread, 0) == WAIT_TIMEOUT; }

void ensure_trace() {
  if (g_stopping.load() || trace_alive()) return;
  stop_trace();
  start_trace();
}

bool flush_needed() { return !IsWindows8OrGreater(); }

void flush_trace() { ControlTraceW(g_session, nullptr, properties(), EVENT_TRACE_CONTROL_FLUSH); }

void stop_trace() {
  g_stopping = true;
  ControlTraceW(g_session, nullptr, properties(), EVENT_TRACE_CONTROL_STOP);
  if (g_consumer != INVALID_PROCESSTRACE_HANDLE) {
    CloseTrace(g_consumer);
    g_consumer = INVALID_PROCESSTRACE_HANDLE;
  }
  if (g_thread) {
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    g_thread = nullptr;
  }
}

}  // namespace screenlight
