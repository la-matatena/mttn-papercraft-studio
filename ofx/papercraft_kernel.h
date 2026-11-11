#ifndef EXPERIMENTAL_USERS_JOSELUISZAPATA_DAVINCI_RESOLVE_PLUGINS_PAPERCRAFT_PRO_OFX_PAPERCRAFT_KERNEL_H_
#define EXPERIMENTAL_USERS_JOSELUISZAPATA_DAVINCI_RESOLVE_PLUGINS_PAPERCRAFT_PRO_OFX_PAPERCRAFT_KERNEL_H_

#include <cstddef>
#include <cstdint>
#include <functional>

namespace papercraft {

// Pixel component bit-depth supported by the OpenFX CPU renderer.
enum class PixelDepth {
  kFloat,
  kShort,
  kByte,
};

// OpenFX alpha premultiplication state of an image buffer.
enum class AlphaMode {
  kPremultiplied = 0,
  kUnPremultiplied = 1,
  kOpaque = 2,
};

// Half-open integer pixel rectangle [x1, x2) x [y1, y2) in OpenFX pixel space.
struct PixelRect {
  int x1 = 0;
  int y1 = 0;
  int x2 = 0;
  int y2 = 0;
};

// Normalized linear RGBA color sample with premultiplied or straight channels.
struct RgbaColor {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 0.0f;
};

// Non-owning view over an RGBA image buffer with signed rowBytes support.
// OpenFX hosts may supply negative row_bytes for flipped scanline storage.
struct ImageView {
  void* data = nullptr;
  PixelRect bounds;
  PixelRect rod;
  std::ptrdiff_t row_bytes = 0;
  PixelDepth depth = PixelDepth::kFloat;
  int components = 4;
  AlphaMode alpha_mode = AlphaMode::kPremultiplied;
};

// Callback executing a scanline range [y_begin, y_end) across worker slices.
// Implementations must invoke the row function synchronously for every row in
// [y_start, y_end) before returning.
using ParallelRowsFn = std::function<void(
    int y_start, int y_end, const std::function<void(int y)>& row_fn)>;

// Intro and outro paper entrance/exit presets for Paper Cutout FX.
enum class CutoutAnimStyle {
  kNone = 0,
  kPaperUnfold = 1,
  kCrumpleBall = 2,
  kAirplaneGlide = 3,
  kFlatPop = 4,
};

// What the Paper Fold & Unfold transition does with its two clips.
enum class FoldTransitionAction {
  kAuto = 0,           // Unfold at a clip start, fold at a clip end.
  kUnfoldIn = 1,       // Incoming clip unfolds over the outgoing clip.
  kFoldOut = 2,        // Outgoing clip folds up and reveals the incoming clip.
  kFoldAndUnfold = 3,  // Outgoing clip folds away, then incoming clip unfolds.
};

// Hinge layout used to fold the sheet into a small packet.
enum class FoldPattern {
  kCraftFlaps = 0,      // Half folds plus asymmetric edge and corner flaps.
  kMapGrid = 1,         // Orthogonal map or poster folds.
  kAccordion = 2,       // Parallel zigzag pleats (valley and mountain).
  kOrigamiCorners = 3,  // Diagonal corner flaps plus a center fold.
};

// Parameter bundle for the Edit-page Paper Fold & Unfold transition.
struct PaperFoldTransitionParams {
  float progress = 0.5f;  // Host-animated [0, 1] transition progress.
  int fold_action = 0;    // FoldTransitionAction.
  int fold_pattern = 0;   // FoldPattern.
  int fold_stages = 4;    // Sequential folds, clamped to [2, 5].
  float direction_angle_deg = 0.0f;
  float fold_depth = 0.55f;      // 3D perspective strength, 0 = flat.
  float slide_distance = 0.6f;   // Packet slide in/out, x frame diagonal.
  bool mirror_h = false;
  bool mirror_v = false;

  // Paper look: optional torn cardstock collar, backside, grain, creases.
  bool enable_border = true;
  float border_width_norm = 0.018f;
  float tear_roughness_norm = 0.012f;
  float tear_frequency = 24.0f;
  float paper_r = 0.965f;
  float paper_g = 0.945f;
  float paper_b = 0.895f;
  float grain_strength = 0.38f;
  float crease_strength = 0.52f;
  int random_seed = 7;

  // Stop-motion stepping and hand jitter.
  bool enable_stop_motion = true;
  int stop_motion_steps = 10;
  float jitter_amount = 0.25f;

