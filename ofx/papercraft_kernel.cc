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

  // La Matatena purple outer badge (#8367c7), deep purple ring (#6a4fb3),
  // warm paper core, and 6-point Matatena jack emblem.
  RgbaColor col = {0.514f, 0.404f, 0.780f, 1.0f};
  const float ring_mask = Smoothstep(0.185f, 0.178f, r);
  col = LerpColor(col, {0.416f, 0.310f, 0.702f, 1.0f}, ring_mask);

  const float inner_disk = Smoothstep(0.145f, 0.138f, r);
  col = LerpColor(col, {0.97f, 0.95f, 0.99f, 1.0f}, inner_disk);

  const float star_r = 0.075f + 0.032f * std::cos(angle * 6.0f);
  const float star_mask = Smoothstep(star_r + 0.004f, star_r - 0.004f, r);
  col = LerpColor(col, {0.514f, 0.404f, 0.780f, 1.0f}, star_mask);

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
  float raw_p = 1.0f;   // Stepped linear progress, 1.0 = resting.
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
    state.raw_p = raw_p;
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

// ---------------------------------------------------------------------------
// Paper fold & unfold machinery, shared by the Paper Fold & Unfold transition
// and the Paper Unfold (3D) intro/outro of Paper Cutout FX.
//
// The sheet folds along up to kMaxFoldHinges straight hinges. Hinge k folds the
// packet left by hinges 0..k-1, so rendering inverse-maps every output pixel
// from the outermost hinge (the last fold made, the first to open) down to the
// flat sheet. All hinges keep one shared core point on their stationary side;
// that part never moves, so hinge k reads the same in flat and folded space.
// ---------------------------------------------------------------------------

float Square(float v) { return v * v; }

// Runs rows in a strided order so each worker's contiguous slice of indices
// samples the whole frame: fold renders cost far more on rows that cross the
// paper, which static per-thread slices would leave on a few threads.
void RunScanlinesInterleaved(int y_begin, int y_end,
                             const ParallelRowsFn& parallel_for,
                             const std::function<void(int y)>& row_fn) {
  const int rows = y_end - y_begin;
  constexpr int kStride = 7919;  // Prime, coprime with any rows < kStride.
  if (rows <= 1 || rows % kStride == 0) {
    RunScanlines(y_begin, y_end, parallel_for, row_fn);
    return;
  }
  RunScanlines(0, rows, parallel_for, [&](int i) {
    row_fn(y_begin + static_cast<int>(
                         (static_cast<std::int64_t>(i) * kStride) % rows));
  });
}

RgbaColor ScalePremult(const RgbaColor& c, float s) {
  return {c.r * s, c.g * s, c.b * s, c.a * s};
}

// Samples a clip at canonical canvas pixel (px, py). Same-size clips map 1:1,
// so unwarped frames reproduce the clip exactly.
RgbaColor SampleClipCanonicalPx(const ImageView& view,
                                const PixelRect& canvas_bounds, float px,
                                float py) {
  if (!IsValidRgbaView(view)) {
    return {};
  }
  const PixelRect rod = EffectiveRod(view);
  if (rod.x1 == canvas_bounds.x1 && rod.x2 == canvas_bounds.x2 &&
      rod.y1 == canvas_bounds.y1 && rod.y2 == canvas_bounds.y2) {
    return SampleBilinearTransparentOutside(view, px, py);
  }
  const float cw =
      static_cast<float>(std::max(1, canvas_bounds.x2 - canvas_bounds.x1));
  const float ch =
      static_cast<float>(std::max(1, canvas_bounds.y2 - canvas_bounds.y1));
  const float sx = static_cast<float>(rod.x1) +
                   (px - static_cast<float>(canvas_bounds.x1)) / cw *
                       static_cast<float>(rod.x2 - rod.x1);
  const float sy = static_cast<float>(rod.y1) +
                   (py - static_cast<float>(canvas_bounds.y1)) / ch *
                       static_cast<float>(rod.y2 - rod.y1);
  return SampleBilinearTransparentOutside(view, sx, sy);
}

// The clip under canvas pixel (x, y). Same-size clips are read directly,
// which equals the bilinear sample at the pixel center but costs one fetch.
RgbaColor ClipPixelAt(const ImageView& view, const PixelRect& canvas_bounds,
                      int x, int y) {
  if (!IsValidRgbaView(view)) {
    return {};
  }
  const PixelRect rod = EffectiveRod(view);
  if (rod.x1 == canvas_bounds.x1 && rod.x2 == canvas_bounds.x2 &&
      rod.y1 == canvas_bounds.y1 && rod.y2 == canvas_bounds.y2) {
    if (x < view.bounds.x1 || x >= view.bounds.x2 || y < view.bounds.y1 ||
        y >= view.bounds.y2) {
      return {};
    }
    return ReadPixel(view, x, y);
  }
  return SampleClipCanonicalPx(view, canvas_bounds,
                               static_cast<float>(x) + 0.5f,
                               static_cast<float>(y) + 0.5f);
}

struct ClipContentSummary {
  bool has_visible_alpha = false;
  bool has_nonblack_rgb = false;
  bool is_full_frame_opaque = false;
};

// Scans a strided grid (about 360 rows) dense enough to catch small logos, so
// the Auto action can tell real media from the empty side of a clip edge.
ClipContentSummary InspectClipContent(const ImageView& view) {
  ClipContentSummary out;
  if (!IsValidRgbaView(view)) {
    return out;
  }
  const PixelRect rod = EffectiveRod(view);
  bool opaque = view.bounds.x1 <= rod.x1 && view.bounds.x2 >= rod.x2 &&
                view.bounds.y1 <= rod.y1 && view.bounds.y2 >= rod.y2;
  const int w = view.bounds.x2 - view.bounds.x1;
  const int h = view.bounds.y2 - view.bounds.y1;
  const int stride = std::max(1, std::min(w, h) / 360);
  for (int y = view.bounds.y1 + stride / 2; y < view.bounds.y2; y += stride) {
    for (int x = view.bounds.x1 + stride / 2; x < view.bounds.x2;
         x += stride) {
      const RgbaColor c = ReadPixel(view, x, y);
      if (c.a > 1e-3f) {
        out.has_visible_alpha = true;
      }
      if (c.r + c.g + c.b > 3e-3f) {
        out.has_nonblack_rgb = true;
      }
      if (c.a < 0.995f) {
        opaque = false;
      }
      if (out.has_visible_alpha && out.has_nonblack_rgb && !opaque) {
        return out;
      }
    }
  }
  out.is_full_frame_opaque = opaque;
  return out;
}

// Shape of the paper sheet before folding, in canonical canvas pixels.
struct SheetGeometry {
  bool valid = false;
  bool analytic_box = false;
  const CoarseSignedDistanceField* sdf = nullptr;
  float cx = 0.0f;  // Bounding-box center.
  float cy = 0.0f;
  float half_w = 1.0f;
  float half_h = 1.0f;
  // Matte outline relative to (cx, cy), grown by the collar; empty for boxes.
  std::vector<std::array<float, 2>> outline;
  float outline_grow = 0.0f;

  // Signed distance to the matte edge (negative inside).
  float DistPx(float ux, float uy) const {
    if (analytic_box) {
      const float dx = std::abs(ux - cx) - half_w;
      const float dy = std::abs(uy - cy) - half_h;
      // Same values as the usual box SDF; hypot only runs near the corners.
      if (dx <= 0.0f || dy <= 0.0f) {
        return std::max(dx, dy);
      }
      return std::hypot(dx, dy);
    }
    return sdf != nullptr ? sdf->SamplePx(ux, uy) : 1e6f;
  }

  // Farthest extent of the paper along unit direction (ux, uy), so hinges
  // cut into round or irregular cutouts instead of their empty bbox corners.
  float Support(float ux, float uy) const {
    if (outline.empty()) {
      return half_w * std::abs(ux) + half_h * std::abs(uy);
    }
    float best = -1e9f;
    for (const auto& p : outline) {
      best = std::max(best, p[0] * ux + p[1] * uy);
    }
    return best + outline_grow;
  }
};

