// Direct unit tests for the PaperCraft pixel kernels (papercraft_kernel.cc).
//
// papercraft_ofx_test.cc drives the built plugin through a mock OpenFX host;
// this suite instead links the kernel source and calls it directly:
//   - internal helpers exposed in papercraft_kernel_internal.h: the exact
//     squared Euclidean distance transforms (checked against brute force),
//     clip inspection, Fold Action resolution, and baked-shadow removal;
//   - the public renderers in papercraft_kernel.h, on small synthetic
//     images: endpoints, render windows, negative row_bytes, and bit depths.
//
// Run with Bazel (`papercraft_kernel_test` in BUILD), as in CI (compile with
// papercraft_kernel.cc against libgtest), or locally with
// `ofx/run_cpp_tests.sh` (falls back to ofx/testing/gtest_lite).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <vector>

#if defined(PAPERCRAFT_STANDALONE_OFX)
#include <gtest/gtest.h>

#include "papercraft_kernel.h"           // NOLINT(build/include)
#include "papercraft_kernel_internal.h"  // NOLINT(build/include)
#else
#include "experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro/ofx/papercraft_kernel.h"
#include "experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro/ofx/papercraft_kernel_internal.h"
#include "testing/base/public/gunit.h"
#endif

namespace papercraft {
namespace {

using internal::BuildCleanCutoutAlphaForClip;
using internal::ClipContentSummary;
using internal::DistanceTransform1D;
using internal::DistanceTransform2D;
using internal::InspectClipContent;
using internal::ResolveFoldAction;

// "No feature" input and output values of the distance transforms.
constexpr float kNoFeature = 1e20f;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

// ParallelRowsFn that runs every row in order on the calling thread.
void SerialRows(int y_begin, int y_end, const std::function<void(int)>& fn) {
  for (int y = y_begin; y < y_end; ++y) {
    fn(y);
  }
}

// ParallelRowsFn that runs rows last-to-first, to show results do not depend
// on the order in which host workers pick up rows.
void ReverseRows(int y_begin, int y_end, const std::function<void(int)>& fn) {
  for (int y = y_end - 1; y >= y_begin; --y) {
    fn(y);
  }
}

// Premultiplied float RGBA image with top-down rows, for synthetic clips.
struct FloatImage {
  FloatImage(int w, int h)
      : width(w), height(h), px(static_cast<std::size_t>(w) * h * 4, 0.0f) {}

  // Pointer to the 4 floats of pixel (x, y).
  float* At(int x, int y) {
    return &px[(static_cast<std::size_t>(y) * width + x) * 4];
  }
  const float* At(int x, int y) const {
    return &px[(static_cast<std::size_t>(y) * width + x) * 4];
  }

  // Stores premultiplied (r, g, b, a) at (x, y).
  void Set(int x, int y, float r, float g, float b, float a) {
    float* p = At(x, y);
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
  }

  // Float view with bounds = RoD = the whole image.
  ImageView View(AlphaMode mode = AlphaMode::kPremultiplied) const {
    ImageView view;
    view.data = const_cast<float*>(px.data());
    view.bounds = {0, 0, width, height};
    view.rod = view.bounds;
    view.row_bytes = static_cast<std::ptrdiff_t>(width) * 4 * sizeof(float);
    view.depth = PixelDepth::kFloat;
    view.components = 4;
    view.alpha_mode = mode;
    return view;
  }

  int width;
  int height;
  std::vector<float> px;
};

// Builds a view over `width` x `height` pixels of `depth` stored in `base`.
// With `bottom_up`, memory holds the rows last-to-first and the view uses a
// negative row_bytes, as some OpenFX hosts do.
ImageView MakeView(void* base, int width, int height, PixelDepth depth,
                   AlphaMode mode, bool bottom_up) {
  const int channel_bytes =
      depth == PixelDepth::kFloat ? 4 : (depth == PixelDepth::kShort ? 2 : 1);
  const std::ptrdiff_t row = static_cast<std::ptrdiff_t>(width) * 4 *
                             channel_bytes;
  ImageView view;
  view.data = static_cast<std::uint8_t*>(base) +
              (bottom_up ? row * (height - 1) : 0);
  view.bounds = {0, 0, width, height};
  view.rod = view.bounds;
  view.row_bytes = bottom_up ? -row : row;
  view.depth = depth;
  view.components = 4;
  view.alpha_mode = mode;
  return view;
}

// Opaque full-frame video stand-in: smooth color ramps plus a bright disc.
FloatImage MakeVideoFrame(int w, int h, float hue) {
  FloatImage img(w, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const bool disc = std::hypot(x - 0.35 * w, y - 0.5 * h) < 0.2 * h;
      img.Set(x, y, disc ? 1.0f : 0.2f + 0.6f * hue,
              disc ? 0.95f : 0.3f + 0.5f * x / w,
              disc ? 0.9f : 0.8f - 0.6f * hue * y / h, 1.0f);
    }
  }
  return img;
}

// Anti-aliased disc coverage: 1 inside radius - 0.5, 0 beyond radius + 0.5.
float DiscCoverage(int x, int y, float cx, float cy, float radius) {
  const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
  return std::clamp(radius + 0.5f - d, 0.0f, 1.0f);
}

// Paper Cutout FX look-alike: a cardstock-colored disc (foreground alpha
// returned in `fg_alpha`) composited Over a soft black drop shadow of
// opacity `shadow_opacity`, offset down-right. Over a black shadow keeps the
// foreground's premultiplied RGB and only raises alpha.
FloatImage MakeCutoutWithShadow(int w, int h, float shadow_opacity,
                                std::vector<float>* fg_alpha) {
  constexpr float kPaper[3] = {0.92f, 0.86f, 0.78f};
  FloatImage img(w, h);
  fg_alpha->assign(static_cast<std::size_t>(w) * h, 0.0f);
  const float cx = 0.45f * w;
  const float cy = 0.45f * h;
  const float radius = 0.25f * h;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float fa = DiscCoverage(x, y, cx, cy, radius);
      const float sd =
          std::hypot(x + 0.5f - (cx + 9.0f), y + 0.5f - (cy + 7.0f));
      const float sa =
          shadow_opacity * std::clamp((radius + 4.0f - sd) / 8.0f, 0.0f, 1.0f);
      (*fg_alpha)[static_cast<std::size_t>(y) * w + x] = fa;
      img.Set(x, y, kPaper[0] * fa, kPaper[1] * fa, kPaper[2] * fa,
              fa + (1.0f - fa) * sa);
    }
  }
  return img;
}