  // Shadows; the angle also sets the light direction on the flaps.
  float shadow_opacity = 0.62f;
  float shadow_angle_deg = -45.0f;
  float shadow_distance_norm = 0.014f;
  float shadow_softness_norm = 0.020f;

  bool source_is_premultiplied = true;
};

// Parameter bundle for the Paper Cutout FX filter renderer.
struct CutoutParams {
  // Media source & placement.
  int source_mode = 0;  // 0 = Timeline Clip, 1 = Built-in Paper Sticker
  bool source_is_premultiplied = true;
  float media_scale = 0.90f;
  float media_offset_x = 0.5f;
  float media_offset_y = 0.5f;
  float media_angle_deg = 0.0f;

  // Chroma key & matte cleanup.
  bool enable_chroma_key = false;
  float key_r = 0.0f;
  float key_g = 1.0f;
  float key_b = 0.0f;
  float key_gain = 1.0f;
  float key_balance = 0.5f;
  float edge_choke_px = 0.0f;

  // Torn paper cutout border.
  bool enable_border = true;
  float border_width_norm = 0.018f;
  float tear_roughness_norm = 0.012f;
  float tear_frequency = 24.0f;
  int tear_detail = 5;
  float edge_boil = 0.15f;
  float paper_r = 0.965f;
  float paper_g = 0.945f;
  float paper_b = 0.895f;

  // Paper fiber grain & fold creases.
  float grain_strength = 0.35f;
  float grain_scale = 1.0f;
  float crease_strength = 0.40f;
  float crease_scale = 3.2f;
  float media_texture_overlay = 0.22f;
  int random_seed = 7;

  // Intro & outro paper animations across the clip duration.
  int intro_style = 2;
  int intro_frames = 14;
  int outro_style = 1;
  int outro_frames = 14;
  float transition_angle_deg = -22.0f;
  float glide_distance = 0.65f;
  bool mirror_transition_h = false;
  bool mirror_transition_v = false;

  // 3D drop shadow.
  float shadow_opacity = 0.62f;
  float shadow_angle_deg = -45.0f;
  float shadow_distance_norm = 0.016f;
  float shadow_softness_norm = 0.018f;

  // Stop-motion stepping & master transform.
  bool enable_stop_motion = true;
  int hold_frames = 3;
  float jitter_pos_norm = 0.0025f;
  float jitter_rot_deg = 0.75f;
  float master_pos_x = 0.5f;
  float master_pos_y = 0.5f;
  float master_scale = 1.0f;
  float master_angle_deg = 0.0f;
  bool flip_h = false;
  bool flip_v = false;

  // Host frame coordinates for intro/outro and stop-motion timing.
  double current_time = 0.0;
  double frame_start = 0.0;
  double frame_end = 120.0;
};

// Returns true if the image view points to a writable/readable 4-channel RGBA
// pixel buffer with non-empty bounds and non-zero rowBytes.
bool IsValidRgbaView(const ImageView& view);

// Renders Paper Cutout FX into `output` across `render_window` using canonical
// full-frame geometry `canvas_bounds`. `source` may be empty/invalid when
// `source_mode == 1` (Built-in Paper Sticker) or the source clip is absent.
void RenderCutoutFrame(const CutoutParams& params, const ImageView& source,
                       const ImageView& output, const PixelRect& canvas_bounds,
                       const PixelRect& render_window,
                       const ParallelRowsFn& parallel_for);

// Quantizes transition progress into stop-motion holds. Exact 0 and 1 are kept
// so the first and last frames match the outgoing and incoming clips.
float ComputeSteppedTransitionProgress(const PaperFoldTransitionParams& params);

// Renders the Paper Fold & Unfold transition. Either source may be invalid or
// fully transparent, which is what the host passes at the start or end of a
// standalone clip.
void RenderPaperFoldTransition(const PaperFoldTransitionParams& params,
                               const ImageView& source_from,
                               const ImageView& source_to,
                               const ImageView& output,
                               const PixelRect& canvas_bounds,
                               const PixelRect& render_window,
                               const ParallelRowsFn& parallel_for);

}  // namespace papercraft

#endif  // EXPERIMENTAL_USERS_JOSELUISZAPATA_DAVINCI_RESOLVE_PLUGINS_PAPERCRAFT_PRO_OFX_PAPERCRAFT_KERNEL_H_