SheetGeometry FullFrameSheet(const PixelRect& canvas_bounds) {
  SheetGeometry g;
  g.valid = true;
  g.analytic_box = true;
  g.cx = 0.5f * static_cast<float>(canvas_bounds.x1 + canvas_bounds.x2);
  g.cy = 0.5f * static_cast<float>(canvas_bounds.y1 + canvas_bounds.y2);
  // 1.5 px of overscan keeps frame-edge pixels fully opaque once flat.
  g.half_w = 0.5f * static_cast<float>(canvas_bounds.x2 - canvas_bounds.x1) +
             1.5f;
  g.half_h = 0.5f * static_cast<float>(canvas_bounds.y2 - canvas_bounds.y1) +
             1.5f;
  return g;
}

// Bounding box of the matte, grown by `grow_px` for the paper collar.
SheetGeometry SheetFromSdf(const CoarseSignedDistanceField& sdf,
                           float grow_px) {
  SheetGeometry g;
  g.sdf = &sdf;
  int min_gx = sdf.width;
  int max_gx = -1;
  int min_gy = sdf.height;
  int max_gy = -1;
  for (int gy = 0; gy < sdf.height; ++gy) {
    const float* row =
        sdf.signed_dist_px.data() + static_cast<std::size_t>(gy) * sdf.width;
    for (int gx = 0; gx < sdf.width; ++gx) {
      if (row[gx] <= 0.0f) {
        min_gx = std::min(min_gx, gx);
        max_gx = std::max(max_gx, gx);
        min_gy = std::min(min_gy, gy);
        max_gy = std::max(max_gy, gy);
      }
    }
  }
  if (max_gx < 0) {
    return g;
  }
  const float step = static_cast<float>(std::max(1, sdf.step));
  const float x1 = sdf.origin_x + static_cast<float>(min_gx) * step - grow_px;
  const float x2 =
      sdf.origin_x + static_cast<float>(max_gx + 1) * step + grow_px;
  const float y1 = sdf.origin_y + static_cast<float>(min_gy) * step - grow_px;
  const float y2 =
      sdf.origin_y + static_cast<float>(max_gy + 1) * step + grow_px;
  g.valid = true;
  g.cx = 0.5f * (x1 + x2);
  g.cy = 0.5f * (y1 + y2);
  g.half_w = std::max(16.0f, 0.5f * (x2 - x1));
  g.half_h = std::max(16.0f, 0.5f * (y2 - y1));

  // Matte edge cells (and cells on the frame border, where the paper ends at
  // the frame), thinned to a few thousand points for Support().
  const int stride = std::max(1, std::max(sdf.width, sdf.height) / 240);
  const float band = -2.0f * step * static_cast<float>(stride);
  for (int gy = min_gy; gy <= max_gy; gy += stride) {
    const float* row =
        sdf.signed_dist_px.data() + static_cast<std::size_t>(gy) * sdf.width;
    const bool border_row = gy < stride || gy >= sdf.height - stride;
    for (int gx = min_gx; gx <= max_gx; gx += stride) {
      const bool border = border_row || gx < stride || gx >= sdf.width - stride;
      if (row[gx] <= 0.0f && (border || row[gx] > band)) {
        g.outline.push_back(
            {sdf.origin_x + (static_cast<float>(gx) + 0.5f) * step - g.cx,
             sdf.origin_y + (static_cast<float>(gy) + 0.5f) * step - g.cy});
      }
    }
  }
  g.outline_grow = grow_px + 0.5f * step;
  return g;
}

constexpr int kMaxFoldHinges = 5;

struct FoldHinge {
  float px = 0.0f;  // Foot point on the hinge line, sheet-local px.
  float py = 0.0f;
  float nx = 1.0f;  // Unit normal pointing into the moving flap.
  float ny = 0.0f;
  float tx = 0.0f;  // Unit tangent along the hinge line.
  float ty = 1.0f;
  float phi = 0.0f;  // Fold angle: 0 = flat, near pi = folded over.
  float cos_phi = 1.0f;
  float sin_phi = 0.0f;
  bool valley = true;  // Valley flaps lift toward the viewer, mountain away.
};

struct FoldLayout {
  std::array<FoldHinge, kMaxFoldHinges> hinges;
  int count = 0;
  float persp_k = 0.0f;  // Tangential magnification per px of lift.
  // Motion of the whole folded packet, applied after the hinges.
  float offset_x = 0.0f;  // Slide offset, canvas px.
  float offset_y = 0.0f;
  float rot_cos = 1.0f;
  float rot_sin = 0.0f;
  float scale = 1.0f;
  float alpha = 1.0f;
  float lift = 0.0f;  // 1 while the packet is in the air (sliding in or out).
};

struct HingeTemplate {
  float deg;      // Normal direction relative to the fold direction.
  float frac;     // Hinge position from the core (0) to the sheet edge (1).
  float max_deg;  // Folded angle; staying under 180 keeps a sliver of depth.
  bool valley;
};

