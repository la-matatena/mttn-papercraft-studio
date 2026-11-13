// CPU pixel kernels for the PaperCraft OpenFX plugins: the Paper Cutout FX
// filter and the Paper Fold & Unfold transition.
//
// Entry points (declared in papercraft_kernel.h) are called from the OpenFX
// render action in papercraft_ofx_plugin.cc:
//   - RenderCutoutFrame: torn-paper cutout of a clip or the built-in sticker,
//     with grain, creases, drop shadow, stop-motion jitter, and intro/outro
//     animations (including the 3D Paper Unfold).
//   - RenderPaperFoldTransition: folds one clip into a paper packet and/or
//     unfolds the other, with flap lighting, crease marks, and cast shadows.
//   - ComputeSteppedTransitionProgress: stop-motion quantized progress.
// Everything else lives in the anonymous namespace, except a few helpers in
// papercraft::internal (papercraft_kernel_internal.h) that
// papercraft_kernel_test.cc unit-tests directly. Both stay hidden in the
// plugin binary (-fvisibility=hidden).
//
// Coordinate spaces and units:
//   - Output px: OpenFX pixel coordinates at the current render scale.
//     Integer (x, y) addresses a pixel; its center is (x + 0.5, y + 0.5).
//   - Canonical px: the pixel grid of `canvas_bounds` (the output RoD, i.e.
//     the full frame), describing a point on the un-animated subject or
//     paper. Renderers inverse-map output px to canonical px.
//   - Source px: pixel coordinates of an input clip buffer. Canonical px are
//     remapped through the clip's RoD so cropped or differently sized inputs
//     line up with the canvas.
//   - Normalized `nu_x/nu_y`: (canonical px - canvas origin) / min(cw, ch).
//     Procedural textures use it so grain and tears look the same at every
//     resolution and aspect ratio.
//   - Normalized `nx/ny`: 0..1 across the canvas per axis (demo sticker,
//     crumple warp), or a lattice coordinate inside the noise helpers.
//   - Sheet-local px: offset from the paper sheet center (SheetGeometry cx,
//     cy). Fold hinges live here.
//   - Packet-local px: sheet-local coordinates of the folded packet, i.e.
//     canvas px with the packet slide, rotation, and scale removed
//     (PacketMapper::ToLocal). EvalFoldStack unfolds them back to flat
//     sheet-local px.
//   - SDF cells: CoarseSignedDistanceField cells of `step` canonical px.
//
// Colors are linear float RGBA and premultiplied everywhere inside this
// file; ReadPixelPtr converts straight or opaque inputs on load.
//
// Threading: renderers split per-pixel work by scanline through
// ParallelRowsFn (see RunScanlines). Lambdas passed to RunScanlines run
// concurrently on host worker threads; they only read shared per-frame state
// and write disjoint output rows (or disjoint grid rows / column blocks), so
// no locking is needed.
#if defined(PAPERCRAFT_STANDALONE_OFX)
#include "papercraft_kernel.h"  // NOLINT(build/include)

#include "papercraft_kernel_internal.h"  // NOLINT(build/include)
#else
#include "experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro/ofx/papercraft_kernel.h"
#include "experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro/ofx/papercraft_kernel_internal.h"
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

// Helpers defined in papercraft::internal (declared in
// papercraft_kernel_internal.h for unit tests), used unqualified below.
using internal::ClipContentSummary;
using internal::DistanceTransform1D;
using internal::DistanceTransform2D;
using internal::InspectClipContent;
using internal::ResolveFoldAction;

// ---------------------------------------------------------------------------
// Scalar and color math
// ---------------------------------------------------------------------------

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;

// Clamps `v` to [lo, hi]. NaN maps to `lo`.
float Clamp(float v, float lo, float hi) {
  return std::min(hi, std::max(lo, v));
}

// Clamps `v` to [0, 1] (alpha, masks, normalized parameters).
float Clamp01(float v) { return Clamp(v, 0.0f, 1.0f); }

// Hermite smoothstep 3t^2 - 2t^3 with t = (x - edge0) / (edge1 - edge0)
// clamped to [0, 1]. Reversed edges (edge0 > edge1) give a falling ramp; the
// code uses that for "inside" masks such as Smoothstep(1.2, -1.2, sdf), an
// anti-aliased ~2.4 px edge around sdf = 0. Equal edges act as a hard step
// at edge0. The saturated-end early-outs return exactly what the clamped
// formula gives (t = 0 -> 0, t = 1 -> 1), so they are bit-identical.
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

// Linear interpolation a + (b - a) * t; `t` is not clamped.
float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// Per-channel Lerp of two colors (premultiplied in practice). Used by the
// bilinear samplers and color blends.
RgbaColor LerpColor(const RgbaColor& a, const RgbaColor& b, float t) {
  return {Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t),
          Lerp(a.a, b.a, t)};
}

// Porter-Duff Over with premultiplied inputs and output:
//   out = fg + bg * (1 - fg.a).
// Only alpha is clamped to [0, 1]; float RGB above 1 (HDR) is kept. When
// fg.a >= 1 the background contributes exactly nothing.
RgbaColor CompositeOver(const RgbaColor& fg, const RgbaColor& bg) {
  const float inv_a = 1.0f - Clamp01(fg.a);
  return {
      fg.r + bg.r * inv_a,
      fg.g + bg.g * inv_a,
      fg.b + bg.b * inv_a,
      Clamp01(fg.a + bg.a * inv_a),
  };
}

// ---------------------------------------------------------------------------
// Pixel I/O (OpenFX image buffers)
// ---------------------------------------------------------------------------

// Bytes per pixel of `view`: 4, 2, or 1 bytes per channel (float, 16-bit,
// 8-bit) times the component count (4 for every valid view).
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

// Address of pixel (x, y) inside `view.data`. (x, y) are absolute OpenFX
// pixel coordinates and must lie inside `view.bounds` (the allocated buffer
// rectangle; `data` points at (bounds.x1, bounds.y1)). No bounds checks.
// row_bytes may be negative for hosts that store scanlines bottom-up.
const std::uint8_t* PixelConstPtr(const ImageView& view, int x, int y) {
  const std::ptrdiff_t pixel_bytes = PixelSizeBytes(view);
  return static_cast<const std::uint8_t*>(view.data) +
         static_cast<std::ptrdiff_t>(y - view.bounds.y1) * view.row_bytes +
         static_cast<std::ptrdiff_t>(x - view.bounds.x1) * pixel_bytes;
}

// Writable twin of PixelConstPtr, used for the output buffer.
std::uint8_t* PixelMutPtr(const ImageView& view, int x, int y) {
  const std::ptrdiff_t pixel_bytes = PixelSizeBytes(view);
  return static_cast<std::uint8_t*>(view.data) +
         static_cast<std::ptrdiff_t>(y - view.bounds.y1) * view.row_bytes +
         static_cast<std::ptrdiff_t>(x - view.bounds.x1) * pixel_bytes;
}

// Region of definition used to map canonical px onto this view: the host RoD
// when it is non-empty, else the buffer bounds. The RoD is the full image
// extent, while `bounds` may cover only the part the host fetched.
PixelRect EffectiveRod(const ImageView& view) {
  if (view.rod.x2 > view.rod.x1 && view.rod.y2 > view.rod.y1) {
    return view.rod;
  }
  return view.bounds;
}

// Decodes the RGBA pixel at `ptr` into normalized float, premultiplied color.
// 16-bit and 8-bit channels scale by 1/65535 and 1/255. kOpaque forces
// alpha = 1 (RGB untouched); kUnPremultiplied multiplies RGB by the clamped
// alpha. memcpy avoids unaligned and strict-aliasing UB and compiles to
// plain loads. Hot path: called per tap from every parallel_for worker.
inline RgbaColor ReadPixelPtr(const std::uint8_t* ptr, PixelDepth depth,
                              AlphaMode alpha_mode) {
  RgbaColor out;
  if (depth == PixelDepth::kFloat) {
    float c[4];
    std::memcpy(c, ptr, sizeof(c));
    out = {c[0], c[1], c[2], c[3]};
  } else if (depth == PixelDepth::kShort) {
    std::uint16_t c[4];
    std::memcpy(c, ptr, sizeof(c));
    constexpr float kInv = 1.0f / 65535.0f;
    out = {c[0] * kInv, c[1] * kInv, c[2] * kInv, c[3] * kInv};
  } else {
    constexpr float kInvByte = 1.0f / 255.0f;
    out = {ptr[0] * kInvByte, ptr[1] * kInvByte, ptr[2] * kInvByte,
           ptr[3] * kInvByte};
  }
  if (alpha_mode == AlphaMode::kOpaque) {
    out.a = 1.0f;
  } else if (alpha_mode == AlphaMode::kUnPremultiplied) {
    const float a = Clamp01(out.a);
    out = {out.r * a, out.g * a, out.b * a, a};
  }
  return out;
}

// ReadPixelPtr at absolute pixel (x, y); (x, y) must lie inside bounds.
RgbaColor ReadPixel(const ImageView& view, int x, int y) {
  return ReadPixelPtr(PixelConstPtr(view, x, y), view.depth, view.alpha_mode);
}

// Encodes premultiplied `color` at `ptr`. Float output is stored unclamped
// (keeps HDR values); 16-bit and 8-bit outputs clamp to [0, 1] and round to
// nearest (+0.5 before truncation). The output is always written
// premultiplied, regardless of the view's alpha_mode.
//
// Invariant: this is correct because the plugin always declares premultiplied
// output. HandleGetClipPreferences in papercraft_ofx_plugin.cc sets
// kOfxImageEffectPropPreMultiplication = kOfxImagePreMultiplied, so hosts
// hand the render action a premultiplied output image and never request a
// straight-alpha one. If that declaration ever changes (e.g. to honor an
// input's kOfxImageUnPreMultiplied), un-premultiply here when
// output.alpha_mode != kPremultiplied.
inline void WritePixelPtr(std::uint8_t* ptr, PixelDepth depth,
                          const RgbaColor& color) {
  if (depth == PixelDepth::kFloat) {
    const float c[4] = {color.r, color.g, color.b, color.a};
    std::memcpy(ptr, c, sizeof(c));
    return;
  }
  if (depth == PixelDepth::kShort) {
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

// Bilinear sample of `view` at source px (px, py), treating everything
// outside `view.bounds` as transparent black so clip edges fade over one
// pixel instead of smearing the border. Pixel centers sit at +0.5, hence the
// -0.5 shift. Points more than 1 px outside return transparent at once.
// Fast path: when all four taps are inside, read them through pointer
// offsets with no per-tap checks. It performs the same reads and Lerp calls
// in the same order as the read_safe path, so results are bit-identical.
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

  const float sx = px - 0.5f;
  const float sy = py - 0.5f;
  const int x0 = static_cast<int>(std::floor(sx));
  const int y0 = static_cast<int>(std::floor(sy));
  const float fx = sx - static_cast<float>(x0);
  const float fy = sy - static_cast<float>(y0);

  if (x0 >= view.bounds.x1 && x0 + 1 < view.bounds.x2 && y0 >= view.bounds.y1 &&
      y0 + 1 < view.bounds.y2) {
    const std::ptrdiff_t px_bytes = PixelSizeBytes(view);
    const std::uint8_t* p00 = PixelConstPtr(view, x0, y0);
    const std::uint8_t* p01 = p00 + view.row_bytes;
    return LerpColor(
        LerpColor(ReadPixelPtr(p00, view.depth, view.alpha_mode),
                  ReadPixelPtr(p00 + px_bytes, view.depth, view.alpha_mode),
                  fx),
        LerpColor(ReadPixelPtr(p01, view.depth, view.alpha_mode),
                  ReadPixelPtr(p01 + px_bytes, view.depth, view.alpha_mode),
                  fx),
        fy);
  }

  auto read_safe = [&](int ix, int iy) -> RgbaColor {
    if (ix < view.bounds.x1 || ix >= view.bounds.x2 || iy < view.bounds.y1 ||
        iy >= view.bounds.y2) {
      return {};
    }
    return ReadPixel(view, ix, iy);
  };

  return LerpColor(
      LerpColor(read_safe(x0, y0), read_safe(x0 + 1, y0), fx),
      LerpColor(read_safe(x0, y0 + 1), read_safe(x0 + 1, y0 + 1), fx), fy);
}

// ---------------------------------------------------------------------------
// Procedural noise and paper textures
//
// All noise is pure (no state), deterministic for a given seed, and safe to
// call from parallel_for workers. Inputs are normalized coordinates (`nu`
// space, or per-axis 0..1 for the crumple warp) times a frequency.
// ---------------------------------------------------------------------------

// Fast deterministic integer spatial hash returning [0, 1].
// Large odd multipliers (two are xxHash32 primes) and an xorshift avalanche
// mix the bits; the low 24 bits are kept so the float conversion is exact.
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
  // Lower lattice corner of the cell.
  int ix = 0;
  int iy = 0;
  // Corner hashes in [0, 1]: n00 = (ix, iy), n10 = (ix + 1, iy),
  // n01 = (ix, iy + 1), n11 = (ix + 1, iy + 1).
  float n00 = 0.0f;
  float n10 = 0.0f;
  float n01 = 0.0f;
  float n11 = 0.0f;
};

// Hashes the four corners of the lattice cell with lower corner (ix, iy).
NoiseCell HashNoiseCell(int ix, int iy, std::uint32_t seed) {
  return {ix,
          iy,
          Hash2D(ix, iy, seed),
          Hash2D(ix + 1, iy, seed),
          Hash2D(ix, iy + 1, seed),
          Hash2D(ix + 1, iy + 1, seed)};
}

// Quintic-faded bilinear interpolation of `cell` at lattice point (x, y).
// The fade 6t^5 - 15t^4 + 10t^3 (Perlin's improved fade) has zero first and
// second derivatives at cell borders, which hides the lattice. (x, y) must
// lie inside the cell.
float InterpolateNoiseCell(const NoiseCell& cell, float x, float y) {
  const float fx = x - static_cast<float>(cell.ix);
  const float fy = y - static_cast<float>(cell.iy);
  const float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
  const float uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
  return Lerp(Lerp(cell.n00, cell.n10, ux), Lerp(cell.n01, cell.n11, ux), uy);
}

