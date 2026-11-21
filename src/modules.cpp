#include <windows.h>
#include <winternl.h>

#include <cstdint>
#include <cwchar>
#include <functional>
#include <string>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr ULONG kProcessBasicInformation = 0;
constexpr ULONG kProcessWow64Information = 26;
constexpr int kMostModules = 4096;

using QueryProcessFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

QueryProcessFn query_process() {
  static const auto query =
      reinterpret_cast<QueryProcessFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
  return query;
}

template <typename Value>
bool read(HANDLE process, ULONGLONG at, Value* value) {
  SIZE_T done = 0;
  return ReadProcessMemory(process, reinterpret_cast<const void*>(ULONG_PTR(at)), value, sizeof *value, &done) &&
         done == sizeof *value;
}

template <typename Pointer>
bool walk(HANDLE process, ULONGLONG head, ULONGLONG path_offset, const std::function<bool(const std::wstring&)>& visit) {
  Pointer link = 0;
  if (!read(process, head, &link)) return false;
  std::wstring path;
  for (int i = 0; i < kMostModules && link && link != head; ++i) {
    USHORT length = 0;
    Pointer buffer = 0;
    if (!read(process, link + path_offset, &length) || !read(process, link + path_offset + sizeof(Pointer), &buffer)) {
      return false;
    }
    if (length && buffer && length < 0x8000) {
      path.assign(length / sizeof(wchar_t), L'\0');
      SIZE_T done = 0;
      if (ReadProcessMemory(process, reinterpret_cast<const void*>(ULONG_PTR(buffer)), path.data(), length, &done) &&
          done == length && visit(path)) {
        return true;
      }
    }
    if (!read(process, link, &link)) return false;
  }
  return false;
}

}  // namespace

bool find_module(HANDLE process, const std::function<bool(const std::wstring&)>& visit) {
  const QueryProcessFn query = query_process();
  if (!query) return false;
  ULONG_PTR peb32 = 0;
  if (query(process, kProcessWow64Information, &peb32, sizeof peb32, nullptr) >= 0 && peb32) {
    uint32_t ldr = 0;
    if (read(process, peb32 + 0x0c, &ldr) && ldr && walk<uint32_t>(process, ldr + 0x14, 0x1c, visit)) return true;
  }
  PROCESS_BASIC_INFORMATION basic{};
  if (query(process, kProcessBasicInformation, &basic, sizeof basic, nullptr) < 0 || !basic.PebBaseAddress) return false;
  uint64_t ldr = 0;
  return read(process, ULONGLONG(ULONG_PTR(basic.PebBaseAddress)) + 0x18, &ldr) && ldr &&
         walk<uint64_t>(process, ldr + 0x20, 0x38, visit);
}

std::wstring file_part(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

}  // namespace screenlight