// Builds the hinges and packet motion for `openness` in [0, 1]: 0 = folded
// packet out of frame (or invisible), 1 = flat sheet at rest. `seed` adds a
// small hand-folded irregularity to hinge angles and positions.
FoldLayout BuildFoldLayout(FoldPattern pattern, int requested_stages,
                           float openness, const SheetGeometry& sheet,
                           float frame_diag, float direction_deg, float depth,
                           float slide_distance, bool mirror_h, bool mirror_v,
                           std::uint32_t seed) {
  FoldLayout layout;
  const int stages = std::min(kMaxFoldHinges, std::max(2, requested_stages));
  layout.count = stages;
  const float q = Clamp01(openness);
  const float mh = mirror_h ? -1.0f : 1.0f;
  const float mv = mirror_v ? -1.0f : 1.0f;
  const float rx = std::max(16.0f, sheet.half_w);
  const float ry = std::max(16.0f, sheet.half_h);
  layout.persp_k =
      0.30f * Clamp(depth, 0.0f, 2.0f) / std::max(64.0f, std::hypot(rx, ry));

  auto unit_at = [&](float deg, float* ux, float* uy) {
    const float rad = (deg + direction_deg) * kDegToRad;
    *ux = mh * std::cos(rad);
    *uy = mv * std::sin(rad);
  };
  auto support = [&](float ux, float uy) {
    return std::max(8.0f, sheet.Support(ux, uy));
  };

  // Entries are listed in folding order; the last ones open first, so fewer
  // stages keep the big half folds and drop the small corner flaps.
  std::array<HingeTemplate, kMaxFoldHinges> tpl{};
  float core_n = -0.10f;  // Core offset along the fold direction, x support.
  float core_t = -0.10f;  // Core offset across it, x support.
  auto take_last = [&](const HingeTemplate* all) {
    for (int k = 0; k < stages; ++k) {
      tpl[k] = all[kMaxFoldHinges - stages + k];
    }
  };
  switch (pattern) {
    case FoldPattern::kMapGrid: {
      constexpr HingeTemplate kMap[kMaxFoldHinges] = {
          {-90.0f, 0.62f, 168.0f, true}, {180.0f, 0.60f, 169.0f, true},
          {0.0f, 0.62f, 170.0f, true},   {90.0f, 0.05f, 174.0f, true},
          {0.0f, 0.05f, 176.0f, true}};
      take_last(kMap);
      break;
    }
    case FoldPattern::kAccordion:
      // Hinge positions are computed below; only angles and parity here.
      for (int k = 0; k < stages; ++k) {
        tpl[k] = {0.0f, 0.0f, 172.0f, (stages - 1 - k) % 2 == 0};
      }
      core_n = -0.68f;
      core_t = 0.0f;
      break;
    case FoldPattern::kOrigamiCorners: {
      constexpr HingeTemplate kCorners[4] = {{45.0f, 0.48f, 164.0f, true},
                                             {135.0f, 0.50f, 165.0f, true},
                                             {-135.0f, 0.50f, 166.0f, true},
                                             {-45.0f, 0.52f, 167.0f, true}};
      for (int k = 0; k + 1 < stages; ++k) {
        tpl[k] = kCorners[k];
      }
      tpl[stages - 1] = {8.0f, 0.05f, 175.0f, true};
      core_n = -0.06f;
      core_t = -0.05f;
      break;
    }
    case FoldPattern::kCraftFlaps:
    default: {
      // Quarter fold that opens first, then dog-eared corners flip open.
      constexpr HingeTemplate kCraft[kMaxFoldHinges] = {
          {140.0f, 0.74f, 158.0f, true}, {-40.0f, 0.70f, 160.0f, true},
          {35.0f, 0.72f, 162.0f, true},  {96.0f, 0.05f, 172.0f, true},
          {6.0f, 0.05f, 176.0f, true}};
      take_last(kCraft);
      break;
    }
  }
  if (pattern != FoldPattern::kAccordion) {
    for (int k = 0; k < stages; ++k) {
      tpl[k].deg += (Hash2D(k, 1, seed) - 0.5f) * 8.0f;
      tpl[k].frac = std::max(0.02f, tpl[k].frac +
                                        (Hash2D(k, 2, seed) - 0.5f) * 0.06f);
    }
  }

  float dir_x = 1.0f;
  float dir_y = 0.0f;
  float across_x = 0.0f;
  float across_y = 1.0f;
  unit_at(0.0f, &dir_x, &dir_y);
  unit_at(90.0f, &across_x, &across_y);
  const float core_x = core_n * support(dir_x, dir_y) * dir_x +
                       core_t * support(across_x, across_y) * across_x;
  const float core_y = core_n * support(dir_x, dir_y) * dir_y +
                       core_t * support(across_x, across_y) * across_y;

  // Folds open over q in [kFoldStart, 1], one stage after another.
  constexpr float kFoldStart = 0.10f;
  constexpr float kOverlap = 0.22f;
  const float u = Clamp01((q - kFoldStart) / (1.0f - kFoldStart));
  const float span = 1.0f / (static_cast<float>(stages) -
                             kOverlap * static_cast<float>(stages - 1));
  for (int k = 0; k < stages; ++k) {
    FoldHinge& h = layout.hinges[k];
    unit_at(tpl[k].deg, &h.nx, &h.ny);
    h.tx = -h.ny;
    h.ty = h.nx;
    h.valley = tpl[k].valley;
    float dist = 0.0f;
    if (pattern == FoldPattern::kAccordion) {
      // Equal strips; hinge k = 0 is the farthest from the core.
      const float sup = support(h.nx, h.ny);
      const float strip = 2.0f * sup / static_cast<float>(stages + 1);
      const float along = -sup + static_cast<float>(stages - k) * strip;
      dist = along - (core_x * h.nx + core_y * h.ny);
    } else {
      const float reach = std::max(
          12.0f, support(h.nx, h.ny) - (core_x * h.nx + core_y * h.ny));
      dist = tpl[k].frac * reach;
    }
    h.px = core_x + dist * h.nx;
    h.py = core_y + dist * h.ny;

    // Hinge stages-1 opens first, hinge 0 last.
    const float start = static_cast<float>(stages - 1 - k) * span *
                        (1.0f - kOverlap);
    const float local = Clamp01((u - start) / span);
    const float ease = local * local * (3.0f - 2.0f * local);
    h.phi = tpl[k].max_deg * kDegToRad * (1.0f - ease);
    h.cos_phi = std::cos(h.phi);
    h.sin_phi = std::sin(h.phi);
  }

  // Packet slides in (or out) over q in [0, kTossEnd] while it is folded.
  constexpr float kTossEnd = 0.18f;
  const float toss = Square(1.0f - Smoothstep(0.0f, kTossEnd, q));
  layout.lift = toss;
  layout.alpha = Smoothstep(0.0f, 0.02f, q);
  layout.scale = 1.0f + 0.12f * toss;
  const float toss_rad = (direction_deg - 120.0f) * kDegToRad;
  const float slide = std::max(0.0f, slide_distance) * frame_diag * toss;
  const float rot = mh * mv * 24.0f * kDegToRad * toss;
  layout.rot_cos = std::cos(rot);
  layout.rot_sin = std::sin(rot);
  layout.offset_x = mh * std::cos(toss_rad) * slide;
  layout.offset_y = mv * std::sin(toss_rad) * slide;
  return layout;
}

// Axis-aligned bounds. Unbounded by default, so fold evaluations whose
// caller never computes bounds lose no paper.
struct BoundsBox {
  float x1 = -1e30f;
  float y1 = -1e30f;
  float x2 = 1e30f;
  float y2 = 1e30f;

  bool Contains(float x, float y) const {
    return x >= x1 && x <= x2 && y >= y1 && y <= y2;
  }
};

using FoldPolygon = std::vector<std::array<float, 2>>;

// Bounds of every piece vertex mapped by `map`, padded by `pad`; empty (no
// point inside) when there are no pieces.
template <typename MapFn>
BoundsBox BoundsOfPieces(const std::vector<FoldPolygon>& pieces, float pad,
                         const MapFn& map) {
  BoundsBox box;
  std::swap(box.x1, box.x2);
  std::swap(box.y1, box.y2);
  for (const FoldPolygon& piece : pieces) {
    for (const auto& v : piece) {
      float x = 0.0f;
      float y = 0.0f;
      map(v[0], v[1], &x, &y);
      box.x1 = std::min(box.x1, x - pad);
      box.x2 = std::max(box.x2, x + pad);
      box.y1 = std::min(box.y1, y - pad);
      box.y2 = std::max(box.y2, y + pad);
    }
  }
  return box;
}

// Part of convex `poly` where `sign` x (distance past hinge `h`) <= `limit`.
FoldPolygon ClipToHingeSide(const FoldPolygon& poly, const FoldHinge& h,
                            float sign, float limit) {
  FoldPolygon out;
  const std::size_t n = poly.size();
  for (std::size_t i = 0; i < n; ++i) {
    const auto& a = poly[i];
    const auto& b = poly[(i + 1) % n];
    const float da =
        sign * ((a[0] - h.px) * h.nx + (a[1] - h.py) * h.ny) - limit;
    const float db =
        sign * ((b[0] - h.px) * h.nx + (b[1] - h.py) * h.ny) - limit;
    if (da <= 0.0f) {
      out.push_back(a);
    }
    if ((da <= 0.0f) != (db <= 0.0f)) {
      const float t = da / (da - db);
      out.push_back({a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])});
    }
  }
  return out;
}

// Box, in hinge coordinates, holding the image of the flap part `flap` once
// it turns about hinge `h`, with the perspective stretch of either face.
FoldPolygon FoldedFlapBounds(const FoldLayout& layout, const FoldHinge& h,
                             const FoldPolygon& flap) {
  float s_max = 0.0f;
  float t_min = 0.0f;
  float t_max = 0.0f;
  bool first = true;
  for (const auto& v : flap) {
    const float dx = v[0] - h.px;
    const float dy = v[1] - h.py;
    const float t = dx * h.tx + dy * h.ty;
    s_max = std::max(s_max, dx * h.nx + dy * h.ny);
    t_min = first ? t : std::min(t_min, t);
    t_max = first ? t : std::max(t_max, t);
    first = false;
  }
  // Mirrors UnfoldFlapPoint's clamps and lift.
  const float c = h.cos_phi >= 0.0f ? std::max(0.03f, h.cos_phi)
                                    : -std::max(0.03f, -h.cos_phi);
  const float d_lo = std::min(0.0f, c * s_max);
  const float d_hi = std::max(0.0f, c * s_max);
  const float lift = s_max * h.sin_phi + (h.cos_phi < 0.0f ? 3.0f : 0.0f);
  const float m_hi = 1.0f + layout.persp_k * lift;
  const float m_lo = std::max(0.5f, 1.0f - layout.persp_k * lift);
  const float t_hi = t_max * (t_max > 0.0f ? m_hi : m_lo);
  const float t_lo = t_min * (t_min < 0.0f ? m_hi : m_lo);
  FoldPolygon box;
  for (const auto& dt : {std::array<float, 2>{d_lo, t_lo},
                         std::array<float, 2>{d_hi, t_lo},
                         std::array<float, 2>{d_hi, t_hi},
                         std::array<float, 2>{d_lo, t_hi}}) {
    box.push_back({h.px + dt[0] * h.nx + dt[1] * h.tx,
                   h.py + dt[0] * h.ny + dt[1] * h.ty});
  }
  return box;
}