// Transparent title stand-in: an anti-aliased colored disc, no shadow.
FloatImage MakeTitle(int w, int h) {
  FloatImage img(w, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float a = DiscCoverage(x, y, 0.55f * w, 0.5f * h, 0.3f * h);
      img.Set(x, y, 0.9f * a, 0.25f * a, 0.3f * a, a);
    }
  }
  return img;
}

// Brute-force 2D squared EDT of a binary grid (0 = feature): the reference
// for DistanceTransform2D.
std::vector<float> BruteForceEdt(const std::vector<float>& grid, int w, int h) {
  std::vector<float> out(grid.size(), kNoFeature);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      long long best = -1;
      for (int fy = 0; fy < h; ++fy) {
        for (int fx = 0; fx < w; ++fx) {
          if (grid[static_cast<std::size_t>(fy) * w + fx] >= 1e18f) {
            continue;
          }
          const long long d = static_cast<long long>(fx - x) * (fx - x) +
                              static_cast<long long>(fy - y) * (fy - y);
          if (best < 0 || d < best) {
            best = d;
          }
        }
      }
      if (best >= 0) {
        out[static_cast<std::size_t>(y) * w + x] = static_cast<float>(best);
      }
    }
  }
  return out;
}

// Random binary grid: each cell is a feature (0) with probability `density`.
std::vector<float> RandomBinaryGrid(int w, int h, double density,
                                    std::mt19937* rng) {
  std::bernoulli_distribution is_feature(density);
  std::vector<float> grid(static_cast<std::size_t>(w) * h);
  for (float& cell : grid) {
    cell = is_feature(*rng) ? 0.0f : kNoFeature;
  }
  return grid;
}

// ---------------------------------------------------------------------------
// Distance transforms
// ---------------------------------------------------------------------------

TEST(DistanceTransformTest, OneDimensionalMatchesBruteForce) {
  std::mt19937 rng(1234);
  std::uniform_int_distribution<int> value(0, 4000);
  std::bernoulli_distribution missing(0.6);
  for (int n : {1, 2, 3, 7, 31, 64, 200}) {
    for (int trial = 0; trial < 20; ++trial) {
      SCOPED_TRACE("n=" + std::to_string(n) + " trial=" +
                   std::to_string(trial));
      std::vector<float> input(n);
      for (float& v : input) {
        // Squared integers (exact in float) or "no feature".
        const int root = value(rng) % 64;
        v = (trial == 0 || missing(rng)) ? kNoFeature
                                         : static_cast<float>(root * root);
      }
      if (trial == 1) {
        input.assign(n, 0.0f);  // Every sample is a feature.
      }
      std::vector<int> v(n);
      std::vector<std::int64_t> z_num(n + 1);
      std::vector<std::int64_t> z_den(n + 1);
      std::vector<float> output(n, -1.0f);
      DistanceTransform1D(input.data(), n, v.data(), z_num.data(),
                          z_den.data(), output.data());
      for (int q = 0; q < n; ++q) {
        float expected = kNoFeature;
        bool found = false;
        for (int r = 0; r < n; ++r) {
          if (input[r] >= 1e18f) {
            continue;
          }
          const float d = static_cast<float>((q - r) * (q - r)) + input[r];
          if (!found || d < expected) {
            expected = d;
            found = true;
          }
        }
        ASSERT_EQ(output[q], expected) << "q=" << q;
      }
    }
  }
}

TEST(DistanceTransformTest, TwoDimensionalMatchesBruteForce) {
  std::mt19937 rng(42);
  struct Size {
    int w;
    int h;
  };
  // Includes 1xN / Nx1 lines and widths that span several 64-column blocks.
  const Size sizes[] = {{1, 1},  {1, 23}, {23, 1}, {8, 8},
                        {33, 20}, {70, 5}, {130, 9}, {65, 31}};
  std::vector<int> scratch;  // Reused across calls, like the SDF builder.
  for (const Size& size : sizes) {
    for (double density : {0.0, 1.0, 0.02, 0.3}) {
      SCOPED_TRACE(std::to_string(size.w) + "x" + std::to_string(size.h) +
                   " density=" + std::to_string(density));
      std::vector<float> grid = RandomBinaryGrid(size.w, size.h, density, &rng);
      const std::vector<float> expected =
          BruteForceEdt(grid, size.w, size.h);
      std::vector<float> serial = grid;
      DistanceTransform2D(size.w, size.h, SerialRows, &serial, &scratch);
      std::vector<float> reversed = grid;
      DistanceTransform2D(size.w, size.h, ReverseRows, &reversed);
      for (std::size_t i = 0; i < grid.size(); ++i) {
        ASSERT_EQ(serial[i], expected[i]) << "cell " << i;
        ASSERT_EQ(reversed[i], expected[i]) << "cell " << i;
      }
    }
  }
}

TEST(DistanceTransformTest, SingleFeatureGivesExactSquaredDistances) {
  constexpr int kW = 9;
  constexpr int kH = 6;
  std::vector<float> grid(kW * kH, kNoFeature);
  grid[2 * kW + 3] = 0.0f;  // Feature at (3, 2).
  DistanceTransform2D(kW, kH, SerialRows, &grid);
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      EXPECT_EQ(grid[y * kW + x],
                static_cast<float>((x - 3) * (x - 3) + (y - 2) * (y - 2)))
          << "(" << x << ", " << y << ")";
    }
  }
}

