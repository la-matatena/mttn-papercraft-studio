#if defined(PAPERCRAFT_STANDALONE_OFX)
#include "papercraft_kernel.h"  // NOLINT(build/include)
#else
#include "experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro/ofx/papercraft_kernel.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

namespace papercraft {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;

float Clamp(float v, float lo, float hi) {
  return std::min(hi, std::max(lo, v));
}

float Clamp01(float v) { return Clamp(v, 0.0f, 1.0f); }

float Smoothstep(float edge0, float edge1, float x) {
  if (std::abs(edge1 - edge0) < 1e-12f) {
    return x >= edge0 ? 1.0f : 0.0f;
  }
  const float t = Clamp01((x - edge0) / (edge1 - edge0));
  return t * t * (3.0f - 2.0f * t);
}

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

RgbaColor LerpColor(const RgbaColor& a, const RgbaColor& b, float t) {
  return {Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t),
          Lerp(a.a, b.a, t)};
}

// Porter-Duff Over with premultiplied inputs and output.
RgbaColor CompositeOver(const RgbaColor& fg, const RgbaColor& bg) {
  const float inv_a = 1.0f - Clamp01(fg.a);
  return {
      fg.r + bg.r * inv_a,
      fg.g + bg.g * inv_a,
      fg.b + bg.b * inv_a,
      Clamp01(fg.a + bg.a * inv_a),
  };
}

std::ptrdiff_t PixelSizeBytes(const ImageView& view) {
  int bytes_per_channel = 4;
  if (view.depth == PixelDepth::kShort) {
    bytes_per_channel = 2;
  } else if (view.depth == PixelDepth::kByte) {
    bytes_per_channel = 1;
  }
  return static_cast<std::ptrdiff_t>(bytes_per_channel) *
         static_cast<std::ptrdiff_t>(view.components);
}

const std::uint8_t* PixelConstPtr(const ImageView& view, int x, int y) {
  const std::ptrdiff_t pixel_bytes = PixelSizeBytes(view);
  return static_cast<const std::uint8_t*>(view.data) +
         static_cast<std::ptrdiff_t>(y - view.bounds.y1) * view.row_bytes +
         static_cast<std::ptrdiff_t>(x - view.bounds.x1) * pixel_bytes;
}

std::uint8_t* PixelMutPtr(const ImageView& view, int x, int y) {
  const std::ptrdiff_t pixel_bytes = PixelSizeBytes(view);
  return static_cast<std::uint8_t*>(view.data) +
         static_cast<std::ptrdiff_t>(y - view.bounds.y1) * view.row_bytes +
         static_cast<std::ptrdiff_t>(x - view.bounds.x1) * pixel_bytes;
}

PixelRect EffectiveRod(const ImageView& view) {
  if (view.rod.x2 > view.rod.x1 && view.rod.y2 > view.rod.y1) {
    return view.rod;
  }
  return view.bounds;
}

RgbaColor ReadPixel(const ImageView& view, int x, int y) {
  const std::uint8_t* ptr = PixelConstPtr(view, x, y);
  RgbaColor out;
  if (view.depth == PixelDepth::kFloat) {
    float c[4];
    std::memcpy(c, ptr, sizeof(c));
    out = {c[0], c[1], c[2], c[3]};
  } else if (view.depth == PixelDepth::kShort) {
    std::uint16_t c[4];
    std::memcpy(c, ptr, sizeof(c));
    constexpr float kInv = 1.0f / 65535.0f;
    out = {c[0] * kInv, c[1] * kInv, c[2] * kInv, c[3] * kInv};
  } else {
    constexpr float kInvByte = 1.0f / 255.0f;
    out = {ptr[0] * kInvByte, ptr[1] * kInvByte, ptr[2] * kInvByte,
           ptr[3] * kInvByte};
  }
  if (view.alpha_mode == AlphaMode::kOpaque) {
    out.a = 1.0f;
  } else if (view.alpha_mode == AlphaMode::kUnPremultiplied) {
    const float a = Clamp01(out.a);
    out = {out.r * a, out.g * a, out.b * a, a};
  }
  return out;
}

void WritePixel(const ImageView& view, int x, int y, const RgbaColor& color) {
  std::uint8_t* ptr = PixelMutPtr(view, x, y);
  if (view.depth == PixelDepth::kFloat) {
    const float c[4] = {color.r, color.g, color.b, color.a};
    std::memcpy(ptr, c, sizeof(c));
    return;
  }
  if (view.depth == PixelDepth::kShort) {
    const std::uint16_t c[4] = {
        static_cast<std::uint16_t>(Clamp01(color.r) * 65535.0f + 0.5f),
        static_cast<std::uint16_t>(Clamp01(color.g) * 65535.0f + 0.5f),
        static_cast<std::uint16_t>(Clamp01(color.b) * 65535.0f + 0.5f),
        static_cast<std::uint16_t>(Clamp01(color.a) * 65535.0f + 0.5f),
    };
    std::memcpy(ptr, c, sizeof(c));
    return;
  }
  ptr[0] = static_cast<std::uint8_t>(Clamp01(color.r) * 255.0f + 0.5f);
  ptr[1] = static_cast<std::uint8_t>(Clamp01(color.g) * 255.0f + 0.5f);
  ptr[2] = static_cast<std::uint8_t>(Clamp01(color.b) * 255.0f + 0.5f);
  ptr[3] = static_cast<std::uint8_t>(Clamp01(color.a) * 255.0f + 0.5f);
}

RgbaColor SampleBilinearClamped(const ImageView& view, float px, float py) {
  if (!IsValidRgbaView(view)) {
    return {};
  }
  const float sx = px - 0.5f;
  const float sy = py - 0.5f;
  const int ix0 = static_cast<int>(std::floor(sx));
  const int iy0 = static_cast<int>(std::floor(sy));
  const float fx = sx - static_cast<float>(ix0);
  const float fy = sy - static_cast<float>(iy0);

  const int x0 = std::min(view.bounds.x2 - 1, std::max(view.bounds.x1, ix0));
  const int x1 =
      std::min(view.bounds.x2 - 1, std::max(view.bounds.x1, ix0 + 1));
  const int y0 = std::min(view.bounds.y2 - 1, std::max(view.bounds.y1, iy0));
  const int y1 =
      std::min(view.bounds.y2 - 1, std::max(view.bounds.y1, iy0 + 1));

  const RgbaColor c00 = ReadPixel(view, x0, y0);
  const RgbaColor c10 = ReadPixel(view, x1, y0);
  const RgbaColor c01 = ReadPixel(view, x0, y1);
  const RgbaColor c11 = ReadPixel(view, x1, y1);
  return LerpColor(LerpColor(c00, c10, fx), LerpColor(c01, c11, fx), fy);
}

RgbaColor SampleBilinearTransparentOutside(const ImageView& view, float px,
                                           float py) {
  if (!IsValidRgbaView(view)) {
    return {};
  }
  const float fx1 = static_cast<float>(view.bounds.x1);
  const float fy1 = static_cast<float>(view.bounds.y1);
  const float fx2 = static_cast<float>(view.bounds.x2);
  const float fy2 = static_cast<float>(view.bounds.y2);
  if (px < fx1 - 1.0f || px > fx2 + 1.0f || py < fy1 - 1.0f ||
      py > fy2 + 1.0f) {
    return {};
  }

  auto read_safe = [&](int ix, int iy) -> RgbaColor {
    if (ix < view.bounds.x1 || ix >= view.bounds.x2 || iy < view.bounds.y1 ||
        iy >= view.bounds.y2) {
      return {};
    }
    return ReadPixel(view, ix, iy);
  };

  const float sx = px - 0.5f;
  const float sy = py - 0.5f;
  const int x0 = static_cast<int>(std::floor(sx));
  const int y0 = static_cast<int>(std::floor(sy));
  const float fx = sx - static_cast<float>(x0);
  const float fy = sy - static_cast<float>(y0);

  return LerpColor(
      LerpColor(read_safe(x0, y0), read_safe(x0 + 1, y0), fx),
      LerpColor(read_safe(x0, y0 + 1), read_safe(x0 + 1, y0 + 1), fx), fy);
}