// Folds the sheet's bounding rectangle, grown by `grow_px`, forward through
// `layout` (EvalFoldStack unfolds in the reverse order). Stores the
// sheet-local bounds after folds 0..k in `levels[k + 1]` (`levels[0]` is the
// flat sheet) and returns the folded pieces: a conservative cover of all the
// paper, since each flap is replaced by a box around its folded image.
std::vector<FoldPolygon> FoldSheetBounds(
    const SheetGeometry& sheet, const FoldLayout& layout, float grow_px,
    std::array<BoundsBox, kMaxFoldHinges + 1>* levels) {
  const float hw = sheet.half_w + grow_px;
  const float hh = sheet.half_h + grow_px;
  std::vector<FoldPolygon> pieces(
      1, FoldPolygon{{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}});
  const auto same = [](float x, float y, float* ox, float* oy) {
    *ox = x;
    *oy = y;
  };
  (*levels)[0] = BoundsOfPieces(pieces, 2.0f, same);
  for (int k = 0; k < layout.count; ++k) {
    const FoldHinge& h = layout.hinges[k];
    if (h.phi > 1e-3f) {
      std::vector<FoldPolygon> next;
      for (const FoldPolygon& piece : pieces) {
        // The stationary side keeps the 1.25 px coverage ramp past the hinge.
        FoldPolygon stay = ClipToHingeSide(piece, h, 1.0f, 1.5f);
        if (stay.size() >= 3) {
          next.push_back(std::move(stay));
        }
        const FoldPolygon flap = ClipToHingeSide(piece, h, -1.0f, 0.0f);
        if (flap.size() >= 3) {
          next.push_back(FoldedFlapBounds(layout, h, flap));
        }
      }
      pieces = std::move(next);
    }
    (*levels)[k + 1] = BoundsOfPieces(pieces, 2.0f, same);
  }
  return pieces;
}

struct FoldShadeContext {
  const SheetGeometry* sheet = nullptr;
  const FoldLayout* layout = nullptr;
  float origin_x = 0.0f;  // Canvas origin and size for texture coordinates.
  float origin_y = 0.0f;
  float min_dim = 1.0f;
  bool enable_border = true;
  float border_px = 0.0f;
  float tear_amp_px = 0.0f;
  float tear_frequency = 24.0f;
  int tear_detail = 5;
  std::uint32_t seed = 7u;
  std::uint32_t tear_seed = 7u;
  float paper_r = 0.965f;
  float paper_g = 0.945f;
  float paper_b = 0.895f;
  float grain_strength = 0.35f;
  float grain_scale = 1.0f;
  float crease_strength = 0.45f;
  float crease_scale = 3.2f;
  float media_overlay = 0.24f;  // Grain/crease multiply on the printed image.
  float hinge_crease = 1.0f;    // Visibility of crease lines along hinges.
  float crease_band_px = 6.0f;
  float subject_margin_px = 0.0f;  // Torn edge must cut this deep to clip ink.
  float light_x = -0.42f;          // Unit vector toward the light.
  float light_y = 0.42f;
  float light_z = 0.80f;
  float flap_shadow_opacity = 0.6f;
  float shadow_dir_x = 0.707f;  // Shadows fall this way.
  float shadow_dir_y = -0.707f;
  float flap_shadow_px = 8.0f;
  float flap_shadow_soft_px = 4.0f;
  // Sheet-local bounds of the paper after folds 0..k, at index k + 1, for
  // EvalFoldStack and EvalFoldCoverage (whose soft edges reach further).
  // Points outside see no paper, which prunes the folded-layer recursion.
  std::array<BoundsBox, kMaxFoldHinges + 1> stack_bounds;
  std::array<BoundsBox, kMaxFoldHinges + 1> coverage_bounds;
};

void ConfigureFoldLight(float shadow_angle_deg, FoldShadeContext* ctx) {
  const float rad = shadow_angle_deg * kDegToRad;
  ctx->shadow_dir_x = std::cos(rad);
  ctx->shadow_dir_y = std::sin(rad);
  ctx->light_x = -0.6f * ctx->shadow_dir_x;
  ctx->light_y = -0.6f * ctx->shadow_dir_y;
  ctx->light_z = 0.8f;
}

// Flat coordinates of the flap point seen at (d, t) from hinge `h`: `d` px
// past the hinge for an open flap, or `-d` px back over the stationary side
// for a folded-over flap.
void UnfoldFlapPoint(const FoldLayout& layout, const FoldHinge& h, float d,
                     float t, bool folded_over, bool toward_viewer, float* xf,
                     float* yf) {
  const float s = folded_over
                      ? std::max(0.0f, -d) / std::max(0.03f, -h.cos_phi)
                      : d / std::max(0.03f, h.cos_phi);
  const float lift = (s * h.sin_phi + (folded_over ? 3.0f : 0.0f)) *
                     (toward_viewer ? 1.0f : -1.0f);
  const float tf = t / std::max(0.5f, 1.0f + layout.persp_k * lift);
  *xf = h.px + s * h.nx + tf * h.tx;
  *yf = h.py + s * h.ny + tf * h.ty;
}

// Lambert shading of the visible flap face relative to a flat sheet.
float FlapLight(const FoldShadeContext& ctx, const FoldHinge& h,
                bool toward_viewer) {
  float nn = (toward_viewer ? -1.0f : 1.0f) * h.sin_phi;
  float nz = h.cos_phi;
  if (nz < 0.0f) {  // The other face is the one facing the viewer.
    nn = -nn;
    nz = -nz;
  }
  const float l_n = ctx.light_x * h.nx + ctx.light_y * h.ny;
  const float lambert = std::max(0.0f, nn * l_n + nz * ctx.light_z);
  constexpr float kAmbient = 0.62f;
  return Clamp((kAmbient + (1.0f - kAmbient) * lambert) /
                   (kAmbient + (1.0f - kAmbient) * ctx.light_z),
               0.62f, 1.15f);
}

// Union coverage of every paper layer at sheet-local (x, y), for shadows.
float EvalFoldCoverage(const FoldShadeContext& ctx, int k, float x, float y,
                       bool back_side, float soft_px) {
  for (; k >= 0; --k) {
    if (!ctx.coverage_bounds[k + 1].Contains(x, y)) {
      return 0.0f;
    }
    const FoldHinge& h = ctx.layout->hinges[k];
    if (h.phi <= 1e-3f) {
      continue;
    }
    const float d = (x - h.px) * h.nx + (y - h.py) * h.ny;
    const float t = (x - h.px) * h.tx + (y - h.py) * h.ty;
    const bool toward_viewer = h.valley != back_side;
    if (h.cos_phi >= 0.0f) {
      if (d > 0.0f) {
        UnfoldFlapPoint(*ctx.layout, h, d, t, false, toward_viewer, &x, &y);
      }
      continue;
    }
    if (d >= 1.25f) {
      return 0.0f;
    }
    const float cov = Smoothstep(1.25f, -1.25f, d);
    float xf = 0.0f;
    float yf = 0.0f;
    UnfoldFlapPoint(*ctx.layout, h, d, t, true, toward_viewer, &xf, &yf);
    const float a = EvalFoldCoverage(ctx, k - 1, xf, yf, !back_side, soft_px);
    if (a >= 0.999f) {
      return cov;
    }
    const float b = EvalFoldCoverage(ctx, k - 1, x, y, back_side, soft_px);
    return cov * (a + b - a * b);
  }
  const float edge =
      ctx.sheet->DistPx(ctx.sheet->cx + x, ctx.sheet->cy + y) - ctx.border_px;
  return Smoothstep(std::max(1.2f, soft_px), -1.2f, edge);
}