TEST(DistanceTransformTest, InvalidOrOversizedGridsAreLeftUntouched) {
  // 961 exceeds the 960-cell SDF limit, 0 is empty: both are no-ops.
  std::vector<float> wide(961, kNoFeature);
  wide[5] = 0.0f;
  const std::vector<float> wide_before = wide;
  DistanceTransform2D(961, 1, SerialRows, &wide);
  EXPECT_TRUE(wide == wide_before);

  std::vector<float> empty;
  DistanceTransform2D(0, 4, SerialRows, &empty);
  EXPECT_TRUE(empty.empty());

  // The largest allowed line still works.
  std::vector<float> line(960, kNoFeature);
  line[0] = 0.0f;
  DistanceTransform2D(960, 1, SerialRows, &line);
  EXPECT_EQ(line[959], 959.0f * 959.0f);
}

// ---------------------------------------------------------------------------
// Stop-motion progress
// ---------------------------------------------------------------------------

TEST(SteppedTransitionProgressTest, KeepsExactEndpoints) {
  PaperFoldTransitionParams params;
  params.enable_stop_motion = true;
  params.stop_motion_steps = 10;
  for (float p : {0.0f, -0.5f, 1e-6f}) {
    params.progress = p;
    EXPECT_EQ(ComputeSteppedTransitionProgress(params), 0.0f) << "p=" << p;
  }
  for (float p : {1.0f, 1.7f, 1.0f - 1e-6f}) {
    params.progress = p;
    EXPECT_EQ(ComputeSteppedTransitionProgress(params), 1.0f) << "p=" << p;
  }
}

TEST(SteppedTransitionProgressTest, SnapsInteriorProgressToBucketCenters) {
  PaperFoldTransitionParams params;
  params.enable_stop_motion = true;
  params.stop_motion_steps = 10;
  // Every progress inside bucket [0.3, 0.4) holds the same pose.
  for (float p : {0.301f, 0.33f, 0.35f, 0.399f}) {
    params.progress = p;
    EXPECT_FLOAT_EQ(ComputeSteppedTransitionProgress(params), 0.35f)
        << "p=" << p;
  }
  // Held frames never reach exactly 0 or 1.
  params.progress = 0.001f;
  EXPECT_FLOAT_EQ(ComputeSteppedTransitionProgress(params), 0.05f);
  params.progress = 0.999f;
  EXPECT_FLOAT_EQ(ComputeSteppedTransitionProgress(params), 0.95f);
  params.stop_motion_steps = 4;
  params.progress = 0.6f;
  EXPECT_FLOAT_EQ(ComputeSteppedTransitionProgress(params), 0.625f);
}

TEST(SteppedTransitionProgressTest, DisabledSteppingPassesProgressThrough) {
  PaperFoldTransitionParams params;
  params.progress = 0.3141f;
  params.enable_stop_motion = false;
  EXPECT_EQ(ComputeSteppedTransitionProgress(params), 0.3141f);
  // Fewer than two steps also disables stepping.
  params.enable_stop_motion = true;
  params.stop_motion_steps = 1;
  EXPECT_EQ(ComputeSteppedTransitionProgress(params), 0.3141f);
}

// ---------------------------------------------------------------------------
// Fold Action resolution
// ---------------------------------------------------------------------------

// Summary shorthands for the clip kinds a transition sees.
ClipContentSummary TransparentEmpty() { return {}; }

ClipContentSummary OpaqueBlackEmpty() {
  ClipContentSummary s;
  s.has_visible_alpha = true;
  s.is_full_frame_opaque = true;
  return s;
}

ClipContentSummary FullFrameVideo() {
  ClipContentSummary s;
  s.has_visible_alpha = true;
  s.has_nonblack_rgb = true;
  s.is_full_frame_opaque = true;
  return s;
}

ClipContentSummary Cutout() {
  ClipContentSummary s;
  s.has_visible_alpha = true;
  s.has_nonblack_rgb = true;
  s.has_cutout_shadow = true;
  s.max_shadow_alpha = 0.6f;
  return s;
}

TEST(ResolveFoldActionTest, ExplicitActionsAreKeptAndClamped) {
  const ClipContentSummary empty = TransparentEmpty();
  const ClipContentSummary video = FullFrameVideo();
  for (const auto& [from, to] : {std::make_pair(empty, video),
                                 std::make_pair(video, empty),
                                 std::make_pair(video, video)}) {
    EXPECT_EQ(ResolveFoldAction(1, from, to), FoldTransitionAction::kUnfoldIn);
    EXPECT_EQ(ResolveFoldAction(2, from, to), FoldTransitionAction::kFoldOut);
    EXPECT_EQ(ResolveFoldAction(3, from, to),
              FoldTransitionAction::kFoldAndUnfold);
    EXPECT_EQ(ResolveFoldAction(99, from, to),
              FoldTransitionAction::kFoldAndUnfold);
  }
  // Negative values clamp to Auto: an empty incoming side folds out.
  EXPECT_EQ(ResolveFoldAction(-4, video, empty),
            FoldTransitionAction::kFoldOut);
}

TEST(ResolveFoldActionTest, AutoFoldsOutAtClipEnd) {
  EXPECT_EQ(ResolveFoldAction(0, FullFrameVideo(), TransparentEmpty()),
            FoldTransitionAction::kFoldOut);
  EXPECT_EQ(ResolveFoldAction(0, Cutout(), TransparentEmpty()),
            FoldTransitionAction::kFoldOut);
  // Regression: hosts may pass the empty side as opaque black, which used to
  // be mistaken for a full-frame incoming clip and unfolded black paper.
  EXPECT_EQ(ResolveFoldAction(0, FullFrameVideo(), OpaqueBlackEmpty()),
            FoldTransitionAction::kFoldOut);
  EXPECT_EQ(ResolveFoldAction(0, Cutout(), OpaqueBlackEmpty()),
            FoldTransitionAction::kFoldOut);
}

