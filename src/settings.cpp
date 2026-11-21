#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr wchar_t kKey[] = L"Software\\Screen Light";
constexpr wchar_t kIniName[] = L"ScreenLight.ini";
constexpr wchar_t kSection[] = L"Settings";
constexpr wchar_t kIgnoredName[] = L"Ignored";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Screen Light";

struct Number {
  const wchar_t* name;
  int Settings::*value;
};

struct Flag {
  const wchar_t* name;
  bool Settings::*value;
};

constexpr Flag kFlags[] = {{L"Notifications", &Settings::notifications}, {L"WatchScreen", &Settings::watch_screen},
                           {L"WatchInput", &Settings::watch_input},       {L"WatchKeyboard", &Settings::watch_keyboard},
                           {L"WatchRemote", &Settings::watch_remote},     {L"AskedForService", &Settings::asked_service}};
constexpr Number kNumbers[] = {{L"Overlay", &Settings::overlay}, {L"OverlayCorner", &Settings::corner}};

Settings g_settings;
bool g_loaded;

std::wstring ini_path() {
  wchar_t path[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
  if (!length || length >= ARRAYSIZE(path)) return std::wstring();
  std::wstring dir(path, length);
  return dir.substr(0, dir.find_last_of(L'\\') + 1) + kIniName;
}

int read_number(const wchar_t* name, int fallback) {
  if (portable()) return int(GetPrivateProfileIntW(kSection, name, fallback, ini_path().c_str()));
  DWORD value = DWORD(fallback), size = sizeof value;
  RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size);
  return int(value);
}

std::vector<std::wstring> read_ignored() {
  std::vector<std::wstring> paths;
  if (portable()) {
    std::vector<wchar_t> text(32768);
    const DWORD length = GetPrivateProfileSectionW(kIgnoredName, text.data(), DWORD(text.size()), ini_path().c_str());
    for (const wchar_t* line = text.data(); line < text.data() + length && *line; line += wcslen(line) + 1) {
      const wchar_t* value = wcschr(line, L'=');
      if (value && value[1]) paths.emplace_back(value + 1);
    }
    return paths;
  }
  DWORD size = 0;
  if (RegGetValueW(HKEY_CURRENT_USER, kKey, kIgnoredName, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS) {
    return paths;
  }
  std::vector<wchar_t> text(size / sizeof(wchar_t) + 2);
  if (RegGetValueW(HKEY_CURRENT_USER, kKey, kIgnoredName, RRF_RT_REG_MULTI_SZ, nullptr, text.data(), &size) != ERROR_SUCCESS) {
    return paths;
  }
  for (const wchar_t* line = text.data(); *line; line += wcslen(line) + 1) paths.emplace_back(line);
  return paths;
}

void write_ini(const Settings& s) {
  std::wstring text = L"\xFEFF[Settings]\r\n";
  for (const Flag& f : kFlags) text += std::wstring(f.name) + L"=" + (s.*f.value ? L"1" : L"0") + L"\r\n";
  for (const Number& n : kNumbers) text += std::wstring(n.name) + L"=" + std::to_wstring(s.*n.value) + L"\r\n";
  text += L"\r\n[Ignored]\r\n";
  for (size_t i = 0; i < s.ignored.size(); ++i) text += std::to_wstring(i + 1) + L"=" + s.ignored[i] + L"\r\n";
  const std::wstring path = ini_path(), temp = path + L".new";
  HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  const bool ok = WriteFile(file, text.data(), DWORD(text.size() * sizeof(wchar_t)), &written, nullptr) &&
                  written == text.size() * sizeof(wchar_t);
  CloseHandle(file);
  if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temp.c_str());
  }
}

void write_registry(const Settings& s) {
  HKEY key;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
  for (const Flag& f : kFlags) {
    const DWORD value = s.*f.value;
    RegSetValueExW(key, f.name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof value);
  }
  for (const Number& n : kNumbers) {
    const DWORD value = DWORD(s.*n.value);
    RegSetValueExW(key, n.name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof value);
  }
  std::wstring list;
  for (const std::wstring& path : s.ignored) list += path + L'\0';
  list += L'\0';
  RegSetValueExW(key, kIgnoredName, 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(list.data()),
                 DWORD(list.size() * sizeof(wchar_t)));
  RegCloseKey(key);
}

std::wstring startup_command() {
  wchar_t self[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
  if (!length || length >= ARRAYSIZE(self)) return std::wstring();
  return L"\"" + std::wstring(self, length) + L"\" --tray";
}

}  // namespace

bool portable() {
  static const bool found = GetFileAttributesW(ini_path().c_str()) != INVALID_FILE_ATTRIBUTES;
  return found;
}

const Settings& settings() {
  if (!g_loaded) {
    for (const Flag& f : kFlags) g_settings.*f.value = read_number(f.name, g_settings.*f.value) != 0;
    for (const Number& n : kNumbers) g_settings.*n.value = read_number(n.name, g_settings.*n.value);
    g_settings.overlay = std::clamp(g_settings.overlay, 0, 2);
    g_settings.corner = std::clamp(g_settings.corner, 0, 3);
    g_settings.ignored = read_ignored();
    g_loaded = true;
  }
  return g_settings;
}

void save_settings(const Settings& s) {
  g_settings = s;
  g_loaded = true;
  if (portable()) {
    write_ini(s);
  } else {
    write_registry(s);
  }
}

bool watched(const Watcher& w) {
  const Settings& s = settings();
  switch (w.source) {
    case Source::kInput:
      return s.watch_input;
    case Source::kKeyboard:
      return s.watch_keyboard;
    case Source::kRemote:
    case Source::kHiddenDesktop:
      return s.watch_remote;
    default:
      return s.watch_screen;
  }
}

bool ignored_path(const std::wstring& path) {
  if (path.empty()) return false;
  const std::vector<std::wstring>& list = settings().ignored;
  return std::any_of(list.begin(), list.end(), [&](const std::wstring& p) { return _wcsicmp(p.c_str(), path.c_str()) == 0; });
}

bool ignored(const Watcher& w) { return ignored_path(w.path); }

bool starts_with_windows() {
  wchar_t value[MAX_PATH * 2];
  DWORD size = sizeof value;
  if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return false;
  return _wcsicmp(value, startup_command().c_str()) == 0;
}

void start_with_windows(bool on) {
  HKEY key;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
  const std::wstring command = startup_command();
  if (on && !command.empty()) {
    RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                   DWORD((command.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(key, kRunValue);
  }
  RegCloseKey(key);
}

void ignore_program(const std::wstring& path, bool on) {
  Settings s = settings();
  s.ignored.erase(std::remove_if(s.ignored.begin(), s.ignored.end(),
                                 [&](const std::wstring& p) { return _wcsicmp(p.c_str(), path.c_str()) == 0; }),
                  s.ignored.end());
  if (on) s.ignored.push_back(path);
  save_settings(s);
}

}  // namespace screenlight