// Shifts the packet so the folded paper stays centered while it is small,
// then eases it back to the sheet's resting place over the last of the unfold.
void CenterFoldedPacket(const FoldShadeContext& ctx, float openness,
                        FoldLayout* layout) {
  const float weight = 1.0f - Smoothstep(0.55f, 1.0f, openness);
  if (weight <= 1e-4f) {
    return;
  }
  constexpr int kGrid = 40;
  const float ext_x = 1.15f * ctx.sheet->half_w;
  const float ext_y = 1.15f * ctx.sheet->half_h;
  double sum = 0.0;
  double sum_x = 0.0;
  double sum_y = 0.0;
  for (int j = 0; j < kGrid; ++j) {
    const float y =
        ext_y * (-1.0f + (2.0f * static_cast<float>(j) + 1.0f) / kGrid);
    for (int i = 0; i < kGrid; ++i) {
      const float x =
          ext_x * (-1.0f + (2.0f * static_cast<float>(i) + 1.0f) / kGrid);
      const float a =
          EvalFoldCoverage(ctx, layout->count - 1, x, y, false, 1.2f);
      sum += a;
      sum_x += a * x;
      sum_y += a * y;
    }
  }
  if (sum < 1e-3) {
    return;
  }
  const float cx = static_cast<float>(sum_x / sum) * weight;
  const float cy = static_cast<float>(sum_y / sum) * weight;
  layout->offset_x -= (layout->rot_cos * cx - layout->rot_sin * cy) *
                      layout->scale;
  layout->offset_y -= (layout->rot_sin * cx + layout->rot_cos * cy) *
                      layout->scale;
}

// Darkened valley and lit ridge along the hinge lines, with a hand-made wobble.
float HingeCreaseMarks(const FoldShadeContext& ctx, float x, float y,
                       float nu_x, float nu_y) {
  if (ctx.hinge_crease <= 1e-3f || ctx.crease_strength <= 1e-3f) {
    return 1.0f;
  }
  const FoldLayout& layout = *ctx.layout;
  const float band = ctx.crease_band_px;
  float nearest = 1e9f;
  for (int j = 0; j < layout.count; ++j) {
    const FoldHinge& h = layout.hinges[j];
    nearest = std::min(nearest,
                       std::abs((x - h.px) * h.nx + (y - h.py) * h.ny));
  }
  if (nearest > band * 2.2f + 1.6f) {
    return 1.0f;
  }
  const float wobble =
      (ValueNoise2D(nu_x * 18.0f, nu_y * 18.0f, ctx.seed + 313u) - 0.5f) *
      3.2f;
  const float k = ctx.crease_strength * ctx.hinge_crease;
  float mul = 1.0f;
  for (int j = 0; j < layout.count; ++j) {
    const FoldHinge& h = layout.hinges[j];
    const float hd =
        std::abs((x - h.px) * h.nx + (y - h.py) * h.ny + wobble);
    if (hd < band * 2.2f) {
      const float valley = Smoothstep(band, 0.0f, hd);
      const float ridge =
          Smoothstep(band * 2.2f, band * 0.9f, hd) * (1.0f - valley);
      mul *= 1.0f + k * (-0.26f * valley + 0.09f * ridge);
    }
  }
  return mul;
}

// One flat leaf of paper: the print on its torn cardstock collar (front), or
// the toned paper back with faint bleed-through of the print.
template <typename SubjectFn>
RgbaColor EvalPaperLeaf(const FoldShadeContext& ctx, float x, float y,
                        bool back_side, float shade,
                        const SubjectFn& subject) {
  const float ux = ctx.sheet->cx + x;
  const float uy = ctx.sheet->cy + y;
  const float dist_px = ctx.sheet->DistPx(ux, uy);
  const float base_edge = dist_px - ctx.border_px;
  if (base_edge > ctx.tear_amp_px + 1.3f) {
    return {};
  }
  const float nu_x = (ux - ctx.origin_x) / ctx.min_dim;
  const float nu_y = (uy - ctx.origin_y) / ctx.min_dim;
  const float tear_n =
      (ctx.tear_amp_px > 0.01f &&
       std::abs(base_edge) <= ctx.tear_amp_px + 3.1f)
          ? TearEdgeNoise(nu_x, nu_y, ctx.tear_frequency, ctx.tear_detail,
                          ctx.tear_seed)
          : 0.0f;
  const float edge_sdf = base_edge - ctx.tear_amp_px * tear_n;
  const float sheet_alpha = Smoothstep(1.2f, -1.2f, edge_sdf);
  if (sheet_alpha <= 1e-4f) {
    return {};
  }

  const float grain =
      ctx.grain_strength > 1e-3f
          ? Lerp(1.0f, PaperFiberGrain(nu_x, nu_y, ctx.grain_scale, ctx.seed),
                 Clamp01(ctx.grain_strength))
          : 1.0f;
  const float crease_val =
      ctx.crease_strength > 1e-3f
          ? PaperCreaseField(nu_x, nu_y, ctx.crease_scale, ctx.seed + 89u)
          : 0.0f;
  const float crease_mul =
      (1.0f - 0.34f * Clamp01(ctx.crease_strength) * crease_val) *
      HingeCreaseMarks(ctx, x, y, nu_x, nu_y);
  const float rim = 1.0f + 0.08f * Smoothstep(3.0f, 0.0f, std::abs(edge_sdf));

  if (back_side) {
    const RgbaColor ink = subject(ux, uy, dist_px);
    float bleed = 1.0f;
    if (ink.a > 1e-3f) {
      const float luma =
          (0.299f * ink.r + 0.587f * ink.g + 0.114f * ink.b) / ink.a;
      bleed = Lerp(1.0f, 0.86f + 0.14f * Clamp01(luma),
                   0.3f * Clamp01(ink.a));
    }
    const float s =
        Clamp(0.93f * grain * crease_mul * rim * shade * bleed, 0.2f, 1.12f);
    return {Clamp01(ctx.paper_r * s) * sheet_alpha,
            Clamp01(ctx.paper_g * 0.985f * s) * sheet_alpha,
            Clamp01(ctx.paper_b * 0.95f * s) * sheet_alpha, sheet_alpha};
  }

  RgbaColor ink = subject(ux, uy, dist_px);
  if (ink.a > 1e-4f) {
    const float ink_mul =
        Lerp(1.0f, grain * crease_mul, Clamp01(ctx.media_overlay)) * shade;
    // Torn edges clip the print only where they cut past the margin, so soft
    // matte edges survive while the collar is narrow or off.
    const float clip =
        Smoothstep(1.2f, -1.2f, edge_sdf - ctx.subject_margin_px);
    ink = {ink.r * ink_mul * clip, ink.g * ink_mul * clip,
           ink.b * ink_mul * clip, ink.a * clip};
  }
  if (!ctx.enable_border || ctx.border_px <= 0.05f) {
    return ink;
  }
  const float front = Clamp(grain * crease_mul * rim * shade, 0.2f, 1.15f);
  const RgbaColor paper = {Clamp01(ctx.paper_r * front) * sheet_alpha,
                           Clamp01(ctx.paper_g * front) * sheet_alpha,
                           Clamp01(ctx.paper_b * front) * sheet_alpha,
                           sheet_alpha};
  return CompositeOver(ink, paper);
}

