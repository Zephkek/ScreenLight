#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "screenlight.h"

namespace screenlight {

HICON make_light(COLORREF color, int size) {
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = size;
  bi.bmiHeader.biHeight = -size;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HBITMAP pixels = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!pixels) return nullptr;
  const float center = (size - 1) / 2.0f, radius = size * 0.34f;
  auto* out = static_cast<uint32_t*>(bits);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float cover = std::clamp(radius - std::hypot(x - center, y - center) + 0.5f, 0.0f, 1.0f);
      out[y * size + x] = uint32_t(cover * 255 + 0.5f) << 24 | (color & 0xff) << 16 | (color & 0xff00) | (color >> 16 & 0xff);
    }
  }
  std::vector<uint8_t> clear((size + 15) / 16 * 2 * size);
  HBITMAP mask = CreateBitmap(size, size, 1, 1, clear.data());
  ICONINFO info{TRUE, 0, 0, mask, pixels};
  HICON icon = CreateIconIndirect(&info);
  DeleteObject(mask);
  DeleteObject(pixels);
  return icon;
}

}  // namespace screenlight
