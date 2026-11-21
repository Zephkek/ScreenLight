#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr ULONGLONG kCompositorEveryMs = 300000;
constexpr ULONGLONG kDriversEveryMs = 30000;
constexpr ULONG kModuleInformation = 11;
constexpr LONG kLengthMismatch = LONG(0xC0000004);

struct KernelModule {
  HANDLE section;
  void* mapped;
  void* base;
  ULONG size;
  ULONG flags;
  USHORT load_order, init_order, load_count, name_offset;
  char path[256];
};

struct KernelModules {
  ULONG count;
  KernelModule modules[1];
};
constexpr const wchar_t* kGraphicsVendors[] = {L"Microsoft", L"NVIDIA", L"Advanced Micro Devices", L"AMD", L"Intel",
                                               L"Qualcomm", L"VMware", L"Broadcom", L"Parallels", L"Oracle",
                                               L"Citrix", L"Red Hat", L"ATI Technologies"};

struct Compositor {
  ULONGLONG at = 0;
  std::vector<Watcher> found;
};

std::mutex g_lock;
std::map<std::wstring, std::wstring, NoCase> g_companies;
std::map<DWORD, Compositor> g_compositors;
std::set<std::wstring, NoCase> g_boot_drivers;
std::vector<Watcher> g_new_drivers;
ULONGLONG g_drivers_at;
bool g_drivers_known;

std::wstring version_field(const std::wstring& path, const wchar_t* field) {
  DWORD ignored = 0;
  const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
  if (!size) return std::wstring();
  std::vector<BYTE> data(size);
  if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return std::wstring();
  struct Translation {
    WORD language, codepage;
  }* translations = nullptr;
  UINT bytes = 0;
  if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &bytes) ||
      bytes < sizeof(Translation)) {
    return std::wstring();
  }
  wchar_t query[96];
  swprintf(query, ARRAYSIZE(query), L"\\StringFileInfo\\%04x%04x\\%ls", translations[0].language, translations[0].codepage, field);
  wchar_t* text = nullptr;
  UINT length = 0;
  if (!VerQueryValueW(data.data(), query, reinterpret_cast<void**>(&text), &length) || !text) return std::wstring();
  return text;
}

const std::wstring& company(const std::wstring& path) {
  auto it = g_companies.find(path);
  if (it == g_companies.end()) it = g_companies.emplace(path, version_field(path, L"CompanyName")).first;
  return it->second;
}

bool expected_in_compositor(const std::wstring& path) {
  const std::wstring& maker = company(path);
  return std::any_of(std::begin(kGraphicsVendors), std::end(kGraphicsVendors),
                     [&](const wchar_t* vendor) { return maker.find(vendor) != std::wstring::npos; });
}

const std::wstring& windows_folder() {
  static const std::wstring folder = [] {
    wchar_t windows[MAX_PATH];
    const UINT length = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
    return length && length < ARRAYSIZE(windows) ? std::wstring(windows, length) : std::wstring();
  }();
  return folder;
}

bool in_windows(const std::wstring& path) {
  const std::wstring& folder = windows_folder();
  return !folder.empty() && path.size() > folder.size() + 1 && path[folder.size()] == L'\\' &&
         _wcsnicmp(path.c_str(), folder.c_str(), folder.size()) == 0;
}

std::wstring label(const std::wstring& path) {
  std::wstring name = version_field(path, L"FileDescription");
  return name.empty() ? file_part(path) : name;
}

std::vector<Watcher> look_at_compositor(DWORD session) {
  std::vector<Watcher> found;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return found;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof entry;
  for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
    DWORD owner = 0;
    if (_wcsicmp(entry.szExeFile, L"dwm.exe") != 0 || !ProcessIdToSessionId(entry.th32ProcessID, &owner) || owner != session) {
      continue;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, entry.th32ProcessID);
    if (!process) continue;
    find_module(process, [&](const std::wstring& path) {
      if (!in_windows(path) && !expected_in_compositor(path)) {
        found.push_back({Source::kCapture, 0, label(path), L"inside Desktop Window Manager, sees the screen", path, 0, true});
      }
      return false;
    });
    CloseHandle(process);
  }
  CloseHandle(snapshot);
  return found;
}