// Front-to-back evaluation of the folded stack at sheet-local (x, y), with
// view-parity aware layer order, flap lighting, and flap cast shadows.
template <typename SubjectFn>
RgbaColor EvalFoldStack(const FoldShadeContext& ctx, int k, float x, float y,
                        bool back_side, float shade,
                        const SubjectFn& subject) {
  const float ao_band = 2.0f * ctx.crease_band_px;
  for (; k >= 0; --k) {
    if (!ctx.stack_bounds[k + 1].Contains(x, y)) {
      return {};
    }
    const FoldHinge& h = ctx.layout->hinges[k];
    if (h.phi <= 1e-3f) {
      continue;
    }
    const float d = (x - h.px) * h.nx + (y - h.py) * h.ny;
    const float t = (x - h.px) * h.tx + (y - h.py) * h.ty;
    const bool toward_viewer = h.valley != back_side;

    if (h.cos_phi >= 0.0f) {
      // Open past 90 degrees: one layer per pixel, darkened in the crease.
      if (d <= 0.0f) {
        shade *= 1.0f - 0.14f * h.sin_phi * Smoothstep(-ao_band, 0.0f, d);
      } else {
        UnfoldFlapPoint(*ctx.layout, h, d, t, false, toward_viewer, &x, &y);
        const float s = d / std::max(0.03f, h.cos_phi);
        shade *= FlapLight(ctx, h, toward_viewer) *
                 (1.0f - 0.15f * h.sin_phi * Smoothstep(ao_band, 0.0f, s));
      }
      continue;
    }

    // Folded over: the flap lies on the stationary side (d < 0).
    if (d >= 1.25f) {
      return {};
    }
    const float cov = Smoothstep(1.25f, -1.25f, d);
    float xf = 0.0f;
    float yf = 0.0f;
    UnfoldFlapPoint(*ctx.layout, h, d, t, true, toward_viewer, &xf, &yf);
    const float flap_shade = shade * FlapLight(ctx, h, toward_viewer);

    if (!toward_viewer) {
      // Mountain flap (or valley seen from behind): it hides under the sheet.
      const RgbaColor top =
          EvalFoldStack(ctx, k - 1, x, y, back_side, shade, subject);
      if (top.a >= 0.999f && cov >= 0.999f) {
        return top;
      }
      const RgbaColor bottom = EvalFoldStack(ctx, k - 1, xf, yf, !back_side,
                                             flap_shade * 0.85f, subject);
      return ScalePremult(CompositeOver(top, bottom), cov);
    }

    const RgbaColor top =
        EvalFoldStack(ctx, k - 1, xf, yf, !back_side, flap_shade, subject);
    if (top.a >= 0.999f && cov >= 0.999f) {
      return top;
    }
    RgbaColor bottom =
        EvalFoldStack(ctx, k - 1, x, y, back_side, shade, subject);
    if (bottom.a > 1e-4f && ctx.flap_shadow_opacity > 1e-4f) {
      // Shadow of the lifted flap, further away while it is still high.
      const float tuck = 0.24f * Smoothstep(-ao_band, 0.0f, d);
      const float off = ctx.flap_shadow_px * (0.25f + 0.75f * h.sin_phi);
      const float sx = x - ctx.shadow_dir_x * off;
      const float sy = y - ctx.shadow_dir_y * off;
      const float ds = (sx - h.px) * h.nx + (sy - h.py) * h.ny;
      float cast = 0.0f;
      if (ds < 1.25f) {
        float sxf = 0.0f;
        float syf = 0.0f;
        UnfoldFlapPoint(*ctx.layout, h, ds,
                        (sx - h.px) * h.tx + (sy - h.py) * h.ty, true,
                        toward_viewer, &sxf, &syf);
        cast = Smoothstep(1.25f, -1.25f, ds) *
               EvalFoldCoverage(ctx, k - 1, sxf, syf, !back_side,
                                ctx.flap_shadow_soft_px);
      }
      const float dark = Clamp(
          ctx.flap_shadow_opacity * std::max(tuck, 0.45f * cast), 0.0f, 0.7f);
      bottom.r *= 1.0f - dark;
      bottom.g *= 1.0f - dark;
      bottom.b *= 1.0f - dark;
    }
    return ScalePremult(CompositeOver(top, bottom), cov);
  }
  return EvalPaperLeaf(ctx, x, y, back_side, shade, subject);
}

// Maps canvas pixels into the folded packet's sheet-local frame.
struct PacketMapper {
  float origin_x = 0.0f;  // Sheet center plus packet offset and jitter.
  float origin_y = 0.0f;
  float cos_r = 1.0f;  // Forward packet rotation.
  float sin_r = 0.0f;
  float inv_scale = 1.0f;

  void ToLocal(float px, float py, float* lx, float* ly) const {
    const float dx = px - origin_x;
    const float dy = py - origin_y;
    *lx = (dx * cos_r + dy * sin_r) * inv_scale;
    *ly = (-dx * sin_r + dy * cos_r) * inv_scale;
  }

  void ToCanvas(float lx, float ly, float* px, float* py) const {
    *px = origin_x + (lx * cos_r - ly * sin_r) / inv_scale;
    *py = origin_y + (lx * sin_r + ly * cos_r) / inv_scale;
  }
};

FoldTransitionAction ResolveFoldAction(int requested,
                                       const ClipContentSummary& from,
                                       const ClipContentSummary& to) {
  const auto action =
      static_cast<FoldTransitionAction>(std::min(3, std::max(0, requested)));
  if (action != FoldTransitionAction::kAuto) {
    return action;
  }
  // Hosts pass the empty side of a clip edge as transparent or opaque black.
  const bool from_empty = !from.has_visible_alpha || !from.has_nonblack_rgb;
  const bool to_empty = !to.has_visible_alpha || !to.has_nonblack_rgb;
  if (to_empty && !from_empty) {
    return FoldTransitionAction::kFoldOut;
  }
  if (from_empty || to.is_full_frame_opaque) {
    return FoldTransitionAction::kUnfoldIn;
  }
  return FoldTransitionAction::kFoldAndUnfold;
}

}  // namespace

