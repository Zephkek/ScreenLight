#include <windows.h>
#include <cfgmgr32.h>
#include <devpropdef.h>
#include <intrin.h>
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
constexpr ULONG kCodeIntegrityInformation = 103;
constexpr ULONG kKernelDebuggerInformation = 35;
constexpr ULONG kTestSigning = 0x2;
constexpr ULONG kDebugMode = 0x80;
constexpr DEVPROPKEY kDeviceStack = {{0x540b947e, 0x8b40, 0x45bc, {0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2}}, 14};
constexpr ULONGLONG kPruneEveryMs = 5000;

std::map<DWORD, Hooked> g_hooked;
ULONGLONG g_pruned_at;
std::vector<Watcher> g_drivers;
std::vector<std::wstring> g_host_makers;
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

std::wstring version_string(const std::wstring& path, const wchar_t* field) {
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
  const std::wstring path = driver_path(filter);
  const std::wstring maker = version_string(path, L"CompanyName");
  if (maker.find(L"Microsoft") != std::wstring::npos) return;
  for (const std::wstring& host : g_host_makers) {
    if (maker.find(host) != std::wstring::npos) return;
  }
  const bool listed = std::any_of(found->begin(), found->end(), [&](const Watcher& w) {
    return w.name == filter || (!path.empty() && _wcsicmp(w.path.c_str(), path.c_str()) == 0);
  });
  if (listed) return;
  const std::wstring label = file_description(path, filter);
  found->push_back({Source::kInput, 0, label,
                    keyboard ? L"keyboard driver, can read and fake keys" : L"mouse driver, can fake clicks",
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
    DEVPROPTYPE type = 0;
    DWORD needed = 0;
    SetupDiGetDevicePropertyW(devices, &device, &kDeviceStack, &type, nullptr, 0, &needed, 0);
    if (needed) {
      std::vector<wchar_t> stack(needed / sizeof(wchar_t) + 2);
      if (SetupDiGetDevicePropertyW(devices, &device, &kDeviceStack, &type, reinterpret_cast<BYTE*>(stack.data()),
                                    needed, nullptr, 0)) {
        for (std::wstring driver : split_multi(stack)) {
          const size_t slash = driver.find_last_of(L'\\');
          if (slash != std::wstring::npos) driver.erase(0, slash + 1);
          add_filter(driver, keyboard, found);
        }
      }
    }
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
                      keyboard ? L"virtual keyboard" : L"virtual mouse",
                      {}, 0, true});
  }
  SetupDiDestroyDeviceInfoList(devices);
}

void look_at_boot() {
  using QueryFn = LONG(WINAPI*)(ULONG, void*, ULONG, ULONG*);
  static const auto query =
      reinterpret_cast<QueryFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
  if (!query) return;
  struct {
    ULONG length;
    ULONG options;
  } integrity = {sizeof integrity, 0};
  struct {
    BOOLEAN enabled;
    BOOLEAN absent;
  } debugger = {};
  if (query(kCodeIntegrityInformation, &integrity, sizeof integrity, nullptr) < 0) integrity.options = 0;
  if (query(kKernelDebuggerInformation, &debugger, sizeof debugger, nullptr) < 0) debugger.enabled = FALSE;
  if (integrity.options & kTestSigning) {
    g_drivers.push_back({Source::kRemote, 0, L"Test mode", L"unsigned drivers can load", {}, 0, true});
  }
  if (debugger.enabled || (integrity.options & kDebugMode)) {
    g_drivers.push_back({Source::kRemote, 0, L"Kernel debugging", L"a debugger can take over this PC", {}, 0, true});
  }
}

void look_at_machine() {
  int info[4] = {};
  __cpuid(info, 1);
  if (!(info[2] & (1 << 31))) return;
  __cpuid(info, 0x40000000);
  char vendor[13] = {};
  memcpy(vendor, &info[1], 4);
  memcpy(vendor + 4, &info[2], 4);
  memcpy(vendor + 8, &info[3], 4);
  if (strcmp(vendor, "Microsoft Hv") == 0) {
    __cpuid(info, 0x40000003);
    if (info[1] & 1) return;
  }
  struct Known {
    const char* vendor;
    const wchar_t* name;
    const wchar_t* makers[2];
  };
  constexpr Known kVendors[] = {{"VMwareVMware", L"VMware", {L"VMware", L"Broadcom"}},
                                {"VBoxVBoxVBox", L"VirtualBox", {L"Oracle", L"innotek"}},
                                {"Microsoft Hv", L"Hyper-V", {}},
                                {"KVMKVMKVM", L"KVM", {L"Red Hat", L"QEMU"}},
                                {"XenVMMXenVMM", L"Xen", {L"Citrix", L"XenSource"}},
                                {"prl hyperv  ", L"Parallels", {L"Parallels"}},
                                {" lrpepyh  vr", L"Parallels", {L"Parallels"}},
                                {"TCGTCGTCGTCG", L"QEMU", {L"Red Hat", L"QEMU"}},
                                {"ACRNACRNACRN", L"ACRN", {}},
                                {"bhyve bhyve ", L"bhyve", {}}};
  std::wstring name = L"Virtual machine";
  for (const Known& known : kVendors) {
    if (strcmp(vendor, known.vendor) != 0) continue;
    name = known.name;
    for (const wchar_t* maker : known.makers) {
      if (maker) g_host_makers.push_back(maker);
    }
  }
  g_drivers.push_back({Source::kRemote, 0, name, L"hosts this PC, sees everything", {}, 0, true});
}

void look_at_drivers() {
  g_drivers.clear();
  g_host_makers.clear();
  look_at_boot();
  look_at_machine();
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
      if (!listed) entry.overlays.push_back({Source::kCapture, 0, overlay.label, L"can record " + game, path, 0, true});
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
