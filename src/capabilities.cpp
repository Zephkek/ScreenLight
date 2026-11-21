#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>

#include <algorithm>
#include <cwchar>
#include <map>
#include <string>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

struct Overlay {
  const wchar_t* dll;
  const wchar_t* label;
};

struct Hooked {
  ULONGLONG checked = 0;
  std::vector<Watcher> overlays;
};

constexpr Overlay kOverlays[] = {
    {L"GameOverlayRenderer64.dll", L"Steam overlay"},  {L"GameOverlayRenderer.dll", L"Steam overlay"},
    {L"DiscordHook64.dll", L"Discord overlay"},         {L"DiscordHook.dll", L"Discord overlay"},
    {L"nvspcap64.dll", L"NVIDIA overlay"},              {L"nvspcap.dll", L"NVIDIA overlay"},
    {L"RTSSHooks64.dll", L"RivaTuner overlay"},         {L"RTSSHooks.dll", L"RivaTuner overlay"},
    {L"bdcam.dll", L"Bandicam"},                        {L"bdcam64.dll", L"Bandicam"},
    {L"IGO64.dll", L"EA app overlay"},                  {L"IGO32.dll", L"EA app overlay"},
    {L"graphics-hook64.dll", L"OBS game capture"},      {L"graphics-hook32.dll", L"OBS game capture"},
    {L"NvFBC64.dll", L"NVIDIA frame capture"},          {L"NvFBC.dll", L"NVIDIA frame capture"},
    {L"NvIFR64.dll", L"NVIDIA frame capture"},          {L"NvIFR.dll", L"NVIDIA frame capture"}};
constexpr const wchar_t* kInboxFilters[] = {L"kbdclass", L"mouclass"};
constexpr wchar_t kClassKey[] = L"SYSTEM\\CurrentControlSet\\Control\\Class\\";
constexpr wchar_t kKeyboardClass[] = L"{4D36E96B-E325-11CE-BFC1-08002BE10318}";
constexpr wchar_t kMouseClass[] = L"{4D36E96F-E325-11CE-BFC1-08002BE10318}";
constexpr GUID kKeyboardGuid = {0x4d36e96b, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
constexpr GUID kMouseGuid = {0x4d36e96f, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
constexpr ULONGLONG kLookAgainMs = 60000;
constexpr ULONGLONG kPruneEveryMs = 5000;

std::map<DWORD, Hooked> g_hooked;
ULONGLONG g_pruned_at;
std::vector<Watcher> g_drivers;
bool g_drivers_stale = true;

std::vector<std::wstring> split_multi(const std::vector<wchar_t>& text) {
  std::vector<std::wstring> parts;
  for (const wchar_t* at = text.data(); at < text.data() + text.size() && *at; at += wcslen(at) + 1) parts.emplace_back(at);
  return parts;
}

std::vector<std::wstring> class_filters(const wchar_t* klass, const wchar_t* value) {
  const std::wstring key = std::wstring(kClassKey) + klass;
  DWORD size = 0;
  if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), value, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS) {
    return {};
  }
  std::vector<wchar_t> text(size / sizeof(wchar_t) + 2);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), value, RRF_RT_REG_MULTI_SZ, nullptr, text.data(), &size) != ERROR_SUCCESS) {
    return {};
  }
  return split_multi(text);
}

bool inbox(const std::wstring& filter) {
  return std::any_of(std::begin(kInboxFilters), std::end(kInboxFilters),
                     [&](const wchar_t* name) { return _wcsicmp(name, filter.c_str()) == 0; });
}

std::wstring driver_path(const std::wstring& service) {
  const std::wstring key = L"SYSTEM\\CurrentControlSet\\Services\\" + service;
  wchar_t raw[MAX_PATH * 2];
  DWORD size = sizeof raw;
  if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"ImagePath", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                   nullptr, raw, &size) != ERROR_SUCCESS) {
    return std::wstring();
  }
  std::wstring path = raw;
  wchar_t windows[MAX_PATH];
  const UINT length = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
  if (_wcsnicmp(path.c_str(), L"\\SystemRoot\\", 12) == 0) path = std::wstring(windows, length) + path.substr(11);
  if (_wcsnicmp(path.c_str(), L"System32\\", 9) == 0) path = std::wstring(windows, length) + L"\\" + path;
  if (_wcsnicmp(path.c_str(), L"\\??\\", 4) == 0) path = path.substr(4);
  return path;
}

std::wstring file_description(const std::wstring& path, const std::wstring& fallback) {
  DWORD ignored = 0;
  const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
  if (!size) return fallback;
  std::vector<BYTE> data(size);
  if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return fallback;
  struct Translation {
    WORD language, codepage;
  }* translations = nullptr;
  UINT bytes = 0;
  if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &bytes) ||
      bytes < sizeof(Translation)) {
    return fallback;
  }
  wchar_t query[64];
  swprintf(query, ARRAYSIZE(query), L"\\StringFileInfo\\%04x%04x\\FileDescription", translations[0].language,
           translations[0].codepage);
  wchar_t* text = nullptr;
  UINT length = 0;
  if (!VerQueryValueW(data.data(), query, reinterpret_cast<void**>(&text), &length) || !text || !*text) return fallback;
  return text;
}

void add_filter(const std::wstring& filter, bool keyboard, std::vector<Watcher>* found) {
  if (inbox(filter)) return;
  const bool listed = std::any_of(found->begin(), found->end(), [&](const Watcher& w) { return w.name == filter || w.path == driver_path(filter); });
  if (listed) return;
  const std::wstring path = driver_path(filter);
  const std::wstring label = file_description(path, filter);
  found->push_back({Source::kInput, 0, label,
                    keyboard ? L"is a driver between your keyboard and Windows, it can read and fake keys"
                             : L"is a driver between your mouse and Windows, it can fake clicks",
                    path, 0, true});
}

