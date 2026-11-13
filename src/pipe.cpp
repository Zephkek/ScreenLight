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
  if (request != L"list") return reply;
  for (const Watcher& w : tracked(session)) {
    reply += L"\n" + std::to_wstring(int(w.source)) + L"\t" + std::to_wstring(w.pid) + L"\t" + w.name + L"\t" + w.what;
  }
  return reply;
}

void serve(HANDLE pipe) {
  wchar_t request[64];
  DWORD read = 0;
  if (!ReadFile(pipe, request, sizeof request - sizeof(wchar_t), &read, nullptr)) return;
  request[read / sizeof(wchar_t)] = 0;
  const std::wstring reply = answer(pipe, request);
  DWORD written = 0;
  WriteFile(pipe, reply.data(), DWORD(reply.size() * sizeof(wchar_t)), &written, nullptr);
  FlushFileBuffers(pipe);
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
  HANDLE connected = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
    HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                   kBufferBytes, kBufferBytes, 0, &security);
    if (pipe == INVALID_HANDLE_VALUE) {
      WaitForSingleObject(stop, 1000);
      continue;
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = connected;
    ResetEvent(connected);
    bool ready = ConnectNamedPipe(pipe, &overlapped) || GetLastError() == ERROR_PIPE_CONNECTED;
    if (!ready && GetLastError() == ERROR_IO_PENDING) {
      HANDLE waits[] = {connected, stop};
      if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
        DWORD ignored = 0;
        ready = GetOverlappedResult(pipe, &overlapped, &ignored, FALSE);
      } else {
        CancelIo(pipe);
      }
    }
    if (ready) {
      serve(pipe);
      DisconnectNamedPipe(pipe);
    }
    CloseHandle(pipe);
  }
  CloseHandle(connected);
  LocalFree(security.lpSecurityDescriptor);
}

std::wstring ask_service(const wchar_t* request) {
  std::vector<wchar_t> reply(kBufferBytes / sizeof(wchar_t) + 1);
  DWORD read = 0;
  if (!CallNamedPipeW(kPipeName, const_cast<wchar_t*>(request), DWORD(wcslen(request) * sizeof(wchar_t)), reply.data(),
                      kBufferBytes, &read, 200)) {
    return std::wstring();
  }
  return std::wstring(reply.data(), read / sizeof(wchar_t));
}

std::vector<Watcher> service_watchers() {
  std::vector<Watcher> found;
  const std::vector<std::wstring> lines = split(ask_service(L"list"), L'\n');
  if (lines.empty() || lines[0].rfind(kHello, 0) != 0) return found;
  static ULONGLONG last_calibration;
  if (!wcstoul(lines[0].c_str() + wcslen(kHello), nullptr, 16) && GetTickCount64() - last_calibration > 30000) {
    last_calibration = GetTickCount64();
    calibrate();
  }
  for (size_t i = 1; i < lines.size(); ++i) {
    const std::vector<std::wstring> fields = split(lines[i], L'\t');
    if (fields.size() != 4) continue;
    found.push_back({Source(_wtoi(fields[0].c_str())), DWORD(_wtoi(fields[1].c_str())), fields[2], fields[3]});
  }
  return found;
}

}  // namespace screenlight