TEST(ResolveFoldActionTest, AutoUnfoldsInAtClipStartOrOverFullFrame) {
  EXPECT_EQ(ResolveFoldAction(0, TransparentEmpty(), FullFrameVideo()),
            FoldTransitionAction::kUnfoldIn);
  EXPECT_EQ(ResolveFoldAction(0, OpaqueBlackEmpty(), Cutout()),
            FoldTransitionAction::kUnfoldIn);
  EXPECT_EQ(ResolveFoldAction(0, TransparentEmpty(), TransparentEmpty()),
            FoldTransitionAction::kUnfoldIn);
  // A full-frame incoming clip covers everything, so it simply unfolds.
  EXPECT_EQ(ResolveFoldAction(0, Cutout(), FullFrameVideo()),
            FoldTransitionAction::kUnfoldIn);
  EXPECT_EQ(ResolveFoldAction(0, FullFrameVideo(), FullFrameVideo()),
            FoldTransitionAction::kUnfoldIn);
}

TEST(ResolveFoldActionTest, AutoFoldsAndUnfoldsBetweenCutouts) {
  EXPECT_EQ(ResolveFoldAction(0, Cutout(), Cutout()),
            FoldTransitionAction::kFoldAndUnfold);
  EXPECT_EQ(ResolveFoldAction(0, FullFrameVideo(), Cutout()),
            FoldTransitionAction::kFoldAndUnfold);
}

TEST(ResolveFoldActionTest, AutoOnInspectedOpaqueBlackFrameFoldsOut) {
  // End to end through InspectClipContent: an opaque black kOpaque frame is
  // the host's "no clip" side of a clip end.
  const FloatImage video = MakeVideoFrame(96, 54, 0.3f);
  FloatImage black(96, 54);
  for (int y = 0; y < 54; ++y) {
    for (int x = 0; x < 96; ++x) {
      black.Set(x, y, 0.0f, 0.0f, 0.0f, 1.0f);
    }
  }
  const ClipContentSummary from = InspectClipContent(video.View());
  const ClipContentSummary to =
      InspectClipContent(black.View(AlphaMode::kOpaque));
  EXPECT_EQ(ResolveFoldAction(0, from, to), FoldTransitionAction::kFoldOut);
  EXPECT_EQ(ResolveFoldAction(0, to, from), FoldTransitionAction::kUnfoldIn);
}

// ---------------------------------------------------------------------------
// Clip inspection
// ---------------------------------------------------------------------------

TEST(InspectClipContentTest, InvalidViewReportsNothing) {
  const ClipContentSummary s = InspectClipContent(ImageView{});
  EXPECT_FALSE(s.has_visible_alpha);
  EXPECT_FALSE(s.has_nonblack_rgb);
  EXPECT_FALSE(s.is_full_frame_opaque);
  EXPECT_FALSE(s.has_cutout_shadow);
  EXPECT_EQ(s.max_shadow_alpha, 0.0f);
}

TEST(InspectClipContentTest, OpaqueFullFrameFastPath) {
  const FloatImage video = MakeVideoFrame(800, 450, 0.5f);
  const ClipContentSummary s =
      InspectClipContent(video.View(AlphaMode::kOpaque));
  EXPECT_TRUE(s.has_visible_alpha);
  EXPECT_TRUE(s.has_nonblack_rgb);
  EXPECT_TRUE(s.is_full_frame_opaque);
  EXPECT_FALSE(s.has_cutout_shadow);
}

TEST(InspectClipContentTest, OpaqueAllBlackStaysBlack) {
  // Stride is min(800, 450) / 360 = 1 here; also use a 2-px stride frame.
  for (int scale : {1, 2}) {
    SCOPED_TRACE("scale=" + std::to_string(scale));
    const int w = 400 * scale;
    const int h = 360 * scale;
    FloatImage black(w, h);  // RGB = 0; kOpaque forces alpha to 1.
    ClipContentSummary s = InspectClipContent(black.View(AlphaMode::kOpaque));
    EXPECT_TRUE(s.has_visible_alpha);
    EXPECT_FALSE(s.has_nonblack_rgb);
    EXPECT_TRUE(s.is_full_frame_opaque);
    EXPECT_FALSE(s.has_cutout_shadow);

    // Content only in the last rows is still found: the fast path stops at
    // the first non-black sample, never before it.
    for (int y = h - 4; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        black.Set(x, y, 0.4f, 0.4f, 0.4f, 1.0f);
      }
    }
    s = InspectClipContent(black.View(AlphaMode::kOpaque));
    EXPECT_TRUE(s.has_nonblack_rgb);
  }
}

TEST(InspectClipContentTest, OpaquePixelsOutsideFastPath) {
  // Premultiplied buffer with alpha 1 everywhere: the sampled scan agrees.
  const FloatImage video = MakeVideoFrame(160, 90, 0.2f);
  ClipContentSummary s = InspectClipContent(video.View());
  EXPECT_TRUE(s.is_full_frame_opaque);
  EXPECT_TRUE(s.has_nonblack_rgb);
  EXPECT_FALSE(s.has_cutout_shadow);

  // A buffer that covers only part of its RoD is never full-frame opaque.
  ImageView cropped = video.View(AlphaMode::kOpaque);
  cropped.rod = {0, 0, 320, 180};
  s = InspectClipContent(cropped);
  EXPECT_FALSE(s.is_full_frame_opaque);
  EXPECT_TRUE(s.has_visible_alpha);
}

TEST(InspectClipContentTest, TransparentClipIsEmpty) {
  const FloatImage clear(160, 90);
  const ClipContentSummary s = InspectClipContent(clear.View());
  EXPECT_FALSE(s.has_visible_alpha);
  EXPECT_FALSE(s.has_nonblack_rgb);
  EXPECT_FALSE(s.is_full_frame_opaque);
  EXPECT_FALSE(s.has_cutout_shadow);
}