// Fast deterministic integer spatial hash returning [0, 1].
float Hash2D(int ix, int iy, std::uint32_t seed) {
  std::uint32_t h = static_cast<std::uint32_t>(ix) * 374761393u +
                    static_cast<std::uint32_t>(iy) * 668265263u +
                    seed * 1442695041u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= (h >> 16);
  return static_cast<float>(h & 0x00FFFFFFu) * (1.0f / 16777215.0f);
}

float ValueNoise2D(float x, float y, std::uint32_t seed) {
  const int ix = static_cast<int>(std::floor(x));
  const int iy = static_cast<int>(std::floor(y));
  const float fx = x - static_cast<float>(ix);
  const float fy = y - static_cast<float>(iy);
  const float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
  const float uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);

  const float n00 = Hash2D(ix, iy, seed);
  const float n10 = Hash2D(ix + 1, iy, seed);
  const float n01 = Hash2D(ix, iy + 1, seed);
  const float n11 = Hash2D(ix + 1, iy + 1, seed);
  return Lerp(Lerp(n00, n10, ux), Lerp(n01, n11, ux), uy);
}

// Signed [-1, 1] fractional Brownian motion for ragged torn paper edges.
float TearEdgeNoise(float nx, float ny, float frequency, int octaves,
                    std::uint32_t seed) {
  const int clamped_octaves = std::min(6, std::max(1, octaves));
  float sum = 0.0f;
  float amp = 0.55f;
  float norm = 0.0f;
  float freq = std::max(0.5f, frequency);
  for (int i = 0; i < clamped_octaves; ++i) {
    const float n = ValueNoise2D(nx * freq, ny * freq,
                                 seed + static_cast<std::uint32_t>(i) * 97u);
    sum += (n * 2.0f - 1.0f) * amp;
    norm += amp;
    freq *= 2.07f;
    amp *= 0.52f;
  }
  return norm > 0.0f ? (sum / norm) : 0.0f;
}

// Fine cardstock fiber grain returning multiplication factor around ~[0.82,
// 1.05].
float PaperFiberGrain(float nx, float ny, float scale, std::uint32_t seed) {
  const float s = 56.0f / std::max(0.1f, scale);
  const float n1 = ValueNoise2D(nx * s * 1.15f, ny * s * 0.90f, seed + 31u);
  const float n2 = ValueNoise2D(nx * s * 2.45f, ny * s * 2.65f, seed + 173u);
  const float grain = n1 * 0.6f + n2 * 0.4f;
  return Lerp(0.81f, 1.04f, grain);
}

// Sharp ridged crease/crumple field returning [0, 1] where 1.0 = sharp fold
// valley.
float PaperCreaseField(float nx, float ny, float scale, std::uint32_t seed) {
  const float s = std::max(0.25f, scale) * 1.6f;
  float ridge_sum = 0.0f;
  float weight = 0.55f;
  float total_w = 0.0f;
  float freq = s;
  for (int i = 0; i < 3; ++i) {
    const float rx = nx * freq + (i == 1 ? 0.37f * ny * freq : 0.0f);
    const float ry = ny * freq + (i == 2 ? -0.41f * nx * freq : 0.0f);
    const float n =
        ValueNoise2D(rx, ry, seed + static_cast<std::uint32_t>(i) * 211u);
    const float ridge = 1.0f - std::abs(2.0f * n - 1.0f);
    ridge_sum += (ridge * ridge * ridge) * weight;
    total_w += weight;
    freq *= 1.95f;
    weight *= 0.55f;
  }
  return total_w > 0.0f ? Clamp01(ridge_sum / total_w) : 0.0f;
}

// Procedural gradient/displacement vector for 3D crumple ball warp.
void CrumpleDisplacement(float nx, float ny, float scale, std::uint32_t seed,
                         float* dx, float* dy, float* crease_shade) {
  const float eps = 0.008f;
  const float c0 = PaperCreaseField(nx, ny, scale, seed);
  const float cx = PaperCreaseField(nx + eps, ny, scale, seed);
  const float cy = PaperCreaseField(nx, ny + eps, scale, seed);
  *dx = (cx - c0) / eps;
  *dy = (cy - c0) / eps;
  *crease_shade = c0;
}

// Vibrant built-in multi-layer paper craft badge/sticker for Generator / empty
// track testing. Coordinates nx, ny are normalized [0, 1] across canvas.
RgbaColor EvaluateDemoSticker(float nx, float ny, float aspect) {
  const float dx = (nx - 0.5f) * aspect;
  const float dy = ny - 0.5f;
  const float r = std::sqrt(dx * dx + dy * dy);
  const float angle = std::atan2(dy, dx);

  // Scalloped craft sticker silhouette + warm cream center badge + star core.
  const float outer_r = 0.25f + 0.014f * std::cos(angle * 12.0f);
  const float outer_mask = Smoothstep(outer_r + 0.004f, outer_r - 0.004f, r);
  if (outer_mask <= 1e-4f) {
    return {};
  }

  // Coral outer circle, teal ring, ivory core, golden 6-point emblem.
  RgbaColor col = {0.91f, 0.34f, 0.21f, 1.0f};
  const float ring_mask = Smoothstep(0.185f, 0.178f, r);
  col = LerpColor(col, {0.20f, 0.64f, 0.53f, 1.0f}, ring_mask);

  const float inner_disk = Smoothstep(0.145f, 0.138f, r);
  col = LerpColor(col, {0.97f, 0.94f, 0.86f, 1.0f}, inner_disk);

  const float star_r = 0.075f + 0.030f * std::cos(angle * 5.0f);
  const float star_mask = Smoothstep(star_r + 0.004f, star_r - 0.004f, r);
  col = LerpColor(col, {0.95f, 0.67f, 0.18f, 1.0f}, star_mask);

  return {col.r * outer_mask, col.g * outer_mask, col.b * outer_mask,
          outer_mask};
}

