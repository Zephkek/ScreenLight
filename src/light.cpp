#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

struct Eye {
  int size;
  float half_width, half_height, lid, pupil;
};

constexpr Eye kEyes[] = {{16, 8, 5, 1, 4},   {20, 10, 6, 1, 5},   {24, 12, 8, 2, 6},
                         {32, 16, 10, 2, 8}, {40, 20, 12.5f, 2.5f, 10}, {48, 24, 15, 3, 12}};
constexpr COLORREF kInk = RGB(61, 66, 74);
constexpr COLORREF kPaper = RGB(218, 221, 225);

Eye eye_for(int size) {
  for (const Eye& e : kEyes) {
    if (e.size == size) return e;
  }
  const float s = size / 16.0f;
  return {size, 8 * s, 5 * s, 1 * s, 4 * s};
}

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

float mix(float from, float to, float t) { return from + (to - from) * t; }

}  // namespace

HICON make_light(bool lit, int size) {
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = size;
  bi.bmiHeader.biHeight = -size;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HBITMAP pixels = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!pixels) return nullptr;
  const Eye eye = eye_for(size);
  const COLORREF iris = lit ? kLitColor : kIdleColor;
  const float c = (size - 1) / 2.0f;
  const float radius = (eye.half_width * eye.half_width + eye.half_height * eye.half_height) / (2 * eye.half_height);
  const float offset = radius - eye.half_height;
  auto* out = static_cast<uint32_t*>(bits);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float edge = std::max(std::hypot(x - c, y - (c + offset)), std::hypot(x - c, y - (c - offset))) - radius;
      const float shape = clamp01(0.5f - edge);
      const float white = clamp01(0.5f - (edge + eye.lid));
      const float pupil = clamp01(0.5f - std::max(std::hypot(x - c, y - c) - eye.pupil, edge + eye.lid));
      const auto channel = [&](BYTE ink, BYTE paper, BYTE color) {
        return uint32_t(mix(mix(ink, paper, white), color, pupil) + 0.5f);
      };
      const uint32_t r = channel(GetRValue(kInk), GetRValue(kPaper), GetRValue(iris));
      const uint32_t g = channel(GetGValue(kInk), GetGValue(kPaper), GetGValue(iris));
      const uint32_t b = channel(GetBValue(kInk), GetBValue(kPaper), GetBValue(iris));
      out[y * size + x] = uint32_t(shape * 255 + 0.5f) << 24 | r << 16 | g << 8 | b;
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