TEST(InspectClipContentTest, TitleWithoutShadowIsNotACutout) {
  const FloatImage title = MakeTitle(160, 90);
  const ClipContentSummary s = InspectClipContent(title.View());
  EXPECT_TRUE(s.has_visible_alpha);
  EXPECT_TRUE(s.has_nonblack_rgb);
  EXPECT_FALSE(s.is_full_frame_opaque);
  EXPECT_FALSE(s.has_cutout_shadow);
}

TEST(InspectClipContentTest, DetectsCutoutWithBakedBlackShadow) {
  std::vector<float> fg_alpha;
  const FloatImage cutout = MakeCutoutWithShadow(160, 90, 0.6f, &fg_alpha);
  const ClipContentSummary s = InspectClipContent(cutout.View());
  EXPECT_TRUE(s.has_visible_alpha);
  EXPECT_TRUE(s.has_nonblack_rgb);
  EXPECT_FALSE(s.is_full_frame_opaque);
  EXPECT_TRUE(s.has_cutout_shadow);
  // Shadow samples top out at the shadow opacity (the 0.985 cap aside).
  EXPECT_NEAR(s.max_shadow_alpha, 0.6f, 0.01f);
}

TEST(InspectClipContentTest, BlackTextEdgesAreNotAShadow) {
  // Colored logo plus opaque black text strokes whose 1-px anti-aliased
  // edges are semi-transparent black, exactly like shadow pixels, but they
  // touch opaque black ink.
  FloatImage img = MakeTitle(160, 90);
  for (int stroke = 0; stroke < 4; ++stroke) {
    const int x0 = 12 + stroke * 12;
    for (int y = 10; y < 80; ++y) {
      for (int x = x0 - 1; x <= x0 + 6; ++x) {
        const bool edge = x == x0 - 1 || x == x0 + 6 || y == 10 || y == 79;
        img.Set(x, y, 0.0f, 0.0f, 0.0f, edge ? 0.5f : 1.0f);
      }
    }
  }
  const ClipContentSummary s = InspectClipContent(img.View());
  EXPECT_TRUE(s.has_visible_alpha);
  EXPECT_TRUE(s.has_nonblack_rgb);
  EXPECT_FALSE(s.is_full_frame_opaque);
  EXPECT_FALSE(s.has_cutout_shadow);
  EXPECT_EQ(s.max_shadow_alpha, 0.0f);
}

// ---------------------------------------------------------------------------
// Baked-shadow removal
// ---------------------------------------------------------------------------

TEST(BuildCleanCutoutAlphaTest, RecoversForegroundAlphaFromShadowedCutout) {
  constexpr int kW = 192;
  constexpr int kH = 108;
  std::vector<float> fg_alpha;
  const FloatImage cutout = MakeCutoutWithShadow(kW, kH, 0.6f, &fg_alpha);
  PixelRect box;
  const std::vector<float> clean = BuildCleanCutoutAlphaForClip(
      cutout.View(), {0, 0, kW, kH}, SerialRows, &box);
  ASSERT_EQ(clean.size(), fg_alpha.size());

  float max_err = 0.0f;
  int shadow_only_px = 0;
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * kW + x;
      const float raw_a = cutout.At(x, y)[3];
      max_err = std::max(max_err, std::fabs(clean[i] - fg_alpha[i]));
      if (fg_alpha[i] >= 1.0f) {
        ASSERT_EQ(clean[i], raw_a) << "interior (" << x << ", " << y << ")";
      }
      if (fg_alpha[i] == 0.0f && raw_a > 0.05f) {
        ++shadow_only_px;
        // Pure shadow is removed completely.
        ASSERT_EQ(clean[i], 0.0f) << "shadow (" << x << ", " << y << ")";
      }
    }
  }
  EXPECT_GT(shadow_only_px, 100);
  EXPECT_LE(max_err, 0.02f);

  // The raw-alpha box spans the disc and its shadow.
  int x1 = kW, y1 = kH, x2 = 0, y2 = 0;
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      if (cutout.At(x, y)[3] != 0.0f) {
        x1 = std::min(x1, x);
        y1 = std::min(y1, y);
        x2 = std::max(x2, x + 1);
        y2 = std::max(y2, y + 1);
      }
    }
  }
  EXPECT_EQ(box.x1, x1);
  EXPECT_EQ(box.y1, y1);
  EXPECT_EQ(box.x2, x2);
  EXPECT_EQ(box.y2, y2);
}

TEST(BuildCleanCutoutAlphaTest, InvalidViewReturnsEmpty) {
  PixelRect box{1, 2, 3, 4};
  EXPECT_TRUE(
      BuildCleanCutoutAlphaForClip(ImageView{}, {0, 0, 8, 8}, SerialRows, &box)
          .empty());
  EXPECT_EQ(box.x2 - box.x1, 0);
}

// ---------------------------------------------------------------------------
// Public renderers
// ---------------------------------------------------------------------------

// Small canvas shared by the renderer smoke tests.
constexpr int kCanvasW = 160;
constexpr int kCanvasH = 90;
constexpr PixelRect kCanvas{0, 0, kCanvasW, kCanvasH};

// Float premultiplied output buffer for kCanvas, pre-filled with `fill`.
std::vector<float> MakeOutput(float fill) {
  return std::vector<float>(static_cast<std::size_t>(kCanvasW) * kCanvasH * 4,
                            fill);
}

// Asserts every value is finite and every alpha lies in [0, 1].
void ExpectSanePremultiplied(const std::vector<float>& px) {
  for (std::size_t i = 0; i < px.size(); i += 4) {
    for (int c = 0; c < 4; ++c) {
      ASSERT_TRUE(std::isfinite(px[i + c])) << "value " << i + c;
    }
    ASSERT_GE(px[i + 3], 0.0f) << "pixel " << i / 4;
    ASSERT_LE(px[i + 3], 1.0f) << "pixel " << i / 4;
  }
}

