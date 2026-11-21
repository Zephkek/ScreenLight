#include <windows.h>
#include <exdisp.h>
#include <shlguid.h>
#include <shldisp.h>
#include <shlobj.h>

#include "screenlight.h"

namespace screenlight {
namespace {

constexpr GUID kShellWindowsClass = {0x9ba05972, 0xf6a8, 0x11cf, {0xa4, 0x42, 0x00, 0xa0, 0xc9, 0x0a, 0x8f, 0x39}};
constexpr GUID kShellWindows = {0x85cb6900, 0x4d95, 0x11cf, {0x96, 0x0c, 0x00, 0x80, 0xc7, 0xf4, 0xee, 0x85}};
constexpr GUID kServiceProvider = {0x6d5140c1, 0x7436, 0x11ce, {0x80, 0x34, 0x00, 0xaa, 0x00, 0x60, 0x09, 0xfa}};
constexpr GUID kTopLevelBrowser = {0x4c96be40, 0x915c, 0x11cf, {0x99, 0xd3, 0x00, 0xaa, 0x00, 0x4a, 0xe8, 0x37}};
constexpr GUID kShellBrowser = {0x000214e2, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kDispatch = {0x00020400, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kFolderViewDual = {0xe7a1af80, 0x4d96, 0x11cf, {0x96, 0x0c, 0x00, 0x80, 0xc7, 0xf4, 0xee, 0x85}};
constexpr GUID kShellDispatch2 = {0xa4c6892c, 0x3ba9, 0x11d2, {0x9d, 0xea, 0x00, 0xc0, 0x4f, 0xb1, 0x61, 0x62}};

template <typename T>
void release(T*& object) {
  if (object) object->Release();
  object = nullptr;
}

bool elevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  const bool yes = GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size) && elevation.TokenIsElevated;
  CloseHandle(token);
  return yes;
}

bool run_from_desktop(const wchar_t* file, const wchar_t* args) {
  IShellWindows* windows = nullptr;
  IDispatch* desktop = nullptr;
  IServiceProvider* provider = nullptr;
  IShellBrowser* browser = nullptr;
  IShellView* view = nullptr;
  IDispatch* background = nullptr;
  IShellFolderViewDual* folder = nullptr;
  IDispatch* application = nullptr;
  IShellDispatch2* shell = nullptr;
  VARIANT location{};
  location.vt = VT_I4;
  location.lVal = CSIDL_DESKTOP;
  VARIANT root{};
  LONG window = 0;
  bool ran = false;
  if (SUCCEEDED(CoCreateInstance(kShellWindowsClass, nullptr, CLSCTX_LOCAL_SERVER, kShellWindows,
                                 reinterpret_cast<void**>(&windows))) &&
      windows->FindWindowSW(&location, &root, SWC_DESKTOP, &window, SWFO_NEEDDISPATCH, &desktop) == S_OK && desktop &&
      SUCCEEDED(desktop->QueryInterface(kServiceProvider, reinterpret_cast<void**>(&provider))) &&
      SUCCEEDED(provider->QueryService(kTopLevelBrowser, kShellBrowser, reinterpret_cast<void**>(&browser))) &&
      SUCCEEDED(browser->QueryActiveShellView(&view)) &&
      SUCCEEDED(view->GetItemObject(SVGIO_BACKGROUND, kDispatch, reinterpret_cast<void**>(&background))) &&
      SUCCEEDED(background->QueryInterface(kFolderViewDual, reinterpret_cast<void**>(&folder))) &&
      SUCCEEDED(folder->get_Application(&application)) &&
      SUCCEEDED(application->QueryInterface(kShellDispatch2, reinterpret_cast<void**>(&shell)))) {
    BSTR path = SysAllocString(file);
    VARIANT arguments{};
    arguments.vt = VT_BSTR;
    arguments.bstrVal = SysAllocString(args);
    VARIANT unused{};
    ran = path && arguments.bstrVal && SUCCEEDED(shell->ShellExecute(path, arguments, unused, unused, unused));
    SysFreeString(path);
    VariantClear(&arguments);
  }
  release(shell);
  release(application);
  release(folder);
  release(background);
  release(view);
  release(browser);
  release(provider);
  release(desktop);
  release(windows);
  return ran;
}

}  // namespace

int run_unelevated(const wchar_t* args) {
  wchar_t self[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
  if (!length || length >= ARRAYSIZE(self)) return 1;
  if (!elevated()) {
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, nullptr, self, args, nullptr, SW_SHOWNORMAL)) > 32 ? 0 : 1;
  }
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  const bool ran = run_from_desktop(self, args);
  if (SUCCEEDED(com)) CoUninitialize();
  return ran ? 0 : 1;
}

}  // namespace screenlight