// Chroma keyer + spill suppressor expecting premultiplied RGBA and returning
// premultiplied RGBA.
RgbaColor ApplyChromaKey(const CutoutParams& params,
                         const RgbaColor& premult_src) {
  const float a = Clamp01(premult_src.a);
  if (a <= 1e-4f) {
    return {};
  }
  const float inv_a = 1.0f / a;
  float r = premult_src.r * inv_a;
  float g = premult_src.g * inv_a;
  float b = premult_src.b * inv_a;

  const float kr = params.key_r;
  const float kg = params.key_g;
  const float kb = params.key_b;

  const float luma = 0.299f * r + 0.587f * g + 0.114f * b;
  const float k_luma = 0.299f * kr + 0.587f * kg + 0.114f * kb;
  const float luma_ratio =
      Clamp((k_luma + 0.06f) / (luma + 0.06f), 0.35f, 2.5f);

  const float dr_c = r * luma_ratio - kr;
  const float dg_c = g * luma_ratio - kg;
  const float db_c = b * luma_ratio - kb;
  const float dist_chroma = std::sqrt(dr_c * dr_c + dg_c * dg_c + db_c * db_c);

  const float dr_rgb = r - kr;
  const float dg_rgb = g - kg;
  const float db_rgb = b - kb;
  const float dist_rgb =
      std::sqrt(dr_rgb * dr_rgb + dg_rgb * dg_rgb + db_rgb * db_rgb);

  const float dist = Lerp(dist_rgb, dist_chroma, Clamp01(params.key_balance));
  const float gain = Clamp(params.key_gain, 0.1f, 3.0f);
  const float low = 0.16f * gain;
  const float high = 0.36f * gain + 0.06f;
  const float key_alpha = Smoothstep(low, high, dist);

  // Suppress green or blue spill along semi-transparent matte edges.
  if (kg >= kr && kg >= kb) {
    const float avg_rb = 0.5f * (r + b);
    if (g > avg_rb) {
      g = Lerp(g, avg_rb, 1.0f - key_alpha * 0.55f);
    }
  } else if (kb >= kr && kb >= kg) {
    const float avg_rg = 0.5f * (r + g);
    if (b > avg_rg) {
      b = Lerp(b, avg_rg, 1.0f - key_alpha * 0.55f);
    }
  }

  const float out_a = a * key_alpha;
  return {Clamp01(r) * out_a, Clamp01(g) * out_a, Clamp01(b) * out_a, out_a};
}

struct SubjectTransform {
  float cw = 1.0f;
  float ch = 1.0f;
  float origin_x = 0.0f;
  float origin_y = 0.0f;
  float cx_canvas = 0.5f;
  float cy_canvas = 0.5f;
  float inv_scale = 1.0f;
  float cos_a = 1.0f;
  float sin_a = 0.0f;
  float offset_x_px = 0.0f;
  float offset_y_px = 0.0f;
  float src_rod_x1 = 0.0f;
  float src_rod_y1 = 0.0f;
  float src_rod_w = 1.0f;
  float src_rod_h = 1.0f;
  bool use_demo = false;
};

SubjectTransform BuildSubjectTransform(const CutoutParams& params,
                                       const ImageView& source,
                                       const PixelRect& canvas_bounds) {
  SubjectTransform xf;
  xf.cw = static_cast<float>(std::max(1, canvas_bounds.x2 - canvas_bounds.x1));
  xf.ch = static_cast<float>(std::max(1, canvas_bounds.y2 - canvas_bounds.y1));
  xf.origin_x = static_cast<float>(canvas_bounds.x1);
  xf.origin_y = static_cast<float>(canvas_bounds.y1);
  xf.cx_canvas = xf.origin_x + xf.cw * 0.5f;
  xf.cy_canvas = xf.origin_y + xf.ch * 0.5f;
  xf.inv_scale = 1.0f / std::max(0.05f, params.media_scale);
  const float rad = -params.media_angle_deg * kDegToRad;
  xf.cos_a = std::cos(rad);
  xf.sin_a = std::sin(rad);
  xf.offset_x_px = (params.media_offset_x - 0.5f) * xf.cw;
  xf.offset_y_px = (params.media_offset_y - 0.5f) * xf.ch;
  xf.use_demo = (params.source_mode == 1) || !IsValidRgbaView(source);
  if (!xf.use_demo) {
    const PixelRect src_rod = EffectiveRod(source);
    xf.src_rod_x1 = static_cast<float>(src_rod.x1);
    xf.src_rod_y1 = static_cast<float>(src_rod.y1);
    xf.src_rod_w = static_cast<float>(std::max(1, src_rod.x2 - src_rod.x1));
    xf.src_rod_h = static_cast<float>(std::max(1, src_rod.y2 - src_rod.y1));
  }
  return xf;
}

// Evaluates canonical subject RGBA at pixel-space coordinates (px, py) relative
// to `canvas_bounds`, after applying Media Placement and optional Chroma Key.
RgbaColor EvaluateSubjectAtCanonicalPx(const CutoutParams& params,
                                       const SubjectTransform& xf,
                                       const ImageView& source, float px,
                                       float py) {
  const float dx = px - (xf.cx_canvas + xf.offset_x_px);
  const float dy = py - (xf.cy_canvas + xf.offset_y_px);

  const float rx =
      (dx * xf.cos_a - dy * xf.sin_a) * xf.inv_scale + xf.cx_canvas;
  const float ry =
      (dx * xf.sin_a + dy * xf.cos_a) * xf.inv_scale + xf.cy_canvas;

  if (xf.use_demo) {
    const float nx = (rx - xf.origin_x) / xf.cw;
    const float ny = (ry - xf.origin_y) / xf.ch;
    return EvaluateDemoSticker(nx, ny, xf.cw / xf.ch);
  }

  // Remap canonical pixel coordinates via full-frame source RoD so cropped
  // input buffers map accurately while SampleBilinearTransparentOutside guards
  // physical bounds.
  const float sx = xf.src_rod_x1 + ((rx - xf.origin_x) / xf.cw) * xf.src_rod_w;
  const float sy = xf.src_rod_y1 + ((ry - xf.origin_y) / xf.ch) * xf.src_rod_h;

  const RgbaColor raw = SampleBilinearTransparentOutside(source, sx, sy);
  if (params.enable_chroma_key) {
    return ApplyChromaKey(params, raw);
  }
  return raw;
}

void RunScanlines(int y_begin, int y_end, const ParallelRowsFn& parallel_for,
                  const std::function<void(int y)>& row_fn) {
  if (y_begin >= y_end) {
    return;
  }
  if (parallel_for) {
    parallel_for(y_begin, y_end, row_fn);
  } else {
    for (int y = y_begin; y < y_end; ++y) {
      row_fn(y);
    }
  }
}

constexpr int kMaxSdfDim = 960;

// Felzenszwalb-Huttenlocher linear-time 1D squared Euclidean distance
// transform.
void DistanceTransform1D(const float* input, int n, int* v, float* z,
                         float* output) {
  int k = -1;
  for (int q = 0; q < n; ++q) {
    if (input[q] >= 1e18f) {
      continue;
    }
    if (k < 0) {
      k = 0;
      v[0] = q;
      z[0] = -1e30f;
      z[1] = 1e30f;
      continue;
    }
    float s = 0.0f;
    while (true) {
      const int r = v[k];
      const float num = (input[q] + static_cast<float>(q) * q) -
                        (input[r] + static_cast<float>(r) * r);
      const float den = static_cast<float>(2 * (q - r));
      s = num / den;
      if (s <= z[k]) {
        --k;
        if (k < 0) {
          break;
        }
      } else {
        break;
      }
    }
    ++k;
    v[k] = q;
    z[k] = s;
    z[k + 1] = 1e30f;
  }
  if (k < 0) {
    for (int q = 0; q < n; ++q) {
      output[q] = 1e20f;
    }
    return;
  }
  int env = 0;
  for (int q = 0; q < n; ++q) {
    while (z[env + 1] < static_cast<float>(q)) {
      ++env;
    }
    const float diff = static_cast<float>(q - v[env]);
    output[q] = diff * diff + input[v[env]];
  }
}

