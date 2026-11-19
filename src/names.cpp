#include <windows.h>

#include <cstring>
#include <cwchar>
#include <string>

#include "screenlight.h"

namespace screenlight {

std::wstring process_path(DWORD pid) {
  std::wstring path;
  if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
    wchar_t buffer[MAX_PATH * 2];
    DWORD size = ARRAYSIZE(buffer);
    if (QueryFullProcessImageNameW(process, 0, buffer, &size)) path.assign(buffer, size);
    CloseHandle(process);
  }
  return path;
}

std::wstring process_name(DWORD pid, const char* fallback) {
  std::wstring name = process_path(pid);
  const size_t slash = name.find_last_of(L'\\');
  if (slash != std::wstring::npos) name.erase(0, slash + 1);
  if (name.empty() && fallback) name.assign(fallback, fallback + strlen(fallback));
  if (name.empty()) name = L"Process " + std::to_wstring(pid);
  return name;
}

std::vector<char> file_bytes(const std::wstring& path) {
  std::vector<char> bytes;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return bytes;
  LARGE_INTEGER size{};
  if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 64 * 1024 * 1024) {
    bytes.resize(size_t(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr) || read != bytes.size()) bytes.clear();
  }
  CloseHandle(file);
  return bytes;
}

bool process_gone(DWORD pid) {
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
  const bool gone = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
  CloseHandle(process);
  return gone;
}

}  // namespace screenlight