TEST(RenderPaperFoldTransitionTest, EndpointsCopyTheClipsExactly) {
  const FloatImage from = MakeVideoFrame(kCanvasW, kCanvasH, 0.1f);
  const FloatImage to = MakeTitle(kCanvasW, kCanvasH);
  for (int action = 0; action <= 3; ++action) {
    SCOPED_TRACE("action=" + std::to_string(action));
    PaperFoldTransitionParams params;
    params.fold_action = action;
    std::vector<float> out = MakeOutput(-1.0f);
    const ImageView out_view =
        MakeView(out.data(), kCanvasW, kCanvasH, PixelDepth::kFloat,
                 AlphaMode::kPremultiplied, false);
    params.progress = 0.0f;
    RenderPaperFoldTransition(params, from.View(), to.View(), out_view,
                              kCanvas, kCanvas, SerialRows);
    EXPECT_TRUE(out == from.px);
    params.progress = 1.0f;
    RenderPaperFoldTransition(params, from.View(), to.View(), out_view,
                              kCanvas, kCanvas, SerialRows);
    EXPECT_TRUE(out == to.px);
    // Mid-transition differs from both clips and stays well-formed.
    params.progress = 0.45f;
    RenderPaperFoldTransition(params, from.View(), to.View(), out_view,
                              kCanvas, kCanvas, SerialRows);
    EXPECT_FALSE(out == from.px);
    EXPECT_FALSE(out == to.px);
    ASSERT_NO_FATAL_FAILURE(ExpectSanePremultiplied(out));
  }
}

TEST(RenderPaperFoldTransitionTest, RenderWindowMatchesFullRender) {
  std::vector<float> fg_alpha;
  const FloatImage from = MakeCutoutWithShadow(kCanvasW, kCanvasH, 0.6f,
                                               &fg_alpha);
  const FloatImage to = MakeVideoFrame(kCanvasW, kCanvasH, 0.7f);
  PaperFoldTransitionParams params;
  params.progress = 0.4f;
  params.fold_action = 3;
  std::vector<float> full = MakeOutput(0.0f);
  RenderPaperFoldTransition(
      params, from.View(), to.View(),
      MakeView(full.data(), kCanvasW, kCanvasH, PixelDepth::kFloat,
               AlphaMode::kPremultiplied, false),
      kCanvas, kCanvas, SerialRows);
  const PixelRect window{30, 20, 120, 70};
  std::vector<float> tile = MakeOutput(-7.0f);
  RenderPaperFoldTransition(
      params, from.View(), to.View(),
      MakeView(tile.data(), kCanvasW, kCanvasH, PixelDepth::kFloat,
               AlphaMode::kPremultiplied, false),
      kCanvas, window, ReverseRows);
  for (int y = 0; y < kCanvasH; ++y) {
    for (int x = 0; x < kCanvasW; ++x) {
      const bool inside =
          x >= window.x1 && x < window.x2 && y >= window.y1 && y < window.y2;
      for (int c = 0; c < 4; ++c) {
        const std::size_t i = (static_cast<std::size_t>(y) * kCanvasW + x) * 4;
        ASSERT_EQ(tile[i + c], inside ? full[i + c] : -7.0f)
            << "(" << x << ", " << y << ") channel " << c;
      }
    }
  }
}

// Renders the transition (action `action`, progress `p`) from `from` to `to`
// into a float buffer stored top-down or bottom-up, and returns the pixels in
// top-down order.
std::vector<float> RenderFoldRows(const FloatImage& from, const FloatImage& to,
                                  int action, float p, bool bottom_up) {
  PaperFoldTransitionParams params;
  params.progress = p;
  params.fold_action = action;
  std::vector<float> mem = MakeOutput(0.0f);
  RenderPaperFoldTransition(
      params, from.View(), to.View(),
      MakeView(mem.data(), kCanvasW, kCanvasH, PixelDepth::kFloat,
               AlphaMode::kPremultiplied, bottom_up),
      kCanvas, kCanvas, SerialRows);
  if (!bottom_up) {
    return mem;
  }
  std::vector<float> top_down(mem.size());
  const std::size_t row = static_cast<std::size_t>(kCanvasW) * 4;
  for (int y = 0; y < kCanvasH; ++y) {
    std::memcpy(&top_down[y * row], &mem[(kCanvasH - 1 - y) * row],
                row * sizeof(float));
  }
  return top_down;
}

TEST(RenderPaperFoldTransitionTest, NegativeRowBytesOutputIsFlippedCopy) {
  std::vector<float> fg_alpha;
  const FloatImage cutout = MakeCutoutWithShadow(kCanvasW, kCanvasH, 0.6f,
                                                 &fg_alpha);
  const FloatImage video = MakeVideoFrame(kCanvasW, kCanvasH, 0.4f);
  for (int action = 0; action <= 3; ++action) {
    for (float p : {0.0f, 0.3f, 0.7f, 1.0f}) {
      SCOPED_TRACE("action=" + std::to_string(action) +
                   " p=" + std::to_string(p));
      EXPECT_TRUE(RenderFoldRows(cutout, video, action, p, false) ==
                  RenderFoldRows(cutout, video, action, p, true));
    }
  }
}