void DistanceTransform2D(int width, int height,
                         const ParallelRowsFn& parallel_for,
                         std::vector<float>* grid) {
  if (width <= 0 || height <= 0 || width > kMaxSdfDim || height > kMaxSdfDim) {
    return;
  }
  RunScanlines(0, width, parallel_for, [&](int x) {
    std::array<float, kMaxSdfDim> line_in;
    std::array<float, kMaxSdfDim> line_out;
    std::array<int, kMaxSdfDim> v;
    std::array<float, kMaxSdfDim + 1> z;
    for (int y = 0; y < height; ++y) {
      line_in[y] = (*grid)[static_cast<std::size_t>(y) * width + x];
    }
    DistanceTransform1D(line_in.data(), height, v.data(), z.data(),
                        line_out.data());
    for (int y = 0; y < height; ++y) {
      (*grid)[static_cast<std::size_t>(y) * width + x] = line_out[y];
    }
  });

  RunScanlines(0, height, parallel_for, [&](int y) {
    std::array<float, kMaxSdfDim> line_out;
    std::array<int, kMaxSdfDim> v;
    std::array<float, kMaxSdfDim + 1> z;
    float* row = grid->data() + static_cast<std::size_t>(y) * width;
    DistanceTransform1D(row, width, v.data(), z.data(), line_out.data());
    std::memcpy(row, line_out.data(),
                static_cast<std::size_t>(width) * sizeof(float));
  });
}

struct CoarseSignedDistanceField {
  int width = 0;
  int height = 0;
  int step = 1;
  float origin_x = 0.0f;
  float origin_y = 0.0f;
  std::vector<float> signed_dist_px;

  float SamplePx(float px, float py) const {
    if (width <= 0 || height <= 0) {
      return 1e6f;
    }
    const float gx = Clamp((px - origin_x) / static_cast<float>(step) - 0.5f,
                           0.0f, static_cast<float>(width - 1));
    const float gy = Clamp((py - origin_y) / static_cast<float>(step) - 0.5f,
                           0.0f, static_cast<float>(height - 1));
    const int x0 = static_cast<int>(std::floor(gx));
    const int y0 = static_cast<int>(std::floor(gy));
    const int x1 = std::min(width - 1, x0 + 1);
    const int y1 = std::min(height - 1, y0 + 1);
    const float fx = gx - static_cast<float>(x0);
    const float fy = gy - static_cast<float>(y0);

    const float d00 = signed_dist_px[static_cast<std::size_t>(y0) * width + x0];
    const float d10 = signed_dist_px[static_cast<std::size_t>(y0) * width + x1];
    const float d01 = signed_dist_px[static_cast<std::size_t>(y1) * width + x0];
    const float d11 = signed_dist_px[static_cast<std::size_t>(y1) * width + x1];
    return Lerp(Lerp(d00, d10, fx), Lerp(d01, d11, fx), fy);
  }
};

CoarseSignedDistanceField BuildSubjectSignedDistanceField(
    const CutoutParams& params, const SubjectTransform& subj_xf,
    const ImageView& source, const PixelRect& canvas_bounds,
    const ParallelRowsFn& parallel_for) {
  const int cw = std::max(1, canvas_bounds.x2 - canvas_bounds.x1);
  const int ch = std::max(1, canvas_bounds.y2 - canvas_bounds.y1);
  const int step = std::max(1, std::max((cw + 959) / 960, (ch + 539) / 540));

  CoarseSignedDistanceField sdf;
  sdf.step = step;
  sdf.width = std::max(1, (cw + step - 1) / step);
  sdf.height = std::max(1, (ch + step - 1) / step);
  sdf.origin_x = static_cast<float>(canvas_bounds.x1);
  sdf.origin_y = static_cast<float>(canvas_bounds.y1);

  const std::size_t total = static_cast<std::size_t>(sdf.width) * sdf.height;
  std::vector<float> outside_sq(total, 1e20f);
  std::vector<float> inside_sq(total, 1e20f);

  RunScanlines(0, sdf.height, parallel_for, [&](int gy) {
    const float py = sdf.origin_y +
                     (static_cast<float>(gy) + 0.5f) * static_cast<float>(step);
    for (int gx = 0; gx < sdf.width; ++gx) {
      const float px = sdf.origin_x + (static_cast<float>(gx) + 0.5f) *
                                          static_cast<float>(step);
      const RgbaColor sample =
          EvaluateSubjectAtCanonicalPx(params, subj_xf, source, px, py);
      const std::size_t idx = static_cast<std::size_t>(gy) * sdf.width + gx;
      if (sample.a >= 0.45f) {
        outside_sq[idx] = 0.0f;
      } else {
        inside_sq[idx] = 0.0f;
      }
    }
  });

  DistanceTransform2D(sdf.width, sdf.height, parallel_for, &outside_sq);
  DistanceTransform2D(sdf.width, sdf.height, parallel_for, &inside_sq);

  sdf.signed_dist_px.resize(total);
  const float half_step = 0.5f * static_cast<float>(step);
  const float step_f = static_cast<float>(step);
  RunScanlines(0, sdf.height, parallel_for, [&](int gy) {
    const std::size_t row_off = static_cast<std::size_t>(gy) * sdf.width;
    for (int gx = 0; gx < sdf.width; ++gx) {
      const std::size_t i = row_off + gx;
      const float osq = outside_sq[i];
      const float isq = inside_sq[i];
      if (osq >= 1e18f) {
        sdf.signed_dist_px[i] = 1e6f;
      } else if (isq >= 1e18f) {
        sdf.signed_dist_px[i] = -1e6f;
      } else if (osq > 0.0f) {
        sdf.signed_dist_px[i] = std::sqrt(osq) * step_f - half_step;
      } else {
        sdf.signed_dist_px[i] = -(std::sqrt(isq) * step_f - half_step);
      }
    }
  });
  return sdf;
}

struct CutoutAnimState {
  CutoutAnimStyle style = CutoutAnimStyle::kNone;
  float ease_p = 1.0f;  // 1.0 = flat/resting, 0.0 = collapsed/folded away
  float pop_scale = 1.0f;
  float angle_rad = 0.0f;
  float tilt_y_cos = 1.0f;
  float perspective_k = 0.0f;
  float perspective_ky = 0.0f;
  float slide_x_px = 0.0f;
  float slide_y_px = 0.0f;
  float crumple_warp_norm = 0.0f;
  float extra_crease_shade = 0.0f;
  float step_time = 0.0f;
  float jitter_dx_px = 0.0f;
  float jitter_dy_px = 0.0f;
  float jitter_rot_rad = 0.0f;
  float cos_m = 1.0f;
  float sin_m = 0.0f;
  float inv_m_scale = 1.0f;
  float a_cos = 1.0f;
  float a_sin = 0.0f;
};

