#include <windows.h>
#include <exdisp.h>
#include <shlguid.h>
#include <shldisp.h>
#include <shlobj.h>

#include "screenlight.h"

namespace screenlight {
namespace {

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
  if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_IShellWindows,
                                 reinterpret_cast<void**>(&windows))) &&
      windows->FindWindowSW(&location, &root, SWC_DESKTOP, &window, SWFO_NEEDDISPATCH, &desktop) == S_OK && desktop &&
      SUCCEEDED(desktop->QueryInterface(IID_IServiceProvider, reinterpret_cast<void**>(&provider))) &&
      SUCCEEDED(provider->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, reinterpret_cast<void**>(&browser))) &&
      SUCCEEDED(browser->QueryActiveShellView(&view)) &&
      SUCCEEDED(view->GetItemObject(SVGIO_BACKGROUND, IID_IDispatch, reinterpret_cast<void**>(&background))) &&
      SUCCEEDED(background->QueryInterface(IID_IShellFolderViewDual, reinterpret_cast<void**>(&folder))) &&
      SUCCEEDED(folder->get_Application(&application)) &&
      SUCCEEDED(application->QueryInterface(IID_IShellDispatch2, reinterpret_cast<void**>(&shell)))) {
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
