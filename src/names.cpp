#include <windows.h>

#include <cstring>
#include <cwchar>
#include <string>

#include "screenlight.h"

namespace screenlight {

std::wstring process_name(DWORD pid, const char* fallback) {
  std::wstring name;
  if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
    wchar_t path[MAX_PATH * 2];
    DWORD size = ARRAYSIZE(path);
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
      const wchar_t* slash = wcsrchr(path, L'\\');
      name = slash ? slash + 1 : path;
    }
    CloseHandle(process);
  }
  if (name.empty() && fallback) name.assign(fallback, fallback + strlen(fallback));
  if (name.empty()) name = L"pid " + std::to_wstring(pid);
  return name;
}

}  // namespace screenlight