CutoutAnimState ComputeCutoutAnimState(const CutoutParams& params, float cw,
                                       float ch) {
  CutoutAnimState state;
  const float min_dim = std::min(cw, ch);

  const float hold = static_cast<float>(std::max(1, params.hold_frames));
  const double rel_frame = Clamp(
      static_cast<float>(params.current_time - params.frame_start), 0.0f,
      static_cast<float>(std::max(0.0, params.frame_end - params.frame_start)));
  state.step_time =
      params.enable_stop_motion
          ? static_cast<float>(params.frame_start) +
                std::floor(static_cast<float>(rel_frame) / hold) * hold
          : static_cast<float>(params.current_time);

  // Stop-motion hand-cut jitter.
  if (params.enable_stop_motion) {
    const float seed_f = static_cast<float>(params.random_seed);
    state.jitter_dx_px = params.jitter_pos_norm * min_dim *
                         std::sin(state.step_time * 12.9898f + seed_f);
    state.jitter_dy_px = params.jitter_pos_norm * min_dim *
                         std::cos(state.step_time * 78.233f + seed_f * 1.37f);
    state.jitter_rot_rad = params.jitter_rot_deg * kDegToRad *
                           std::sin(state.step_time * 43.17f + seed_f * 2.11f);
  }

  // Compute intro / outro progress across clip duration; pick the closer
  // boundary when short clips cause intro and outro windows to overlap.
  float t_in = 1.0f;
  const bool has_intro = (params.intro_style > 0 && params.intro_frames > 0);
  if (has_intro) {
    t_in = Clamp01((state.step_time - static_cast<float>(params.frame_start)) /
                   static_cast<float>(params.intro_frames));
  }
  float t_out = 1.0f;
  const bool has_outro = (params.outro_style > 0 && params.outro_frames > 0);
  if (has_outro) {
    t_out = Clamp01((static_cast<float>(params.frame_end) - state.step_time) /
                    static_cast<float>(params.outro_frames));
  }

  float raw_p = 1.0f;
  int active_style = 0;
  if (has_intro && t_in < 1.0f && (!has_outro || t_in <= t_out)) {
    raw_p = t_in;
    active_style = params.intro_style;
  } else if (has_outro && t_out < 1.0f) {
    raw_p = t_out;
    active_style = params.outro_style;
  }

  if (active_style > 0 && raw_p < 1.0f) {
    state.style = static_cast<CutoutAnimStyle>(active_style);
    const float one_minus = 1.0f - raw_p;
    state.ease_p = 1.0f - one_minus * one_minus * one_minus;
    const float inv = 1.0f - state.ease_p;
    const float mh = params.mirror_transition_h ? -1.0f : 1.0f;
    const float mv = params.mirror_transition_v ? -1.0f : 1.0f;

    switch (state.style) {
      case CutoutAnimStyle::kPaperUnfold: {
        const float fold_rad = Clamp(inv * 1.48f, 0.0f, 1.54f);
        state.tilt_y_cos = std::max(0.03f, std::cos(fold_rad));
        state.perspective_k = mh * 0.42f * std::sin(fold_rad);
        state.perspective_ky = mv * 0.18f * std::sin(fold_rad);
        state.slide_y_px = mv * (-0.06f) * ch * inv;
        state.pop_scale = 0.48f + 0.52f * state.ease_p;
        state.crumple_warp_norm = 0.015f * inv;
        state.extra_crease_shade = 0.45f * inv;
        break;
      }
      case CutoutAnimStyle::kCrumpleBall: {
        state.pop_scale = std::max(0.06f, 0.22f + 0.78f * state.ease_p);
        state.angle_rad = mh * mv * 35.0f * inv * kDegToRad;
        state.slide_y_px = mv * 0.08f * ch * inv;
        state.crumple_warp_norm = 0.085f * inv;
        state.extra_crease_shade = 0.65f * inv;
        break;
      }
      case CutoutAnimStyle::kAirplaneGlide: {
        const float rad = params.transition_angle_deg * kDegToRad;
        state.slide_x_px =
            mh * params.glide_distance * cw * inv * std::cos(rad);
        state.slide_y_px =
            mv * params.glide_distance * ch * inv * std::sin(rad);
        state.angle_rad =
            mh * params.transition_angle_deg * 0.4f * inv * kDegToRad;
        state.tilt_y_cos = std::max(0.25f, std::cos(inv * 0.65f));
        state.pop_scale = 0.65f + 0.35f * state.ease_p;
        break;
      }
      case CutoutAnimStyle::kFlatPop: {
        const float c1 = 1.70158f;
        const float c3 = c1 + 1.0f;
        const float pm1 = raw_p - 1.0f;
        const float overshoot = 1.0f + c3 * pm1 * pm1 * pm1 + c1 * pm1 * pm1;
        state.pop_scale = std::max(0.01f, overshoot);
        state.slide_y_px = mv * (-0.18f) * ch * inv;
        break;
      }
      case CutoutAnimStyle::kNone:
        break;
    }
  }

  const float master_rad =
      -(params.master_angle_deg * kDegToRad + state.jitter_rot_rad);
  state.cos_m = std::cos(master_rad);
  state.sin_m = std::sin(master_rad);
  state.inv_m_scale = 1.0f / std::max(0.01f, params.master_scale);
  state.a_cos = std::cos(-state.angle_rad);
  state.a_sin = std::sin(-state.angle_rad);
  return state;
}

// Inverse-maps output pixel (x, y) into canonical unwarped paper space (ux,
// uy).
void MapOutputToPaperCanonical(const CutoutParams& params,
                               const CutoutAnimState& anim, float cw, float ch,
                               float origin_x, float origin_y, float x, float y,
                               bool apply_crumple, float* ux, float* uy,
                               float* fold_shading_out) {
  const float cx_out = origin_x + params.master_pos_x * cw + anim.jitter_dx_px;
  const float cy_out = origin_y + params.master_pos_y * ch + anim.jitter_dy_px;

  const float dx = x - cx_out;
  const float dy = y - cy_out;

  float rx = (dx * anim.cos_m - dy * anim.sin_m) * anim.inv_m_scale;
  float ry = (dx * anim.sin_m + dy * anim.cos_m) * anim.inv_m_scale;
  if (params.flip_h) {
    rx = -rx;
  }
  if (params.flip_v) {
    ry = -ry;
  }

  // Inverse transition animation transform.
  rx -= anim.slide_x_px;
  ry -= anim.slide_y_px;

  const float inv_pop = 1.0f / anim.pop_scale;
  const float ax = (rx * anim.a_cos - ry * anim.a_sin) * inv_pop;
  const float ay = (rx * anim.a_sin + ry * anim.a_cos) * inv_pop;

  // Apply inverse 3D perspective hinge tilt across horizontal/vertical axes.
  const float w_persp = std::max(0.15f, 1.0f + anim.perspective_k * (ax / cw) +
                                            anim.perspective_ky * (ay / ch));
  float px = ax / (anim.tilt_y_cos * w_persp);
  float py = ay / w_persp;

  // Apply radial crumple displacement during crumple/unfold intro/outro.
  float crease_val = 0.0f;
  if (apply_crumple && anim.crumple_warp_norm > 1e-4f) {
    float gdx = 0.0f;
    float gdy = 0.0f;
    const float cnx = 0.5f + px / cw;
    const float cny = 0.5f + py / ch;
    CrumpleDisplacement(cnx, cny, params.crease_scale,
                        static_cast<std::uint32_t>(params.random_seed) + 503u,
                        &gdx, &gdy, &crease_val);
    const float warp_px = anim.crumple_warp_norm * std::min(cw, ch) * 0.08f;
    px += gdx * warp_px;
    py += gdy * warp_px;
  }

  *ux = origin_x + 0.5f * cw + px;
  *uy = origin_y + 0.5f * ch + py;
  *fold_shading_out = crease_val * anim.extra_crease_shade;
}

}  // namespace

bool IsValidRgbaView(const ImageView& view) {
  return view.data != nullptr && view.components == 4 && view.row_bytes != 0 &&
         view.bounds.x2 > view.bounds.x1 && view.bounds.y2 > view.bounds.y1;
}

float ComputeEffectiveTransitionProgress(const TransitionParams& params) {
  const float p = Clamp01(params.progress);
  if (p <= 1e-5f) {
    return params.reverse_direction ? 1.0f : 0.0f;
  }
  if (p >= 1.0f - 1e-5f) {
    return params.reverse_direction ? 0.0f : 1.0f;
  }
  float shaped = p;
  if (params.enable_stop_motion && params.stop_motion_steps > 1) {
    const float steps = static_cast<float>(params.stop_motion_steps);
    shaped = Clamp01(std::floor(p * steps) / steps);
  }
  return params.reverse_direction ? (1.0f - shaped) : shaped;
}