bool IsValidRgbaView(const ImageView& view) {
  return view.data != nullptr && view.components == 4 && view.row_bytes != 0 &&
         view.bounds.x2 > view.bounds.x1 && view.bounds.y2 > view.bounds.y1;
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

  // Paper Unfold (3D): fold the finished cutout along real paper hinges.
  if (anim.style == CutoutAnimStyle::kPaperUnfold) {
    const SheetGeometry sheet = SheetFromSdf(sdf, border_px + tear_amp_px);
    if (!sheet.valid) {
      RunScanlines(y_begin, y_end, parallel_for, [&](int y) {
        for (int x = x_begin; x < x_end; ++x) {
          WritePixel(output, x, y, {});
        }
      });
      return;
    }
    const float q = anim.raw_p;
    // Glide Angle (default -22) also turns the fold direction (default 0).
    FoldLayout layout = BuildFoldLayout(
        FoldPattern::kCraftFlaps, 4, q, sheet,
        std::hypot(cw, ch), params.transition_angle_deg + 22.0f, 0.55f, 0.0f,
        params.mirror_transition_h, params.mirror_transition_v, seed);

    FoldShadeContext ctx;
    ctx.sheet = &sheet;
    ctx.layout = &layout;
    ctx.origin_x = origin_x;
    ctx.origin_y = origin_y;
    ctx.min_dim = min_dim;
    ctx.enable_border = params.enable_border;
    ctx.border_px = border_px;
    ctx.tear_amp_px = tear_amp_px;
    ctx.tear_frequency = params.tear_frequency;
    ctx.tear_detail = params.tear_detail;
    ctx.seed = seed;
    ctx.tear_seed = boil_seed;
    ctx.paper_r = params.paper_r;
    ctx.paper_g = params.paper_g;
    ctx.paper_b = params.paper_b;
    ctx.grain_strength = params.grain_strength;
    ctx.grain_scale = params.grain_scale;
    ctx.crease_strength = std::max(0.3f, params.crease_strength);
    ctx.crease_scale = params.crease_scale;
    ctx.media_overlay =
        std::max(params.media_texture_overlay, 0.25f * (1.0f - q));
    ctx.hinge_crease = 1.0f - q * q;
    ctx.crease_band_px = std::max(2.5f, 0.0055f * min_dim);
    // Like the resting cutout: the collar never clips the print, while
    // borderless cutouts keep the matte edge.
    ctx.subject_margin_px = params.enable_border ? 1e5f : 0.0f;
    ConfigureFoldLight(params.shadow_angle_deg, &ctx);
    ctx.flap_shadow_opacity = std::max(0.45f, params.shadow_opacity);
    ctx.flap_shadow_px = std::max(3.0f, 0.011f * min_dim);
    ctx.flap_shadow_soft_px = std::max(2.0f, 0.006f * min_dim);
    // Bounds per fold level prune the layer recursion away from the paper.
    const float sdf_slack = 2.0f * static_cast<float>(sdf.step) + 3.0f;
    FoldSheetBounds(sheet, layout, border_px + tear_amp_px + sdf_slack,
                    &ctx.stack_bounds);
    FoldSheetBounds(sheet, layout,
                    border_px +
                        std::max({1.2f, sh_soft_px, ctx.flap_shadow_soft_px}) +
                        sdf_slack,
                    &ctx.coverage_bounds);
    CenterFoldedPacket(ctx, q, &layout);

    auto sample_subject = [&](float ux, float uy, float dist_px) {
      const RgbaColor s =
          EvaluateSubjectAtCanonicalPx(params, subj_xf, src_view, ux, uy);
      if (std::abs(params.edge_choke_px) <= 0.01f || s.a <= 1e-4f) {
        return s;
      }
      return ScalePremult(s, Smoothstep(params.edge_choke_px + 1.0f,
                                        params.edge_choke_px - 1.0f, dist_px));
    };

    const float cx_out =
        origin_x + params.master_pos_x * cw + anim.jitter_dx_px;
    const float cy_out =
        origin_y + params.master_pos_y * ch + anim.jitter_dy_px;
    PacketMapper packet;  // Relative to the canvas center after the master.
    packet.origin_x = sheet.cx - (origin_x + 0.5f * cw) + layout.offset_x;
    packet.origin_y = sheet.cy - (origin_y + 0.5f * ch) + layout.offset_y;
    packet.cos_r = layout.rot_cos;
    packet.sin_r = layout.rot_sin;
    packet.inv_scale = 1.0f / std::max(0.05f, layout.scale);
    auto to_local = [&](float px, float py, float* lx, float* ly) {
      const float dx = px - cx_out;
      const float dy = py - cy_out;
      float rx = (dx * anim.cos_m - dy * anim.sin_m) * anim.inv_m_scale;
      float ry = (dx * anim.sin_m + dy * anim.cos_m) * anim.inv_m_scale;
      if (params.flip_h) {
        rx = -rx;
      }
      if (params.flip_v) {
        ry = -ry;
      }
      packet.ToLocal(rx, ry, lx, ly);
    };

    const float lift_k = 1.0f + 2.5f * layout.lift;
    RunScanlinesInterleaved(y_begin, y_end, parallel_for, [&](int y) {
      const float py_out = static_cast<float>(y) + 0.5f;
      for (int x = x_begin; x < x_end; ++x) {
        const float px_out = static_cast<float>(x) + 0.5f;
        float lx = 0.0f;
        float ly = 0.0f;
        to_local(px_out, py_out, &lx, &ly);
        const RgbaColor fg = ScalePremult(
            EvalFoldStack(ctx, layout.count - 1, lx, ly, false, 1.0f,
                          sample_subject),
            layout.alpha);
        if (fg.a >= 0.999f) {
          WritePixel(output, x, y, fg);
          continue;
        }
        float shadow_a = 0.0f;
        if (params.shadow_opacity > 1e-4f) {
          float slx = 0.0f;
          float sly = 0.0f;
          to_local(px_out - sh_dx_px * lift_k, py_out - sh_dy_px * lift_k,
                   &slx, &sly);
          shadow_a = params.shadow_opacity * layout.alpha *
                     EvalFoldCoverage(ctx, layout.count - 1, slx, sly, false,
                                      sh_soft_px);
        }
        WritePixel(output, x, y,
                   CompositeOver(fg, {0.0f, 0.0f, 0.0f, shadow_a}));
      }
    });
    return;
  }

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

float ComputeSteppedTransitionProgress(
    const PaperFoldTransitionParams& params) {
  const float p = Clamp01(params.progress);
  if (p <= 1e-5f) {
    return 0.0f;
  }
  if (p >= 1.0f - 1e-5f) {
    return 1.0f;
  }
  if (!params.enable_stop_motion || params.stop_motion_steps < 2) {
    return p;
  }
  const float steps = static_cast<float>(params.stop_motion_steps);
  return (std::floor(p * steps) + 0.5f) / steps;
}

void RenderPaperFoldTransition(const PaperFoldTransitionParams& params,
                               const ImageView& source_from,
                               const ImageView& source_to,
                               const ImageView& output,
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

  ImageView from_view = source_from;
  ImageView to_view = source_to;
  if (!params.source_is_premultiplied) {
    for (ImageView* view : {&from_view, &to_view}) {
      if (view->alpha_mode == AlphaMode::kPremultiplied) {
        view->alpha_mode = AlphaMode::kUnPremultiplied;
      }
    }
  }

  // Writes `weight` x `clip` (or transparency) over the render window.
  auto fill_with_clip = [&](const ImageView* clip, float weight) {
    const bool use_clip =
        clip != nullptr && weight > 1e-4f && IsValidRgbaView(*clip);
    RunScanlines(y_begin, y_end, parallel_for, [&](int y) {
      for (int x = x_begin; x < x_end; ++x) {
        const RgbaColor c =
            use_clip
                ? ScalePremult(ClipPixelAt(*clip, canvas_bounds, x, y), weight)
                : RgbaColor{};
        WritePixel(output, x, y, c);
      }
    });
  };

  const float p = ComputeSteppedTransitionProgress(params);
  if (p <= 0.0f) {
    fill_with_clip(&from_view, 1.0f);
    return;
  }
  if (p >= 1.0f) {
    fill_with_clip(&to_view, 1.0f);
    return;
  }

  const ClipContentSummary from_info = InspectClipContent(from_view);
  const ClipContentSummary to_info = InspectClipContent(to_view);
  const FoldTransitionAction action =
      ResolveFoldAction(params.fold_action, from_info, to_info);

  // The active clip folds or unfolds; the other one shows underneath.
  const ImageView* active = &to_view;
  const ClipContentSummary* active_info = &to_info;
  const ImageView* under = &from_view;
  float under_weight = 1.0f;
  float q = p;  // Openness of the active sheet: 1 = flat, 0 = folded away.
  switch (action) {
    case FoldTransitionAction::kFoldOut:
      active = &from_view;
      active_info = &from_info;
      under = &to_view;
      q = 1.0f - p;
      // See-through outgoing clips let the incoming clip fade in early.
      under_weight = from_info.is_full_frame_opaque
                         ? 1.0f
                         : Smoothstep(0.0f, 0.25f, p);
      break;
    case FoldTransitionAction::kFoldAndUnfold:
      if (p < 0.5f) {
        active = &from_view;
        active_info = &from_info;
        q = 1.0f - 2.0f * p;
      } else {
        q = 2.0f * p - 1.0f;
      }
      under = nullptr;
      under_weight = 0.0f;
      break;
    case FoldTransitionAction::kAuto:
    case FoldTransitionAction::kUnfoldIn:
      // A see-through incoming clip fades the outgoing one out at the end.
      under_weight = to_info.is_full_frame_opaque
                         ? 1.0f
                         : Smoothstep(1.0f, 0.75f, p);
      break;
  }
  if (!active_info->has_visible_alpha || q <= 0.0f) {
    fill_with_clip(under, under_weight);
    return;
  }

  const float cw =
      static_cast<float>(std::max(1, canvas_bounds.x2 - canvas_bounds.x1));
  const float ch =
      static_cast<float>(std::max(1, canvas_bounds.y2 - canvas_bounds.y1));
  const float min_dim = std::min(cw, ch);
  // Collar, creases, jitter, and drop shadow settle away as the sheet lands
  // flat, so the last unfold frame matches the clean clip.
  const float settle = Smoothstep(1.0f, 0.88f, q);
  const float full_border =
      params.enable_border ? std::max(0.0f, params.border_width_norm * min_dim)
                           : 0.0f;
  const float full_tear =
      params.enable_border
          ? std::max(0.0f, params.tear_roughness_norm * min_dim)
          : 0.0f;

  CoarseSignedDistanceField matte_sdf;
  SheetGeometry sheet;
  float sdf_step = 0.0f;
  if (active_info->is_full_frame_opaque) {
    sheet = FullFrameSheet(canvas_bounds);
  } else {
    CutoutParams matte_params;
    matte_params.source_mode = 0;
    matte_params.media_scale = 1.0f;
    matte_params.enable_chroma_key = false;
    const SubjectTransform identity =
        BuildSubjectTransform(matte_params, *active, canvas_bounds);
    matte_sdf = BuildSubjectSignedDistanceField(
        matte_params, identity, *active, canvas_bounds, parallel_for);
    sheet = SheetFromSdf(matte_sdf, full_border + full_tear);
    sdf_step = static_cast<float>(matte_sdf.step);
  }
  if (!sheet.valid) {
    fill_with_clip(under, under_weight);
    return;
  }

  const std::uint32_t seed =
      static_cast<std::uint32_t>(std::max(0, params.random_seed));
  FoldLayout layout = BuildFoldLayout(
      static_cast<FoldPattern>(std::min(3, std::max(0, params.fold_pattern))),
      params.fold_stages, q, sheet, std::hypot(cw, ch),
      params.direction_angle_deg, params.fold_depth, params.slide_distance,
      params.mirror_h, params.mirror_v, seed);

  FoldShadeContext ctx;
  ctx.sheet = &sheet;
  ctx.layout = &layout;
  ctx.origin_x = static_cast<float>(canvas_bounds.x1);
  ctx.origin_y = static_cast<float>(canvas_bounds.y1);
  ctx.min_dim = min_dim;
  ctx.enable_border = params.enable_border;
  ctx.border_px = full_border * settle;
  ctx.tear_amp_px = full_tear * settle;
  ctx.tear_frequency = params.tear_frequency;
  ctx.tear_detail = 5;
  ctx.seed = seed;
  ctx.tear_seed = seed + 17u;
  ctx.paper_r = params.paper_r;
  ctx.paper_g = params.paper_g;
  ctx.paper_b = params.paper_b;
  ctx.grain_strength = params.grain_strength;
  ctx.grain_scale = 1.0f;
  ctx.crease_strength = params.crease_strength;
  ctx.crease_scale = 3.4f;
  ctx.media_overlay = 0.30f * Clamp01(params.crease_strength) * settle;
  ctx.hinge_crease = settle;
  ctx.crease_band_px = std::max(2.5f, 0.0055f * min_dim);
  // Keep soft matte edges (and the coarse matte's slack) while the collar is
  // narrower than that margin.
  ctx.subject_margin_px = std::max(0.0f, 2.0f + sdf_step - ctx.border_px);
  ConfigureFoldLight(params.shadow_angle_deg, &ctx);
  ctx.flap_shadow_opacity = params.shadow_opacity;
  ctx.flap_shadow_px = std::max(3.0f, 0.011f * min_dim);
  ctx.flap_shadow_soft_px = std::max(2.0f, 0.006f * min_dim);

  // Drop shadow grows and softens while the packet is lifted.
  const float lift_k = 1.0f + 2.5f * layout.lift;
  const float sh_dist = params.shadow_distance_norm * min_dim * lift_k;
  const float sh_dx = ctx.shadow_dir_x * sh_dist;
  const float sh_dy = ctx.shadow_dir_y * sh_dist;
  const float sh_soft =
      std::max(1.2f, params.shadow_softness_norm * min_dim *
                         (1.0f + 1.5f * layout.lift));
  const float sh_opacity =
      params.shadow_opacity * settle * (1.0f - 0.35f * layout.lift);

  // Conservative bounds of the paper (torn edge included) and of the soft
  // coverage used by shadows, per fold level; the coarse matte adds slack.
  const float sdf_slack = 2.0f * sdf_step + 3.0f;
  const std::vector<FoldPolygon> paper_cover = FoldSheetBounds(
      sheet, layout, ctx.border_px + ctx.tear_amp_px + sdf_slack,
      &ctx.stack_bounds);
  const std::vector<FoldPolygon> coverage_cover = FoldSheetBounds(
      sheet, layout,
      ctx.border_px + std::max(sh_soft, ctx.flap_shadow_soft_px) + sdf_slack,
      &ctx.coverage_bounds);
  CenterFoldedPacket(ctx, q, &layout);

  // Stop-motion hand jitter: one pose per held step.
  float jitter_x = 0.0f;
  float jitter_y = 0.0f;
  float jitter_rot = 0.0f;
  if (params.enable_stop_motion && params.jitter_amount > 1e-4f) {
    const float step = std::floor(
        p * static_cast<float>(std::max(2, params.stop_motion_steps)));
    const float sf = step * 1.618f + static_cast<float>(seed) * 0.37f;
    const float amp = Clamp01(params.jitter_amount) * settle;
    jitter_x = 0.008f * amp * min_dim * std::sin(sf * 12.9898f);
    jitter_y = 0.008f * amp * min_dim * std::cos(sf * 78.233f);
    jitter_rot = 2.2f * amp * kDegToRad * std::sin(sf * 43.17f);
  }
  PacketMapper packet;
  packet.origin_x = sheet.cx + layout.offset_x + jitter_x;
  packet.origin_y = sheet.cy + layout.offset_y + jitter_y;
  const float packet_rot =
      std::atan2(layout.rot_sin, layout.rot_cos) + jitter_rot;
  packet.cos_r = std::cos(packet_rot);
  packet.sin_r = std::sin(packet_rot);
  packet.inv_scale = 1.0f / std::max(0.05f, layout.scale);

  auto sample_active = [&](float ux, float uy, float /*dist_px*/) {
    return SampleClipCanonicalPx(*active, canvas_bounds, ux, uy);
  };
  const bool has_under =
      under != nullptr && under_weight > 1e-4f && IsValidRgbaView(*under);

  // Only pixels near the packet (or its drop shadow) need the fold stack; the
  // rest of the frame, most of it while the packet is small, is the underlay.
  const auto to_canvas = [&](float lx, float ly, float* cx, float* cy) {
    packet.ToCanvas(lx, ly, cx, cy);
  };
  const auto to_shadow = [&](float lx, float ly, float* cx, float* cy) {
    packet.ToCanvas(lx, ly, cx, cy);
    *cx += sh_dx;
    *cy += sh_dy;
  };
  const BoundsBox paper_box = BoundsOfPieces(paper_cover, 3.0f, to_canvas);
  const BoundsBox shadow_box = BoundsOfPieces(coverage_cover, 3.0f, to_shadow);

  RunScanlinesInterleaved(y_begin, y_end, parallel_for, [&](int y) {
    const float py_out = static_cast<float>(y) + 0.5f;
    const bool row_paper = py_out >= paper_box.y1 && py_out <= paper_box.y2;
    const bool row_shadow = sh_opacity > 1e-4f && py_out >= shadow_box.y1 &&
                            py_out <= shadow_box.y2;
    for (int x = x_begin; x < x_end; ++x) {
      const float px_out = static_cast<float>(x) + 0.5f;
      RgbaColor fg;
      if (row_paper && px_out >= paper_box.x1 && px_out <= paper_box.x2) {
        float lx = 0.0f;
        float ly = 0.0f;
        packet.ToLocal(px_out, py_out, &lx, &ly);
        fg = ScalePremult(EvalFoldStack(ctx, layout.count - 1, lx, ly, false,
                                        1.0f, sample_active),
                          layout.alpha);
        if (fg.a >= 0.999f) {
          WritePixel(output, x, y, fg);
          continue;
        }
      }
      RgbaColor bg =
          has_under
              ? ScalePremult(ClipPixelAt(*under, canvas_bounds, x, y),
                             under_weight)
              : RgbaColor{};
      if (row_shadow && px_out >= shadow_box.x1 && px_out <= shadow_box.x2) {
        float slx = 0.0f;
        float sly = 0.0f;
        packet.ToLocal(px_out - sh_dx, py_out - sh_dy, &slx, &sly);
        const float a = sh_opacity * layout.alpha *
                        EvalFoldCoverage(ctx, layout.count - 1, slx, sly,
                                         false, sh_soft);
        if (a > 1e-4f) {
          bg = CompositeOver({0.0f, 0.0f, 0.0f, a}, bg);
        }
      }
      WritePixel(output, x, y, CompositeOver(fg, bg));
    }
  });
}

}  // namespace papercraft