// Smooth 2D value noise in [0, 1] at lattice coordinate (x, y); one lattice
// unit is roughly one noise feature.
float ValueNoise2D(float x, float y, std::uint32_t seed) {
  const int ix = static_cast<int>(std::floor(x));
  const int iy = static_cast<int>(std::floor(y));
  return InterpolateNoiseCell(HashNoiseCell(ix, iy, seed), x, y);
}

// Signed [-1, 1] fractional Brownian motion for ragged torn paper edges.
// (nx, ny) are normalized `nu` coordinates; `frequency` is the base feature
// count per min(frame width, height), floored at 0.5. Octaves clamp to
// [1, 6]. Lacunarity 2.07 (slightly off 2 so octaves do not align) and gain
// 0.52; each octave gets its own seed (+97 * i). Dividing by the summed
// amplitudes keeps the result in [-1, 1].
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

// Fine cardstock fiber grain: a brightness multiplier in [0.81, 1.04] at
// normalized (nx, ny). Two anisotropic value-noise octaves (stretched
// differently in x and y, like paper fibers); 56 is the fiber density per
// min(frame width, height) at scale 1. Callers blend the result toward 1 by
// Grain Strength.
float PaperFiberGrain(float nx, float ny, float scale, std::uint32_t seed) {
  const float s = 56.0f / std::max(0.1f, scale);
  const float n1 = ValueNoise2D(nx * s * 1.15f, ny * s * 0.90f, seed + 31u);
  const float n2 = ValueNoise2D(nx * s * 2.45f, ny * s * 2.65f, seed + 173u);
  const float grain = n1 * 0.6f + n2 * 0.4f;
  return Lerp(0.81f, 1.04f, grain);
}

// Ridged-noise octaves shared by PaperCreaseField and CrumpleDisplacement
// (they must match for CrumpleDisplacement to stay bit-identical).
constexpr int kCreaseOctaves = 3;

// Sheared lattice coordinates of crease octave `i` at normalized (nx, ny).
// Octaves 1 and 2 are sheared in opposite directions so creases do not all
// follow the lattice axes.
void CreaseOctaveCoords(float nx, float ny, float freq, int i, float* rx,
                        float* ry) {
  *rx = nx * freq + (i == 1 ? 0.37f * ny * freq : 0.0f);
  *ry = ny * freq + (i == 2 ? -0.41f * nx * freq : 0.0f);
}

// Ridged transform of value noise n in [0, 1]: (1 - |2n - 1|)^3 peaks
// sharply at n = 0.5, which turns smooth noise into thin crease lines.
float RidgeOf(float n) {
  const float ridge = 1.0f - std::abs(2.0f * n - 1.0f);
  return ridge * ridge * ridge;
}

// Sharp ridged crease/crumple field returning [0, 1] where 1.0 = sharp fold
// valley, at normalized (nx, ny). `scale` sets the crease density (x 1.6,
// floored at 0.25). Octaves use lacunarity 1.95, gain 0.55, and seed
// +211 * i.
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
//
// Outputs: *dx, *dy = forward-difference gradient of the crease field per
// normalized unit (kEps = 0.008 of the canvas width or height), and
// *crease_shade = the field at (nx, ny). Cell reuse is exact because the
// same HashNoiseCell values feed the same InterpolateNoiseCell arithmetic,
// in the same order, as in ValueNoise2D.
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

// ---------------------------------------------------------------------------
// Subject evaluation: built-in sticker, chroma key, media placement
// ---------------------------------------------------------------------------

// Vibrant built-in multi-layer paper craft badge/sticker for Generator / empty
// track testing. Coordinates nx, ny are normalized [0, 1] across canvas.
// `aspect` = canvas width / height keeps the badge round; radii are in units
// of canvas height. The +-0.004 smoothstep widths give a soft edge of about
// 4 px on each side at 1080p. Returns premultiplied color, transparent
// outside the scalloped silhouette.
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
//
// Un-premultiplies, then measures the distance to the key color as a blend
// (Key Balance: 0 = plain RGB distance, 1 = chroma distance). The chroma
// distance first rescales the pixel to the key's Rec.601 luma (ratio clamped
// to [0.35, 2.5]; the +0.06 keeps blacks from blowing up the ratio). Matte
// alpha ramps from 0 at distance 0.16 * gain to 1 at 0.36 * gain + 0.06
// (tuned by eye). Spill: for a green (or blue) key, the dominant channel is
// pulled toward the mean of the other two, fully where keyed out and by 45%
// where fully kept.
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

// Per-frame constants of the Media Placement inverse transform, built once by
// BuildSubjectTransform so EvaluateSubjectAtCanonicalPx is a cheap per-pixel
// rotate/scale/remap. Canvas values are canonical px.
struct SubjectTransform {
  // Canvas (output RoD) size in px, at least 1, and reciprocals.
  float cw = 1.0f;
  float ch = 1.0f;
  float inv_cw = 1.0f;
  float inv_ch = 1.0f;
  // Canvas origin (canvas_bounds.x1, canvas_bounds.y1).
  float origin_x = 0.0f;
  float origin_y = 0.0f;
  // Canvas center.
  float cx_canvas = 0.5f;
  float cy_canvas = 0.5f;
  // 1 / Media Scale (scale floored at 0.05).
  float inv_scale = 1.0f;
  // Cosine and sine of -Media Angle (the inverse rotation).
  float cos_a = 1.0f;
  float sin_a = 0.0f;
  // Media Offset from the canvas center, px (offset 0.5 = centered).
  float offset_x_px = 0.0f;
  float offset_y_px = 0.0f;
  // Source RoD origin and size, source px.
  float src_rod_x1 = 0.0f;
  float src_rod_y1 = 0.0f;
  float src_rod_w = 1.0f;
  float src_rod_h = 1.0f;
  // Source px per canonical px, per axis.
  float rod_scale_x = 1.0f;
  float rod_scale_y = 1.0f;
  // Render the built-in sticker (Source Mode 1, or no valid source clip).
  bool use_demo = false;
};

// Precomputes the SubjectTransform for `params`, `source`, and the canonical
// canvas. Runs once per render on the calling thread.
SubjectTransform BuildSubjectTransform(const CutoutParams& params,
                                       const ImageView& source,
                                       const PixelRect& canvas_bounds) {
  SubjectTransform xf;
  xf.cw = static_cast<float>(std::max(1, canvas_bounds.x2 - canvas_bounds.x1));
  xf.ch = static_cast<float>(std::max(1, canvas_bounds.y2 - canvas_bounds.y1));
  xf.inv_cw = 1.0f / xf.cw;
  xf.inv_ch = 1.0f / xf.ch;
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
    xf.rod_scale_x = xf.src_rod_w * xf.inv_cw;
    xf.rod_scale_y = xf.src_rod_h * xf.inv_ch;
  }
  return xf;
}

// Evaluates canonical subject RGBA at pixel-space coordinates (px, py) relative
// to `canvas_bounds`, after applying Media Placement and optional Chroma Key.
// Inverse placement: translate to the placed center, rotate by -angle, divide
// by Media Scale, then translate back to the canvas center. Returns
// premultiplied color. Read-only; called per pixel from parallel_for workers.
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
    const float nx = (rx - xf.origin_x) * xf.inv_cw;
    const float ny = (ry - xf.origin_y) * xf.inv_ch;
    return EvaluateDemoSticker(nx, ny, xf.cw * xf.inv_ch);
  }

  // Remap canonical pixel coordinates via full-frame source RoD so cropped
  // input buffers map accurately while SampleBilinearTransparentOutside guards
  // physical bounds.
  const float sx = xf.src_rod_x1 + (rx - xf.origin_x) * xf.rod_scale_x;
  const float sy = xf.src_rod_y1 + (ry - xf.origin_y) * xf.rod_scale_y;

  const RgbaColor raw = SampleBilinearTransparentOutside(source, sx, sy);
  if (params.enable_chroma_key) {
    return ApplyChromaKey(params, raw);
  }
  return raw;
}

// ---------------------------------------------------------------------------
// Scanline dispatch
// ---------------------------------------------------------------------------

// Runs row_fn(y) for every y in [y_begin, y_end): through the host's
// `parallel_for` when provided (rows then run concurrently, in any order, on
// OpenFX multiThread workers), else serially on the calling thread. Also used
// for non-row indices such as SDF column blocks. `row_fn` must only write
// state owned by its own index. Returns after every row has run.
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

// Writes transparent black over output px [x_begin, x_end) x
// [y_begin, y_end), which must lie inside `output.bounds`. Streams each row
// through one pointer (WritePixelPtr) instead of recomputing the address
// per pixel; rows run on parallel_for workers.
void ClearOutputRows(const ImageView& output, int x_begin, int x_end,
                     int y_begin, int y_end,
                     const ParallelRowsFn& parallel_for) {
  const std::ptrdiff_t out_pixel_bytes = PixelSizeBytes(output);
  const PixelDepth out_depth = output.depth;
  RunScanlines(y_begin, y_end, parallel_for, [&](int y) {
    std::uint8_t* out_px = PixelMutPtr(output, x_begin, y);
    for (int x = x_begin; x < x_end; ++x, out_px += out_pixel_bytes) {
      WritePixelPtr(out_px, out_depth, {});
    }
  });
}

// ---------------------------------------------------------------------------
// Signed distance field of the subject matte
//
// The matte is thresholded on a coarse grid (at most 960 x 540 cells), then
// an exact Euclidean distance transform gives every cell its distance, in
// canonical px, to the matte edge. Torn borders, chokes, drop shadows, and
// fold sheets read it as a smooth "distance to the subject" function.
// ---------------------------------------------------------------------------

// Largest SDF grid side, in cells. Bounds the per-row stack scratch of
// DistanceTransform2D (about 23 KB per worker) and matches the 960-cell
// width target in BuildSubjectSignedDistanceField.
constexpr int kMaxSdfDim = 960;

}  // namespace