TEST(RenderPaperFoldTransitionTest, BottomUpInputsRenderLikeTopDownInputs) {
  const FloatImage from = MakeTitle(kCanvasW, kCanvasH);
  const FloatImage to = MakeVideoFrame(kCanvasW, kCanvasH, 0.9f);
  // Store both inputs bottom-up and read them through negative row_bytes.
  auto flipped = [](const FloatImage& img) {
    FloatImage out(img.width, img.height);
    for (int y = 0; y < img.height; ++y) {
      std::memcpy(out.At(0, img.height - 1 - y), img.At(0, y),
                  sizeof(float) * 4 * img.width);
    }
    return out;
  };
  const FloatImage from_mem = flipped(from);
  const FloatImage to_mem = flipped(to);
  PaperFoldTransitionParams params;
  params.progress = 0.35f;
  std::vector<float> expected = MakeOutput(0.0f);
  std::vector<float> actual = MakeOutput(0.0f);
  const ImageView expected_view =
      MakeView(expected.data(), kCanvasW, kCanvasH, PixelDepth::kFloat,
               AlphaMode::kPremultiplied, false);
  const ImageView actual_view =
      MakeView(actual.data(), kCanvasW, kCanvasH, PixelDepth::kFloat,
               AlphaMode::kPremultiplied, false);
  RenderPaperFoldTransition(params, from.View(), to.View(), expected_view,
                            kCanvas, kCanvas, SerialRows);
  RenderPaperFoldTransition(
      params,
      MakeView(const_cast<float*>(from_mem.px.data()), kCanvasW, kCanvasH,
               PixelDepth::kFloat, AlphaMode::kPremultiplied, true),
      MakeView(const_cast<float*>(to_mem.px.data()), kCanvasW, kCanvasH,
               PixelDepth::kFloat, AlphaMode::kPremultiplied, true),
      actual_view, kCanvas, kCanvas, SerialRows);
  EXPECT_TRUE(actual == expected);
}

TEST(RenderPaperFoldTransitionTest, ByteAndShortOutputsQuantizeFloatOutput) {
  const FloatImage from = MakeVideoFrame(kCanvasW, kCanvasH, 0.6f);
  std::vector<float> fg_alpha;
  const FloatImage to = MakeCutoutWithShadow(kCanvasW, kCanvasH, 0.6f,
                                             &fg_alpha);
  const std::size_t n = static_cast<std::size_t>(kCanvasW) * kCanvasH * 4;
  for (float p : {0.25f, 0.6f}) {
    SCOPED_TRACE("p=" + std::to_string(p));
    PaperFoldTransitionParams params;
    params.progress = p;
    std::vector<float> out_f(n);
    std::vector<std::uint16_t> out_s(n);
    std::vector<std::uint8_t> out_b(n);
    RenderPaperFoldTransition(
        params, from.View(), to.View(),
        MakeView(out_f.data(), kCanvasW, kCanvasH, PixelDepth::kFloat,
                 AlphaMode::kPremultiplied, false),
        kCanvas, kCanvas, SerialRows);
    RenderPaperFoldTransition(
        params, from.View(), to.View(),
        MakeView(out_s.data(), kCanvasW, kCanvasH, PixelDepth::kShort,
                 AlphaMode::kPremultiplied, false),
        kCanvas, kCanvas, SerialRows);
    RenderPaperFoldTransition(
        params, from.View(), to.View(),
        MakeView(out_b.data(), kCanvasW, kCanvasH, PixelDepth::kByte,
                 AlphaMode::kPremultiplied, false),
        kCanvas, kCanvas, SerialRows);
    for (std::size_t i = 0; i < n; ++i) {
      const float f = std::clamp(out_f[i], 0.0f, 1.0f);
      ASSERT_NEAR(out_s[i] / 65535.0f, f, 0.5f / 65535.0f + 1e-6f)
          << "value " << i;
      ASSERT_NEAR(out_b[i] / 255.0f, f, 0.5f / 255.0f + 1e-6f)
          << "value " << i;
    }
  }
}

TEST(RenderPaperFoldTransitionTest, ByteAndShortInputsMatchEquivalentFloat) {
  // Integer inputs decode as k / 255 (k / 65535); float inputs holding the
  // same decoded values must render bit-identically.
  const FloatImage from_src = MakeVideoFrame(kCanvasW, kCanvasH, 0.25f);
  const FloatImage to_src = MakeTitle(kCanvasW, kCanvasH);
  const std::size_t n = from_src.px.size();
  std::vector<std::uint8_t> from_b(n);
  std::vector<std::uint8_t> to_b(n);
  std::vector<std::uint16_t> from_s(n);
  std::vector<std::uint16_t> to_s(n);
  FloatImage from_fb(kCanvasW, kCanvasH);
  FloatImage to_fb(kCanvasW, kCanvasH);
  FloatImage from_fs(kCanvasW, kCanvasH);
  FloatImage to_fs(kCanvasW, kCanvasH);
  for (std::size_t i = 0; i < n; ++i) {
    from_b[i] = static_cast<std::uint8_t>(from_src.px[i] * 255.0f + 0.5f);
    to_b[i] = static_cast<std::uint8_t>(to_src.px[i] * 255.0f + 0.5f);
    from_s[i] = static_cast<std::uint16_t>(from_src.px[i] * 65535.0f + 0.5f);
    to_s[i] = static_cast<std::uint16_t>(to_src.px[i] * 65535.0f + 0.5f);
    from_fb.px[i] = static_cast<float>(from_b[i]) * (1.0f / 255.0f);
    to_fb.px[i] = static_cast<float>(to_b[i]) * (1.0f / 255.0f);
    from_fs.px[i] = static_cast<float>(from_s[i]) * (1.0f / 65535.0f);
    to_fs.px[i] = static_cast<float>(to_s[i]) * (1.0f / 65535.0f);
  }
  PaperFoldTransitionParams params;
  params.progress = 0.5f;
  params.fold_action = 1;
  auto render = [&](const ImageView& from, const ImageView& to) {
    std::vector<float> out = MakeOutput(0.0f);
    RenderPaperFoldTransition(
        params, from, to,
        MakeView(out.data(), kCanvasW, kCanvasH, PixelDepth::kFloat,
                 AlphaMode::kPremultiplied, false),
        kCanvas, kCanvas, SerialRows);
    return out;
  };
  constexpr AlphaMode kPre = AlphaMode::kPremultiplied;
  EXPECT_TRUE(render(MakeView(from_b.data(), kCanvasW, kCanvasH,
                              PixelDepth::kByte, kPre, false),
                     MakeView(to_b.data(), kCanvasW, kCanvasH,
                              PixelDepth::kByte, kPre, false)) ==
              render(from_fb.View(), to_fb.View()));
  EXPECT_TRUE(render(MakeView(from_s.data(), kCanvasW, kCanvasH,
                              PixelDepth::kShort, kPre, false),
                     MakeView(to_s.data(), kCanvasW, kCanvasH,
                              PixelDepth::kShort, kPre, false)) ==
              render(from_fs.View(), to_fs.View()));
}

