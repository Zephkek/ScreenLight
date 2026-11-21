#include <windows.h>
#include <sddl.h>

#include <cwchar>
#include <string>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\ScreenLight";
constexpr wchar_t kHello[] = L"screenlight 1";
constexpr DWORD kBufferBytes = 64 * 1024;
constexpr DWORD kInstances = 4;
constexpr DWORD kClientMs = 1000;
constexpr DWORD kAskMs = 1000;
constexpr DWORD kSenderMs = 50;

bool g_service_seen;

std::wstring hello() {
  wchar_t text[48];
  swprintf(text, ARRAYSIZE(text), L"%ls %lx", kHello, read_code());
  return text;
}

std::wstring answer(HANDLE pipe, const std::wstring& request) {
  ULONG session = 0;
  GetNamedPipeClientSessionId(pipe, &session);
  std::wstring reply = hello();
  if (request == L"calibrate") {
    ULONG pid = 0;
    if (GetNamedPipeClientProcessId(pipe, &pid)) begin_calibration(pid);
    return reply;
  }
  if (request == L"sender") {
    DWORD pid = 0, tid = 0;
    if (find_sender(session, &pid, &tid)) {
      reply += L"\n" + std::to_wstring(pid) + L"\t" + process_name(pid) + L"\t" + process_path(pid);
    }
    return reply;
  }
  if (request != L"list") return reply;
  std::vector<Watcher> list = tracked(session);
  std::vector<DWORD> known;
  for (const Watcher& w : list) {
    if (w.source == Source::kCapture) known.push_back(w.pid);
  }
  for (Watcher& w : earlier_captures(session, known)) list.push_back(std::move(w));
  for (const Watcher& w : list) {
    reply += L"\n" + std::to_wstring(int(w.source)) + L"\t" + std::to_wstring(w.pid) + L"\t" + w.name + L"\t" + w.what + L"\t" + w.path + L"\t" + std::to_wstring(w.window);
  }
  return reply;
}

bool transfer(HANDLE pipe, HANDLE done, bool write, void* data, DWORD size, DWORD* moved) {
  OVERLAPPED overlapped{};
  overlapped.hEvent = done;
  ResetEvent(done);
  const BOOL ok = write ? WriteFile(pipe, data, size, nullptr, &overlapped) : ReadFile(pipe, data, size, nullptr, &overlapped);
  if (!ok && GetLastError() != ERROR_IO_PENDING) return false;
  if (WaitForSingleObject(done, kClientMs) != WAIT_OBJECT_0) {
    CancelIo(pipe);
    GetOverlappedResult(pipe, &overlapped, moved, TRUE);
    return false;
  }
  return GetOverlappedResult(pipe, &overlapped, moved, FALSE);
}

void serve(HANDLE pipe, HANDLE done) {
  wchar_t request[64];
  DWORD read = 0;
  if (!transfer(pipe, done, false, request, sizeof request - sizeof(wchar_t), &read)) return;
  request[read / sizeof(wchar_t)] = 0;
  std::wstring reply = answer(pipe, request);
  DWORD written = 0;
  if (transfer(pipe, done, true, reply.data(), DWORD(reply.size() * sizeof(wchar_t)), &written)) FlushFileBuffers(pipe);
}

HANDLE make_pipe(SECURITY_ATTRIBUTES* security) {
  return CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                          PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, kInstances,
                          kBufferBytes, kBufferBytes, 0, security);
}

HANDLE next_pipe(SECURITY_ATTRIBUTES* security, HANDLE stop) {
  HANDLE pipe = make_pipe(security);
  while (pipe == INVALID_HANDLE_VALUE && WaitForSingleObject(stop, 1000) == WAIT_TIMEOUT) pipe = make_pipe(security);
  return pipe;
}

bool wait_for_client(HANDLE pipe, HANDLE done, HANDLE stop) {
  OVERLAPPED overlapped{};
  overlapped.hEvent = done;
  ResetEvent(done);
  if (ConnectNamedPipe(pipe, &overlapped) || GetLastError() == ERROR_PIPE_CONNECTED) return true;
  if (GetLastError() != ERROR_IO_PENDING) return false;
  HANDLE waits[] = {done, stop};
  if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
    CancelIo(pipe);
    return false;
  }
  DWORD ignored = 0;
  return GetOverlappedResult(pipe, &overlapped, &ignored, FALSE);
}

HANDLE open_service() {
  return CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
}