// The two distance transforms live in papercraft::internal so
// papercraft_kernel_test.cc can check them against a brute-force EDT.
namespace internal {

// Felzenszwalb-Huttenlocher linear-time 1D squared Euclidean distance
// transform over integer-valued input.
//
// `input[q]` is either an exact squared integer distance (< 2^24, so exactly
// representable in float) or >= 1e18 for "no feature". Parabola intersections
// are kept as exact rationals num/den (den > 0) and compared with 64-bit
// cross-multiplication, which avoids a float division per step. Output values
// are (q - v)^2 + input[v], exact integers in float, or 1e20 if the line has no
// feature.
//
// Math: D(q) = min_v (q - v)^2 + f(v) is the lower envelope of parabolas
// rooted at the feature cells. v[0..k] holds the envelope's parabola roots
// and z[k] the abscissa where parabola k takes over:
//   s = ((f(q) + q^2) - (f(r) + r^2)) / (2 (q - r)).
// Values fit easily in int64 (f < 2^24, q < 960). The caller provides the
// scratch: v needs n entries, z_num and z_den n + 1. Reference:
// Felzenszwalb & Huttenlocher, "Distance Transforms of Sampled Functions"
// (Theory of Computing, 2012).
void DistanceTransform1D(const float* input, int n, int* v, std::int64_t* z_num,
                         std::int64_t* z_den, float* output) {
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
    while (env < k &&
           z_num[env + 1] < static_cast<std::int64_t>(q) * z_den[env + 1]) {
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
//
// Threading: phase 1 hands each worker a block of kColBlock (64) columns;
// its slice of `scratch` is block-major (block x0 starts at x0 * height), so
// writes never overlap. Phase 2 hands each worker one grid row with stack
// scratch. `scratch` lets callers reuse the int buffer across calls (its
// nullptr default is declared in papercraft_kernel_internal.h). Grids
// larger than kMaxSdfDim on either side are left untouched.
void DistanceTransform2D(int width, int height,
                         const ParallelRowsFn& parallel_for,
                         std::vector<float>* grid, std::vector<int>* scratch) {
  if (width <= 0 || height <= 0 || width > kMaxSdfDim || height > kMaxSdfDim) {
    return;
  }
  std::vector<int> local_scratch;
  std::vector<int>& down_all = scratch != nullptr ? *scratch : local_scratch;
  down_all.resize(static_cast<std::size_t>(width) * height);
  constexpr int kColBlock = 64;
  const int num_blocks = (width + kColBlock - 1) / kColBlock;
  RunScanlines(0, num_blocks, parallel_for, [&](int block) {
    const int x0 = block * kColBlock;
    const int bw = std::min(kColBlock, width - x0);
    // Top-down sweep: last[c] = row of the latest feature at or above y
    // (-1 = none yet); d[c] receives the distance to it, or -1.
    std::array<int, kColBlock> last;
    last.fill(-1);
    int* down = down_all.data() + static_cast<std::size_t>(x0) * height;
    for (int y = 0; y < height; ++y) {
      const float* row =
          grid->data() + static_cast<std::size_t>(y) * width + x0;
      int* d = down + static_cast<std::size_t>(y) * bw;
      for (int c = 0; c < bw; ++c) {
        if (row[c] < 1e18f) {
          last[c] = y;
        }
        d[c] = last[c] < 0 ? -1 : y - last[c];
      }
    }
    // Bottom-up sweep: next[c] = row of the nearest feature at or below y.
    // Keep the closer of the two and store its square (1e20 = no feature in
    // this column).
    std::array<int, kColBlock> next;
    next.fill(-1);
    for (int y = height - 1; y >= 0; --y) {
      float* row = grid->data() + static_cast<std::size_t>(y) * width + x0;
      const int* d = down + static_cast<std::size_t>(y) * bw;
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

}  // namespace internal

namespace {

// Coarse signed distance to the subject matte edge, in canonical px
// (negative inside the matte). Read-only after construction, so workers may
// sample it concurrently.
struct CoarseSignedDistanceField {
  // Grid size in cells.
  int width = 0;
  int height = 0;
  // Canonical px per cell side.
  int step = 1;
  // Canonical px of the grid's (0, 0) corner (the canvas origin).
  float origin_x = 0.0f;
  float origin_y = 0.0f;
  // Row-major, width x height, px. +1e6 everywhere when the matte is empty,
  // -1e6 everywhere when it covers the whole grid.
  std::vector<float> signed_dist_px;

  // Grid coordinate of pixel offset `d`. Power-of-two steps (the common 1080p
  // and UHD cases) use an exact reciprocal multiply instead of a division.
  // The reciprocal is a power of two, so the multiply rounds exactly like
  // the division would.
  float ToGrid(float d) const {
    if ((step & (step - 1)) == 0) {
      return d * (1.0f / static_cast<float>(step));
    }
    return d / static_cast<float>(step);
  }

  // Bilinear signed distance in px at canonical px (px, py). Cell centers sit
  // at (g + 0.5) * step; queries off the grid clamp to the border cells. An
  // empty field returns 1e6 (far outside). Called from workers.
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

// Builds the subject's CoarseSignedDistanceField over `canvas_bounds`.
//
// 1. Grid: step = smallest integer that fits the canvas in 960 x 540 cells
//    (2 at 1080p, 4 at UHD). Each cell samples the subject at its center;
//    alpha >= 0.45 (near 50%, so the zero level sits on the visible edge)
//    counts as inside. With `ignore_shadow`, near-black semi-transparent
//    samples (RGB <= 1.5% of alpha, alpha < 0.985: the baked drop shadow of
//    a Paper Cutout FX clip) count as outside.
// 2. Two exact squared EDTs: distance to the nearest inside cell
//    (outside_sq) and to the nearest outside cell (inside_sq).
// 3. Signed px: sqrt(osq) * step - step / 2 outside, the negated inside
//    distance inside; the half-cell shift puts the zero level between the
//    centers of the last inside and first outside cells. +-1e6 when the
//    matte is empty or full.
//
// Runs on the render thread before the pixel loop; each phase fans out
// through `parallel_for` (each worker writes its own grid rows).
CoarseSignedDistanceField BuildSubjectSignedDistanceField(
    const CutoutParams& params, const SubjectTransform& subj_xf,
    const ImageView& source, const PixelRect& canvas_bounds,
    const ParallelRowsFn& parallel_for, bool ignore_shadow = false) {
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
      const bool is_shadow_px =
          ignore_shadow && sample.a < 0.985f &&
          std::max({sample.r, sample.g, sample.b}) <= 0.015f * sample.a;
      if (!is_shadow_px && sample.a >= 0.45f) {
        outside_sq[idx] = 0.0f;
      } else {
        inside_sq[idx] = 0.0f;
      }
    }
  });

  std::vector<int> dt_scratch;
  DistanceTransform2D(sdf.width, sdf.height, parallel_for, &outside_sq,
                      &dt_scratch);
  DistanceTransform2D(sdf.width, sdf.height, parallel_for, &inside_sq,
                      &dt_scratch);

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

// ---------------------------------------------------------------------------
// Cutout animation: stop motion, intro/outro, inverse mapping
// ---------------------------------------------------------------------------

// Time-dependent animation state of Paper Cutout FX for one frame, computed
// once by ComputeCutoutAnimState and read by every worker.
struct CutoutAnimState {
  // Active intro/outro style; kNone while resting.
  CutoutAnimStyle style = CutoutAnimStyle::kNone;
  float raw_p = 1.0f;  // Stepped linear progress, 1.0 = resting.
  // Ease-out cubic of raw_p: 1 - (1 - raw_p)^3.
  float ease_p = 1.0f;  // 1.0 = flat/resting, 0.0 = collapsed/folded away
  // Uniform paper scale (1 = rest).
  float pop_scale = 1.0f;
  // Animation rotation, radians.
  float angle_rad = 0.0f;
  // Horizontal foreshortening cos(tilt); 1 = facing the camera.
  float tilt_y_cos = 1.0f;
  // Keystone gradients of the perspective divisor w per canvas width (k) and
  // height (ky); see MapOutputToPaperCanonical.
  float perspective_k = 0.0f;
  float perspective_ky = 0.0f;
  // Animation translation, canonical px.
  float slide_x_px = 0.0f;
  float slide_y_px = 0.0f;
  // Crumple displacement strength (0 = off) and crease darkening factor.
  float crumple_warp_norm = 0.0f;
  float extra_crease_shade = 0.0f;
  // Host frame time after stop-motion quantization, frames.
  float step_time = 0.0f;
  // Hand jitter of the current held step: px offsets and radians.
  float jitter_dx_px = 0.0f;
  float jitter_dy_px = 0.0f;
  float jitter_rot_rad = 0.0f;
  // Inverse master rotation (master angle plus jitter) and 1 / scale.
  float cos_m = 1.0f;
  float sin_m = 0.0f;
  float inv_m_scale = 1.0f;
  // Inverse animation rotation (-angle_rad).
  float a_cos = 1.0f;
  float a_sin = 0.0f;
};

// Evaluates the cutout animation for the current frame; cw, ch = canvas px.
// Runs once per render on the calling thread.
//
// Stop motion floors time to multiples of Hold Frames from the clip start,
// so poses hold. Jitter feeds the stepped time into sin/cos with the classic
// shader "random" constants (12.9898, 78.233, 43.17), giving every step a
// stable pseudo-random offset. Intro progress rises 0 -> 1 over Intro Frames
// from the clip start; outro progress falls 1 -> 0 over Outro Frames before
// the clip end. Per-style factors below are tuned by eye.
CutoutAnimState ComputeCutoutAnimState(const CutoutParams& params, float cw,
                                       float ch) {
  CutoutAnimState state;
  const float min_dim = std::min(cw, ch);

  const float hold = static_cast<float>(std::max(1, params.hold_frames));
  // Timing math stays in double until the end: host frame numbers can be
  // large (long timelines, timecode-based numbering ~86400+), where float
  // spacing is 1/128 frame or coarser and float subtraction of two large
  // frame numbers loses the fractional part. For ordinary frame numbers
  // (integers below 2^24) every step is exact either way, so results are
  // unchanged. NaN clamps to 0, like Clamp().
  const double rel_frame =
      std::min(std::max(0.0, params.frame_end - params.frame_start),
               std::max(0.0, params.current_time - params.frame_start));
  const double step_time =
      params.enable_stop_motion
          ? params.frame_start + std::floor(rel_frame / hold) * hold
          : params.current_time;
  // Float copy for jitter phases and edge boil (pseudo-random inputs).
  state.step_time = static_cast<float>(step_time);

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
    t_in = Clamp01(static_cast<float>(step_time - params.frame_start) /
                   static_cast<float>(params.intro_frames));
  }
  float t_out = 1.0f;
  const bool has_outro = (params.outro_style > 0 && params.outro_frames > 0);
  if (has_outro) {
    t_out = Clamp01(static_cast<float>(params.frame_end - step_time) /
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
      // RenderCutoutFrame renders this style with the fold machinery and only
      // reads raw_p, ease_p, jitter, and the master terms, so the tilt,
      // perspective, and crumple values set here are currently unused.
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
      // Shrinks into a spinning crumpled ball (scale down to 0.22).
      case CutoutAnimStyle::kCrumpleBall: {
        state.pop_scale = std::max(0.06f, 0.22f + 0.78f * state.ease_p);
        state.angle_rad = mh * mv * 35.0f * inv * kDegToRad;
        state.slide_y_px = mv * 0.08f * ch * inv;
        state.crumple_warp_norm = 0.085f * inv;
        state.extra_crease_shade = 0.65f * inv;
        break;
      }
      // Slides in along Glide Angle over Glide Distance x canvas size, banking
      // slightly.
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
      // easeOutBack: c1 = 1.70158 is Penner's standard overshoot constant
      // (about 10% overshoot before settling at scale 1).
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

  // Master transform terms, negated so they invert the forward transform.
  const float master_rad =
      -(params.master_angle_deg * kDegToRad + state.jitter_rot_rad);
  state.cos_m = std::cos(master_rad);
  state.sin_m = std::sin(master_rad);
  state.inv_m_scale = 1.0f / std::max(0.01f, params.master_scale);
  state.a_cos = std::cos(-state.angle_rad);
  state.a_sin = std::sin(-state.angle_rad);
  return state;
}

// Inverse-maps output px (x, y) (a pixel center) into canonical, unwarped
// paper space (*ux, *uy). Undoes, in reverse order, the forward chain:
// crumple warp -> perspective tilt -> pop scale and rotation -> slide ->
// flips -> master scale/rotation -> master position plus jitter.
// cw, ch, origin_x, origin_y describe the canvas in canonical px.
// `apply_crumple` = false skips the 3-octave crumple warp (used for the soft
// drop shadow, where it would not show). *fold_shading_out receives the
// crease darkening (crease value x extra_crease_shade; 0 without crumple).
// Called per pixel from parallel_for workers.
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

  const float inv_cw = 1.0f / cw;
  const float inv_ch = 1.0f / ch;

  // Apply inverse 3D perspective hinge tilt across horizontal/vertical axes.
  // Without tilt or perspective this divides by exactly 1, so skip it.
  float px = ax;
  float py = ay;
  // Projective divide by w = 1 + k * x / cw + ky * y / ch (floored at 0.15 so
  // the plane never flips behind the camera); x is also divided by the tilt
  // foreshortening.
  if (anim.perspective_k != 0.0f || anim.perspective_ky != 0.0f ||
      anim.tilt_y_cos != 1.0f) {
    const float w_persp =
        std::max(0.15f, 1.0f + anim.perspective_k * (ax * inv_cw) +
                            anim.perspective_ky * (ay * inv_ch));
    px = ax / (anim.tilt_y_cos * w_persp);
    py = ay / w_persp;
  }

  // Apply radial crumple displacement during crumple/unfold intro/outro.
  float crease_val = 0.0f;
  if (apply_crumple && anim.crumple_warp_norm > 1e-4f) {
    float gdx = 0.0f;
    float gdy = 0.0f;
    const float cnx = 0.5f + px * inv_cw;
    const float cny = 0.5f + py * inv_ch;
    CrumpleDisplacement(cnx, cny, params.crease_scale,
                        static_cast<std::uint32_t>(params.random_seed) + 503u,
                        &gdx, &gdy, &crease_val);
    // The finite-difference gradient is large (~1 / kEps); 0.08 tames it.
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

// Returns v * v.
float Square(float v) { return v * v; }

// Scales all four premultiplied channels by s (fades color and coverage).
RgbaColor ScalePremult(const RgbaColor& c, float s) {
  return {c.r * s, c.g * s, c.b * s, c.a * s};
}

// ---------------------------------------------------------------------------
// Clip sampling and inspection (transition inputs)
//
// The transition receives full-frame video, transparent titles or logos, or
// Paper Cutout FX output with a baked drop shadow. These helpers sample
// clips in canonical px, classify them, and strip baked shadows.
// ---------------------------------------------------------------------------

// Samples a clip at canonical canvas pixel (px, py). Same-size clips map 1:1,
// so unwarped frames reproduce the clip exactly.
// Other clips (proxies, other resolutions) are remapped linearly from the
// canvas onto their RoD. Outside the clip is transparent.
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
  const float sx =
      static_cast<float>(rod.x1) + (px - static_cast<float>(canvas_bounds.x1)) /
                                       cw * static_cast<float>(rod.x2 - rod.x1);
  const float sy =
      static_cast<float>(rod.y1) + (py - static_cast<float>(canvas_bounds.y1)) /
                                       ch * static_cast<float>(rod.y2 - rod.y1);
  return SampleBilinearTransparentOutside(view, sx, sy);
}

// The clip under canvas pixel (x, y). Same-size clips are read directly,
// which equals the bilinear sample at the pixel center but costs one fetch.
// (x, y) is an integer canonical pixel; transparent outside the buffer. The
// direct read is bit-identical for finite pixels: at the pixel center the
// bilinear weights are 0, and Lerp(a, b, 0) == a.
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

// ClipContentSummary (what InspectClipContent learned about a clip) is
// defined in papercraft_kernel_internal.h.

}  // namespace

namespace internal {

// Scans a strided grid (about 360 rows) dense enough to catch small logos and
// soft baked drop shadows (e.g. from Paper Cutout FX), while ignoring 1-2 px
// anti-aliased edges of raw black text or logos.
// shortcut: shadowless cutouts (Shadow Opacity = 0) look like raw clips; add a
// manual clip-mode parameter if shadowless cutouts need single-border auto.
//
// The grid stride is min(width, height) / 360 px (about 640 x 360 samples
// at 16:9). touches_opaque_black rejects samples within 2 px of opaque black
// ink, i.e. anti-aliased text edges. Runs serially on the render thread,
// once per needed clip per frame.
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
  // Opaque full-frame clips can have neither transparency nor a baked shadow,
  // so only `has_nonblack_rgb` needs pixels. Stop at the first non-black
  // sample; an all-black frame (the host's "empty" side of a clip edge, which
  // ResolveFoldAction relies on) still scans the whole grid and stays black.
  if (view.alpha_mode == AlphaMode::kOpaque && opaque) {
    out.has_visible_alpha = true;
    out.is_full_frame_opaque = true;
    for (int y = view.bounds.y1 + stride / 2;
         y < view.bounds.y2 && !out.has_nonblack_rgb; y += stride) {
      for (int x = view.bounds.x1 + stride / 2; x < view.bounds.x2;
           x += stride) {
        const RgbaColor c = ReadPixel(view, x, y);
        if (c.r + c.g + c.b > 3e-3f) {
          out.has_nonblack_rgb = true;
          break;
        }
      }
    }
    return out;
  }
  int shadow_samples = 0;
  auto touches_opaque_black = [&](int x, int y) {
    constexpr int kNear[12][2] = {{-1, 0},  {1, 0},  {0, -1}, {0, 1},
                                  {-1, -1}, {1, -1}, {-1, 1}, {1, 1},
                                  {-2, 0},  {2, 0},  {0, -2}, {0, 2}};
    for (const auto& off : kNear) {
      const int nx = x + off[0];
      const int ny = y + off[1];
      if (nx < view.bounds.x1 || nx >= view.bounds.x2 || ny < view.bounds.y1 ||
          ny >= view.bounds.y2) {
        continue;
      }
      const RgbaColor n = ReadPixel(view, nx, ny);
      if (n.a >= 0.96f && std::max({n.r, n.g, n.b}) <= 0.03f) {
        return true;
      }
    }
    return false;
  };
  for (int y = view.bounds.y1 + stride / 2; y < view.bounds.y2; y += stride) {
    for (int x = view.bounds.x1 + stride / 2; x < view.bounds.x2; x += stride) {
      const RgbaColor c = ReadPixel(view, x, y);
      if (c.a > 1e-3f) {
        out.has_visible_alpha = true;
      }
      if (c.r + c.g + c.b > 3e-3f) {
        out.has_nonblack_rgb = true;
      }
      if (c.a < 0.995f) {
        opaque = false;
        if (c.a >= 0.04f && c.a <= 0.985f &&
            std::max({c.r, c.g, c.b}) <= 0.015f * c.a &&
            !touches_opaque_black(x, y)) {
          ++shadow_samples;
          out.max_shadow_alpha = std::max(out.max_shadow_alpha, c.a);
        }
      }
    }
  }
  out.is_full_frame_opaque = opaque;
  out.has_cutout_shadow =
      !opaque && out.has_nonblack_rgb && shadow_samples >= 2;
  return out;
}

}  // namespace internal

namespace {

// Recovers per-pixel un-shadowed alpha for a clip composited over a black drop
// shadow `{0, 0, 0, sh_a}`. Since `CompositeOver(fg, {0, 0, 0, sh_a})` leaves
// `fg.rgb` unchanged and only raises alpha where `fg.a < 1`, replacing `c.a`
// with `clean_alpha` restores `fg` at 1-px torn-fiber resolution.
//
// Returns one alpha per pixel of `view.bounds` (row-major, bw x bh), or an
// empty vector for invalid views or buffers over 64 Mpx (2^26 px, a 256 MB
// float cap). `sdf` is the clip's matte in canonical px (built with
// ignore_shadow), so each buffer pixel is first mapped RoD -> canvas.
// Per pixel:
//   - alpha <= 1e-4 -> 0; alpha >= 0.995 or deep inside the matte
//     (sdf < -3 - step) -> keep; far outside (sdf > 2.5 + step) -> 0 (pure
//     shadow).
//   - near the edge: the shadow is black, so fg.rgb = fg.a x paper color.
//     The paper color comes from opaque neighbors within 2 px (then 3 px),
//     giving alpha ~= max(rgb) / local max(rgb), clamped to [max(rgb), c.a].
//   - no opaque neighbors: colored pixels keep alpha; black ones fade out
//     with distance past the edge.
// Rows run in parallel; each worker writes only its own row.
//
// `raw_alpha_box` (optional) receives the bounding box, in buffer px of
// `view`, of every pixel whose decoded raw alpha is not exactly 0 (NaN
// counts as non-zero); x2 <= x1 when there is none. It costs one compare per
// pixel already being read, and lets RenderPaperFoldTransition skip the
// resting-shadow work away from the clip (see ClipBoxToCanonical).
std::vector<float> BuildCleanCutoutAlpha(const ImageView& view,
                                         const PixelRect& canvas_bounds,
                                         const CoarseSignedDistanceField& sdf,
                                         const ParallelRowsFn& parallel_for,
                                         PixelRect* raw_alpha_box = nullptr) {
  if (raw_alpha_box != nullptr) {
    *raw_alpha_box = {};
  }
  if (!IsValidRgbaView(view)) {
    return {};
  }
  const int bw = view.bounds.x2 - view.bounds.x1;
  const int bh = view.bounds.y2 - view.bounds.y1;
  if (bw <= 0 || bh <= 0 ||
      static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh) > 67108864u) {
    return {};
  }
  const PixelRect rod = EffectiveRod(view);
  const float cw =
      static_cast<float>(std::max(1, canvas_bounds.x2 - canvas_bounds.x1));
  const float ch =
      static_cast<float>(std::max(1, canvas_bounds.y2 - canvas_bounds.y1));
  const float rod_w = static_cast<float>(std::max(1, rod.x2 - rod.x1));
  const float rod_h = static_cast<float>(std::max(1, rod.y2 - rod.y1));
  const float sdf_step = static_cast<float>(std::max(1, sdf.step));
  std::vector<float> clean_alpha(
      static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh), 0.0f);
  const std::ptrdiff_t px_bytes = PixelSizeBytes(view);
  const PixelDepth view_depth = view.depth;
  const AlphaMode view_alpha = view.alpha_mode;
  // Per-row first/last column (buffer-relative) with non-zero raw alpha;
  // each worker writes only its own row's entries. row_first = bw means none.
  std::vector<int> row_first;
  std::vector<int> row_last;
  if (raw_alpha_box != nullptr) {
    row_first.assign(static_cast<std::size_t>(bh), bw);
    row_last.assign(static_cast<std::size_t>(bh), -1);
  }

  RunScanlines(0, bh, parallel_for, [&](int ry) {
    const int iy = view.bounds.y1 + ry;
    const float uy =
        static_cast<float>(canvas_bounds.y1) +
        (static_cast<float>(iy) + 0.5f - static_cast<float>(rod.y1)) / rod_h *
            ch;
    float* out_row = clean_alpha.data() + static_cast<std::size_t>(ry) * bw;
    const std::uint8_t* in_px = PixelConstPtr(view, view.bounds.x1, iy);
    int first = bw;
    int last = -1;
    for (int rx = 0; rx < bw; ++rx, in_px += px_bytes) {
      const int ix = view.bounds.x1 + rx;
      const RgbaColor c = ReadPixelPtr(in_px, view_depth, view_alpha);
      if (!(c.a == 0.0f)) {
        first = std::min(first, rx);
        last = rx;
      }
      if (c.a <= 1e-4f) {
        out_row[rx] = 0.0f;
        continue;
      }
      if (c.a >= 0.995f) {
        out_row[rx] = c.a;
        continue;
      }
      const float ux =
          static_cast<float>(canvas_bounds.x1) +
          (static_cast<float>(ix) + 0.5f - static_cast<float>(rod.x1)) / rod_w *
              cw;
      const float dist_px = sdf.SamplePx(ux, uy);
      if (dist_px < -3.0f - sdf_step) {
        out_row[rx] = c.a;
        continue;
      }
      if (dist_px > 2.5f + sdf_step) {
        out_row[rx] = 0.0f;
        continue;
      }
      const float max_c = std::max({c.r, c.g, c.b});
      float local_max = -1.0f;
      constexpr int kNear[12][2] = {{-1, 0},  {1, 0},  {0, -1}, {0, 1},
                                    {-1, -1}, {1, -1}, {-1, 1}, {1, 1},
                                    {-2, 0},  {2, 0},  {0, -2}, {0, 2}};
      for (const auto& off : kNear) {
        const int nx = ix + off[0];
        const int ny = iy + off[1];
        if (nx >= view.bounds.x1 && nx < view.bounds.x2 &&
            ny >= view.bounds.y1 && ny < view.bounds.y2) {
          const RgbaColor n = ReadPixel(view, nx, ny);
          if (n.a >= 0.99f) {
            local_max = std::max(local_max, std::max({n.r, n.g, n.b}));
          }
        }
      }
      if (local_max < 0.0f) {
        for (const auto& off :
             {std::array<int, 2>{-3, 0}, {3, 0}, {0, -3}, {0, 3}}) {
          const int nx = ix + off[0];
          const int ny = iy + off[1];
          if (nx >= view.bounds.x1 && nx < view.bounds.x2 &&
              ny >= view.bounds.y1 && ny < view.bounds.y2) {
            const RgbaColor n = ReadPixel(view, nx, ny);
            if (n.a >= 0.99f) {
              local_max = std::max(local_max, std::max({n.r, n.g, n.b}));
            }
          }
        }
      }
      if (local_max > 0.04f) {
        out_row[rx] =
            max_c <= 1e-3f ? 0.0f : Clamp(max_c / local_max, max_c, c.a);
      } else if (max_c > 0.02f * c.a) {
        out_row[rx] = c.a;
      } else {
        out_row[rx] = c.a * Smoothstep(1.0f, -0.8f, dist_px);
      }
    }
    if (raw_alpha_box != nullptr) {
      row_first[static_cast<std::size_t>(ry)] = first;
      row_last[static_cast<std::size_t>(ry)] = last;
    }
  });
  if (raw_alpha_box != nullptr) {
    PixelRect box{bw, bh, -1, -1};  // Buffer-relative, inclusive maxima.
    for (int ry = 0; ry < bh; ++ry) {
      if (row_last[static_cast<std::size_t>(ry)] < 0) {
        continue;
      }
      box.x1 = std::min(box.x1, row_first[static_cast<std::size_t>(ry)]);
      box.x2 = std::max(box.x2, row_last[static_cast<std::size_t>(ry)]);
      box.y1 = std::min(box.y1, ry);
      box.y2 = ry;
    }
    if (box.y2 >= 0) {
      *raw_alpha_box = {view.bounds.x1 + box.x1, view.bounds.y1 + box.y1,
                        view.bounds.x1 + box.x2 + 1,
                        view.bounds.y1 + box.y2 + 1};
    }
  }
  return clean_alpha;
}

// SampleBilinearTransparentOutside, but alpha comes from `clean_alpha`
// (indexed like view.bounds) instead of the buffer, which removes a baked
// shadow: Over a black shadow leaves premultiplied RGB untouched, so buffer
// RGB with clean alpha is the original cutout. (px, py) are source px. The
// pointer fast path is bit-identical to the read_safe path.
RgbaColor SampleBilinearCleanAlpha(const ImageView& view,
                                   const std::vector<float>& clean_alpha,
                                   float px, float py) {
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
  const int bw = view.bounds.x2 - view.bounds.x1;
  const float sx = px - 0.5f;
  const float sy = py - 0.5f;
  const int x0 = static_cast<int>(std::floor(sx));
  const int y0 = static_cast<int>(std::floor(sy));
  const float fx = sx - static_cast<float>(x0);
  const float fy = sy - static_cast<float>(y0);

  if (x0 >= view.bounds.x1 && x0 + 1 < view.bounds.x2 && y0 >= view.bounds.y1 &&
      y0 + 1 < view.bounds.y2) {
    const std::ptrdiff_t px_bytes = PixelSizeBytes(view);
    const std::uint8_t* p00 = PixelConstPtr(view, x0, y0);
    const std::uint8_t* p01 = p00 + view.row_bytes;
    const std::size_t a_idx =
        static_cast<std::size_t>(y0 - view.bounds.y1) * bw +
        (x0 - view.bounds.x1);
    auto read_fast = [&](const std::uint8_t* ptr,
                         std::size_t idx) -> RgbaColor {
      RgbaColor c = ReadPixelPtr(ptr, view.depth, view.alpha_mode);
      c.a = clean_alpha[idx];
      return c;
    };
    return LerpColor(LerpColor(read_fast(p00, a_idx),
                               read_fast(p00 + px_bytes, a_idx + 1), fx),
                     LerpColor(read_fast(p01, a_idx + bw),
                               read_fast(p01 + px_bytes, a_idx + bw + 1), fx),
                     fy);
  }

  auto read_safe = [&](int ix, int iy) -> RgbaColor {
    if (ix < view.bounds.x1 || ix >= view.bounds.x2 || iy < view.bounds.y1 ||
        iy >= view.bounds.y2) {
      return {};
    }
    RgbaColor c = ReadPixel(view, ix, iy);
    c.a = clean_alpha[static_cast<std::size_t>(iy - view.bounds.y1) * bw +
                      (ix - view.bounds.x1)];
    return c;
  };
  return LerpColor(
      LerpColor(read_safe(x0, y0), read_safe(x0 + 1, y0), fx),
      LerpColor(read_safe(x0, y0 + 1), read_safe(x0 + 1, y0 + 1), fx), fy);
}

// SampleClipCanonicalPx with clean alpha: the same canonical px -> source px
// mapping, sampled through SampleBilinearCleanAlpha.
RgbaColor SampleCleanClipCanonicalPx(const ImageView& view,
                                     const std::vector<float>& clean_alpha,
                                     const PixelRect& canvas_bounds, float px,
                                     float py) {
  if (!IsValidRgbaView(view)) {
    return {};
  }
  const PixelRect rod = EffectiveRod(view);
  if (rod.x1 == canvas_bounds.x1 && rod.x2 == canvas_bounds.x2 &&
      rod.y1 == canvas_bounds.y1 && rod.y2 == canvas_bounds.y2) {
    return SampleBilinearCleanAlpha(view, clean_alpha, px, py);
  }
  const float cw =
      static_cast<float>(std::max(1, canvas_bounds.x2 - canvas_bounds.x1));
  const float ch =
      static_cast<float>(std::max(1, canvas_bounds.y2 - canvas_bounds.y1));
  const float sx =
      static_cast<float>(rod.x1) + (px - static_cast<float>(canvas_bounds.x1)) /
                                       cw * static_cast<float>(rod.x2 - rod.x1);
  const float sy =
      static_cast<float>(rod.y1) + (py - static_cast<float>(canvas_bounds.y1)) /
                                       ch * static_cast<float>(rod.y2 - rod.y1);
  return SampleBilinearCleanAlpha(view, clean_alpha, sx, sy);
}

// Clean alpha at integer canonical pixel (x, y): a direct lookup for
// same-size clips (bit-identical to the bilinear sample at the pixel
// center), else the bilinear clean sample.
float CleanClipAlphaAt(const ImageView& view,
                       const std::vector<float>& clean_alpha,
                       const PixelRect& canvas_bounds, int x, int y) {
  if (!IsValidRgbaView(view)) {
    return 0.0f;
  }
  const PixelRect rod = EffectiveRod(view);
  if (rod.x1 == canvas_bounds.x1 && rod.x2 == canvas_bounds.x2 &&
      rod.y1 == canvas_bounds.y1 && rod.y2 == canvas_bounds.y2) {
    if (x < view.bounds.x1 || x >= view.bounds.x2 || y < view.bounds.y1 ||
        y >= view.bounds.y2) {
      return 0.0f;
    }
    const int bw = view.bounds.x2 - view.bounds.x1;
    return clean_alpha[static_cast<std::size_t>(y - view.bounds.y1) * bw +
                       (x - view.bounds.x1)];
  }
  return SampleCleanClipCanonicalPx(view, clean_alpha, canvas_bounds,
                                    static_cast<float>(x) + 0.5f,
                                    static_cast<float>(y) + 0.5f)
      .a;
}

// Maps `src_box` (buffer px of `view`, half-open) to a conservative
// half-open box of integer canonical pixels: outside it, ClipPixelAt and
// CleanClipAlphaAt read only buffer pixels outside `src_box` (or nothing),
// so if `src_box` holds every non-zero alpha they return exactly 0 alpha.
// Same-size clips read pixel (x, y) directly, so the box is unchanged.
// Remapped clips sample bilinearly at s = rod.x1 + (x + 0.5 - canvas.x1) *
// rod_w / cw; a tap inside [b1, b2) needs s in [b1 - 0.5, b2 + 0.5). That
// interval is mapped back exactly in double and padded by 2 px plus one
// source pixel's canonical width to absorb the float rounding of the
// forward map. Returns an empty box (x2 <= x1) for an empty `src_box`.
PixelRect ClipBoxToCanonical(const ImageView& view,
                             const PixelRect& canvas_bounds,
                             const PixelRect& src_box) {
  if (src_box.x2 <= src_box.x1 || src_box.y2 <= src_box.y1) {
    return {};
  }
  const PixelRect rod = EffectiveRod(view);
  if (rod.x1 == canvas_bounds.x1 && rod.x2 == canvas_bounds.x2 &&
      rod.y1 == canvas_bounds.y1 && rod.y2 == canvas_bounds.y2) {
    return src_box;
  }
  // Same denominators as SampleClipCanonicalPx.
  const double cw = std::max(1, canvas_bounds.x2 - canvas_bounds.x1);
  const double ch = std::max(1, canvas_bounds.y2 - canvas_bounds.y1);
  const double rod_w = rod.x2 - rod.x1;
  const double rod_h = rod.y2 - rod.y1;
  if (rod_w <= 0.0 || rod_h <= 0.0) {
    return {};
  }
  // Canonical pixel index whose center maps to source coordinate s.
  auto to_canvas = [](double s, double r1, double c1, double r_len,
                      double c_len) {
    return c1 + (s - r1) * c_len / r_len - 0.5;
  };
  const double pad_x = 2.0 + std::ceil(cw / rod_w);
  const double pad_y = 2.0 + std::ceil(ch / rod_h);
  const double x1 = std::floor(to_canvas(src_box.x1 - 0.5, rod.x1,
                                         canvas_bounds.x1, rod_w, cw)) -
                    pad_x;
  const double x2 = std::ceil(to_canvas(src_box.x2 + 0.5, rod.x1,
                                        canvas_bounds.x1, rod_w, cw)) +
                    pad_x;
  const double y1 = std::floor(to_canvas(src_box.y1 - 0.5, rod.y1,
                                         canvas_bounds.y1, rod_h, ch)) -
                    pad_y;
  const double y2 = std::ceil(to_canvas(src_box.y2 + 0.5, rod.y1,
                                        canvas_bounds.y1, rod_h, ch)) +
                    pad_y;
  // Clamp to a safe int range; callers compare against output pixels.
  auto to_int = [](double v) {
    return static_cast<int>(std::min(1e9, std::max(-1e9, v)));
  };
  return {to_int(x1), to_int(y1), to_int(x2), to_int(y2)};
}

// ---------------------------------------------------------------------------
// Fold geometry: sheet shape, hinges, per-frame fold layout
// ---------------------------------------------------------------------------

// Shape of the paper sheet before folding, in canonical canvas pixels.
// Either an analytic box (opaque full-frame clips) or a matte bounding box
// plus SDF and outline (cutouts, titles, logos).
struct SheetGeometry {
  // False when the matte is empty: there is no paper to fold.
  bool valid = false;
  // DistPx uses the exact box SDF instead of `sdf`.
  bool analytic_box = false;
  // Matte SDF for non-box sheets; not owned, must outlive the sheet.
  const CoarseSignedDistanceField* sdf = nullptr;
  float cx = 0.0f;  // Bounding-box center.
  float cy = 0.0f;
  // Half bounding-box size, px.
  float half_w = 1.0f;
  float half_h = 1.0f;
  // Matte outline relative to (cx, cy), grown by the collar; empty for boxes.
  std::vector<std::array<float, 2>> outline;
  // Added to outline support: collar growth plus half an SDF cell.
  float outline_grow = 0.0f;

  // Signed distance to the matte edge (negative inside).
  // (ux, uy) are canonical px.
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
  // (ux, uy) is a unit vector; the result is px from (cx, cy). Boxes use the
  // rectangle support h(u) = half_w |ux| + half_h |uy|. Costs O(outline);
  // only called while building the layout, never per pixel.
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

// Sheet of an opaque full-frame clip: the canvas as an analytic box.
SheetGeometry FullFrameSheet(const PixelRect& canvas_bounds) {
  SheetGeometry g;
  g.valid = true;
  g.analytic_box = true;
  g.cx = 0.5f * static_cast<float>(canvas_bounds.x1 + canvas_bounds.x2);
  g.cy = 0.5f * static_cast<float>(canvas_bounds.y1 + canvas_bounds.y2);
  // 1.5 px of overscan keeps frame-edge pixels fully opaque once flat.
  g.half_w =
      0.5f * static_cast<float>(canvas_bounds.x2 - canvas_bounds.x1) + 1.5f;
  g.half_h =
      0.5f * static_cast<float>(canvas_bounds.y2 - canvas_bounds.y1) + 1.5f;
  return g;
}

// Bounding box of the matte, grown by `grow_px` for the paper collar.
// Returns an invalid sheet when no cell is inside. Half sizes are floored at
// 16 px. The outline keeps every `stride`-th cell (about 240 per side) that
// lies within two strides of the matte edge or on the frame border. Serial;
// runs once per frame.
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

// Maximum folds per sheet (Fold Stages clamps to [2, 5]). Also bounds the
// recursion depth of EvalFoldStack and EvalFoldCoverage.
constexpr int kMaxFoldHinges = 5;

// One straight fold line in sheet-local px. A point's signed distance past
// the hinge is d = (p - foot) . n; the flap is the d > 0 side, which turns by
// phi about the line.
struct FoldHinge {
  float px = 0.0f;  // Foot point on the hinge line, sheet-local px.
  float py = 0.0f;
  float nx = 1.0f;  // Unit normal pointing into the moving flap.
  float ny = 0.0f;
  float tx = 0.0f;  // Unit tangent along the hinge line.
  float ty = 1.0f;
  float phi = 0.0f;  // Fold angle: 0 = flat, near pi = folded over.
  // Cached cos(phi), sin(phi); cos_phi < 0 means the flap is folded over.
  float cos_phi = 1.0f;
  float sin_phi = 0.0f;
  bool valley = true;  // Valley flaps lift toward the viewer, mountain away.
};

// Fold state of one frame: hinges in folding order (hinge 0 is folded first
// and opened last) plus the rigid motion of the whole packet.
struct FoldLayout {
  std::array<FoldHinge, kMaxFoldHinges> hinges;
  int count = 0;
  float persp_k = 0.0f;  // Tangential magnification per px of lift.
  // Motion of the whole folded packet, applied after the hinges.
  float offset_x = 0.0f;  // Slide offset, canvas px.
  float offset_y = 0.0f;
  // Packet rotation, packet scale (> 1 while lifted toward the camera), and
  // global fade-in alpha.
  float rot_cos = 1.0f;
  float rot_sin = 0.0f;
  float scale = 1.0f;
  float alpha = 1.0f;
  float lift = 0.0f;  // 1 while the packet is in the air (sliding in or out).
};

// Per-pattern recipe for one hinge (angles in degrees).
struct HingeTemplate {
  float deg;      // Normal direction relative to the fold direction.
  float frac;     // Hinge position from the core (0) to the sheet edge (1).
  float max_deg;  // Folded angle; staying under 180 keeps a sliver of depth.
  // Valley (true) or mountain fold.
  bool valley;
};

// Builds the hinges and packet motion for `openness` in [0, 1]: 0 = folded
// packet out of frame (or invisible), 1 = flat sheet at rest. `seed` adds a
// small hand-folded irregularity to hinge angles and positions.
//
// frame_diag = canvas diagonal px (unit of `slide_distance`); direction_deg
// rotates the whole pattern; `depth` (Fold Depth) scales the perspective.
// Timeline over q: in [0, 0.18] the folded packet tosses in (offset, spin,
// and scale follow (1 - smoothstep)^2); in [0.10, 1] the hinges open one
// after another, each over `span` with 22% overlap. Runs once per frame on
// the render thread.
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
  // Up to ~30% x depth magnification for a flap lifted by a sheet radius.
  layout.persp_k =
      0.30f * Clamp(depth, 0.0f, 2.0f) / std::max(64.0f, std::hypot(rx, ry));

  // Unit vector at `deg` relative to the fold direction, with mirroring.
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
          {-90.0f, 0.62f, 168.0f, true},
          {180.0f, 0.60f, 169.0f, true},
          {0.0f, 0.62f, 170.0f, true},
          {90.0f, 0.05f, 174.0f, true},
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
          {140.0f, 0.74f, 158.0f, true},
          {-40.0f, 0.70f, 160.0f, true},
          {35.0f, 0.72f, 162.0f, true},
          {96.0f, 0.05f, 172.0f, true},
          {6.0f, 0.05f, 176.0f, true}};
      take_last(kCraft);
      break;
    }
  }
  // Hand-folded irregularity: +-4 degrees and +-0.03 reach per hinge.
  if (pattern != FoldPattern::kAccordion) {
    for (int k = 0; k < stages; ++k) {
      tpl[k].deg += (Hash2D(k, 1, seed) - 0.5f) * 8.0f;
      tpl[k].frac =
          std::max(0.02f, tpl[k].frac + (Hash2D(k, 2, seed) - 0.5f) * 0.06f);
    }
  }

  // Core point (sheet-local px) that stays put on every hinge's stationary
  // side.
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
    const float start =
        static_cast<float>(stages - 1 - k) * span * (1.0f - kOverlap);
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
  // Toss path: 120 degrees off the fold direction, spinning up to 24 deg.
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

  // Inclusive containment test.
  bool Contains(float x, float y) const {
    return x >= x1 && x <= x2 && y >= y1 && y <= y2;
  }
};

// Fixed-capacity convex polygon for fold-bounds clipping (avoids per-polygon
// heap allocations during FoldSheetBounds).
// Clipping a convex polygon by one line adds at most one vertex, so 4 + 5
// vertices is the worst case; extra vertices would be silently dropped.
struct FoldPolygon {
  static constexpr std::size_t kMaxVertices = 16;
  std::array<std::array<float, 2>, kMaxVertices> pts{};
  std::size_t count = 0;