void RenderCutoutFrame(const CutoutParams& params, const ImageView& source,
                       const ImageView& output, const PixelRect& canvas_bounds,
                       const PixelRect& render_window,
                       const ParallelRowsFn& parallel_for) {
  if (!IsValidRgbaView(output)) {
    return;
  }
  const int y_begin = std::max(render_window.y1, output.bounds.y1);
  const int y_end = std::min(render_window.y2, output.bounds.y2);
  const int x_begin = std::max(render_window.x1, output.bounds.x1);
  const int x_end = std::min(render_window.x2, output.bounds.x2);
  if (x_begin >= x_end || y_begin >= y_end) {
    return;
  }

  ImageView src_view = source;
  if (!params.source_is_premultiplied &&
      src_view.alpha_mode == AlphaMode::kPremultiplied) {
    src_view.alpha_mode = AlphaMode::kUnPremultiplied;
  }

  const SubjectTransform subj_xf =
      BuildSubjectTransform(params, src_view, canvas_bounds);
  const float cw = subj_xf.cw;
  const float ch = subj_xf.ch;
  const float min_dim = std::min(cw, ch);
  const float origin_x = subj_xf.origin_x;
  const float origin_y = subj_xf.origin_y;

  const CutoutAnimState anim = ComputeCutoutAnimState(params, cw, ch);

  // If collapsed below visibility threshold at transition endpoints, clear.
  if (anim.ease_p <= 1e-4f && anim.style != CutoutAnimStyle::kNone) {
    RunScanlines(y_begin, y_end, parallel_for, [&](int y) {
      for (int x = x_begin; x < x_end; ++x) {
        WritePixel(output, x, y, {});
      }
    });
    return;
  }

  const CoarseSignedDistanceField sdf = BuildSubjectSignedDistanceField(
      params, subj_xf, src_view, canvas_bounds, parallel_for);

  const float border_px =
      params.enable_border ? std::max(0.0f, params.border_width_norm * min_dim)
                           : 0.0f;
  const float tear_amp_px =
      params.enable_border
          ? std::max(0.0f, params.tear_roughness_norm * min_dim)
          : 0.0f;
  const std::uint32_t seed =
      static_cast<std::uint32_t>(std::max(0, params.random_seed));
  const std::uint32_t boil_seed =
      seed +
      static_cast<std::uint32_t>(std::floor(anim.step_time * params.edge_boil));

  const float sh_rad = params.shadow_angle_deg * kDegToRad;
  const float sh_dx_px =
      params.shadow_distance_norm * min_dim * std::cos(sh_rad);
  const float sh_dy_px =
      params.shadow_distance_norm * min_dim * std::sin(sh_rad);
  const float sh_soft_px =
      std::max(1.0f, params.shadow_softness_norm * min_dim);

  RunScanlines(y_begin, y_end, parallel_for, [&](int y) {
    const float py_out = static_cast<float>(y) + 0.5f;
    for (int x = x_begin; x < x_end; ++x) {
      const float px_out = static_cast<float>(x) + 0.5f;
      float ux = 0.0f;
      float uy = 0.0f;
      float anim_crease = 0.0f;
      MapOutputToPaperCanonical(params, anim, cw, ch, origin_x, origin_y,
                                px_out, py_out, true, &ux, &uy, &anim_crease);

      const float nu_x = (ux - origin_x) / min_dim;
      const float nu_y = (uy - origin_y) / min_dim;

      // Evaluate paper tear boundary only within the possible torn-edge band.
      const float dist_px = sdf.SamplePx(ux, uy);
      const float base_edge_dist = dist_px - border_px;
      const float tear_n =
          (params.enable_border && tear_amp_px > 0.01f &&
           std::abs(base_edge_dist) <= tear_amp_px + 3.1f)
              ? TearEdgeNoise(nu_x, nu_y, params.tear_frequency,
                              params.tear_detail, boil_seed)
              : 0.0f;
      const float edge_sdf = base_edge_dist - tear_amp_px * tear_n;
      const float sheet_alpha = Smoothstep(1.2f, -1.2f, edge_sdf);

      // Evaluate soft 3D drop shadow behind paper cutout without re-evaluating
      // multi-octave crumple displacement.
      float shadow_alpha = 0.0f;
      if (params.shadow_opacity > 1e-4f) {
        float sux = 0.0f;
        float suy = 0.0f;
        float unused_crease = 0.0f;
        MapOutputToPaperCanonical(params, anim, cw, ch, origin_x, origin_y,
                                  px_out - sh_dx_px, py_out - sh_dy_px, false,
                                  &sux, &suy, &unused_crease);
        const float s_dist_px = sdf.SamplePx(sux, suy) - border_px;
        shadow_alpha =
            params.shadow_opacity * Smoothstep(sh_soft_px, -2.0f, s_dist_px);
      }

      const RgbaColor shadow_rgba = {0.0f, 0.0f, 0.0f, shadow_alpha};
      if (sheet_alpha <= 1e-4f) {
        WritePixel(output, x, y, shadow_rgba);
        continue;
      }

      // Compute cardstock fiber grain & fold crease shading.
      const float grain =
          Lerp(1.0f, PaperFiberGrain(nu_x, nu_y, params.grain_scale, seed),
               Clamp01(params.grain_strength));
      const float crease_val =
          PaperCreaseField(nu_x, nu_y, params.crease_scale, seed + 89u);
      const float crease_mul =
          1.0f - 0.34f * Clamp01(params.crease_strength) * crease_val;
      const float rim_bright =
          1.0f + 0.08f * Smoothstep(3.0f, 0.0f, std::abs(edge_sdf));
      const float shade =
          Clamp(grain * crease_mul * rim_bright * (1.0f - 0.35f * anim_crease),
                0.25f, 1.15f);

      const RgbaColor paper_fg = {
          Clamp01(params.paper_r * shade) * sheet_alpha,
          Clamp01(params.paper_g * shade) * sheet_alpha,
          Clamp01(params.paper_b * shade) * sheet_alpha,
          sheet_alpha,
      };

      // Evaluate full-res subject over textured cardstock collar.
      RgbaColor subject =
          EvaluateSubjectAtCanonicalPx(params, subj_xf, src_view, ux, uy);
      if (std::abs(params.edge_choke_px) > 0.01f && subject.a > 1e-4f) {
        const float choke_mask = Smoothstep(
            params.edge_choke_px + 1.0f, params.edge_choke_px - 1.0f, dist_px);
        const float new_a = Clamp01(subject.a * choke_mask);
        const float scale_a = new_a / subject.a;
        subject = {subject.r * scale_a, subject.g * scale_a,
                   subject.b * scale_a, new_a};
      }

      // Apply printed cardstock crease texture subtly onto the subject ink.
      if (params.media_texture_overlay > 1e-4f && subject.a > 1e-4f) {
        const float ink_mul = Lerp(1.0f, grain * crease_mul,
                                   Clamp01(params.media_texture_overlay));
        subject.r *= ink_mul;
        subject.g *= ink_mul;
        subject.b *= ink_mul;
      }

      const RgbaColor cutout_fg =
          params.enable_border
              ? CompositeOver(subject, paper_fg)
              : RgbaColor{subject.r * sheet_alpha, subject.g * sheet_alpha,
                          subject.b * sheet_alpha, subject.a * sheet_alpha};

      WritePixel(output, x, y, CompositeOver(cutout_fg, shadow_rgba));
    }
  });
}