std::wstring device_property(HDEVINFO devices, SP_DEVINFO_DATA* device, DWORD property, std::vector<wchar_t>* multi = nullptr) {
  DWORD type = 0, size = 0;
  SetupDiGetDeviceRegistryPropertyW(devices, device, property, &type, nullptr, 0, &size);
  if (!size) return std::wstring();
  std::vector<wchar_t> text(size / sizeof(wchar_t) + 2);
  if (!SetupDiGetDeviceRegistryPropertyW(devices, device, property, &type, reinterpret_cast<BYTE*>(text.data()), size, nullptr)) {
    return std::wstring();
  }
  if (multi) *multi = text;
  return text.data();
}

bool starts(const wchar_t* id, const wchar_t* prefix) { return _wcsnicmp(id, prefix, wcslen(prefix)) == 0; }

bool virtual_device(DEVINST node) {
  for (int depth = 0; depth < 32; ++depth) {
    DEVINST parent = 0;
    wchar_t id[MAX_DEVICE_ID_LEN];
    if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS || CM_Get_Device_IDW(parent, id, ARRAYSIZE(id), 0) != CR_SUCCESS) {
      return false;
    }
    if (starts(id, L"PCI\\") || starts(id, L"ACPI\\") || starts(id, L"HTREE\\")) return false;
    if (starts(id, L"ROOT\\") || starts(id, L"SWD\\")) return true;
    node = parent;
  }
  return false;
}

void scan_devices(const GUID& klass, bool keyboard, std::vector<Watcher>* found) {
  HDEVINFO devices = SetupDiGetClassDevsW(&klass, nullptr, nullptr, DIGCF_PRESENT);
  if (devices == INVALID_HANDLE_VALUE) return;
  SP_DEVINFO_DATA device{};
  device.cbSize = sizeof device;
  for (DWORD i = 0; SetupDiEnumDeviceInfo(devices, i, &device); ++i) {
    for (DWORD property : {DWORD(SPDRP_UPPERFILTERS), DWORD(SPDRP_LOWERFILTERS)}) {
      std::vector<wchar_t> multi;
      if (!device_property(devices, &device, property, &multi).empty()) {
        for (const std::wstring& filter : split_multi(multi)) add_filter(filter, keyboard, found);
      }
    }
    wchar_t id[MAX_DEVICE_ID_LEN];
    const bool rooted = CM_Get_Device_IDW(device.DevInst, id, ARRAYSIZE(id), 0) == CR_SUCCESS &&
                        (_wcsnicmp(id, L"ROOT\\", 5) == 0 || _wcsnicmp(id, L"SWD\\", 4) == 0);
    if (!rooted && !virtual_device(device.DevInst)) continue;
    std::wstring name = device_property(devices, &device, SPDRP_FRIENDLYNAME);
    if (name.empty()) name = device_property(devices, &device, SPDRP_DEVICEDESC);
    if (name.empty()) name = keyboard ? L"Virtual keyboard" : L"Virtual mouse";
    found->push_back({Source::kInput, 0, name,
                      keyboard ? L"is a virtual keyboard, a program can type with it as if it were real"
                               : L"is a virtual mouse, a program can click with it as if it were real",
                      {}, 0, true});
  }
  SetupDiDestroyDeviceInfoList(devices);
}

void look_at_drivers() {
  g_drivers.clear();
  for (const wchar_t* value : {L"UpperFilters", L"LowerFilters"}) {
    for (const std::wstring& filter : class_filters(kKeyboardClass, value)) add_filter(filter, true, &g_drivers);
    for (const std::wstring& filter : class_filters(kMouseClass, value)) add_filter(filter, false, &g_drivers);
  }
  scan_devices(kKeyboardGuid, true, &g_drivers);
  scan_devices(kMouseGuid, false, &g_drivers);
  g_drivers_stale = false;
}

void look_at_foreground() {
  HWND window = GetForegroundWindow();
  DWORD pid = 0;
  if (window) GetWindowThreadProcessId(window, &pid);
  if (!pid || pid == GetCurrentProcessId()) return;
  const ULONGLONG now = GetTickCount64();
  Hooked& entry = g_hooked[pid];
  if (entry.checked && now - entry.checked < kLookAgainMs) return;
  entry.checked = now;
  entry.overlays.clear();
  HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!process) return;
  const std::wstring game = process_name(pid);
  find_module(process, [&](const std::wstring& path) {
    const std::wstring name = file_part(path);
    for (const Overlay& overlay : kOverlays) {
      if (_wcsicmp(name.c_str(), overlay.dll) != 0) continue;
      const bool listed = std::any_of(entry.overlays.begin(), entry.overlays.end(),
                                      [&](const Watcher& w) { return w.name == overlay.label; });
      if (!listed) entry.overlays.push_back({Source::kCapture, 0, overlay.label, L"is inside " + game + L" and can record it", path, 0, true});
    }
    return false;
  });
  CloseHandle(process);
}

void prune() {
  const ULONGLONG now = GetTickCount64();
  if (now - g_pruned_at < kPruneEveryMs) return;
  g_pruned_at = now;
  for (auto it = g_hooked.begin(); it != g_hooked.end();) {
    it = process_gone(it->first) ? g_hooked.erase(it) : std::next(it);
  }
}

}  // namespace

void devices_changed() { g_drivers_stale = true; }

std::vector<Watcher> possible_watchers() {
  if (g_drivers_stale) look_at_drivers();
  look_at_foreground();
  prune();
  std::vector<Watcher> found = g_drivers;
  for (const auto& [pid, entry] : g_hooked) found.insert(found.end(), entry.overlays.begin(), entry.overlays.end());
  return found;
}

}  // namespace screenlight
