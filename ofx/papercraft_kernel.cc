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
  // Saturated ends (most pixels far from an edge) skip the division.
  if (edge0 < edge1 ? x <= edge0 : x >= edge0) {
    return 0.0f;
  }
  if (edge0 < edge1 ? x >= edge1 : x <= edge1) {
    return 1.0f;
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

// Hashed corner values of one integer value-noise lattice cell.
struct NoiseCell {
  int ix = 0;
  int iy = 0;
  float n00 = 0.0f;
  float n10 = 0.0f;
  float n01 = 0.0f;
  float n11 = 0.0f;
};

NoiseCell HashNoiseCell(int ix, int iy, std::uint32_t seed) {
  return {ix,
          iy,
          Hash2D(ix, iy, seed),
          Hash2D(ix + 1, iy, seed),
          Hash2D(ix, iy + 1, seed),
          Hash2D(ix + 1, iy + 1, seed)};
}

// Quintic-faded bilinear interpolation of `cell` at lattice point (x, y).
float InterpolateNoiseCell(const NoiseCell& cell, float x, float y) {
  const float fx = x - static_cast<float>(cell.ix);
  const float fy = y - static_cast<float>(cell.iy);
  const float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
  const float uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
  return Lerp(Lerp(cell.n00, cell.n10, ux), Lerp(cell.n01, cell.n11, ux), uy);
}

float ValueNoise2D(float x, float y, std::uint32_t seed) {
  const int ix = static_cast<int>(std::floor(x));
  const int iy = static_cast<int>(std::floor(y));
  return InterpolateNoiseCell(HashNoiseCell(ix, iy, seed), x, y);
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
constexpr int kCreaseOctaves = 3;

// Sheared lattice coordinates of crease octave `i` at normalized (nx, ny).
void CreaseOctaveCoords(float nx, float ny, float freq, int i, float* rx,
                        float* ry) {
  *rx = nx * freq + (i == 1 ? 0.37f * ny * freq : 0.0f);
  *ry = ny * freq + (i == 2 ? -0.41f * nx * freq : 0.0f);
}

float RidgeOf(float n) {
  const float ridge = 1.0f - std::abs(2.0f * n - 1.0f);
  return ridge * ridge * ridge;
}

float PaperCreaseField(float nx, float ny, float scale, std::uint32_t seed) {
  const float s = std::max(0.25f, scale) * 1.6f;
  float ridge_sum = 0.0f;
  float weight = 0.55f;
  float total_w = 0.0f;
  float freq = s;
  for (int i = 0; i < kCreaseOctaves; ++i) {
    float rx = 0.0f;
    float ry = 0.0f;
    CreaseOctaveCoords(nx, ny, freq, i, &rx, &ry);
    const float n =
        ValueNoise2D(rx, ry, seed + static_cast<std::uint32_t>(i) * 211u);
    ridge_sum += RidgeOf(n) * weight;
    total_w += weight;
    freq *= 1.95f;
    weight *= 0.55f;
  }
  return total_w > 0.0f ? Clamp01(ridge_sum / total_w) : 0.0f;
}

// Procedural gradient/displacement vector for 3D crumple ball warp.
//
// Evaluates the crease field at (nx, ny), (nx + eps, ny), and (nx, ny + eps)
// in one pass. The three samples usually fall in the same lattice cell per
// octave, so cell hashes are reused; results are bit-identical to calling
// PaperCreaseField three times.
void CrumpleDisplacement(float nx, float ny, float scale, std::uint32_t seed,
                         float* dx, float* dy, float* crease_shade) {
  constexpr float kEps = 0.008f;
  const float sx[3] = {nx, nx + kEps, nx};
  const float sy[3] = {ny, ny, ny + kEps};
  const float s = std::max(0.25f, scale) * 1.6f;
  float ridge_sum[3] = {0.0f, 0.0f, 0.0f};
  float weight = 0.55f;
  float total_w = 0.0f;
  float freq = s;
  for (int i = 0; i < kCreaseOctaves; ++i) {
    const std::uint32_t oct_seed = seed + static_cast<std::uint32_t>(i) * 211u;
    NoiseCell cell;
    bool have_cell = false;
    for (int k = 0; k < 3; ++k) {
      float rx = 0.0f;
      float ry = 0.0f;
      CreaseOctaveCoords(sx[k], sy[k], freq, i, &rx, &ry);
      const int ix = static_cast<int>(std::floor(rx));
      const int iy = static_cast<int>(std::floor(ry));
      if (!have_cell || cell.ix != ix || cell.iy != iy) {
        cell = HashNoiseCell(ix, iy, oct_seed);
        have_cell = true;
      }
      ridge_sum[k] += RidgeOf(InterpolateNoiseCell(cell, rx, ry)) * weight;
    }
    total_w += weight;
    freq *= 1.95f;
    weight *= 0.55f;
  }
  float c[3];
  for (int k = 0; k < 3; ++k) {
    c[k] = total_w > 0.0f ? Clamp01(ridge_sum[k] / total_w) : 0.0f;
  }
  *dx = (c[1] - c[0]) / kEps;
  *dy = (c[2] - c[0]) / kEps;
  *crease_shade = c[0];
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
// transform over integer-valued input.
//
// `input[q]` is either an exact squared integer distance (< 2^24, so exactly
// representable in float) or >= 1e18 for "no feature". Parabola intersections
// are kept as exact rationals num/den (den > 0) and compared with 64-bit
// cross-multiplication, which avoids a float division per step. Output values
// are (q - v)^2 + input[v], exact integers in float, or 1e20 if the line has no
// feature.
void DistanceTransform1D(const float* input, int n, int* v,
                         std::int64_t* z_num, std::int64_t* z_den,
                         float* output) {
  int k = -1;
  for (int q = 0; q < n; ++q) {
    if (input[q] >= 1e18f) {
      continue;
    }
    const std::int64_t fq =
        static_cast<std::int64_t>(input[q]) + static_cast<std::int64_t>(q) * q;
    if (k < 0) {
      k = 0;
      v[0] = q;
      continue;
    }
    std::int64_t s_num = 0;
    std::int64_t s_den = 1;
    while (true) {
      const int r = v[k];
      const std::int64_t fr = static_cast<std::int64_t>(input[r]) +
                              static_cast<std::int64_t>(r) * r;
      s_num = fq - fr;
      s_den = 2 * static_cast<std::int64_t>(q - r);
      // Pop while s <= z[k]; z[0] is -infinity.
      if (k > 0 && s_num * z_den[k] <= z_num[k] * s_den) {
        --k;
      } else {
        break;
      }
    }
    ++k;
    v[k] = q;
    z_num[k] = s_num;
    z_den[k] = s_den;
  }
  if (k < 0) {
    for (int q = 0; q < n; ++q) {
      output[q] = 1e20f;
    }
    return;
  }
  int env = 0;
  for (int q = 0; q < n; ++q) {
    // Advance while z[env + 1] < q; z[k + 1] is +infinity.
    while (env < k && z_num[env + 1] < static_cast<std::int64_t>(q) *
                                           z_den[env + 1]) {
      ++env;
    }
    const float diff = static_cast<float>(q - v[env]);
    output[q] = diff * diff + input[v[env]];
  }
}

// Exact 2D squared Euclidean distance transform of a binary grid whose cells
// are either 0 (feature) or >= 1e18 (background).
//
// For binary input the column pass reduces to the squared distance to the
// nearest feature cell in the same column, so it is computed with two linear
// sweeps instead of the general lower-envelope algorithm. The values are
// identical to Felzenszwalb-Huttenlocher's (small squared integers are exact in
// float, and columns without features stay at 1e20). The row pass then runs
// the general 1D transform over those squared distances.
void DistanceTransform2D(int width, int height,
                         const ParallelRowsFn& parallel_for,
                         std::vector<float>* grid) {
  if (width <= 0 || height <= 0 || width > kMaxSdfDim || height > kMaxSdfDim) {
    return;
  }
  constexpr int kColBlock = 64;
  const int num_blocks = (width + kColBlock - 1) / kColBlock;
  RunScanlines(0, num_blocks, parallel_for, [&](int block) {
    const int x0 = block * kColBlock;
    const int bw = std::min(kColBlock, width - x0);
    // Distance to the nearest feature above (inclusive), or -1 if none yet.
    std::array<int, kColBlock> last;
    last.fill(-1);
    std::vector<int> down(static_cast<std::size_t>(bw) * height);
    for (int y = 0; y < height; ++y) {
      const float* row = grid->data() + static_cast<std::size_t>(y) * width + x0;
      int* d = down.data() + static_cast<std::size_t>(y) * bw;
      for (int c = 0; c < bw; ++c) {
        if (row[c] < 1e18f) {
          last[c] = y;
        }
        d[c] = last[c] < 0 ? -1 : y - last[c];
      }
    }
    std::array<int, kColBlock> next;
    next.fill(-1);
    for (int y = height - 1; y >= 0; --y) {
      float* row = grid->data() + static_cast<std::size_t>(y) * width + x0;
      const int* d = down.data() + static_cast<std::size_t>(y) * bw;
      for (int c = 0; c < bw; ++c) {
        if (d[c] == 0) {
          next[c] = y;
        }
        int best = d[c];
        if (next[c] >= 0 && (best < 0 || next[c] - y < best)) {
          best = next[c] - y;
        }
        row[c] = best < 0 ? 1e20f
                          : static_cast<float>(best) * static_cast<float>(best);
      }
    }
  });

  RunScanlines(0, height, parallel_for, [&](int y) {
    std::array<float, kMaxSdfDim> line_out;
    std::array<int, kMaxSdfDim> v;
    std::array<std::int64_t, kMaxSdfDim + 1> z_num;
    std::array<std::int64_t, kMaxSdfDim + 1> z_den;
    float* row = grid->data() + static_cast<std::size_t>(y) * width;
    DistanceTransform1D(row, width, v.data(), z_num.data(), z_den.data(),
                        line_out.data());
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

  // Grid coordinate of pixel offset `d`. Power-of-two steps (the common 1080p
  // and UHD cases) use an exact reciprocal multiply instead of a division.
  float ToGrid(float d) const {
    if ((step & (step - 1)) == 0) {
      return d * (1.0f / static_cast<float>(step));
    }
    return d / static_cast<float>(step);
  }

  float SamplePx(float px, float py) const {
    if (width <= 0 || height <= 0) {
      return 1e6f;
    }
    const float gx = Clamp(ToGrid(px - origin_x) - 0.5f, 0.0f,
                           static_cast<float>(width - 1));
    const float gy = Clamp(ToGrid(py - origin_y) - 0.5f, 0.0f,
                           static_cast<float>(height - 1));
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
  float raw_p = 1.0f;
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
  // Without tilt or perspective this divides by exactly 1, so skip it.
  float px = ax;
  float py = ay;
  if (anim.perspective_k != 0.0f || anim.perspective_ky != 0.0f ||
      anim.tilt_y_cos != 1.0f) {
    const float w_persp =
        std::max(0.15f, 1.0f + anim.perspective_k * (ax / cw) +
                            anim.perspective_ky * (ay / ch));
    px = ax / (anim.tilt_y_cos * w_persp);
    py = ay / w_persp;
  }

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

RgbaColor ScalePremult(const RgbaColor& c, float s) {
  return {c.r * s, c.g * s, c.b * s, c.a * s};
}

// Samples a clip in canonical canvas pixel space [origin_x, origin_x + cw] x
// [origin_y, origin_y + ch], clamping inside full-frame RoDs so unwarped frames
// match the raw timeline clip without a half-pixel edge vignette, while keeping
// cropped sub-tiles transparent outside `view.bounds`.
RgbaColor SampleClipCanonicalPx(const ImageView& view,
                                const PixelRect& canvas_bounds, float px,
                                float py) {
  if (!IsValidRgbaView(view)) {
    return {};
  }
  const float ox = static_cast<float>(canvas_bounds.x1);
  const float oy = static_cast<float>(canvas_bounds.y1);
  const float cw = static_cast<float>(std::max(1, canvas_bounds.x2 - canvas_bounds.x1));
  const float ch = static_cast<float>(std::max(1, canvas_bounds.y2 - canvas_bounds.y1));
  if (px < ox - 0.5f || px > ox + cw + 0.5f || py < oy - 0.5f ||
      py > oy + ch + 0.5f) {
    return {};
  }

  const PixelRect rod = EffectiveRod(view);
  const float rod_x1 = static_cast<float>(rod.x1);
  const float rod_y1 = static_cast<float>(rod.y1);
  const float rod_w = static_cast<float>(std::max(1, rod.x2 - rod.x1));
  const float rod_h = static_cast<float>(std::max(1, rod.y2 - rod.y1));
  const float sx = rod_x1 + ((px - ox) / cw) * rod_w;
  const float sy = rod_y1 + ((py - oy) / ch) * rod_h;
  return SampleBilinearTransparentOutside(view, sx, sy);
}

struct ClipContentSummary {
  bool has_visible_alpha = false;
  bool has_nonblack_rgb = false;
  bool is_full_frame_opaque = false;
};

ClipContentSummary InspectClipContent(const ImageView& view,
                                      const PixelRect& canvas_bounds) {
  ClipContentSummary out;
  if (!IsValidRgbaView(view)) {
    return out;
  }
  const PixelRect rod = EffectiveRod(view);
  const bool covers_canvas =
      view.bounds.x1 <= rod.x1 && view.bounds.x2 >= rod.x2 &&
      view.bounds.y1 <= rod.y1 && view.bounds.y2 >= rod.y2 &&
      (rod.x2 - rod.x1) >= (canvas_bounds.x2 - canvas_bounds.x1) &&
      (rod.y2 - rod.y1) >= (canvas_bounds.y2 - canvas_bounds.y1);

  float min_a = 1.0f;
  constexpr int kGridX = 24;
  constexpr int kGridY = 16;
  const int bw = std::max(1, view.bounds.x2 - view.bounds.x1);
  const int bh = std::max(1, view.bounds.y2 - view.bounds.y1);
  for (int gy = 0; gy < kGridY; ++gy) {
    const int iy = view.bounds.y1 + ((gy * 2 + 1) * bh) / (kGridY * 2);
    for (int gx = 0; gx < kGridX; ++gx) {
      const int ix = view.bounds.x1 + ((gx * 2 + 1) * bw) / (kGridX * 2);
      const RgbaColor c = ReadPixel(view, ix, iy);
      if (c.a > 1e-3f) {
        out.has_visible_alpha = true;
      }
      if (c.r + c.g + c.b > 3e-3f) {
        out.has_nonblack_rgb = true;
      }
      min_a = std::min(min_a, c.a);
    }
  }
  out.is_full_frame_opaque = covers_canvas && (min_a >= 0.995f);
  return out;
}

constexpr int kMaxFoldHinges = 5;

struct FoldHinge {
  float px = 0.0f;  // Anchor offset relative to sheet center (sheet_cx, sheet_cy)
  float py = 0.0f;
  float nx = 1.0f;  // Unit normal pointing toward folding flap side (d > 0)
  float ny = 0.0f;
  float tx = 0.0f;  // Unit tangent (-ny, nx)
  float ty = 1.0f;
  float phi = 0.0f;  // Current fold angle in [0, pi]; 0 = flat, ~pi = folded over
  float cos_phi = 1.0f;
  float sin_phi = 0.0f;
};

struct FoldHingeSchedule {
  std::array<FoldHinge, kMaxFoldHinges> hinges;
  int count = 0;
  float packet_scale = 1.0f;
  float packet_alpha = 1.0f;
  float slide_x_px = 0.0f;
  float slide_y_px = 0.0f;
  float packet_rot_cos = 1.0f;
  float packet_rot_sin = 0.0f;
  float sheet_diag = 1.0f;
  float persp_k = 0.16f;
};

// Populates sequential fold hinges around a stationary packet core so that
// every hinge satisfies d_k(core) < 0 regardless of aspect ratio or angle.
FoldHingeSchedule BuildFoldHinges(FoldPattern pattern, int requested_stages,
                                  float openness, float sheet_w, float sheet_h,
                                  float cw, float ch, float direction_deg,
                                  float fold_depth, bool mirror_h,
                                  bool mirror_v) {
  FoldHingeSchedule sched;
  const int stages = std::min(kMaxFoldHinges, std::max(2, requested_stages));
  sched.count = stages;
  sched.sheet_diag = std::max(64.0f, std::hypot(sheet_w, sheet_h));
  sched.persp_k = 0.28f * Clamp(fold_depth, 0.0f, 1.5f);

  const float q = Clamp01(openness);
  const float mh = mirror_h ? -1.0f : 1.0f;
  const float mv = mirror_v ? -1.0f : 1.0f;

  // Compact folded-packet entrance/exit scale, slide, and hand-tossed rotation
  // in the earliest ~26% of openness; completely neutral for q >= 0.28.
  sched.packet_alpha = Smoothstep(0.0f, 0.055f, q);
  sched.packet_scale =
      Lerp(0.22f, 1.0f, Smoothstep(0.0f, 0.24f, q));
  const float slide_env = 1.0f - Smoothstep(0.0f, 0.26f, q);
  const float dir_rad = direction_deg * kDegToRad;
  sched.slide_x_px = -mh * 0.07f * cw * slide_env * std::cos(dir_rad);
  sched.slide_y_px = -mv * 0.07f * ch * slide_env * std::sin(dir_rad);
  const float pkt_rot = -mh * mv * 11.0f * kDegToRad * slide_env;
  sched.packet_rot_cos = std::cos(pkt_rot);
  sched.packet_rot_sin = std::sin(pkt_rot);

  const float rx = std::max(16.0f, 0.5f * sheet_w);
  const float ry = std::max(16.0f, 0.5f * sheet_h);

  struct HingeTemplate {
    float deg;
    float frac;
    float max_deg;
  };
  std::array<HingeTemplate, kMaxFoldHinges> tpl{};
  float core_x = 0.0f;
  float core_y = 0.0f;

  auto make_normal = [&](float deg, float* nx_out, float* ny_out) {
    const float rad = (deg + direction_deg) * kDegToRad;
    *nx_out = std::cos(rad) * mh;
    *ny_out = std::sin(rad) * mv;
  };

  if (pattern == FoldPattern::kAccordion) {
    float nx = 1.0f;
    float ny = 0.0f;
    make_normal(0.0f, &nx, &ny);
    const float sup = rx * std::abs(nx) + ry * std::abs(ny);
    core_x = -0.68f * sup * nx;
    core_y = -0.68f * sup * ny;
    for (int k = 0; k < stages; ++k) {
      // k = stages - 1 opens first (closest to core); k = 0 opens last.
      const float t_strip =
          static_cast<float>(stages - k) / static_cast<float>(stages + 1);
      tpl[k] = {0.0f, 0.14f + 0.68f * t_strip,
                (k % 2 == 0) ? 171.0f : 167.0f};
    }
  } else if (pattern == FoldPattern::kMapGrid) {
    core_x = -0.18f * rx * mh;
    core_y = -0.18f * ry * mv;
    constexpr HingeTemplate kMap5[5] = {
        {-90.0f, 0.44f, 168.0f},
        {180.0f, 0.44f, 169.0f},
        {0.0f, 0.46f, 171.0f},
        {90.0f, 0.18f, 174.0f},
        {0.0f, 0.16f, 175.0f},
    };
    const int offset = 5 - stages;
    for (int k = 0; k < stages; ++k) {
      tpl[k] = kMap5[offset + k];
    }
  } else if (pattern == FoldPattern::kOrigamiCorners) {
    core_x = -0.08f * rx * mh;
    core_y = -0.06f * ry * mv;
    constexpr HingeTemplate kOrigami5[5] = {
        {-45.0f, 0.44f, 164.0f},
        {135.0f, 0.44f, 165.0f},
        {-135.0f, 0.42f, 166.0f},
        {45.0f, 0.40f, 168.0f},
        {8.0f, 0.15f, 174.0f},
    };
    for (int k = 0; k < stages; ++k) {
      tpl[k] = (stages == 5) ? kOrigami5[k] : kOrigami5[k];
    }
    if (stages < 5) {
      // Ensure outermost stage folds half the packet before corners open.
      tpl[stages - 1] = {8.0f, 0.16f, 173.0f};
    }
  } else {
    // FoldPattern::kCraftFlaps (default Unfold Paper Animator look):
    // packet half-folds open first, followed by asymmetric edge & corner flaps.
    core_x = -0.14f * rx * mh;
    core_y = -0.10f * ry * mv;
    if (stages == 2) {
      tpl[0] = {138.0f, 0.46f, 165.0f};
      tpl[1] = {7.0f, 0.15f, 174.0f};
    } else if (stages == 3) {
      tpl[0] = {138.0f, 0.48f, 164.0f};
      tpl[1] = {-76.0f, 0.44f, 167.0f};
      tpl[2] = {7.0f, 0.15f, 174.0f};
    } else if (stages == 4) {
      tpl[0] = {138.0f, 0.50f, 164.0f};
      tpl[1] = {-76.0f, 0.45f, 167.0f};
      tpl[2] = {96.0f, 0.16f, 173.0f};
      tpl[3] = {7.0f, 0.14f, 175.0f};
    } else {
      tpl[0] = {42.0f, 0.54f, 162.0f};
      tpl[1] = {138.0f, 0.50f, 164.0f};
      tpl[2] = {-76.0f, 0.45f, 167.0f};
      tpl[3] = {96.0f, 0.16f, 173.0f};
      tpl[4] = {7.0f, 0.14f, 175.0f};
    }
  }

  const float u_fold = Clamp01((q - 0.06f) / 0.94f);
  constexpr float kOverlap = 0.22f;
  const float span =
      1.0f / (static_cast<float>(stages) -
              kOverlap * static_cast<float>(stages - 1));

  for (int k = 0; k < stages; ++k) {
    FoldHinge& h = sched.hinges[k];
    make_normal(tpl[k].deg, &h.nx, &h.ny);
    h.tx = -h.ny;
    h.ty = h.nx;

    const float sup = rx * std::abs(h.nx) + ry * std::abs(h.ny);
    const float core_proj = core_x * h.nx + core_y * h.ny;
    const float reach = std::max(12.0f, sup - core_proj);
    const float dist_from_core = tpl[k].frac * reach;
    h.px = core_x + dist_from_core * h.nx;
    h.py = core_y + dist_from_core * h.ny;

    // Stage k = stages - 1 opens first (order = 0); k = 0 opens last.
    const int order = (stages - 1) - k;
    const float start = static_cast<float>(order) * span * (1.0f - kOverlap);
    const float local_t = Clamp01((u_fold - start) / span);
    const float ease = local_t * local_t * (3.0f - 2.0f * local_t);
    h.phi = (tpl[k].max_deg * kDegToRad) * (1.0f - ease);
    h.cos_phi = std::cos(h.phi);
    h.sin_phi = std::sin(h.phi);
  }
  return sched;
}

struct SheetGeometry {
  bool has_sheet = false;
  bool use_analytic_box = false;
  const CoarseSignedDistanceField* sdf = nullptr;
  float box_cx = 0.0f;
  float box_cy = 0.0f;
  float box_half_w = 1.0f;
  float box_half_h = 1.0f;
  float sheet_cx = 0.0f;
  float sheet_cy = 0.0f;
  float sheet_w = 1.0f;
  float sheet_h = 1.0f;

  float SampleDistPx(float ux, float uy) const {
    if (use_analytic_box) {
      const float dx = std::abs(ux - box_cx) - box_half_w;
      const float dy = std::abs(uy - box_cy) - box_half_h;
      const float outside = std::hypot(std::max(dx, 0.0f), std::max(dy, 0.0f));
      const float inside = std::min(std::max(dx, dy), 0.0f);
      return outside + inside;
    }
    return sdf != nullptr ? sdf->SamplePx(ux, uy) : 1e6f;
  }
};

SheetGeometry BuildSheetGeometryFromSdf(const CoarseSignedDistanceField& sdf,
                                        const PixelRect& canvas_bounds,
                                        float expand_px) {
  SheetGeometry geom;
  geom.sdf = &sdf;
  const float step_f = static_cast<float>(std::max(1, sdf.step));
  const float threshold = expand_px + step_f;
  int min_gx = sdf.width;
  int max_gx = -1;
  int min_gy = sdf.height;
  int max_gy = -1;
  for (int gy = 0; gy < sdf.height; ++gy) {
    const std::size_t row_off = static_cast<std::size_t>(gy) * sdf.width;
    for (int gx = 0; gx < sdf.width; ++gx) {
      if (sdf.signed_dist_px[row_off + gx] <= threshold) {
        min_gx = std::min(min_gx, gx);
        max_gx = std::max(max_gx, gx);
        min_gy = std::min(min_gy, gy);
        max_gy = std::max(max_gy, gy);
      }
    }
  }
  if (min_gx > max_gx || min_gy > max_gy) {
    geom.has_sheet = false;
    return geom;
  }
  geom.has_sheet = true;
  const float x1 = std::max(static_cast<float>(canvas_bounds.x1) - expand_px,
                            sdf.origin_x + static_cast<float>(min_gx) * step_f);
  const float x2 =
      std::min(static_cast<float>(canvas_bounds.x2) + expand_px,
               sdf.origin_x + static_cast<float>(max_gx + 1) * step_f);
  const float y1 = std::max(static_cast<float>(canvas_bounds.y1) - expand_px,
                            sdf.origin_y + static_cast<float>(min_gy) * step_f);
  const float y2 =
      std::min(static_cast<float>(canvas_bounds.y2) + expand_px,
               sdf.origin_y + static_cast<float>(max_gy + 1) * step_f);
  geom.sheet_cx = 0.5f * (x1 + x2);
  geom.sheet_cy = 0.5f * (y1 + y2);
  geom.sheet_w = std::max(32.0f, x2 - x1);
  geom.sheet_h = std::max(32.0f, y2 - y1);
  return geom;
}

struct PaperFoldRenderContext {
  const SheetGeometry* sheet = nullptr;
  const FoldHingeSchedule* sched = nullptr;
  float origin_x = 0.0f;
  float origin_y = 0.0f;
  float min_dim = 1.0f;
  bool enable_border = true;
  float border_px = 0.0f;
  float tear_amp_px = 0.0f;
  float tear_frequency = 24.0f;
  int tear_detail = 5;
  float paper_r = 0.965f;
  float paper_g = 0.945f;
  float paper_b = 0.895f;
  float grain_strength = 0.35f;
  float grain_scale = 1.0f;
  float crease_strength = 0.45f;
  float crease_scale = 3.2f;
  float media_overlay = 0.24f;
  float hinge_crease_boost = 1.0f;
  float flap_shadow_opacity = 0.60f;
  float flap_sh_dx_px = 5.0f;
  float flap_sh_dy_px = 5.0f;
  std::uint32_t seed = 7u;
  std::uint32_t boil_seed = 7u;
};

// Fast SDF-only alpha lookup through the folded hinge stack (used for cast
// shadows so occluded/shadow rays skip texture noise and source pixel reads).
float EvalFoldStackAlphaOnly(const PaperFoldRenderContext& ctx, int k, float x,
                             float y, bool view_flipped) {
  while (k >= 0) {
    const FoldHinge& h = ctx.sched->hinges[k];
    if (h.phi <= 1e-3f) {
      --k;
      continue;
    }
    const float d = (x - h.px) * h.nx + (y - h.py) * h.ny;
    const float t = (x - h.px) * h.tx + (y - h.py) * h.ty;
    if (h.cos_phi >= 0.0f) {
      if (d > 0.0f) {
        const float s_flat = d / std::max(0.03f, h.cos_phi);
        const float z_lift = s_flat * h.sin_phi;
        const float persp =
            1.0f + ctx.sched->persp_k * (z_lift / ctx.sched->sheet_diag);
        const float t_flat = t / persp;
        x = h.px + s_flat * h.nx + t_flat * h.tx;
        y = h.py + s_flat * h.ny + t_flat * h.ty;
      }
      --k;
      continue;
    }
    if (d >= 1.5f) {
      return 0.0f;
    }
    const float cov = Smoothstep(1.5f, -1.5f, d);
    const float d_clamped = std::min(0.0f, d);
    const float s_flat = (-d_clamped) / std::max(0.03f, -h.cos_phi);
    const float z_lift = s_flat * h.sin_phi + 4.0f;
    const float persp =
        1.0f + ctx.sched->persp_k * (z_lift / ctx.sched->sheet_diag);
    const float t_flat = t / persp;
    const float xf = h.px + s_flat * h.nx + t_flat * h.tx;
    const float yf = h.py + s_flat * h.ny + t_flat * h.ty;

    if (!view_flipped) {
      const float top_a = EvalFoldStackAlphaOnly(ctx, k - 1, xf, yf, true);
      if (top_a >= 0.99f) {
        return top_a * cov;
      }
      const float bot_a = EvalFoldStackAlphaOnly(ctx, k - 1, x, y, false);
      return (top_a + bot_a * (1.0f - top_a)) * cov;
    } else {
      const float top_a = EvalFoldStackAlphaOnly(ctx, k - 1, x, y, true);
      if (top_a >= 0.99f) {
        return top_a * cov;
      }
      const float bot_a = EvalFoldStackAlphaOnly(ctx, k - 1, xf, yf, false);
      return (top_a + bot_a * (1.0f - top_a)) * cov;
    }
  }

  const float ux = ctx.sheet->sheet_cx + x;
  const float uy = ctx.sheet->sheet_cy + y;
  const float edge_dist = ctx.sheet->SampleDistPx(ux, uy) - ctx.border_px;
  return Smoothstep(2.0f, -2.0f, edge_dist);
}

// Evaluates one flat canonical leaf pixel (either front subject + cardstock
// border, or textured cream/kraft paper backside when `view_flipped` is true).
template <typename SubjectSamplerFn>
RgbaColor EvalFlatPaperLeaf(const PaperFoldRenderContext& ctx, float x, float y,
                            bool view_flipped, float shade_mul,
                            const SubjectSamplerFn& sample_subject) {
  const float ux = ctx.sheet->sheet_cx + x;
  const float uy = ctx.sheet->sheet_cy + y;
  const float dist_px = ctx.sheet->SampleDistPx(ux, uy);
  const float base_edge_dist = dist_px - ctx.border_px;

  const float nu_x = (ux - ctx.origin_x) / ctx.min_dim;
  const float nu_y = (uy - ctx.origin_y) / ctx.min_dim;
  const float tear_n =
      (ctx.enable_border && ctx.tear_amp_px > 0.01f &&
       std::abs(base_edge_dist) <= ctx.tear_amp_px + 3.1f)
          ? TearEdgeNoise(nu_x, nu_y, ctx.tear_frequency, ctx.tear_detail,
                          ctx.boil_seed)
          : 0.0f;
  const float edge_sdf = base_edge_dist - ctx.tear_amp_px * tear_n;
  const float sheet_alpha = Smoothstep(1.2f, -1.2f, edge_sdf);
  if (sheet_alpha <= 1e-4f) {
    return {};
  }

  const float grain =
      Lerp(1.0f, PaperFiberGrain(nu_x, nu_y, ctx.grain_scale, ctx.seed),
           Clamp01(ctx.grain_strength));
  const float crease_val =
      PaperCreaseField(nu_x, nu_y, ctx.crease_scale, ctx.seed + 89u);

  // Subtle organic fold-hinge crease marks along every fold line on the sheet.
  float hinge_mark = 1.0f;
  if (ctx.crease_strength > 1e-3f && ctx.hinge_crease_boost > 1e-3f) {
    const float wobble =
        (ValueNoise2D(nu_x * 18.0f, nu_y * 18.0f, ctx.seed + 313u) - 0.5f) *
        3.2f;
    const float band_px = std::max(2.5f, 0.0055f * ctx.min_dim);
    for (int j = 0; j < ctx.sched->count; ++j) {
      const FoldHinge& hj = ctx.sched->hinges[j];
      const float hd =
          std::abs((x - hj.px) * hj.nx + (y - hj.py) * hj.ny + wobble);
      if (hd < band_px * 2.2f) {
        const float valley = Smoothstep(band_px, 0.0f, hd);
        const float ridge =
            Smoothstep(band_px * 2.2f, band_px * 0.9f, hd) * (1.0f - valley);
        hinge_mark *=
            1.0f + ctx.crease_strength * ctx.hinge_crease_boost *
                       (-0.26f * valley + 0.09f * ridge);
      }
    }
  }

  const float crease_mul =
      (1.0f - 0.34f * Clamp01(ctx.crease_strength) * crease_val) * hinge_mark;
  const float rim_bright =
      1.0f + 0.08f * Smoothstep(3.0f, 0.0f, std::abs(edge_sdf));

  if (view_flipped) {
    // Paper reverse side: warm slightly toned cardstock with fiber grain,
    // crease ridges, and subtle front-ink bleed-through for realism.
    const RgbaColor subj_hint = sample_subject(ux, uy, dist_px);
    const float luma_bleed =
        subj_hint.a > 1e-3f
            ? (0.299f * subj_hint.r + 0.587f * subj_hint.g +
               0.114f * subj_hint.b) /
                  std::max(0.05f, subj_hint.a)
            : 1.0f;
    const float bleed_mul = Lerp(1.0f, 0.88f + 0.12f * luma_bleed,
                                 0.28f * Clamp01(subj_hint.a));
    const float back_shade =
        Clamp(0.94f * grain * crease_mul * rim_bright * shade_mul * bleed_mul,
              0.24f, 1.12f);
    return {
        Clamp01(ctx.paper_r * back_shade) * sheet_alpha,
        Clamp01(ctx.paper_g * 0.99f * back_shade) * sheet_alpha,
        Clamp01(ctx.paper_b * 0.96f * back_shade) * sheet_alpha,
        sheet_alpha,
    };
  }

  const float front_shade =
      Clamp(grain * crease_mul * rim_bright * shade_mul, 0.24f, 1.15f);
  const RgbaColor paper_fg = {
      Clamp01(ctx.paper_r * front_shade) * sheet_alpha,
      Clamp01(ctx.paper_g * front_shade) * sheet_alpha,
      Clamp01(ctx.paper_b * front_shade) * sheet_alpha,
      sheet_alpha,
  };

  RgbaColor subject = sample_subject(ux, uy, dist_px);
  if (subject.a > 1e-4f) {
    const float ink_tex =
        Lerp(1.0f, grain * crease_mul, Clamp01(ctx.media_overlay));
    const float total_ink_mul = Clamp(ink_tex * shade_mul, 0.25f, 1.15f);
    subject.r = Clamp(subject.r * total_ink_mul, 0.0f, subject.a);
    subject.g = Clamp(subject.g * total_ink_mul, 0.0f, subject.a);
    subject.b = Clamp(subject.b * total_ink_mul, 0.0f, subject.a);
  }

  if (ctx.enable_border && ctx.border_px > 0.05f) {
    return CompositeOver(subject, paper_fg);
  }
  return ScalePremult(subject, sheet_alpha);
}

// Top-to-bottom folded origami stack evaluator with exact view-parity occlusion
// and cast shadows from folded flaps onto underlying paper layers.
template <typename SubjectSamplerFn>
RgbaColor EvalFoldStackRgba(const PaperFoldRenderContext& ctx, int k, float x,
                            float y, bool view_flipped, float shade_mul,
                            const SubjectSamplerFn& sample_subject) {
  const float valley_w = std::max(4.0f, 0.018f * ctx.min_dim);
  while (k >= 0) {
    const FoldHinge& h = ctx.sched->hinges[k];
    if (h.phi <= 1e-3f) {
      --k;
      continue;
    }
    const float d = (x - h.px) * h.nx + (y - h.py) * h.ny;
    const float t = (x - h.px) * h.tx + (y - h.py) * h.ty;

    if (h.cos_phi >= 0.0f) {
      // Flap is open past 90 degrees: single-valued mapping, no overlap.
      if (d <= 0.0f) {
        const float ao =
            1.0f - 0.14f * h.sin_phi * Smoothstep(-valley_w, 0.0f, d);
        shade_mul *= ao;
      } else {
        const float s_flat = d / std::max(0.03f, h.cos_phi);
        const float z_lift = s_flat * h.sin_phi;
        const float persp =
            1.0f + ctx.sched->persp_k * (z_lift / ctx.sched->sheet_diag);
        const float t_flat = t / persp;
        x = h.px + s_flat * h.nx + t_flat * h.tx;
        y = h.py + s_flat * h.ny + t_flat * h.ty;
        const float light_dot = h.nx * (-0.62f) + h.ny * (-0.78f);
        const float flap_light =
            Clamp(0.93f + 0.09f * h.cos_phi + 0.10f * h.sin_phi * light_dot,
                  0.74f, 1.08f);
        const float ao =
            1.0f - 0.15f * h.sin_phi * Smoothstep(valley_w, 0.0f, s_flat);
        shade_mul *= flap_light * ao;
      }
      --k;
      continue;
    }

    // Flap is folded over (> 90 degrees) onto the d <= 0 stationary side.
    if (d >= 1.25f) {
      return {};
    }
    const float fold_cov = Smoothstep(1.25f, -1.25f, d);
    const float d_clamped = std::min(0.0f, d);
    const float abs_c = std::max(0.03f, -h.cos_phi);
    const float s_flat = (-d_clamped) / abs_c;
    const float z_lift = s_flat * h.sin_phi + 4.0f;
    const float persp =
        1.0f + ctx.sched->persp_k * (z_lift / ctx.sched->sheet_diag);
    const float t_flat = t / persp;
    const float xf = h.px + s_flat * h.nx + t_flat * h.tx;
    const float yf = h.py + s_flat * h.ny + t_flat * h.ty;

    const float light_dot = h.nx * (-0.65f) + h.ny * (-0.76f);
    const float flap_shade =
        Clamp(0.91f + 0.09f * light_dot * (-h.cos_phi) + 0.05f * h.sin_phi,
              0.74f, 1.04f);

    if (!view_flipped) {
      const RgbaColor top = EvalFoldStackRgba(
          ctx, k - 1, xf, yf, true, shade_mul * flap_shade, sample_subject);
      if (top.a >= 0.995f && fold_cov >= 0.999f) {
        return top;
      }
      RgbaColor bot = EvalFoldStackRgba(ctx, k - 1, x, y, false, shade_mul,
                                        sample_subject);
      if (bot.a > 1e-4f && ctx.flap_shadow_opacity > 1e-4f) {
        const float tuck_ao =
            0.24f * ctx.flap_shadow_opacity * Smoothstep(-valley_w, 0.0f, d);
        const float sh_x = x - ctx.flap_sh_dx_px;
        const float sh_y = y - ctx.flap_sh_dy_px;
        const float d_sh = std::min(0.0f, (sh_x - h.px) * h.nx + (sh_y - h.py) * h.ny);
        const float t_sh = (sh_x - h.px) * h.tx + (sh_y - h.py) * h.ty;
        const float s_sh = (-d_sh) / abs_c;
        const float xf_sh = h.px + s_sh * h.nx + t_sh * h.tx;
        const float yf_sh = h.py + s_sh * h.ny + t_sh * h.ty;
        const float flap_cov = EvalFoldStackAlphaOnly(ctx, k - 1, xf_sh, yf_sh, true);
        const float sh_dark =
            Clamp(std::max(tuck_ao, 0.44f * ctx.flap_shadow_opacity * flap_cov),
                  0.0f, 0.65f);
        bot.r *= (1.0f - sh_dark);
        bot.g *= (1.0f - sh_dark);
        bot.b *= (1.0f - sh_dark);
      }
      return ScalePremult(CompositeOver(top, bot), fold_cov);
    } else {
      const RgbaColor top = EvalFoldStackRgba(ctx, k - 1, x, y, true, shade_mul,
                                              sample_subject);
      if (top.a >= 0.995f && fold_cov >= 0.999f) {
        return top;
      }
      const RgbaColor bot = EvalFoldStackRgba(
          ctx, k - 1, xf, yf, false, shade_mul * flap_shade * 0.85f,
          sample_subject);
      return ScalePremult(CompositeOver(top, bot), fold_cov);
    }
  }

  return EvalFlatPaperLeaf(ctx, x, y, view_flipped, shade_mul, sample_subject);
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

  // Real multi-stage paper folding/unfolding for Paper Unfold (3D) intro/outro.
  if (anim.style == CutoutAnimStyle::kPaperUnfold && anim.raw_p < 1.0f) {
    const SheetGeometry sheet_geom =
        BuildSheetGeometryFromSdf(sdf, canvas_bounds, border_px + tear_amp_px);
    if (!sheet_geom.has_sheet) {
      RunScanlines(y_begin, y_end, parallel_for, [&](int y) {
        for (int x = x_begin; x < x_end; ++x) {
          WritePixel(output, x, y, {});
        }
      });
      return;
    }

    const float fold_openness = Lerp(anim.raw_p, anim.ease_p, 0.45f);
    const FoldHingeSchedule sched = BuildFoldHinges(
        FoldPattern::kCraftFlaps, 4, fold_openness, sheet_geom.sheet_w,
        sheet_geom.sheet_h, cw, ch, params.transition_angle_deg + 22.0f, 0.55f,
        params.mirror_transition_h, params.mirror_transition_v);

    PaperFoldRenderContext fctx;
    fctx.sheet = &sheet_geom;
    fctx.sched = &sched;
    fctx.origin_x = origin_x;
    fctx.origin_y = origin_y;
    fctx.min_dim = min_dim;
    fctx.enable_border = params.enable_border;
    fctx.border_px = border_px;
    fctx.tear_amp_px = tear_amp_px;
    fctx.tear_frequency = params.tear_frequency;
    fctx.tear_detail = params.tear_detail;
    fctx.paper_r = params.paper_r;
    fctx.paper_g = params.paper_g;
    fctx.paper_b = params.paper_b;
    fctx.grain_strength = params.grain_strength;
    fctx.grain_scale = params.grain_scale;
    fctx.crease_strength = std::max(0.30f, params.crease_strength);
    fctx.crease_scale = params.crease_scale;
    fctx.media_overlay =
        std::max(params.media_texture_overlay, 0.28f * (1.0f - fold_openness));
    fctx.hinge_crease_boost = 1.0f - fold_openness * fold_openness;
    fctx.flap_shadow_opacity = std::max(0.45f, params.shadow_opacity);
    fctx.flap_sh_dx_px = std::max(3.0f, 0.011f * min_dim);
    fctx.flap_sh_dy_px = std::max(3.0f, 0.011f * min_dim);
    fctx.seed = seed;
    fctx.boil_seed = boil_seed;

    auto sample_cutout_subject = [&](float ux, float uy,
                                     float dist_px) -> RgbaColor {
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
      return subject;
    };

    const float cx_out =
        origin_x + params.master_pos_x * cw + anim.jitter_dx_px;
    const float cy_out =
        origin_y + params.master_pos_y * ch + anim.jitter_dy_px;
    const float sheet_dx = sheet_geom.sheet_cx - (origin_x + 0.5f * cw);
    const float sheet_dy = sheet_geom.sheet_cy - (origin_y + 0.5f * ch);
    const float inv_pkt_scale = 1.0f / std::max(0.05f, sched.packet_scale);

    auto map_to_sheet_local = [&](float px_in, float py_in, float* lx,
                                  float* ly) {
      const float dx = px_in - cx_out;
      const float dy = py_in - cy_out;
      float rx = (dx * anim.cos_m - dy * anim.sin_m) * anim.inv_m_scale;
      float ry = (dx * anim.sin_m + dy * anim.cos_m) * anim.inv_m_scale;
      if (params.flip_h) {
        rx = -rx;
      }
      if (params.flip_v) {
        ry = -ry;
      }
      rx -= sheet_dx + sched.slide_x_px;
      ry -= sheet_dy + sched.slide_y_px;
      *lx = (rx * sched.packet_rot_cos - ry * sched.packet_rot_sin) *
            inv_pkt_scale;
      *ly = (rx * sched.packet_rot_sin + ry * sched.packet_rot_cos) *
            inv_pkt_scale;
    };

    RunScanlines(y_begin, y_end, parallel_for, [&](int y) {
      const float py_out = static_cast<float>(y) + 0.5f;
      for (int x = x_begin; x < x_end; ++x) {
        const float px_out = static_cast<float>(x) + 0.5f;
        float lx = 0.0f;
        float ly = 0.0f;
        map_to_sheet_local(px_out, py_out, &lx, &ly);

        const RgbaColor fg = ScalePremult(
            EvalFoldStackRgba(fctx, sched.count - 1, lx, ly, false, 1.0f,
                              sample_cutout_subject),
            sched.packet_alpha);

        RgbaColor shadow_rgba{};
        if (fg.a < 0.999f && params.shadow_opacity > 1e-4f) {
          float slx = 0.0f;
          float sly = 0.0f;
          map_to_sheet_local(px_out - sh_dx_px, py_out - sh_dy_px, &slx, &sly);
          const float sh_cov =
              EvalFoldStackAlphaOnly(fctx, sched.count - 1, slx, sly, false);
          shadow_rgba = {0.0f, 0.0f, 0.0f,
                         params.shadow_opacity * sched.packet_alpha * sh_cov};
        }
        WritePixel(output, x, y, CompositeOver(fg, shadow_rgba));
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

      // Soft 3D drop shadow behind the paper cutout, evaluated without the
      // multi-octave crumple displacement. Only needed where the cutout is not
      // fully opaque.
      auto shadow_at_pixel = [&]() -> RgbaColor {
        float shadow_alpha = 0.0f;
        if (params.shadow_opacity > 1e-4f) {
          float sux = 0.0f;
          float suy = 0.0f;
          float unused_crease = 0.0f;
          MapOutputToPaperCanonical(params, anim, cw, ch, origin_x, origin_y,
                                    px_out - sh_dx_px, py_out - sh_dy_px,
                                    false, &sux, &suy, &unused_crease);
          const float s_dist_px = sdf.SamplePx(sux, suy) - border_px;
          shadow_alpha =
              params.shadow_opacity * Smoothstep(sh_soft_px, -2.0f, s_dist_px);
        }
        return {0.0f, 0.0f, 0.0f, shadow_alpha};
      };

      if (sheet_alpha <= 1e-4f) {
        WritePixel(output, x, y, shadow_at_pixel());
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

      // Over() ignores the background once fg.a >= 1, so skip the shadow.
      const RgbaColor shadow_rgba =
          cutout_fg.a >= 1.0f ? RgbaColor{} : shadow_at_pixel();
      WritePixel(output, x, y, CompositeOver(cutout_fg, shadow_rgba));
    }
  });
}

float ComputeSteppedTransitionProgress(
    const PaperFoldTransitionParams& params) {
  const float raw_p = Clamp01(params.progress);
  if (raw_p <= 1e-5f) {
    return 0.0f;
  }
  if (raw_p >= 1.0f - 1e-5f) {
    return 1.0f;
  }
  if (!params.enable_stop_motion || params.stop_motion_steps < 2) {
    return raw_p;
  }
  const float steps = static_cast<float>(std::max(2, params.stop_motion_steps));
  const float bucket = std::floor(std::min(0.9999f, raw_p) * steps);
  return Clamp01((bucket + 0.5f) / steps);
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
    if (from_view.alpha_mode == AlphaMode::kPremultiplied) {
      from_view.alpha_mode = AlphaMode::kUnPremultiplied;
    }
    if (to_view.alpha_mode == AlphaMode::kPremultiplied) {
      to_view.alpha_mode = AlphaMode::kUnPremultiplied;
    }
  }

  const float p = ComputeSteppedTransitionProgress(params);
  auto copy_clip_to_output = [&](const ImageView& src) {
    if (!IsValidRgbaView(src)) {
      RunScanlines(y_begin, y_end, parallel_for, [&](int y) {
        for (int x = x_begin; x < x_end; ++x) {
          WritePixel(output, x, y, {});
        }
      });
      return;
    }
    RunScanlines(y_begin, y_end, parallel_for, [&](int y) {
      const float py = static_cast<float>(y) + 0.5f;
      for (int x = x_begin; x < x_end; ++x) {
        const float px = static_cast<float>(x) + 0.5f;
        WritePixel(output, x, y,
                   SampleClipCanonicalPx(src, canvas_bounds, px, py));
      }
    });
  };

  if (p <= 1e-5f) {
    copy_clip_to_output(from_view);
    return;
  }
  if (p >= 1.0f - 1e-5f) {
    copy_clip_to_output(to_view);
    return;
  }

  const ClipContentSummary from_info =
      InspectClipContent(from_view, canvas_bounds);
  const ClipContentSummary to_info = InspectClipContent(to_view, canvas_bounds);

  FoldTransitionAction action = static_cast<FoldTransitionAction>(
      Clamp(static_cast<float>(params.fold_action), 0.0f, 3.0f));
  if (action == FoldTransitionAction::kAuto) {
    if (!to_info.has_visible_alpha) {
      action = FoldTransitionAction::kFoldOut;
    } else if (!from_info.has_visible_alpha) {
      action = FoldTransitionAction::kUnfoldIn;
    } else if (!to_info.has_nonblack_rgb && from_info.has_nonblack_rgb) {
      action = FoldTransitionAction::kFoldOut;
    } else if (!from_info.has_nonblack_rgb && to_info.has_nonblack_rgb) {
      action = FoldTransitionAction::kUnfoldIn;
    } else if (!to_info.is_full_frame_opaque) {
      action = FoldTransitionAction::kFoldAndUnfold;
    } else {
      action = FoldTransitionAction::kUnfoldIn;
    }
  }

  ImageView active_view;
  ClipContentSummary active_info;
  ImageView underlay_view;
  float underlay_weight = 0.0f;
  float openness = 1.0f;

  switch (action) {
    case FoldTransitionAction::kFoldOut:
      active_view = from_view;
      active_info = from_info;
      underlay_view = to_view;
      openness = 1.0f - p;
      underlay_weight = from_info.is_full_frame_opaque
                            ? 1.0f
                            : Smoothstep(0.0f, 0.24f, p);
      break;
    case FoldTransitionAction::kFoldAndUnfold:
      if (p < 0.5f) {
        active_view = from_view;
        active_info = from_info;
        openness = 1.0f - p * 2.0f;
      } else {
        active_view = to_view;
        active_info = to_info;
        openness = (p - 0.5f) * 2.0f;
      }
      underlay_weight = 0.0f;
      break;
    case FoldTransitionAction::kUnfoldIn:
    case FoldTransitionAction::kAuto:
      active_view = to_view;
      active_info = to_info;
      underlay_view = from_view;
      openness = p;
      underlay_weight = to_info.is_full_frame_opaque
                            ? 1.0f
                            : Smoothstep(1.0f, 0.76f, p);
      break;
  }

  const float cw =
      static_cast<float>(std::max(1, canvas_bounds.x2 - canvas_bounds.x1));
  const float ch =
      static_cast<float>(std::max(1, canvas_bounds.y2 - canvas_bounds.y1));
  const float min_dim = std::min(cw, ch);
  const float origin_x = static_cast<float>(canvas_bounds.x1);
  const float origin_y = static_cast<float>(canvas_bounds.y1);

  if (!active_info.has_visible_alpha || openness <= 1e-4f) {
    RunScanlines(y_begin, y_end, parallel_for, [&](int y) {
      const float py = static_cast<float>(y) + 0.5f;
      for (int x = x_begin; x < x_end; ++x) {
        const float px = static_cast<float>(x) + 0.5f;
        const RgbaColor bg =
            underlay_weight > 1e-4f
                ? ScalePremult(
                      SampleClipCanonicalPx(underlay_view, canvas_bounds, px,
                                            py),
                      underlay_weight)
                : RgbaColor{};
        WritePixel(output, x, y, bg);
      }
    });
    return;
  }

  // Smoothly settle temporary transition collar/crease/jitter into the raw clip
  // over openness in [0.88, 1.0] so endpoints never pop on timeline cuts.
  const float settle = Smoothstep(1.0f, 0.88f, openness);
  const float max_border_px =
      params.enable_border ? std::max(0.0f, params.border_width_norm * min_dim)
                           : 0.0f;
  const float max_tear_px =
      params.enable_border
          ? std::max(0.0f, params.tear_roughness_norm * min_dim)
          : 0.0f;
  const float border_px = max_border_px * settle;
  const float tear_amp_px = max_tear_px * settle;

  CoarseSignedDistanceField alpha_sdf;
  SheetGeometry sheet_geom;
  if (active_info.is_full_frame_opaque) {
    sheet_geom.has_sheet = true;
    sheet_geom.use_analytic_box = true;
    sheet_geom.box_cx = origin_x + 0.5f * cw;
    sheet_geom.box_cy = origin_y + 0.5f * ch;
    sheet_geom.box_half_w = 0.5f * cw;
    sheet_geom.box_half_h = 0.5f * ch;
    sheet_geom.sheet_cx = sheet_geom.box_cx;
    sheet_geom.sheet_cy = sheet_geom.box_cy;
    sheet_geom.sheet_w = cw;
    sheet_geom.sheet_h = ch;
  } else {
    CutoutParams sdf_params;
    sdf_params.source_mode = 0;
    sdf_params.media_scale = 1.0f;
    sdf_params.enable_chroma_key = false;
    const SubjectTransform id_xf =
        BuildSubjectTransform(sdf_params, active_view, canvas_bounds);
    alpha_sdf = BuildSubjectSignedDistanceField(sdf_params, id_xf, active_view,
                                                canvas_bounds, parallel_for);
    sheet_geom = BuildSheetGeometryFromSdf(alpha_sdf, canvas_bounds,
                                           max_border_px + max_tear_px);
    if (!sheet_geom.has_sheet) {
      copy_clip_to_output(underlay_view);
      return;
    }
  }

  const FoldPattern pattern = static_cast<FoldPattern>(
      Clamp(static_cast<float>(params.fold_pattern), 0.0f, 3.0f));
  const FoldHingeSchedule sched = BuildFoldHinges(
      pattern, params.fold_stages, openness, sheet_geom.sheet_w,
      sheet_geom.sheet_h, cw, ch, params.direction_angle_deg, params.fold_depth,
      params.mirror_h, params.mirror_v);

  const std::uint32_t seed =
      static_cast<std::uint32_t>(std::max(0, params.random_seed));
  const int step_idx =
      static_cast<int>(std::round(p * static_cast<float>(std::max(2, params.stop_motion_steps))));
  const std::uint32_t boil_seed =
      seed + static_cast<std::uint32_t>(std::max(0, step_idx) * 13);

  float jitter_dx = 0.0f;
  float jitter_dy = 0.0f;
  float jitter_rot = 0.0f;
  if (params.enable_stop_motion && params.jitter_amount > 1e-4f) {
    const float sf = static_cast<float>(step_idx) + static_cast<float>(seed);
    const float amp = Clamp(params.jitter_amount, 0.0f, 1.0f) * settle;
    jitter_dx = 0.008f * amp * min_dim * std::sin(sf * 12.9898f);
    jitter_dy = 0.008f * amp * min_dim * std::cos(sf * 78.233f);
    jitter_rot = 2.2f * amp * kDegToRad * std::sin(sf * 43.17f);
  }
  const float j_cos = std::cos(-(jitter_rot));
  const float j_sin = std::sin(-(jitter_rot));

  PaperFoldRenderContext fctx;
  fctx.sheet = &sheet_geom;
  fctx.sched = &sched;
  fctx.origin_x = origin_x;
  fctx.origin_y = origin_y;
  fctx.min_dim = min_dim;
  fctx.enable_border = params.enable_border;
  fctx.border_px = border_px;
  fctx.tear_amp_px = tear_amp_px;
  fctx.tear_frequency = params.tear_frequency;
  fctx.tear_detail = 5;
  fctx.paper_r = params.paper_r;
  fctx.paper_g = params.paper_g;
  fctx.paper_b = params.paper_b;
  fctx.grain_strength = params.grain_strength;
  fctx.grain_scale = 1.0f;
  fctx.crease_strength = params.crease_strength;
  fctx.crease_scale = 3.4f;
  fctx.media_overlay = 0.30f * Clamp01(params.crease_strength) * settle;
  fctx.hinge_crease_boost = settle;
  fctx.flap_shadow_opacity = params.shadow_opacity;
  fctx.flap_sh_dx_px = std::max(3.0f, 0.011f * min_dim);
  fctx.flap_sh_dy_px = std::max(3.0f, 0.011f * min_dim);
  fctx.seed = seed;
  fctx.boil_seed = boil_seed;

  const float drop_sh_rad = -45.0f * kDegToRad;
  const float drop_sh_dist =
      std::max(3.0f, params.shadow_softness_norm * 0.85f * min_dim);
  const float sh_dx_px = drop_sh_dist * std::cos(drop_sh_rad);
  const float sh_dy_px = drop_sh_dist * std::sin(drop_sh_rad);
  const float drop_sh_opacity = params.shadow_opacity * settle;
  const float inv_pkt_scale = 1.0f / std::max(0.05f, sched.packet_scale);

  auto map_to_sheet_local = [&](float px_in, float py_in, float* lx,
                                float* ly) {
    const float dx =
        px_in - (sheet_geom.sheet_cx + jitter_dx + sched.slide_x_px);
    const float dy =
        py_in - (sheet_geom.sheet_cy + jitter_dy + sched.slide_y_px);
    const float jx = dx * j_cos - dy * j_sin;
    const float jy = dx * j_sin + dy * j_cos;
    *lx = (jx * sched.packet_rot_cos - jy * sched.packet_rot_sin) *
          inv_pkt_scale;
    *ly = (jx * sched.packet_rot_sin + jy * sched.packet_rot_cos) *
          inv_pkt_scale;
  };

  auto sample_active_subject = [&](float ux, float uy,
                                   float /*dist_px*/) -> RgbaColor {
    return SampleClipCanonicalPx(active_view, canvas_bounds, ux, uy);
  };

  const bool has_underlay =
      underlay_weight > 1e-4f && IsValidRgbaView(underlay_view);

  RunScanlines(y_begin, y_end, parallel_for, [&](int y) {
    const float py_out = static_cast<float>(y) + 0.5f;
    for (int x = x_begin; x < x_end; ++x) {
      const float px_out = static_cast<float>(x) + 0.5f;
      float lx = 0.0f;
      float ly = 0.0f;
      map_to_sheet_local(px_out, py_out, &lx, &ly);

      const RgbaColor fg = ScalePremult(
          EvalFoldStackRgba(fctx, sched.count - 1, lx, ly, false, 1.0f,
                            sample_active_subject),
          sched.packet_alpha);

      if (fg.a >= 0.999f) {
        WritePixel(output, x, y, fg);
        continue;
      }

      RgbaColor bg = has_underlay
                         ? ScalePremult(SampleClipCanonicalPx(
                                            underlay_view, canvas_bounds,
                                            px_out, py_out),
                                        underlay_weight)
                         : RgbaColor{};

      if (drop_sh_opacity > 1e-4f) {
        float slx = 0.0f;
        float sly = 0.0f;
        map_to_sheet_local(px_out - sh_dx_px, py_out - sh_dy_px, &slx, &sly);
        const float sh_cov =
            EvalFoldStackAlphaOnly(fctx, sched.count - 1, slx, sly, false);
        const float sh_a = drop_sh_opacity * sched.packet_alpha * sh_cov;
        if (sh_a > 1e-4f) {
          bg = CompositeOver({0.0f, 0.0f, 0.0f, sh_a}, bg);
        }
      }

      WritePixel(output, x, y, CompositeOver(fg, bg));
    }
  });
}

}  // namespace papercraft