  // Minimal vector-like API, so range-for and ClipToHingeSide work as they
  // would with std::vector.
  FoldPolygon() = default;
  FoldPolygon(std::initializer_list<std::array<float, 2>> init) {
    for (const auto& p : init) {
      push_back(p);
    }
  }

  void push_back(const std::array<float, 2>& p) {
    if (count < kMaxVertices) {
      pts[count++] = p;
    }
  }
  std::size_t size() const { return count; }
  const std::array<float, 2>& operator[](std::size_t i) const { return pts[i]; }
  const std::array<float, 2>* begin() const { return pts.data(); }
  const std::array<float, 2>* end() const { return pts.data() + count; }
};

// Bounds of every piece vertex mapped by `map`, padded by `pad`; empty (no
// point inside) when there are no pieces.
// MapFn is void(float x, float y, float* ox, float* oy). The box starts
// inverted (x1 = +1e30, x2 = -1e30) so an empty list contains nothing.
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
// One Sutherland-Hodgman pass against a half-plane: sign = +1 keeps the
// stationary side (d <= limit), sign = -1 keeps the flap side (d >= -limit).
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
// The folded distance from the hinge is c * s, with c = cos(phi) kept at
// least 0.03 away from 0 like UnfoldFlapPoint's divisor; tangential extents
// grow or shrink by the perspective magnification of the lifted face.
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
  for (const auto& dt :
       {std::array<float, 2>{d_lo, t_lo}, std::array<float, 2>{d_hi, t_lo},
        std::array<float, 2>{d_hi, t_hi}, std::array<float, 2>{d_lo, t_hi}}) {
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
// Each level is padded by 2 px. Up to 2^count pieces. Serial; runs once per
// frame per bounds set.
std::vector<FoldPolygon> FoldSheetBounds(
    const SheetGeometry& sheet, const FoldLayout& layout, float grow_px,
    std::array<BoundsBox, kMaxFoldHinges + 1>* levels) {
  const float hw = sheet.half_w + grow_px;
  const float hh = sheet.half_h + grow_px;
  std::vector<FoldPolygon> pieces;
  pieces.reserve(1u << std::min(layout.count, kMaxFoldHinges));
  pieces.push_back(FoldPolygon{{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}});
  std::vector<FoldPolygon> next;
  next.reserve(pieces.capacity());
  const auto same = [](float x, float y, float* ox, float* oy) {
    *ox = x;
    *oy = y;
  };
  (*levels)[0] = BoundsOfPieces(pieces, 2.0f, same);
  for (int k = 0; k < layout.count; ++k) {
    const FoldHinge& h = layout.hinges[k];
    if (h.phi > 1e-3f) {
      next.clear();
      for (const FoldPolygon& piece : pieces) {
        // The stationary side keeps the 1.25 px coverage ramp past the hinge.
        const FoldPolygon stay = ClipToHingeSide(piece, h, 1.0f, 1.5f);
        if (stay.size() >= 3) {
          next.push_back(stay);
        }
        const FoldPolygon flap = ClipToHingeSide(piece, h, -1.0f, 0.0f);
        if (flap.size() >= 3) {
          next.push_back(FoldedFlapBounds(layout, h, flap));
        }
      }
      std::swap(pieces, next);
    }
    (*levels)[k + 1] = BoundsOfPieces(pieces, 2.0f, same);
  }
  return pieces;
}

// ---------------------------------------------------------------------------
// Fold shading and per-pixel evaluation
// ---------------------------------------------------------------------------

// Everything the per-pixel fold evaluators need. Built once per frame on the
// render thread, then shared read-only by all parallel_for workers. Lengths
// are canonical px unless noted.
struct FoldShadeContext {
  // Not owned; must outlive the render.
  const SheetGeometry* sheet = nullptr;
  const FoldLayout* layout = nullptr;
  float origin_x = 0.0f;  // Canvas origin and size for texture coordinates.
  float origin_y = 0.0f;
  // min(canvas width, height): unit of the normalized `nu` coordinates.
  float min_dim = 1.0f;
  bool cutout_sheet = false;  // Input already carries its cutout border.
  // Distance past the coarse matte edge still sampled on cutout sheets.
  float cutout_sdf_slack_px = 5.0f;
  // Torn collar: width and tear amplitude in px, tear features per min_dim,
  // and tear octaves.
  bool enable_border = true;
  float border_px = 0.0f;
  float tear_amp_px = 0.0f;
  float tear_frequency = 24.0f;
  int tear_detail = 5;
  // Texture seed; tear seed (Paper Cutout FX boils it over time).
  std::uint32_t seed = 7u;
  std::uint32_t tear_seed = 7u;
  // Cardstock color, linear 0..1.
  float paper_r = 0.965f;
  float paper_g = 0.945f;
  float paper_b = 0.895f;
  // Grain and crease strength (0..1) and scale, as in CutoutParams.
  float grain_strength = 0.35f;
  float grain_scale = 1.0f;
  float crease_strength = 0.45f;
  float crease_scale = 3.2f;
  float media_overlay = 0.24f;  // Grain/crease multiply on the printed image.
  float hinge_crease = 1.0f;    // Visibility of crease lines along hinges.
  // Half width of the dark valley line along hinges.
  float crease_band_px = 6.0f;
  float subject_margin_px = 0.0f;  // Torn edge must cut this deep to clip ink.
  float light_x = -0.42f;          // Unit vector toward the light.
  float light_y = 0.42f;
  float light_z = 0.80f;
  // Flap cast shadows: max darkening, fall direction, offset at full lift, and
  // softness.
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
  // FlapLight per hinge, indexed [k][toward_viewer]; ConfigureFoldLight.
  std::array<std::array<float, 2>, kMaxFoldHinges> flap_light = {};
};

// Lambert shading of the visible flap face relative to a flat sheet.
// The flap normal in the (hinge normal, z) plane is (+-sin phi, cos phi),
// flipped toward the viewer. Ambient 0.62 plus Lambert diffuse, divided by
// the flat sheet's value so an unfolded flap gives 1; clamped to
// [0.62, 1.15].
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

// Points the light opposite the drop-shadow direction (shadows fall along
// shadow_angle_deg) at elevation (0.6 horizontal, 0.8 vertical: a unit
// vector) and caches FlapLight for every hinge. Call once ctx->layout is
// final.
void ConfigureFoldLight(float shadow_angle_deg, FoldShadeContext* ctx) {
  const float rad = shadow_angle_deg * kDegToRad;
  ctx->shadow_dir_x = std::cos(rad);
  ctx->shadow_dir_y = std::sin(rad);
  ctx->light_x = -0.6f * ctx->shadow_dir_x;
  ctx->light_y = -0.6f * ctx->shadow_dir_y;
  ctx->light_z = 0.8f;
  if (ctx->layout != nullptr) {
    for (int k = 0; k < ctx->layout->count; ++k) {
      const FoldHinge& h = ctx->layout->hinges[k];
      ctx->flap_light[k][0] = FlapLight(*ctx, h, false);
      ctx->flap_light[k][1] = FlapLight(*ctx, h, true);
    }
  }
}

// Flat coordinates of the flap point seen at (d, t) from hinge `h`: `d` px
// past the hinge for an open flap, or `-d` px back over the stationary side
// for a folded-over flap.
//
// Inverts the hinge rotation: projected distance d maps to flat distance
// s = d / cos(phi) (open) or -d / -cos(phi) (folded over), with the cosine
// kept >= 0.03 so near-vertical flaps stay finite. The face sits
// s * sin(phi) above or below the sheet (+3 px for folded-over layers so
// they never coincide), and the tangential coordinate is divided by the
// perspective magnification 1 + persp_k * lift (floored at 0.5).
void UnfoldFlapPoint(const FoldLayout& layout, const FoldHinge& h, float d,
                     float t, bool folded_over, bool toward_viewer, float* xf,
                     float* yf) {
  const float s = folded_over ? std::max(0.0f, -d) / std::max(0.03f, -h.cos_phi)
                              : d / std::max(0.03f, h.cos_phi);
  const float lift = (s * h.sin_phi + (folded_over ? 3.0f : 0.0f)) *
                     (toward_viewer ? 1.0f : -1.0f);
  const float tf = t / std::max(0.5f, 1.0f + layout.persp_k * lift);
  *xf = h.px + s * h.nx + tf * h.tx;
  *yf = h.py + s * h.ny + tf * h.ty;
}

// Union coverage of every paper layer at sheet-local (x, y), for shadows.
//
// k = outermost hinge still to undo; (x, y) = packet-local px seen through
// hinges 0..k; back_side flips valley and mountain. Returns paper coverage
// in [0, 1] with a soft_px edge; overlapping layers combine as a union
// (a + b - a * b). Folded-over hinges recurse into the flap and the sheet
// beneath (depth <= kMaxFoldHinges); coverage_bounds prunes empty regions.
// Called per pixel from parallel_for workers.
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
// Uses the coverage-weighted centroid over a 40 x 40 grid spanning 1.15x the
// sheet half extents, rotated and scaled into canvas space. The weight fades
// out over openness 0.55..1. Serial: ~1600 EvalFoldCoverage calls a frame.
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
  layout->offset_x -=
      (layout->rot_cos * cx - layout->rot_sin * cy) * layout->scale;
  layout->offset_y -=
      (layout->rot_sin * cx + layout->rot_cos * cy) * layout->scale;
}

// Darkened valley and lit ridge along the hinge lines, with a hand-made wobble.
// (x, y) are flat sheet-local px; nu_x/nu_y feed the +-1.6 px wobble noise.
// Returns a brightness multiplier (1 away from hinges): per hinge, up to 26%
// darker in the valley and 9% brighter on the ridge at full strength. The
// first loop is a cheap early-out when every hinge is out of reach.
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
    nearest =
        std::min(nearest, std::abs((x - h.px) * h.nx + (y - h.py) * h.ny));
  }
  if (nearest > band * 2.2f + 1.6f) {
    return 1.0f;
  }
  const float wobble =
      (ValueNoise2D(nu_x * 18.0f, nu_y * 18.0f, ctx.seed + 313u) - 0.5f) * 3.2f;
  const float k = ctx.crease_strength * ctx.hinge_crease;
  float mul = 1.0f;
  for (int j = 0; j < layout.count; ++j) {
    const FoldHinge& h = layout.hinges[j];
    const float hd = std::abs((x - h.px) * h.nx + (y - h.py) * h.ny + wobble);
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
//
// (x, y) = flat sheet-local px; `shade` = lighting multiplier gathered so
// far; subject(ux, uy, dist_px) returns premultiplied print color at
// canonical px. Two modes:
//   - cutout_sheet: the clip is already a finished cutout with its own torn
//     border, so its (clean) alpha is the sheet; only shading and the paper
//     back are added.
//   - otherwise: the torn collar is built here from the sheet SDF, the
//     border width, and tear noise (~2.4 px anti-aliased edge).
// The back is paper color x grain x creases x 0.93, slightly cooler
// (G x 0.985, B x 0.95), with up to 30% luma bleed-through of the print.
// `rim` brightens the torn edge up to 8% within 3 px (exposed fibers).
// Leaf of the EvalFoldStack recursion; runs inside parallel_for workers.
template <typename SubjectFn>
RgbaColor EvalPaperLeaf(const FoldShadeContext& ctx, float x, float y,
                        bool back_side, float shade, const SubjectFn& subject) {
  const float ux = ctx.sheet->cx + x;
  const float uy = ctx.sheet->cy + y;
  const float dist_px = ctx.sheet->DistPx(ux, uy);
  if (ctx.cutout_sheet) {
    if (dist_px > ctx.cutout_sdf_slack_px) {
      return {};
    }
    const RgbaColor ink = subject(ux, uy, dist_px);
    if (ink.a <= 1e-4f) {
      return {};
    }
    const float nu_x = (ux - ctx.origin_x) / ctx.min_dim;
    const float nu_y = (uy - ctx.origin_y) / ctx.min_dim;
    const float hinge_mul = HingeCreaseMarks(ctx, x, y, nu_x, nu_y);
    if (back_side) {
      const float sheet_alpha = Clamp01(ink.a);
      const float grain =
          ctx.grain_strength > 1e-3f
              ? Lerp(1.0f,
                     PaperFiberGrain(nu_x, nu_y, ctx.grain_scale, ctx.seed),
                     Clamp01(ctx.grain_strength))
              : 1.0f;
      const float crease_val =
          ctx.crease_strength > 1e-3f
              ? PaperCreaseField(nu_x, nu_y, ctx.crease_scale, ctx.seed + 89u)
              : 0.0f;
      const float crease_mul =
          (1.0f - 0.34f * Clamp01(ctx.crease_strength) * crease_val) *
          hinge_mul;
      const float luma =
          (0.299f * ink.r + 0.587f * ink.g + 0.114f * ink.b) / ink.a;
      const float bleed =
          Lerp(1.0f, 0.86f + 0.14f * Clamp01(luma), 0.3f * Clamp01(ink.a));
      const float s =
          Clamp(0.93f * grain * crease_mul * shade * bleed, 0.2f, 1.12f);
      return {Clamp01(ctx.paper_r * s) * sheet_alpha,
              Clamp01(ctx.paper_g * 0.985f * s) * sheet_alpha,
              Clamp01(ctx.paper_b * 0.95f * s) * sheet_alpha, sheet_alpha};
    }
    const float mul = hinge_mul * shade;
    return {ink.r * mul, ink.g * mul, ink.b * mul, ink.a};
  }
  const float base_edge = dist_px - ctx.border_px;
  if (base_edge > ctx.tear_amp_px + 1.3f) {
    return {};
  }
  const float nu_x = (ux - ctx.origin_x) / ctx.min_dim;
  const float nu_y = (uy - ctx.origin_y) / ctx.min_dim;
  const float tear_n =
      (ctx.tear_amp_px > 0.01f && std::abs(base_edge) <= ctx.tear_amp_px + 3.1f)
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
      bleed = Lerp(1.0f, 0.86f + 0.14f * Clamp01(luma), 0.3f * Clamp01(ink.a));
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
//
// k = hinge to undo next (start at layout.count - 1); (x, y) = packet-local
// px; back_side = looking at the paper back; `shade` = lighting so far.
//   - Open hinges (cos phi >= 0): flap pixels map back to flat; flap light
//     and crease ambient occlusion multiply `shade`.
//   - Folded-over hinges (cos phi < 0): two layers, the flap and the sheet.
//     Valley flaps lie on top, mountain flaps below; they are composited
//     with premultiplied Over and scaled by the 2.5 px coverage ramp `cov`.
// If the top layer and `cov` are both >= 0.999, the bottom recursion is
// skipped: the bottom could add < 0.1%, so this is an approximation, not a
// bit-identical shortcut. stack_bounds prunes regions without paper, which
// keeps the worst case of 2^k calls rare. Returns premultiplied color.
// Called per pixel from parallel_for workers.
template <typename SubjectFn>
RgbaColor EvalFoldStack(const FoldShadeContext& ctx, int k, float x, float y,
                        bool back_side, float shade, const SubjectFn& subject) {
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
    const bool toward_viewer = h.valley != back_side;

    if (h.cos_phi >= 0.0f) {
      // Open past 90 degrees: one layer per pixel, darkened in the crease.
      if (d <= 0.0f) {
        shade *= 1.0f - 0.14f * h.sin_phi * Smoothstep(-ao_band, 0.0f, d);
      } else {
        const float t = (x - h.px) * h.tx + (y - h.py) * h.ty;
        UnfoldFlapPoint(*ctx.layout, h, d, t, false, toward_viewer, &x, &y);
        const float s = d / std::max(0.03f, h.cos_phi);
        shade *= ctx.flap_light[k][toward_viewer ? 1 : 0] *
                 (1.0f - 0.15f * h.sin_phi * Smoothstep(ao_band, 0.0f, s));
      }
      continue;
    }

    // Folded over: the flap lies on the stationary side (d < 0).
    if (d >= 1.25f) {
      return {};
    }
    const float t = (x - h.px) * h.tx + (y - h.py) * h.ty;
    const float cov = Smoothstep(1.25f, -1.25f, d);
    float xf = 0.0f;
    float yf = 0.0f;
    UnfoldFlapPoint(*ctx.layout, h, d, t, true, toward_viewer, &xf, &yf);
    const float flap_shade = shade * ctx.flap_light[k][toward_viewer ? 1 : 0];

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
      // `tuck` darkens the crease contact; total darkening is capped at 70%.
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

// ---------------------------------------------------------------------------
// Packet motion and fold action
// ---------------------------------------------------------------------------

// Maps canvas pixels into the folded packet's sheet-local frame.
// Origin is in canvas px for the transition; for the cutout's Paper Unfold
// it is relative to the canvas center in master-local px. ToLocal applies
// the inverse rigid motion; ToCanvas is the forward motion, used to map
// packet bounds to the canvas for pruning.
struct PacketMapper {
  float origin_x = 0.0f;  // Sheet center plus packet offset and jitter.
  float origin_y = 0.0f;
  float cos_r = 1.0f;  // Forward packet rotation.
  float sin_r = 0.0f;
  // 1 / packet scale.
  float inv_scale = 1.0f;

  // Canvas -> packet-local px.
  void ToLocal(float px, float py, float* lx, float* ly) const {
    const float dx = px - origin_x;
    const float dy = py - origin_y;
    *lx = (dx * cos_r + dy * sin_r) * inv_scale;
    *ly = (-dx * sin_r + dy * cos_r) * inv_scale;
  }

  // Packet-local -> canvas px.
  void ToCanvas(float lx, float ly, float* px, float* py) const {
    *px = origin_x + (lx * cos_r - ly * sin_r) / inv_scale;
    *py = origin_y + (lx * sin_r + ly * cos_r) / inv_scale;
  }
};

// Builds the matte SDF of a transition clip: identity placement, so the
// matte is the clip alpha in canonical px of `canvas_bounds`. With
// `ignore_shadow`, a Paper Cutout FX clip's baked black drop shadow counts
// as outside the matte. Shared by RenderPaperFoldTransition and
// internal::BuildCleanCutoutAlphaForClip so the unit tests exercise the
// exact render setup. Runs on the calling thread; fans out via parallel_for.
CoarseSignedDistanceField BuildClipMatteSdf(const ImageView& clip,
                                            const PixelRect& canvas_bounds,
                                            const ParallelRowsFn& parallel_for,
                                            bool ignore_shadow) {
  CutoutParams matte_params;
  matte_params.source_mode = 0;
  matte_params.media_scale = 1.0f;
  matte_params.enable_chroma_key = false;
  const SubjectTransform identity =
      BuildSubjectTransform(matte_params, clip, canvas_bounds);
  return BuildSubjectSignedDistanceField(
      matte_params, identity, clip, canvas_bounds, parallel_for, ignore_shadow);
}

}  // namespace

namespace internal {

// Resolves Fold Action (clamped to [0, 3]). kAuto inspects both clips: fold
// out when only the incoming side is empty (clip end), unfold in when the
// outgoing side is empty or the incoming clip is opaque full-frame (it
// covers everything), otherwise fold one cutout away and unfold the other.
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

// Test-only entry point (see papercraft_kernel_internal.h): the same matte
// SDF and clean-alpha calls RenderPaperFoldTransition makes for a cutout
// clip (`is_cutout` = true there).
std::vector<float> BuildCleanCutoutAlphaForClip(
    const ImageView& clip, const PixelRect& canvas_bounds,
    const ParallelRowsFn& parallel_for, PixelRect* raw_alpha_box) {
  const CoarseSignedDistanceField sdf = BuildClipMatteSdf(
      clip, canvas_bounds, parallel_for, /*ignore_shadow=*/true);
  return BuildCleanCutoutAlpha(clip, canvas_bounds, sdf, parallel_for,
                               raw_alpha_box);
}

}  // namespace internal

// ---------------------------------------------------------------------------
// Public renderers (declared in papercraft_kernel.h)
//
// Called from the OpenFX render action (kOfxImageEffectActionRender) in
// papercraft_ofx_plugin.cc, once per render-window request. canvas_bounds
// is the output RoD (full frame at the current render scale). Setup runs on
// the calling host thread; per-pixel work fans out through `parallel_for`.
// ---------------------------------------------------------------------------

// See header. Only 4-component buffers are supported; negative row_bytes is
// valid.
bool IsValidRgbaView(const ImageView& view) {
  return view.data != nullptr && view.components == 4 && view.row_bytes != 0 &&
         view.bounds.x2 > view.bounds.x1 && view.bounds.y2 > view.bounds.y1;
}

// See header. Flat path per output pixel: output px -> canonical px
// (MapOutputToPaperCanonical) -> torn-edge alpha from the SDF plus tear
// noise -> subject with Edge Choke -> grain and crease overlay -> paper
// collar under the subject -> drop shadow under everything. The Paper
// Unfold (3D) style takes the fold-machinery branch instead. Output is
// premultiplied.
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

  // "Source Is Premultiplied" off overrides host metadata for straight-alpha
  // media that the host labels as premultiplied.
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

  const std::ptrdiff_t out_pixel_bytes = PixelSizeBytes(output);
  const PixelDepth out_depth = output.depth;

  // If collapsed below visibility threshold at transition endpoints, clear.
  if (anim.ease_p <= 1e-4f && anim.style != CutoutAnimStyle::kNone) {
    ClearOutputRows(output, x_begin, x_end, y_begin, y_end, parallel_for);
    return;
  }

  // The coarse SDF (the most expensive setup step) is built only when
  // something reads the distance to the matte.
  const bool need_sdf = params.enable_border || params.shadow_opacity > 1e-4f ||
                        std::abs(params.edge_choke_px) > 0.01f ||
                        anim.style == CutoutAnimStyle::kPaperUnfold;
  const CoarseSignedDistanceField sdf =
      need_sdf ? BuildSubjectSignedDistanceField(params, subj_xf, src_view,
                                                 canvas_bounds, parallel_for)
               : CoarseSignedDistanceField{};

  const float border_px =
      params.enable_border ? std::max(0.0f, params.border_width_norm * min_dim)
                           : 0.0f;
  const float tear_amp_px =
      params.enable_border
          ? std::max(0.0f, params.tear_roughness_norm * min_dim)
          : 0.0f;
  const std::uint32_t seed =
      static_cast<std::uint32_t>(std::max(0, params.random_seed));
  // Edge Boil: the tear seed advances every 1 / edge_boil stepped frames,
  // re-cutting the torn edge like stop motion.
  const std::uint32_t boil_seed =
      seed + static_cast<std::uint32_t>(
                 std::max(0.0f, std::floor(anim.step_time * params.edge_boil)));

  // Drop shadow offset and softness, px (fractions of min_dim).
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
      ClearOutputRows(output, x_begin, x_end, y_begin, y_end, parallel_for);
      return;
    }
    const float q = anim.raw_p;
    // Glide Angle (default -22) also turns the fold direction (default 0).
    FoldLayout layout = BuildFoldLayout(
        FoldPattern::kCraftFlaps, 4, q, sheet, std::hypot(cw, ch),
        params.transition_angle_deg + 22.0f, 0.55f, 0.0f,
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
    // Folds always show some creasing; grain shows more while folded, and the
    // hinge creases fade as the sheet lands flat.
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
    // Coarse-SDF error margin: two cells plus 3 px.
    const float sdf_slack = 2.0f * static_cast<float>(sdf.step) + 3.0f;
    const std::vector<FoldPolygon> paper_cover = FoldSheetBounds(
        sheet, layout, border_px + tear_amp_px + sdf_slack, &ctx.stack_bounds);
    const std::vector<FoldPolygon> coverage_cover = FoldSheetBounds(
        sheet, layout,
        border_px + std::max({1.2f, sh_soft_px, ctx.flap_shadow_soft_px}) +
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
    // Output px -> packet-local px: inverse master rotation/scale and flips,
    // then the inverse packet motion (which replaces slide and pop here).
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

    // The drop shadow reaches up to 3.5x further while the packet is lifted.
    const float lift_k = 1.0f + 2.5f * layout.lift;
    const float m_scale = 1.0f / anim.inv_m_scale;
    // Forward of to_local, used to map packet bounds into output px.
    const auto to_canvas = [&](float lx, float ly, float* cx, float* cy) {
      float rx = 0.0f;
      float ry = 0.0f;
      packet.ToCanvas(lx, ly, &rx, &ry);
      if (params.flip_h) {
        rx = -rx;
      }
      if (params.flip_v) {
        ry = -ry;
      }
      *cx = cx_out + (rx * anim.cos_m + ry * anim.sin_m) * m_scale;
      *cy = cy_out + (-rx * anim.sin_m + ry * anim.cos_m) * m_scale;
    };
    const auto to_shadow = [&](float lx, float ly, float* cx, float* cy) {
      to_canvas(lx, ly, cx, cy);
      *cx += sh_dx_px * lift_k;
      *cy += sh_dy_px * lift_k;
    };
    // Output-space boxes of the paper and its shadow; pixels outside both stay
    // transparent without evaluating the fold stack.
    const BoundsBox paper_box = BoundsOfPieces(paper_cover, 3.0f, to_canvas);
    const BoundsBox shadow_box =
        BoundsOfPieces(coverage_cover, 3.0f, to_shadow);

    RunScanlines(y_begin, y_end, parallel_for, [&](int y) {
      const float py_out = static_cast<float>(y) + 0.5f;
      const bool row_paper = py_out >= paper_box.y1 && py_out <= paper_box.y2;
      const bool row_shadow = params.shadow_opacity > 1e-4f &&
                              py_out >= shadow_box.y1 &&
                              py_out <= shadow_box.y2;
      std::uint8_t* out_px = PixelMutPtr(output, x_begin, y);
      if (!row_paper && !row_shadow) {
        for (int x = x_begin; x < x_end; ++x, out_px += out_pixel_bytes) {
          WritePixelPtr(out_px, out_depth, {});
        }
        return;
      }
      for (int x = x_begin; x < x_end; ++x, out_px += out_pixel_bytes) {
        const float px_out = static_cast<float>(x) + 0.5f;
        RgbaColor fg;
        if (row_paper && px_out >= paper_box.x1 && px_out <= paper_box.x2) {
          float lx = 0.0f;
          float ly = 0.0f;
          to_local(px_out, py_out, &lx, &ly);
          fg = ScalePremult(EvalFoldStack(ctx, layout.count - 1, lx, ly, false,
                                          1.0f, sample_subject),
                            layout.alpha);
          // The shadow underneath would add < 0.1% alpha; skip it.
          if (fg.a >= 0.999f) {
            WritePixelPtr(out_px, out_depth, fg);
            continue;
          }
        }
        float shadow_a = 0.0f;
        if (row_shadow && px_out >= shadow_box.x1 && px_out <= shadow_box.x2) {
          float slx = 0.0f;
          float sly = 0.0f;
          to_local(px_out - sh_dx_px * lift_k, py_out - sh_dy_px * lift_k, &slx,
                   &sly);
          shadow_a = params.shadow_opacity * layout.alpha *
                     EvalFoldCoverage(ctx, layout.count - 1, slx, sly, false,
                                      sh_soft_px);
        }
        WritePixelPtr(out_px, out_depth,
                      CompositeOver(fg, {0.0f, 0.0f, 0.0f, shadow_a}));
      }
    });
    return;
  }

  // Flat path (no fold): every output pixel is evaluated independently by the
  // parallel_for workers below.
  const float inv_min_dim = 1.0f / min_dim;
  RunScanlines(y_begin, y_end, parallel_for, [&](int y) {
    const float py_out = static_cast<float>(y) + 0.5f;
    std::uint8_t* out_px = PixelMutPtr(output, x_begin, y);
    for (int x = x_begin; x < x_end; ++x, out_px += out_pixel_bytes) {
      const float px_out = static_cast<float>(x) + 0.5f;
      float ux = 0.0f;
      float uy = 0.0f;
      float anim_crease = 0.0f;
      MapOutputToPaperCanonical(params, anim, cw, ch, origin_x, origin_y,
                                px_out, py_out, true, &ux, &uy, &anim_crease);

      const float nu_x = (ux - origin_x) * inv_min_dim;
      const float nu_y = (uy - origin_y) * inv_min_dim;

      // Evaluate paper tear boundary only within the possible torn-edge band.
      float dist_px = 0.0f;
      float edge_sdf = 0.0f;
      float sheet_alpha = 1.0f;
      if (need_sdf) {
        dist_px = sdf.SamplePx(ux, uy);
        const float base_edge_dist =
            dist_px - border_px -
            (params.enable_border ? 0.0f
                                  : std::max(0.0f, params.edge_choke_px));
        const float tear_n =
            (params.enable_border && tear_amp_px > 0.01f &&
             std::abs(base_edge_dist) <= tear_amp_px + 3.1f)
                ? TearEdgeNoise(nu_x, nu_y, params.tear_frequency,
                                params.tear_detail, boil_seed)
                : 0.0f;
        edge_sdf = base_edge_dist - tear_amp_px * tear_n;
        sheet_alpha = Smoothstep(1.2f, -1.2f, edge_sdf);
      }

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
                                    px_out - sh_dx_px, py_out - sh_dy_px, false,
                                    &sux, &suy, &unused_crease);
          const float s_dist_px = sdf.SamplePx(sux, suy) - border_px;
          shadow_alpha =
              params.shadow_opacity * Smoothstep(sh_soft_px, -2.0f, s_dist_px);
        }
        return {0.0f, 0.0f, 0.0f, shadow_alpha};
      };

      if (sheet_alpha <= 1e-4f) {
        WritePixelPtr(out_px, out_depth, shadow_at_pixel());
        continue;
      }

      // Evaluate full-res subject before cardstock grain/crease shading.
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

      const bool need_overlay =
          params.media_texture_overlay > 1e-4f && subject.a > 1e-4f;
      const bool need_paper_bg = params.enable_border && subject.a < 1.0f;

      float grain = 1.0f;
      float crease_mul = 1.0f;
      if (need_overlay || need_paper_bg) {
        if (params.grain_strength > 1e-3f) {
          grain =
              Lerp(1.0f, PaperFiberGrain(nu_x, nu_y, params.grain_scale, seed),
                   Clamp01(params.grain_strength));
        }
        if (params.crease_strength > 1e-3f) {
          const float crease_val =
              PaperCreaseField(nu_x, nu_y, params.crease_scale, seed + 89u);
          crease_mul =
              1.0f - 0.34f * Clamp01(params.crease_strength) * crease_val;
        }
      }

      // Apply printed cardstock crease texture subtly onto the subject ink.
      if (need_overlay) {
        const float ink_mul = Lerp(1.0f, grain * crease_mul,
                                   Clamp01(params.media_texture_overlay));
        subject.r *= ink_mul;
        subject.g *= ink_mul;
        subject.b *= ink_mul;
      }

      RgbaColor cutout_fg;
      if (need_paper_bg) {
        const float rim_bright =
            1.0f + 0.08f * Smoothstep(3.0f, 0.0f, std::abs(edge_sdf));
        const float shade = Clamp(
            grain * crease_mul * rim_bright * (1.0f - 0.35f * anim_crease),
            0.25f, 1.15f);
        const RgbaColor paper_fg = {
            Clamp01(params.paper_r * shade) * sheet_alpha,
            Clamp01(params.paper_g * shade) * sheet_alpha,
            Clamp01(params.paper_b * shade) * sheet_alpha,
            sheet_alpha,
        };
        cutout_fg = CompositeOver(subject, paper_fg);
      } else if (params.enable_border) {
        cutout_fg = subject;
      } else {
        cutout_fg = {subject.r * sheet_alpha, subject.g * sheet_alpha,
                     subject.b * sheet_alpha, subject.a * sheet_alpha};
      }

      // Over() ignores the background once fg.a >= 1, so skip the shadow.
      const RgbaColor shadow_rgba =
          cutout_fg.a >= 1.0f ? RgbaColor{} : shadow_at_pixel();
      WritePixelPtr(out_px, out_depth, CompositeOver(cutout_fg, shadow_rgba));
    }
  });
}