TEST(RenderCutoutFrameTest, RendersPaperCutoutOverTransparency) {
  const FloatImage source = MakeVideoFrame(kCanvasW, kCanvasH, 0.5f);
  CutoutParams params;
  params.current_time = 60.0;  // Mid-clip: no intro or outro.
  std::vector<float> out = MakeOutput(-1.0f);
  RenderCutoutFrame(params, source.View(AlphaMode::kOpaque),
                    MakeView(out.data(), kCanvasW, kCanvasH,
                             PixelDepth::kFloat, AlphaMode::kPremultiplied,
                             false),
                    kCanvas, kCanvas, SerialRows);
  ASSERT_NO_FATAL_FAILURE(ExpectSanePremultiplied(out));
  auto alpha_at = [&](int x, int y) {
    return out[(static_cast<std::size_t>(y) * kCanvasW + x) * 4 + 3];
  };
  // The scaled-down sheet covers the center; the corners stay see-through.
  EXPECT_NEAR(alpha_at(kCanvasW / 2, kCanvasH / 2), 1.0f, 1e-4f);
  EXPECT_LT(alpha_at(0, 0), 0.5f);
  EXPECT_LT(alpha_at(kCanvasW - 1, kCanvasH - 1), 0.5f);
}

TEST(RenderCutoutFrameTest, BuiltInStickerRendersWithoutSource) {
  CutoutParams params;
  params.source_mode = 1;
  params.current_time = 60.0;
  std::vector<float> out = MakeOutput(-1.0f);
  RenderCutoutFrame(params, ImageView{},
                    MakeView(out.data(), kCanvasW, kCanvasH,
                             PixelDepth::kFloat, AlphaMode::kPremultiplied,
                             false),
                    kCanvas, kCanvas, SerialRows);
  ASSERT_NO_FATAL_FAILURE(ExpectSanePremultiplied(out));
  float max_alpha = 0.0f;
  for (std::size_t i = 3; i < out.size(); i += 4) {
    max_alpha = std::max(max_alpha, out[i]);
  }
  EXPECT_GT(max_alpha, 0.99f);
}

TEST(RenderCutoutFrameTest, NegativeRowBytesAndRenderWindowAreConsistent) {
  const FloatImage source = MakeTitle(kCanvasW, kCanvasH);
  for (int intro : {0, 1, 2}) {
    SCOPED_TRACE("intro_style=" + std::to_string(intro));
    CutoutParams params;
    params.intro_style = intro;
    params.current_time = 5.0;  // Inside the 14-frame intro.
    std::vector<float> top_down = MakeOutput(0.0f);
    RenderCutoutFrame(params, source.View(),
                      MakeView(top_down.data(), kCanvasW, kCanvasH,
                               PixelDepth::kFloat, AlphaMode::kPremultiplied,
                               false),
                      kCanvas, kCanvas, SerialRows);
    std::vector<float> bottom_up = MakeOutput(0.0f);
    RenderCutoutFrame(params, source.View(),
                      MakeView(bottom_up.data(), kCanvasW, kCanvasH,
                               PixelDepth::kFloat, AlphaMode::kPremultiplied,
                               true),
                      kCanvas, kCanvas, ReverseRows);
    const std::size_t row = static_cast<std::size_t>(kCanvasW) * 4;
    for (int y = 0; y < kCanvasH; ++y) {
      ASSERT_EQ(std::memcmp(&top_down[y * row],
                            &bottom_up[(kCanvasH - 1 - y) * row],
                            row * sizeof(float)),
                0)
          << "row " << y;
    }
    // A render window writes only its rows/columns, with the same values.
    const PixelRect window{10, 15, 90, 60};
    std::vector<float> tile = MakeOutput(-3.0f);
    RenderCutoutFrame(params, source.View(),
                      MakeView(tile.data(), kCanvasW, kCanvasH,
                               PixelDepth::kFloat, AlphaMode::kPremultiplied,
                               false),
                      kCanvas, window, SerialRows);
    for (int y = 0; y < kCanvasH; ++y) {
      for (int x = 0; x < kCanvasW; ++x) {
        const bool inside =
            x >= window.x1 && x < window.x2 && y >= window.y1 && y < window.y2;
        const std::size_t i = y * row + static_cast<std::size_t>(x) * 4;
        ASSERT_EQ(tile[i + 3], inside ? top_down[i + 3] : -3.0f)
            << "(" << x << ", " << y << ")";
      }
    }
  }
}

TEST(RenderCutoutFrameTest, InvalidOutputIsANoOp) {
  const FloatImage source = MakeTitle(kCanvasW, kCanvasH);
  std::vector<float> out = MakeOutput(-5.0f);
  ImageView bad = MakeView(out.data(), kCanvasW, kCanvasH, PixelDepth::kFloat,
                           AlphaMode::kPremultiplied, false);
  bad.components = 3;  // The kernels require RGBA.
  RenderCutoutFrame(CutoutParams{}, source.View(), bad, kCanvas, kCanvas,
                    SerialRows);
  PaperFoldTransitionParams fold;
  fold.progress = 0.5f;
  RenderPaperFoldTransition(fold, source.View(), source.View(), bad, kCanvas,
                            kCanvas, SerialRows);
  EXPECT_TRUE(std::all_of(out.begin(), out.end(),
                          [](float v) { return v == -5.0f; }));
}

}  // namespace
}  // namespace papercraft
