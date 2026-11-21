#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr const wchar_t* kPublishers[] = {
    L"Broadcom Inc",          L"VMware, Inc.",                 L"Oracle America, Inc.",       L"Discord Inc.",
    L"Valve Corp.",           L"Valve Corporation",            L"OBS Project, LLC",           L"NVIDIA Corporation",
    L"Advanced Micro Devices, Inc.", L"Advanced Micro Devices INC.", L"Intel Corporation",   L"Logitech Inc",
    L"Logitech Europe S.A.",  L"Razer USA Ltd.",               L"Corsair Memory, Inc.",       L"SteelSeries ApS",
    L"ASUSTeK COMPUTER INC.", L"Micro-Star International CO., LTD.", L"Elgato Systems GmbH", L"Microsoft Corporation",
    L"Zoom Video Communications, Inc.", L"Spotify AB",         L"NV Access Limited",          L"Freedom Scientific Inc",
    L"Epic Games Inc.",       L"Riot Games, Inc.",             L"Electronic Arts, Inc.",      L"Ubisoft Entertainment Sweden AB",
    L"Blizzard Entertainment, Inc.", L"Overwolf Ltd",          L"Medal B.V.",                 L"Mozilla Corporation",
    L"Google LLC",            L"Opera Norway AS"};

enum class State { kChecking, kKnown, kUnknown };

std::mutex g_lock;
std::map<std::wstring, State, NoCase> g_states;
HWND g_notify;

std::wstring signer(const std::wstring& path) {
  WINTRUST_FILE_INFO file{};
  file.cbStruct = sizeof file;
  file.pcwszFilePath = path.c_str();
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  WINTRUST_DATA data{};
  data.cbStruct = sizeof data;
  data.dwUIChoice = WTD_UI_NONE;
  data.fdwRevocationChecks = WTD_REVOKE_NONE;
  data.dwUnionChoice = WTD_CHOICE_FILE;
  data.pFile = &file;
  data.dwStateAction = WTD_STATEACTION_VERIFY;
  data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
  HWND none = static_cast<HWND>(INVALID_HANDLE_VALUE);
  std::wstring name;
  if (WinVerifyTrust(none, &action, &data) == ERROR_SUCCESS) {
    CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
    CRYPT_PROVIDER_SGNR* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
    if (signer && signer->csCertChain && signer->pasCertChain && signer->pasCertChain[0].pCert) {
      wchar_t text[256];
      if (CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, text,
                             ARRAYSIZE(text)) > 1) {
        name = text;
      }
    }
  }
  data.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(none, &action, &data);
  return name;
}

DWORD WINAPI verify(void* raw) {
  const std::unique_ptr<std::wstring> path(static_cast<std::wstring*>(raw));
  const std::wstring name = signer(*path);
  bool known = false;
  for (const wchar_t* publisher : kPublishers) known |= _wcsicmp(name.c_str(), publisher) == 0;
  {
    std::lock_guard<std::mutex> hold(g_lock);
    g_states[*path] = known ? State::kKnown : State::kUnknown;
  }
  if (g_notify) PostMessageW(g_notify, kRefreshMessage, 0, 0);
  return 0;
}

}  // namespace

void watch_publishers(HWND notify) { g_notify = notify; }

bool known_publisher(const std::wstring& path) {
  if (path.empty()) return false;
  std::lock_guard<std::mutex> hold(g_lock);
  if (const auto it = g_states.find(path); it != g_states.end()) return it->second != State::kUnknown;
  g_states[path] = State::kChecking;
  if (!QueueUserWorkItem(verify, new std::wstring(path), WT_EXECUTELONGFUNCTION)) g_states[path] = State::kUnknown;
  return true;
}

}  // namespace screenlight