void RenderTransitionFrame(const TransitionParams& params,
                           const ImageView& source_from,
                           const ImageView& source_to, const ImageView& output,
                           const PixelRect& canvas_bounds,
                           const PixelRect& render_window,
                           const ParallelRowsFn& parallel_for) {
  if (!IsValidRgbaView(output)) {
    return;
  }
  const int y_begin = std::max(render_window.y1, output.bounds.y1);
  const int y_end = std::min(render_window.y2, output.bounds.y2);
  const int x_begin = std::max(render_window.x1, output.bounds.x1);
  const int x_end = std::min(render_window.x2, output.bounds.x2);
  if (x_begin >= x_end || y_begin >= y_end) {
    return;
  }

  const float p = ComputeEffectiveTransitionProgress(params);
  auto copy_clip = [&](const ImageView& clip) {
    RunScanlines(y_begin, y_end, parallel_for, [&](int y) {
      for (int x = x_begin; x < x_end; ++x) {
        if (IsValidRgbaView(clip) && x >= clip.bounds.x1 &&
            x < clip.bounds.x2 && y >= clip.bounds.y1 && y < clip.bounds.y2) {
          WritePixel(output, x, y, ReadPixel(clip, x, y));
        } else {
          WritePixel(output, x, y, {});
        }
      }
    });
  };

  if (p <= 1e-5f) {
    copy_clip(source_from);
    return;
  }
  if (p >= 1.0f - 1e-5f) {
    copy_clip(source_to);
    return;
  }

  const float cw =
      static_cast<float>(std::max(1, canvas_bounds.x2 - canvas_bounds.x1));
  const float ch =
      static_cast<float>(std::max(1, canvas_bounds.y2 - canvas_bounds.y1));
  const float min_dim = std::min(cw, ch);
  const float origin_x = static_cast<float>(canvas_bounds.x1);
  const float origin_y = static_cast<float>(canvas_bounds.y1);
  const float mh = params.mirror_h ? -1.0f : 1.0f;
  const float mv = params.mirror_v ? -1.0f : 1.0f;
  const std::uint32_t seed =
      static_cast<std::uint32_t>(std::max(0, params.random_seed));

  auto to_clip_coords = [&](const ImageView& clip, float nx,
                            float ny) -> std::pair<float, float> {
    if (!IsValidRgbaView(clip)) {
      return {0.0f, 0.0f};
    }
    const PixelRect clip_rod = EffectiveRod(clip);
    const float sw = static_cast<float>(std::max(1, clip_rod.x2 - clip_rod.x1));
    const float sh = static_cast<float>(std::max(1, clip_rod.y2 - clip_rod.y1));
    return {static_cast<float>(clip_rod.x1) + nx * sw,
            static_cast<float>(clip_rod.y1) + ny * sh};
  };

  if (params.kind == TransitionKind::kUnfold) {
    const float inv_p = 1.0f - p;
    const float tilt_cos = std::max(0.05f, std::cos(inv_p * 1.48f));
    const float persp_k = mh * 0.45f * std::sin(inv_p * 1.48f);
    const float tilt_y_shift =
        mv * std::tan(Clamp(params.fold_tilt_deg, -60.0f, 60.0f) * kDegToRad) *
        0.25f * inv_p;
    const float slide_x = mh * params.slide_distance * inv_p;
    const float shadow_soft_px =
        std::max(2.0f, params.shadow_softness_norm * min_dim);

    RunScanlines(y_begin, y_end, parallel_for, [&](int y) {
      const float ny = (static_cast<float>(y) + 0.5f - origin_y) / ch;
      for (int x = x_begin; x < x_end; ++x) {
        const float nx = (static_cast<float>(x) + 0.5f - origin_x) / cw;
        const auto [fx, fy] = to_clip_coords(source_from, nx, ny);
        const RgbaColor bg = SampleBilinearClamped(source_from, fx, fy);

        const float dx = nx - 0.5f - slide_x;
        const float dy = ny - 0.5f - tilt_y_shift * dx;
        const float w_p = std::max(0.20f, 1.0f + persp_k * dx);
        const float ux = 0.5f + dx / (tilt_cos * w_p);
        const float uy = 0.5f + dy / w_p;

        const float in_sheet_dist =
            std::max(std::abs(ux - 0.5f) - 0.5f, std::abs(uy - 0.5f) - 0.5f);
        const float sheet_alpha =
            Smoothstep(1.5f / min_dim, -1.5f / min_dim, in_sheet_dist);
        const float shadow_alpha =
            params.shadow_opacity * (1.0f - p * p * p) *
            Smoothstep(shadow_soft_px / min_dim, -2.0f / min_dim,
                       in_sheet_dist - 0.012f);

        const RgbaColor shadowed_bg = {
            bg.r * (1.0f - shadow_alpha),
            bg.g * (1.0f - shadow_alpha),
            bg.b * (1.0f - shadow_alpha),
            bg.a,
        };

        if (sheet_alpha <= 1e-4f) {
          WritePixel(output, x, y, shadowed_bg);
          continue;
        }

        const auto [tx, ty] = to_clip_coords(source_to, ux, uy);
        RgbaColor fg = SampleBilinearClamped(source_to, tx, ty);
        const float crease =
            PaperCreaseField(ux * (cw / min_dim), uy * (ch / min_dim),
                             params.crease_scale, seed + 41u);
        const float hinge_fold =
            1.0f - 0.25f * inv_p * std::abs(std::sin(ux * kPi * 3.0f));
        const float c_mul =
            hinge_fold *
            (1.0f - 0.35f * params.crease_intensity * inv_p * crease);
        const float out_fg_a = Clamp01(fg.a) * sheet_alpha;
        fg.r = Clamp(fg.r * c_mul * sheet_alpha, 0.0f, out_fg_a);
        fg.g = Clamp(fg.g * c_mul * sheet_alpha, 0.0f, out_fg_a);
        fg.b = Clamp(fg.b * c_mul * sheet_alpha, 0.0f, out_fg_a);
        fg.a = out_fg_a;
        WritePixel(output, x, y, CompositeOver(fg, shadowed_bg));
      }
    });
    return;
  }

  if (params.kind == TransitionKind::kCrumple) {
    const bool first_half = (p < 0.5f);
    const float phase = first_half ? (p * 2.0f) : ((p - 0.5f) * 2.0f);
    const float crumple_amt = first_half ? phase : (1.0f - phase);
    const float scale =
        first_half ? Lerp(1.0f, 0.14f, phase) : Lerp(0.14f, 1.0f, phase);
    const float spin_rad = mh * mv * params.spin_angle_deg * crumple_amt *
                           kDegToRad * (first_half ? 1.0f : -1.0f);
    const float cos_s = std::cos(-spin_rad);
    const float sin_s = std::sin(-spin_rad);
    const ImageView& active_clip = first_half ? source_from : source_to;
    const float mid_fade = std::sin(p * kPi);
    const float sh_rad = -45.0f * kDegToRad;
    const float sh_dx = mh * 0.025f * std::cos(sh_rad);
    const float sh_dy = mv * 0.025f * std::sin(sh_rad);
    const float shadow_soft = std::max(0.01f, params.shadow_softness_norm);

    RunScanlines(y_begin, y_end, parallel_for, [&](int y) {
      const float ny = (static_cast<float>(y) + 0.5f - origin_y) / ch;
      for (int x = x_begin; x < x_end; ++x) {
        const float nx = (static_cast<float>(x) + 0.5f - origin_x) / cw;

        auto eval_sheet_uv = [&](float px_n, float py_n, bool apply_crumple,
                                 float* ux_out, float* uy_out,
                                 float* crease_out) {
          const float cx = (px_n - 0.5f) / scale;
          const float cy = (py_n - 0.5f) / scale;
          float rx = 0.5f + cx * cos_s - cy * sin_s;
          float ry = 0.5f + cx * sin_s + cy * cos_s;
          if (apply_crumple) {
            float gdx = 0.0f;
            float gdy = 0.0f;
            const float sample_x = 0.5f + (rx - 0.5f) * mh;
            const float sample_y = 0.5f + (ry - 0.5f) * mv;
            CrumpleDisplacement(sample_x, sample_y, params.crease_scale,
                                seed + 131u, &gdx, &gdy, crease_out);
            rx += mh * gdx * params.crumple_intensity * crumple_amt * 0.045f;
            ry += mv * gdy * params.crumple_intensity * crumple_amt * 0.045f;
          }
          *ux_out = rx;
          *uy_out = ry;
        };

        float ux = 0.0f;
        float uy = 0.0f;
        float crease = 0.0f;
        eval_sheet_uv(nx, ny, true, &ux, &uy, &crease);
        const float radial_noise =
            0.05f * crumple_amt *
            TearEdgeNoise(0.5f + (ux - 0.5f) * mh, 0.5f + (uy - 0.5f) * mv,
                          8.0f, 3, seed + 221u);
        const float box_dist =
            std::max(std::abs(ux - 0.5f), std::abs(uy - 0.5f)) - 0.5f +
            radial_noise;
        const float sheet_a =
            Smoothstep(2.0f / min_dim, -2.0f / min_dim, box_dist);

        float shadow_a = 0.0f;
        if (params.shadow_opacity > 1e-4f) {
          float sux = 0.0f;
          float suy = 0.0f;
          float unused_c = 0.0f;
          eval_sheet_uv(nx - sh_dx, ny - sh_dy, false, &sux, &suy, &unused_c);
          const float s_dist =
              std::max(std::abs(sux - 0.5f), std::abs(suy - 0.5f)) - 0.5f;
          if (s_dist < shadow_soft) {
            const auto [sax, say] = to_clip_coords(active_clip, sux, suy);
            const float src_shadow_a =
                Clamp01(SampleBilinearClamped(active_clip, sax, say).a);
            shadow_a = params.shadow_opacity * mid_fade * src_shadow_a *
                       Smoothstep(shadow_soft, -0.01f, s_dist);
          }
        }

        const RgbaColor shadow_rgba = {0.0f, 0.0f, 0.0f, shadow_a};
        if (sheet_a <= 1e-4f) {
          WritePixel(output, x, y, shadow_rgba);
          continue;
        }

        const auto [ax, ay] = to_clip_coords(active_clip, ux, uy);
        RgbaColor fg = SampleBilinearClamped(active_clip, ax, ay);
        const float fold_dark =
            1.0f - 0.45f * params.crease_intensity * mid_fade * crease;
        const float out_fg_a = Clamp01(fg.a) * sheet_a;
        fg.r = Clamp(fg.r * fold_dark * sheet_a, 0.0f, out_fg_a);
        fg.g = Clamp(fg.g * fold_dark * sheet_a, 0.0f, out_fg_a);
        fg.b = Clamp(fg.b * fold_dark * sheet_a, 0.0f, out_fg_a);
        fg.a = out_fg_a;
        WritePixel(output, x, y, CompositeOver(fg, shadow_rgba));
      }
    });
    return;
  }

  // TransitionKind::kRipWipe — jagged torn-paper edge reveal with cardstock
  // rim.
  const float rad = params.rip_angle_deg * kDegToRad;
  const float dir_x = mh * std::cos(rad);
  const float dir_y = mv * std::sin(rad);
  const float half_proj = 0.5f * (std::abs(dir_x) + std::abs(dir_y));
  const float tear_w = std::max(0.0f, params.tear_width_norm);
  const float tear_amp = std::max(0.0f, params.tear_roughness_norm);
  const float shadow_pad = std::max(0.01f, params.shadow_softness_norm);
  const float sweep_margin = tear_w + tear_amp + shadow_pad + 0.04f;
  const float front_pos =
      Lerp(-half_proj - sweep_margin, half_proj + sweep_margin, p);
  const float aa_px = 1.5f / min_dim;

  RunScanlines(y_begin, y_end, parallel_for, [&](int y) {
    const float ny = (static_cast<float>(y) + 0.5f - origin_y) / ch;
    for (int x = x_begin; x < x_end; ++x) {
      const float nx = (static_cast<float>(x) + 0.5f - origin_x) / cw;
      const float proj = (nx - 0.5f) * dir_x + (ny - 0.5f) * dir_y;
      const float base_dist = proj - front_pos;

      // Narrow-band early-outs skip multi-octave tear/grain noise and unused
      // source fetches outside the rip transition band.
      if (base_dist - tear_amp >= std::max(shadow_pad, aa_px)) {
        const auto [fx, fy] = to_clip_coords(source_from, nx, ny);
        WritePixel(output, x, y, SampleBilinearClamped(source_from, fx, fy));
        continue;
      }
      if (base_dist + tear_amp <= -tear_w - aa_px) {
        const auto [tx, ty] = to_clip_coords(source_to, nx, ny);
        WritePixel(output, x, y, SampleBilinearClamped(source_to, tx, ty));
        continue;
      }

      const float n = TearEdgeNoise(nx * (cw / min_dim), ny * (ch / min_dim),
                                    params.tear_frequency, 6, seed + 307u);
      const float torn_dist = base_dist + tear_amp * n;

      const auto [fx, fy] = to_clip_coords(source_from, nx, ny);
      const RgbaColor clip_a = SampleBilinearClamped(source_from, fx, fy);
      const float sh_a =
          params.shadow_opacity * Smoothstep(shadow_pad, 0.0f, torn_dist);
      const RgbaColor bg_shadowed = {
          clip_a.r * (1.0f - sh_a),
          clip_a.g * (1.0f - sh_a),
          clip_a.b * (1.0f - sh_a),
          clip_a.a,
      };

      const float outer_mask = Smoothstep(aa_px, -aa_px, torn_dist);
      if (outer_mask <= 1e-4f) {
        WritePixel(output, x, y, bg_shadowed);
        continue;
      }

      const auto [tx, ty] = to_clip_coords(source_to, nx, ny);
      const RgbaColor clip_b = SampleBilinearClamped(source_to, tx, ty);

      // Torn white/cream paper fiber rim in [-tear_w, 0], incoming clip B for
      // < -tear_w.
      const float grain =
          PaperFiberGrain(nx * (cw / min_dim), ny * (ch / min_dim), 1.0f, seed);
      const float rim_shade =
          grain * (0.88f + 0.14f * Smoothstep(0.0f, -tear_w * 0.5f, torn_dist));
      const RgbaColor paper_col = {
          Clamp01(params.paper_r * rim_shade),
          Clamp01(params.paper_g * rim_shade),
          Clamp01(params.paper_b * rim_shade),
          1.0f,
      };

      const float inner_b_mask =
          Smoothstep(-tear_w + aa_px, -tear_w - aa_px, torn_dist);
      const float paper_w = outer_mask * (1.0f - inner_b_mask);
      const float clip_b_w = outer_mask * inner_b_mask;
      const float bg_w = 1.0f - outer_mask;
      WritePixel(
          output, x, y,
          {
              paper_col.r * paper_w + clip_b.r * clip_b_w +
                  bg_shadowed.r * bg_w,
              paper_col.g * paper_w + clip_b.g * clip_b_w +
                  bg_shadowed.g * bg_w,
              paper_col.b * paper_w + clip_b.b * clip_b_w +
                  bg_shadowed.b * bg_w,
              Clamp01(paper_w + clip_b.a * clip_b_w + bg_shadowed.a * bg_w),
          });
    }
  });
}

}  // namespace papercraft
