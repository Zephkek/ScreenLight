#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

struct OpenAdapter {
  WCHAR device[32];
  UINT adapter;
  LUID luid;
  UINT source;
};

struct CloseAdapter {
  UINT adapter;
};

struct Escape {
  UINT adapter;
  UINT device;
  UINT type;
  UINT flags;
  void* data;
  UINT size;
  UINT context;
};

struct DuplContext {
  UINT status;
  HANDLE pid;
  UINT presents;
  LARGE_INTEGER last_present;
  LARGE_INTEGER last_mouse;
  CHAR name[16];
};

struct DuplSnapshot {
  UINT size;
  UINT session_processes;
  UINT active_connections;
  UINT sources;
  UINT contexts;
  UINT padding;
  DuplContext entries[1];
};

static_assert(sizeof(DuplContext) == 56 && offsetof(DuplSnapshot, entries) == 24, "d3dkmt layout changed");

constexpr UINT kSnapshotEscape = 10;

using OpenFn = LONG(APIENTRY*)(OpenAdapter*);
using CloseFn = LONG(APIENTRY*)(const CloseAdapter*);
using EscapeFn = LONG(APIENTRY*)(const Escape*);

struct Kmt {
  OpenFn open = nullptr;
  CloseFn close = nullptr;
  EscapeFn escape = nullptr;
};

const Kmt& kmt() {
  static const Kmt fns = [] {
    Kmt k;
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    k.open = reinterpret_cast<OpenFn>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName"));
    k.close = reinterpret_cast<CloseFn>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
    k.escape = reinterpret_cast<EscapeFn>(GetProcAddress(gdi, "D3DKMTEscape"));
    return k;
  }();
  return fns;
}

void read_snapshot(const Kmt& k, UINT adapter, std::vector<Watcher>* out) {
  std::vector<uint8_t> buf(sizeof(DuplSnapshot));
  auto* snap = reinterpret_cast<DuplSnapshot*>(buf.data());
  snap->size = offsetof(DuplSnapshot, entries);
  Escape esc{adapter, 0, kSnapshotEscape, 0, buf.data(), snap->size, 0};
  if (k.escape(&esc) < 0) return;
  const UINT need = snap->size;
  if (need < offsetof(DuplSnapshot, entries) || need > (1u << 20)) return;
  buf.assign(std::max<size_t>(need, sizeof(DuplSnapshot)), 0);
  snap = reinterpret_cast<DuplSnapshot*>(buf.data());
  snap->size = need;
  esc.data = buf.data();
  esc.size = need;
  if (k.escape(&esc) < 0) return;
  const uint64_t count = uint64_t(snap->sources) * snap->contexts;
  if (offsetof(DuplSnapshot, entries) + count * sizeof(DuplContext) > need) return;
  for (uint64_t i = 0; i < count; ++i) {
    const DuplContext& entry = snap->entries[i];
    if (entry.status != 1) continue;
    const DWORD pid = DWORD(ULONG_PTR(entry.pid));
    if (std::any_of(out->begin(), out->end(), [pid](const Watcher& w) { return w.pid == pid; })) continue;
    char raw[sizeof entry.name + 1] = {};
    memcpy(raw, entry.name, sizeof entry.name);
    out->push_back({Source::kDuplication, pid, process_name(pid, raw), L"copies the screen", process_path(pid)});
  }
}

}  // namespace

std::vector<Watcher> duplicating_processes() {
  std::vector<Watcher> found;
  const Kmt& k = kmt();
  if (!k.open || !k.close || !k.escape) return found;
  DISPLAY_DEVICEW device{};
  device.cb = sizeof device;
  for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &device, 0); ++i, device.cb = sizeof device) {
    if (!(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
    if (device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) continue;
    OpenAdapter open{};
    lstrcpynW(open.device, device.DeviceName, ARRAYSIZE(open.device));
    if (k.open(&open) < 0) continue;
    read_snapshot(k, open.adapter, &found);
    const CloseAdapter close{open.adapter};
    k.close(&close);
  }
  return found;
}

}  // namespace screenlight
