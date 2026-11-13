// Internal helpers of papercraft_kernel.cc exposed only for unit tests.
//
// Not part of the plugin API: papercraft_ofx_plugin.cc must keep using
// papercraft_kernel.h. These functions live in papercraft::internal so
// papercraft_kernel_test.cc can test the distance transform, clip inspection,
// fold-action resolution, and baked-shadow removal directly instead of only
// through whole renders. The plugin is built with -fvisibility=hidden and
// exports just OfxGetPlugin / OfxGetNumberOfPlugins, so these symbols never
// leave the bundle.
#ifndef EXPERIMENTAL_USERS_JOSELUISZAPATA_DAVINCI_RESOLVE_PLUGINS_PAPERCRAFT_PRO_OFX_PAPERCRAFT_KERNEL_INTERNAL_H_
#define EXPERIMENTAL_USERS_JOSELUISZAPATA_DAVINCI_RESOLVE_PLUGINS_PAPERCRAFT_PRO_OFX_PAPERCRAFT_KERNEL_INTERNAL_H_

#include <cstdint>
#include <vector>

#if defined(PAPERCRAFT_STANDALONE_OFX)
#include "papercraft_kernel.h"  // NOLINT(build/include)
#else
#include "experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro/ofx/papercraft_kernel.h"
#endif

namespace papercraft {
namespace internal {

// What InspectClipContent learned about a clip (sampled, not exhaustive).
struct ClipContentSummary {
  // Some sample has alpha > 0.001.
  bool has_visible_alpha = false;
  // Some sample has r + g + b > 0.003.
  bool has_nonblack_rgb = false;
  // The buffer covers the RoD and every sample has alpha >= 0.995.
  bool is_full_frame_opaque = false;
  // Looks like Paper Cutout FX output: not opaque, has color, and at least two
  // soft black shadow samples away from opaque black ink.
  bool has_cutout_shadow = false;
  // Strongest shadow sample alpha (about the cutout's Shadow Opacity).
  float max_shadow_alpha = 0.0f;
};

// Linear-time 1D squared Euclidean distance transform over `n` samples.
// `input[q]` is an exact squared integer distance (< 2^24) or >= 1e18 for "no
// feature". Writes min_v (q - v)^2 + input[v] to `output[q]`, or 1e20 for
// every q when the line has no feature. Scratch: `v` needs n entries,
// `z_num` and `z_den` n + 1.
void DistanceTransform1D(const float* input, int n, int* v,
                         std::int64_t* z_num, std::int64_t* z_den,
                         float* output);

// In-place exact 2D squared Euclidean distance transform of a row-major
// `width` x `height` binary grid: cells are 0 (feature) or >= 1e18
// (background). Afterwards each cell holds its squared distance in cells to
// the nearest feature, or 1e20 when the grid has none. Grids with a side
// <= 0 or > 960 are left untouched. `scratch` (optional) is reused across
// calls.
void DistanceTransform2D(int width, int height,
                         const ParallelRowsFn& parallel_for,
                         std::vector<float>* grid,
                         std::vector<int>* scratch = nullptr);

// Samples `view` on a strided grid (about 360 rows) and classifies it as
// opaque full frame, transparent/empty, or Paper Cutout FX output with a
// baked black drop shadow. Invalid views return an all-false summary.
ClipContentSummary InspectClipContent(const ImageView& view);

// Resolves the Fold Action parameter (clamped to [0, 3], a
// FoldTransitionAction). Explicit actions are returned unchanged; kAuto picks
// one from the two clip summaries.
FoldTransitionAction ResolveFoldAction(int requested,
                                       const ClipContentSummary& from,
                                       const ClipContentSummary& to);

// Recovers per-pixel un-shadowed alpha of a Paper Cutout FX clip exactly as
// RenderPaperFoldTransition does: builds the clip's matte SDF with the baked
// shadow ignored (identity placement over `canvas_bounds`), then strips the
// black shadow. Returns one alpha per pixel of `clip.bounds` (row-major), or
// an empty vector for invalid or oversized views. `raw_alpha_box` (optional)
// receives the buffer-px box of every pixel with non-zero raw alpha.
std::vector<float> BuildCleanCutoutAlphaForClip(
    const ImageView& clip, const PixelRect& canvas_bounds,
    const ParallelRowsFn& parallel_for, PixelRect* raw_alpha_box = nullptr);

}  // namespace internal
}  // namespace papercraft

#endif  // EXPERIMENTAL_USERS_JOSELUISZAPATA_DAVINCI_RESOLVE_PLUGINS_PAPERCRAFT_PRO_OFX_PAPERCRAFT_KERNEL_INTERNAL_H_