std::wstring driver_file(const char* raw) {
  wchar_t wide[MAX_PATH];
  if (!MultiByteToWideChar(CP_ACP, 0, raw, -1, wide, ARRAYSIZE(wide))) return std::wstring();
  std::wstring path = wide;
  const std::wstring& windows = windows_folder();
  if (_wcsnicmp(path.c_str(), L"\\SystemRoot\\", 12) == 0) path = windows + path.substr(11);
  if (_wcsnicmp(path.c_str(), L"\\??\\", 4) == 0) path = path.substr(4);
  if (_wcsnicmp(path.c_str(), L"\\Windows\\", 9) == 0) path = windows + path.substr(8);
  return path;
}

std::vector<std::wstring> loaded_drivers() {
  using QueryFn = LONG(WINAPI*)(ULONG, void*, ULONG, ULONG*);
  static const auto query =
      reinterpret_cast<QueryFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
  if (!query) return {};
  std::vector<BYTE> buffer(1 << 17);
  LONG status = kLengthMismatch;
  for (int tries = 0; tries < 4 && status == kLengthMismatch; ++tries) {
    ULONG needed = 0;
    status = query(kModuleInformation, buffer.data(), ULONG(buffer.size()), &needed);
    if (status == kLengthMismatch) buffer.resize(std::max<size_t>(needed, buffer.size() * 2) + 4096);
  }
  if (status < 0) return {};
  const auto* list = reinterpret_cast<const KernelModules*>(buffer.data());
  const size_t room = (buffer.size() - offsetof(KernelModules, modules)) / sizeof(KernelModule);
  std::vector<std::wstring> paths;
  for (ULONG i = 0; i < list->count && i < room; ++i) {
    char raw[sizeof list->modules[i].path + 1] = {};
    memcpy(raw, list->modules[i].path, sizeof list->modules[i].path);
    std::wstring path = driver_file(raw);
    if (!path.empty()) paths.push_back(std::move(path));
  }
  return paths;
}

void look_at_drivers() {
  const std::vector<std::wstring> now = loaded_drivers();
  if (now.empty()) return;
  if (!g_drivers_known) {
    g_boot_drivers.insert(now.begin(), now.end());
    g_drivers_known = true;
    return;
  }
  std::vector<Watcher> found;
  SYSTEMTIME clock;
  GetLocalTime(&clock);
  for (const std::wstring& path : now) {
    if (g_boot_drivers.count(path) || company(path).find(L"Microsoft") != std::wstring::npos) continue;
    const auto earlier = std::find_if(g_new_drivers.begin(), g_new_drivers.end(), [&](const Watcher& w) { return w.path == path; });
    if (earlier != g_new_drivers.end()) {
      found.push_back(*earlier);
      continue;
    }
    found.push_back({Source::kRemote, 0, label(path), L"kernel driver, can see everything", path, 0, true});
  }
  g_new_drivers = std::move(found);
}

}  // namespace

void remember_drivers() {
  std::lock_guard<std::mutex> hold(g_lock);
  look_at_drivers();
  g_drivers_at = GetTickCount64();
}

std::vector<Watcher> system_watchers(DWORD session) {
  std::lock_guard<std::mutex> hold(g_lock);
  const ULONGLONG now = GetTickCount64();
  Compositor& compositor = g_compositors[session];
  if (!compositor.at || now - compositor.at > kCompositorEveryMs) {
    compositor.found = look_at_compositor(session);
    compositor.at = now;
  }
  if (now - g_drivers_at > kDriversEveryMs) {
    look_at_drivers();
    g_drivers_at = now;
  }
  std::vector<Watcher> found = compositor.found;
  found.insert(found.end(), g_new_drivers.begin(), g_new_drivers.end());
  return found;
}

}  // namespace screenlight
