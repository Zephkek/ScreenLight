#include <windows.h>
#include <evntcons.h>
#include <evntrace.h>
#include <tdh.h>

#include <atomic>
#include <climits>
#include <cwchar>
#include <map>
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
constexpr ULONGLONG kEverything = ~0ULL;
constexpr UCHAR kStopOpcode = 2;
constexpr USHORT kUpdateEvent = 1;
constexpr ULONG kSyncRequest = 0x80000000;
constexpr ULONGLONG kCalibrateMs = 5000;
constexpr ULONGLONG kReadShowsForMs = 5000;

std::atomic<DWORD> g_calibrating{0};
std::atomic<ULONGLONG> g_calibrate_until{0};
std::atomic<ULONG> g_read_code{0};
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

bool windows_file(DWORD pid, const wchar_t* file) {
  wchar_t windows[MAX_PATH];
  const UINT length = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
  if (!length || length >= ARRAYSIZE(windows)) return false;
  return _wcsicmp(process_path(pid).c_str(), (std::wstring(windows, length) + file).c_str()) == 0;
}

void screen_update(PEVENT_RECORD record) {
  ULONG type = 0;
  if (!uint_property(record, L"Type", &type) || !(type & kSyncRequest)) return;
  const DWORD pid = record->EventHeader.ProcessId;
  if (pid == g_calibrating && GetTickCount64() < g_calibrate_until) {
    g_read_code = type;
    g_calibrating = 0;
    return;
  }
  if (type == g_read_code && !windows_file(pid, L"\\System32\\consent.exe")) {
    track_seen(Source::kScreenRead, pid, L"reads the screen", kReadShowsForMs);
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
  if (record->EventHeader.EventDescriptor.Opcode == kStopOpcode) return;
  const DWORD pid = record->EventHeader.ProcessId;
  switch (dwm_call(record)) {
    case DwmCall::kRegisterThumbnail:
      if (!windows_file(pid, L"\\explorer.exe")) track_open(Source::kThumbnail, pid, L"shows a live copy of a window");
      break;
    case DwmCall::kUnregisterThumbnail:
      track_close(Source::kThumbnail, pid);
      break;
    case DwmCall::kSharedSurface:
      if (!tracking(Source::kMagnifier, pid)) track_seen(Source::kThumbnail, pid, L"copies a window through dwm", kReadShowsForMs);
      break;
    case DwmCall::kOther:
      break;
  }
}

void graphics_capture(PEVENT_RECORD record) {
  ULONG pid = 0;
  if (!uint_property(record, L"processId", &pid) || !pid) return;
  const USHORT id = record->EventHeader.EventDescriptor.Id;
  if (id == kCaptureStarted) track_open(Source::kCapture, pid, L"captures the screen or a window");
  if (id == kCaptureStopped) track_close(Source::kCapture, pid);
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

DWORD WINAPI consume(void*) {
  ProcessTrace(&g_consumer, 1, nullptr, nullptr);
  return 0;
}

}  // namespace

void WINAPI handle_trace_event(PEVENT_RECORD record) {
  const EVENT_HEADER& header = record->EventHeader;
  if ((header.ProviderId == kWin32k || header.ProviderId == kWin32k7) && header.EventDescriptor.Id == kUpdateEvent) {
    screen_update(record);
  } else if (header.ProviderId == kMagnification) {
    track_seen(Source::kMagnifier, header.ProcessId, L"magnifies the screen", kReadShowsForMs);
  } else if (header.ProviderId == kDwmApi || header.ProviderId == kDwmApi7) {
    dwm_api(record);
  } else if (header.ProviderId == kGraphicsCapture) {
    graphics_capture(record);
  }
}

void begin_calibration(DWORD pid) {
  g_calibrate_until = GetTickCount64() + kCalibrateMs;
  g_calibrating = pid;
}

ULONG read_code() { return g_read_code; }

bool start_trace() {
  ControlTraceW(0, kSessionName, properties(), EVENT_TRACE_CONTROL_STOP);
  if (StartTraceW(&g_session, kSessionName, properties()) != ERROR_SUCCESS) return false;
  for (const GUID& provider : {kWin32k, kWin32k7}) {
    EnableTraceEx2(g_session, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION,
                   kWindowUpdateKeyword, 0, 0, nullptr);
  }
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
  return g_thread != nullptr;
}

void flush_trace() { ControlTraceW(g_session, nullptr, properties(), EVENT_TRACE_CONTROL_FLUSH); }

void stop_trace() {
  ControlTraceW(g_session, nullptr, properties(), EVENT_TRACE_CONTROL_STOP);
  if (g_consumer != INVALID_PROCESSTRACE_HANDLE) CloseTrace(g_consumer);
  if (g_thread) {
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    g_thread = nullptr;
  }
}

}  // namespace screenlight