// See header. Interior progress snaps to the middle of its stop-motion
// bucket, (floor(p * N) + 0.5) / N, so held frames never reach exactly 0
// or 1.
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

// See header. The `active` clip is the paper sheet that folds or unfolds;
// the `under` clip shows behind it, scaled by under_weight. Openness q:
// 1 = flat, 0 = folded away. Sheets come in three kinds:
//   - opaque full-frame clips: an analytic box (FullFrameSheet);
//   - transparent clips (titles, logos): their matte SDF plus a torn collar;
//   - Paper Cutout FX output (has_cutout_shadow): the clip's own torn border
//     is reused and its baked shadow stripped (BuildCleanCutoutAlpha); the
//     3D shadow replaces it and hands back to the baked one near rest.
// Setup (inspection, SDF, clean alpha, layout, bounds) runs on the calling
// thread; rows then run on parallel_for workers. Output is premultiplied.
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

  // Writes `weight` x `clip` (or transparency) over the render window,
  // streaming each output row through one pointer.
  auto fill_with_clip = [&](const ImageView* clip, float weight) {
    const bool use_clip =
        clip != nullptr && weight > 1e-4f && IsValidRgbaView(*clip);
    if (!use_clip) {
      ClearOutputRows(output, x_begin, x_end, y_begin, y_end, parallel_for);
      return;
    }
    const std::ptrdiff_t out_pixel_bytes = PixelSizeBytes(output);
    const PixelDepth out_depth = output.depth;
    RunScanlines(y_begin, y_end, parallel_for, [&](int y) {
      std::uint8_t* out_px = PixelMutPtr(output, x_begin, y);
      for (int x = x_begin; x < x_end; ++x, out_px += out_pixel_bytes) {
        WritePixelPtr(
            out_px, out_depth,
            ScalePremult(ClipPixelAt(*clip, canvas_bounds, x, y), weight));
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

  // Inspect only the clips the requested action can use; each scan reads
  // about 230k pixels.
  const auto requested = static_cast<FoldTransitionAction>(
      std::min(3, std::max(0, params.fold_action)));
  const bool need_from =
      requested == FoldTransitionAction::kAuto ||
      requested == FoldTransitionAction::kFoldOut ||
      (requested == FoldTransitionAction::kFoldAndUnfold && p < 0.5f);
  const bool need_to =
      requested == FoldTransitionAction::kAuto ||
      requested == FoldTransitionAction::kUnfoldIn ||
      (requested == FoldTransitionAction::kFoldAndUnfold && p >= 0.5f);
  const ClipContentSummary from_info =
      need_from ? InspectClipContent(from_view) : ClipContentSummary{};
  const ClipContentSummary to_info =
      need_to ? InspectClipContent(to_view) : ClipContentSummary{};
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
      under_weight =
          from_info.is_full_frame_opaque ? 1.0f : Smoothstep(0.0f, 0.25f, p);
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
      under_weight =
          to_info.is_full_frame_opaque ? 1.0f : Smoothstep(1.0f, 0.75f, p);
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

  const bool is_cutout =
      !active_info->is_full_frame_opaque && active_info->has_cutout_shadow;
  CoarseSignedDistanceField matte_sdf;
  SheetGeometry sheet;
  float sdf_step = 0.0f;
  if (active_info->is_full_frame_opaque) {
    sheet = FullFrameSheet(canvas_bounds);
  } else {
    // Identity placement: the matte is the clip alpha in canonical px.
    matte_sdf =
        BuildClipMatteSdf(*active, canvas_bounds, parallel_for, is_cutout);
    sheet =
        SheetFromSdf(matte_sdf, is_cutout ? 0.0f : (full_border + full_tear));
    sdf_step = static_cast<float>(matte_sdf.step);
  }
  if (!sheet.valid) {
    fill_with_clip(under, under_weight);
    return;
  }
  PixelRect active_alpha_box;
  const std::vector<float> clean_alpha =
      is_cutout ? BuildCleanCutoutAlpha(*active, canvas_bounds, matte_sdf,
                                        parallel_for, &active_alpha_box)
                : std::vector<float>{};
  const bool use_clean = !clean_alpha.empty();
  // Coarse-SDF error margin: two cells plus 3 px.
  const float sdf_slack = 2.0f * sdf_step + 3.0f;

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
  ctx.cutout_sheet = use_clean;
  ctx.cutout_sdf_slack_px = sdf_slack;
  ctx.enable_border = params.enable_border && !use_clean;
  ctx.border_px = use_clean ? 0.0f : full_border * settle;
  ctx.tear_amp_px = use_clean ? 0.0f : full_tear * settle;
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
  ctx.media_overlay =
      use_clean ? 0.0f : 0.30f * Clamp01(params.crease_strength) * settle;
  ctx.hinge_crease = settle;
  ctx.crease_band_px = std::max(2.5f, 0.0055f * min_dim);
  // Keep soft matte edges (and the coarse matte's slack) while the collar is
  // narrower than that margin.
  ctx.subject_margin_px = std::max(0.0f, 2.0f + sdf_step - ctx.border_px);
  ConfigureFoldLight(params.shadow_angle_deg, &ctx);
  ctx.flap_shadow_opacity = params.shadow_opacity;
  ctx.flap_shadow_px = std::max(3.0f, 0.011f * min_dim);
  ctx.flap_shadow_soft_px = std::max(2.0f, 0.006f * min_dim);

  // Drop shadow grows and softens while the packet is lifted. For pre-cutout
  // clips, keep the 3D shadow alive and hand it off into the cutout's resting
  // shadow instead of fading it out.
  const float lift_k = 1.0f + 2.5f * layout.lift;
  const float sh_dist = params.shadow_distance_norm * min_dim * lift_k;
  const float sh_dx = ctx.shadow_dir_x * sh_dist;
  const float sh_dy = ctx.shadow_dir_y * sh_dist;
  const float sh_soft = std::max(1.2f, params.shadow_softness_norm * min_dim *
                                           (1.0f + 1.5f * layout.lift));
  const float rest_sh_target = active_info->max_shadow_alpha > 1e-3f
                                   ? active_info->max_shadow_alpha
                                   : params.shadow_opacity;
  const float sh_opacity =
      use_clean ? Lerp(params.shadow_opacity * (1.0f - 0.35f * layout.lift),
                       rest_sh_target, Smoothstep(0.65f, 0.95f, q))
                : params.shadow_opacity * settle * (1.0f - 0.35f * layout.lift);
  const float rest_shadow_blend = use_clean ? Smoothstep(0.90f, 1.0f, q) : 0.0f;
  const bool rest_blend_on = use_clean && rest_shadow_blend > 1e-4f;
  // The resting-shadow blend reads the active clip at every pixel, but
  // outside the clip's non-zero alpha its raw and clean alpha are both 0, so
  // the blend reduces to sh_a = Lerp(sh_a, 0, blend). Pixels outside this
  // box (output px == canonical px) skip the two clip reads, and rows
  // outside it that miss the paper and shadow boxes take the fast path.
  const PixelRect rest_box =
      rest_blend_on
          ? ClipBoxToCanonical(*active, canvas_bounds, active_alpha_box)
          : PixelRect{};

  // Conservative bounds of the paper (torn edge included) and of the soft
  // coverage used by shadows, per fold level; the coarse matte adds slack.
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
    // Golden-ratio spacing decorrelates consecutive steps.
    const float sf = step * 1.618f + static_cast<float>(seed) * 0.37f;
    const float amp = Clamp01(params.jitter_amount) *
                      (use_clean ? Smoothstep(0.85f, 0.25f, q) : settle);
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
    return use_clean ? SampleCleanClipCanonicalPx(*active, clean_alpha,
                                                  canvas_bounds, ux, uy)
                     : SampleClipCanonicalPx(*active, canvas_bounds, ux, uy);
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
  const std::ptrdiff_t out_pixel_bytes = PixelSizeBytes(output);
  const PixelDepth out_depth = output.depth;
  // Fast underlay copy: a same-size underlay covering the whole row span can
  // be streamed with pointer increments. ClipPixelAt would do the same direct
  // reads, so this is bit-identical; only an exact weight of 1 skips the
  // scale (c * 1.0f == c), so the copy matches ScalePremult bit for bit.
  bool under_direct_x = false;
  if (has_under) {
    const PixelRect under_rod = EffectiveRod(*under);
    under_direct_x =
        under_rod.x1 == canvas_bounds.x1 && under_rod.x2 == canvas_bounds.x2 &&
        under_rod.y1 == canvas_bounds.y1 && under_rod.y2 == canvas_bounds.y2 &&
        under->bounds.x1 <= x_begin && under->bounds.x2 >= x_end;
  }
  const std::ptrdiff_t under_pixel_bytes =
      has_under ? PixelSizeBytes(*under) : 0;

  RunScanlines(y_begin, y_end, parallel_for, [&](int y) {
    const float py_out = static_cast<float>(y) + 0.5f;
    const bool row_paper = py_out >= paper_box.y1 && py_out <= paper_box.y2;
    const bool row_shadow = sh_opacity > 1e-4f && py_out >= shadow_box.y1 &&
                            py_out <= shadow_box.y2;
    std::uint8_t* out_px = PixelMutPtr(output, x_begin, y);
    const bool row_rest = rest_blend_on && y >= rest_box.y1 && y < rest_box.y2;
    // Rows that miss the paper and shadow boxes (and the clip's resting
    // shadow, see rest_box) are just the underlay.
    if (!row_paper && !row_shadow && !row_rest) {
      if (!has_under) {
        for (int x = x_begin; x < x_end; ++x, out_px += out_pixel_bytes) {
          WritePixelPtr(out_px, out_depth, {});
        }
      } else if (under_direct_x && y >= under->bounds.y1 &&
                 y < under->bounds.y2) {
        const std::uint8_t* under_px = PixelConstPtr(*under, x_begin, y);
        const PixelDepth under_depth = under->depth;
        const AlphaMode under_alpha = under->alpha_mode;
        if (under_weight == 1.0f) {
          for (int x = x_begin; x < x_end;
               ++x, out_px += out_pixel_bytes, under_px += under_pixel_bytes) {
            WritePixelPtr(out_px, out_depth,
                          ReadPixelPtr(under_px, under_depth, under_alpha));
          }
        } else {
          for (int x = x_begin; x < x_end;
               ++x, out_px += out_pixel_bytes, under_px += under_pixel_bytes) {
            WritePixelPtr(
                out_px, out_depth,
                ScalePremult(ReadPixelPtr(under_px, under_depth, under_alpha),
                             under_weight));
          }
        }
      } else {
        for (int x = x_begin; x < x_end; ++x, out_px += out_pixel_bytes) {
          WritePixelPtr(out_px, out_depth,
                        ScalePremult(ClipPixelAt(*under, canvas_bounds, x, y),
                                     under_weight));
        }
      }
      return;
    }
    for (int x = x_begin; x < x_end; ++x, out_px += out_pixel_bytes) {
      const float px_out = static_cast<float>(x) + 0.5f;
      RgbaColor fg;
      if (row_paper && px_out >= paper_box.x1 && px_out <= paper_box.x2) {
        float lx = 0.0f;
        float ly = 0.0f;
        packet.ToLocal(px_out, py_out, &lx, &ly);
        fg = ScalePremult(EvalFoldStack(ctx, layout.count - 1, lx, ly, false,
                                        1.0f, sample_active),
                          layout.alpha);
        // The underlay and shadow would add < 0.1%; skip them.
        if (fg.a >= 0.999f) {
          WritePixelPtr(out_px, out_depth, fg);
          continue;
        }
      }
      RgbaColor bg =
          has_under ? ScalePremult(ClipPixelAt(*under, canvas_bounds, x, y),
                                   under_weight)
                    : RgbaColor{};
      float sh_a = 0.0f;
      if (row_shadow && px_out >= shadow_box.x1 && px_out <= shadow_box.x2) {
        float slx = 0.0f;
        float sly = 0.0f;
        packet.ToLocal(px_out - sh_dx, py_out - sh_dy, &slx, &sly);
        sh_a =
            sh_opacity * layout.alpha *
            EvalFoldCoverage(ctx, layout.count - 1, slx, sly, false, sh_soft);
      }
      // Near rest, blend toward the clip's own baked shadow, recovered by
      // inverting Over with a black shadow:
      // sh = (raw.a - clean.a) / (1 - clean.a).
      // Outside rest_box both alphas are exactly 0, so rest_sh_a is 0 and
      // the clip reads are skipped (same Lerp arguments, same result).
      if (rest_blend_on) {
        if (row_rest && x >= rest_box.x1 && x < rest_box.x2) {
          const RgbaColor raw_c = ClipPixelAt(*active, canvas_bounds, x, y);
          const float clean_a =
              CleanClipAlphaAt(*active, clean_alpha, canvas_bounds, x, y);
          if (clean_a < 0.99f) {
            const float rest_sh_a =
                Clamp01((raw_c.a - clean_a) / (1.0f - clean_a)) * layout.alpha;
            sh_a = Lerp(sh_a, rest_sh_a, rest_shadow_blend);
          }
        } else {
          sh_a = Lerp(sh_a, 0.0f, rest_shadow_blend);
        }
      }
      if (sh_a > 1e-4f) {
        bg = CompositeOver({0.0f, 0.0f, 0.0f, sh_a}, bg);
      }
      WritePixelPtr(out_px, out_depth, CompositeOver(fg, bg));
    }
  });
}

}  // namespace papercraft
