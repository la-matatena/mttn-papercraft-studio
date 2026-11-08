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

// Transition style presets for dedicated Edit-tab video transitions.
enum class TransitionKind {
  kUnfold = 0,
  kCrumple = 1,
  kRipWipe = 2,
};

// Parameter bundle for the Paper Cutout FX Filter/Generator renderer.
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

  // Intro & outro paper transitions across the clip duration.
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

// Parameter bundle for Edit-tab Paper Transitions.
struct TransitionParams {
  TransitionKind kind = TransitionKind::kUnfold;
  float progress = 0.5f;
  bool reverse_direction = false;
  bool enable_stop_motion = true;
  int stop_motion_steps = 12;
  bool mirror_h = false;
  bool mirror_v = false;

  // Unfold / Crumple / Rip shared styling controls.
  float fold_tilt_deg = 22.0f;
  float slide_distance = 0.55f;
  float crumple_intensity = 0.22f;
  float crease_scale = 4.0f;
  float spin_angle_deg = 35.0f;
  float rip_angle_deg = 12.0f;
  float tear_width_norm = 0.028f;
  float tear_roughness_norm = 0.032f;
  float tear_frequency = 24.0f;
  float shadow_opacity = 0.60f;
  float shadow_softness_norm = 0.020f;
  float crease_intensity = 0.45f;
  float paper_r = 0.960f;
  float paper_g = 0.940f;
  float paper_b = 0.890f;
  int random_seed = 7;
};

// Returns true if the image view points to a writable/readable 4-channel RGBA
// pixel buffer with non-empty bounds and non-zero rowBytes.
bool IsValidRgbaView(const ImageView& view);

// Computes normalized [0, 1] transition progress after reverse and optional
// stop-motion quantization, preserving exact 0.0 and 1.0 endpoints.
float ComputeEffectiveTransitionProgress(const TransitionParams& params);

// Renders Paper Cutout FX into `output` across `render_window` using canonical
// full-frame geometry `canvas_bounds`. `source` may be empty/invalid when
// `source_mode == 1` or operating in OpenFX Generator context.
void RenderCutoutFrame(const CutoutParams& params, const ImageView& source,
                       const ImageView& output, const PixelRect& canvas_bounds,
                       const PixelRect& render_window,
                       const ParallelRowsFn& parallel_for);

// Renders an Edit-tab Paper Transition between `source_from` and `source_to`
// into `output` across `render_window` using canonical `canvas_bounds`.
void RenderTransitionFrame(const TransitionParams& params,
                           const ImageView& source_from,
                           const ImageView& source_to, const ImageView& output,
                           const PixelRect& canvas_bounds,
                           const PixelRect& render_window,
                           const ParallelRowsFn& parallel_for);

}  // namespace papercraft

#endif  // EXPERIMENTAL_USERS_JOSELUISZAPATA_DAVINCI_RESOLVE_PLUGINS_PAPERCRAFT_PRO_OFX_PAPERCRAFT_KERNEL_H_