std::wstring ask(const wchar_t* request, DWORD wait_ms) {
  HANDLE pipe = open_service();
  if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY && WaitNamedPipeW(kPipeName, wait_ms)) {
    pipe = open_service();
  }
  if (pipe == INVALID_HANDLE_VALUE) return std::wstring();
  std::wstring reply;
  std::vector<wchar_t> buffer(kBufferBytes / sizeof(wchar_t));
  DWORD mode = PIPE_READMODE_MESSAGE, read = 0;
  OVERLAPPED overlapped{};
  overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (overlapped.hEvent && SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) &&
      (TransactNamedPipe(pipe, const_cast<wchar_t*>(request), DWORD(wcslen(request) * sizeof(wchar_t)), buffer.data(),
                         kBufferBytes, nullptr, &overlapped) ||
       GetLastError() == ERROR_IO_PENDING)) {
    if (WaitForSingleObject(overlapped.hEvent, wait_ms) != WAIT_OBJECT_0) CancelIo(pipe);
    if (GetOverlappedResult(pipe, &overlapped, &read, TRUE)) reply.assign(buffer.data(), read / sizeof(wchar_t));
  }
  if (overlapped.hEvent) CloseHandle(overlapped.hEvent);
  CloseHandle(pipe);
  return reply;
}

void calibrate() {
  if (ask_service(L"calibrate").rfind(kHello, 0) != 0) return;
  HDC screen = GetDC(nullptr);
  HDC memory = CreateCompatibleDC(screen);
  HBITMAP pixel = CreateCompatibleBitmap(screen, 1, 1);
  HGDIOBJ old = SelectObject(memory, pixel);
  BitBlt(memory, 0, 0, 1, 1, screen, 0, 0, SRCCOPY);
  SelectObject(memory, old);
  DeleteObject(pixel);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
}

std::vector<std::wstring> split(const std::wstring& text, wchar_t separator) {
  std::vector<std::wstring> parts;
  size_t start = 0;
  for (size_t at; (at = text.find(separator, start)) != std::wstring::npos; start = at + 1) {
    parts.push_back(text.substr(start, at - start));
  }
  parts.push_back(text.substr(start));
  return parts;
}

}  // namespace

void serve_pipe(HANDLE stop) {
  SECURITY_ATTRIBUTES security{sizeof security, nullptr, FALSE};
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1,
                                                            &security.lpSecurityDescriptor, nullptr)) {
    return;
  }
  HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE listening = next_pipe(&security, stop);
  while (listening != INVALID_HANDLE_VALUE && WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
    const bool ready = wait_for_client(listening, done, stop);
    HANDLE next = next_pipe(&security, stop);
    if (ready) {
      serve(listening, done);
      DisconnectNamedPipe(listening);
    }
    CloseHandle(listening);
    listening = next;
  }
  if (listening != INVALID_HANDLE_VALUE) CloseHandle(listening);
  CloseHandle(done);
  LocalFree(security.lpSecurityDescriptor);
}

std::wstring ask_service(const wchar_t* request) { return ask(request, kAskMs); }

DWORD service_sender(std::wstring* name, std::wstring* path) {
  const std::vector<std::wstring> lines = split(ask(L"sender", kSenderMs), L'\n');
  if (lines.size() < 2 || lines[0].rfind(kHello, 0) != 0) return 0;
  const std::vector<std::wstring> fields = split(lines[1], L'\t');
  if (fields.size() != 3) return 0;
  *name = fields[1];
  *path = fields[2];
  return wcstoul(fields[0].c_str(), nullptr, 10);
}

bool service_running() { return g_service_seen; }

std::vector<Watcher> service_watchers() {
  std::vector<Watcher> found;
  const std::vector<std::wstring> lines = split(ask_service(L"list"), L'\n');
  g_service_seen = !lines.empty() && lines[0].rfind(kHello, 0) == 0;
  if (!g_service_seen) return found;
  static ULONGLONG last_calibration;
  if (!wcstoul(lines[0].c_str() + wcslen(kHello), nullptr, 16) && GetTickCount64() - last_calibration > 30000) {
    last_calibration = GetTickCount64();
    calibrate();
  }
  for (size_t i = 1; i < lines.size(); ++i) {
    const std::vector<std::wstring> fields = split(lines[i], L'\t');
    if (fields.size() != 6) continue;
    found.push_back({Source(_wtoi(fields[0].c_str())), DWORD(_wtoi(fields[1].c_str())), fields[2], fields[3], fields[4],
                     ULONG(wcstoul(fields[5].c_str(), nullptr, 10))});
  }
  return found;
}

}  // namespace screenlight
