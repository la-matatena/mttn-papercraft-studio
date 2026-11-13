#if defined(PAPERCRAFT_STANDALONE_OFX)
#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxMultiThread.h"
#include "ofxParam.h"
#include "ofxProperty.h"
#include "papercraft_kernel.h"  // NOLINT(build/include)
#else
#include "experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro/ofx/openfx_include/ofxCore.h"
#include "experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro/ofx/openfx_include/ofxImageEffect.h"
#include "experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro/ofx/openfx_include/ofxMultiThread.h"
#include "experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro/ofx/openfx_include/ofxParam.h"
#include "experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro/ofx/openfx_include/ofxProperty.h"
#include "experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro/ofx/papercraft_kernel.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>

namespace papercraft {
namespace ofx {
namespace {

constexpr int kPluginCount = 2;
constexpr int kVersionMajor = 3;
constexpr int kVersionMinor = 1;
constexpr const char* kVersionLabel = "3.1.0";

// Each registered OpenFX plugin gets its own entry points and host suites.
enum class PluginRole {
  kCutout = 0,
  kFoldTransition = 1,
};

struct HostSuites {
  OfxHost* host = nullptr;
  const OfxPropertySuiteV1* prop = nullptr;
  const OfxParameterSuiteV1* param = nullptr;
  const OfxImageEffectSuiteV1* effect = nullptr;
  const OfxMultiThreadSuiteV1* thread = nullptr;
};

HostSuites& MutableSuites(PluginRole role) {
  static std::array<HostSuites, kPluginCount> suites;
  return suites[static_cast<std::size_t>(role)];
}

bool StrEq(const char* a, const char* b) {
  return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

class ScopedOfxImage {
 public:
  explicit ScopedOfxImage(const OfxImageEffectSuiteV1* effect)
      : effect_(effect) {}
  ~ScopedOfxImage() { Reset(); }

  ScopedOfxImage(const ScopedOfxImage&) = delete;
  ScopedOfxImage& operator=(const ScopedOfxImage&) = delete;

  void Reset() {
    if (handle != nullptr && effect_ != nullptr &&
        effect_->clipReleaseImage != nullptr) {
      effect_->clipReleaseImage(handle);
      handle = nullptr;
    }
  }

  OfxPropertySetHandle handle = nullptr;

 private:
  const OfxImageEffectSuiteV1* effect_ = nullptr;
};

struct PluginInstance {
  PluginRole role = PluginRole::kCutout;
  OfxImageClipHandle output_clip = nullptr;
  OfxImageClipHandle source_clip = nullptr;
  OfxImageClipHandle source_from_clip = nullptr;
  OfxImageClipHandle source_to_clip = nullptr;

  // Cutout FX parameters (several are shared with the fold transition).
  OfxParamHandle source_mode = nullptr;
  OfxParamHandle source_premult = nullptr;
  OfxParamHandle media_scale = nullptr;
  OfxParamHandle media_offset = nullptr;
  OfxParamHandle media_angle = nullptr;
  OfxParamHandle enable_chroma_key = nullptr;
  OfxParamHandle key_color = nullptr;
  OfxParamHandle key_gain = nullptr;
  OfxParamHandle key_balance = nullptr;
  OfxParamHandle edge_choke_px = nullptr;
  OfxParamHandle enable_border = nullptr;
  OfxParamHandle border_width = nullptr;
  OfxParamHandle tear_roughness = nullptr;
  OfxParamHandle tear_frequency = nullptr;
  OfxParamHandle tear_detail = nullptr;
  OfxParamHandle edge_boil = nullptr;
  OfxParamHandle paper_color = nullptr;
  OfxParamHandle grain_strength = nullptr;
  OfxParamHandle grain_scale = nullptr;
  OfxParamHandle crease_strength = nullptr;
  OfxParamHandle crease_scale = nullptr;
  OfxParamHandle media_texture_overlay = nullptr;
  OfxParamHandle random_seed = nullptr;
  OfxParamHandle intro_style = nullptr;
  OfxParamHandle intro_frames = nullptr;
  OfxParamHandle outro_style = nullptr;
  OfxParamHandle outro_frames = nullptr;
  OfxParamHandle transition_angle = nullptr;
  OfxParamHandle glide_distance = nullptr;
  OfxParamHandle mirror_trans_h = nullptr;
  OfxParamHandle mirror_trans_v = nullptr;
  OfxParamHandle shadow_opacity = nullptr;
  OfxParamHandle shadow_angle = nullptr;
  OfxParamHandle shadow_distance = nullptr;
  OfxParamHandle shadow_softness = nullptr;
  OfxParamHandle enable_stop_motion = nullptr;
  OfxParamHandle hold_frames = nullptr;
  OfxParamHandle jitter_pos = nullptr;
  OfxParamHandle jitter_rot = nullptr;
  OfxParamHandle master_pos = nullptr;
  OfxParamHandle master_scale = nullptr;
  OfxParamHandle master_angle = nullptr;
  OfxParamHandle flip_h = nullptr;
  OfxParamHandle flip_v = nullptr;

  // Paper Fold & Unfold transition parameters.
  OfxParamHandle transition_progress = nullptr;
  OfxParamHandle fold_action = nullptr;
  OfxParamHandle fold_pattern = nullptr;
  OfxParamHandle fold_stages = nullptr;
  OfxParamHandle fold_direction = nullptr;
  OfxParamHandle fold_depth = nullptr;
  OfxParamHandle slide_distance = nullptr;
  OfxParamHandle stop_motion_steps = nullptr;
  OfxParamHandle jitter_amount = nullptr;
};

// Shared state for one multiThread dispatch. Workers pull small chunks of rows
// from `next_row` so fast threads (and performance cores) keep working while
// rows covering the paper, which cost much more than empty background rows,
// are spread across all threads.
struct ParallelDispatchContext {
  std::atomic<int> next_row{0};
  int y_end = 0;
  int chunk = 1;
  const std::function<void(int y)>* row_fn = nullptr;
};

void ParallelThreadWorker(unsigned int /*thread_index*/,
                          unsigned int thread_max, void* custom_arg) {
  auto* ctx = static_cast<ParallelDispatchContext*>(custom_arg);
  if (ctx == nullptr || ctx->row_fn == nullptr || thread_max == 0) {
    return;
  }
  while (true) {
    const int begin =
        ctx->next_row.fetch_add(ctx->chunk, std::memory_order_relaxed);
    if (begin >= ctx->y_end) {
      return;
    }
    const int end = std::min(ctx->y_end, begin + ctx->chunk);
    for (int y = begin; y < end; ++y) {
      (*ctx->row_fn)(y);
    }
  }
}

OfxPropertySetHandle DefineParamCommon(const HostSuites& s,
                                       OfxParamSetHandle param_set,
                                       const char* param_type, const char* name,
                                       const char* label, const char* parent,
                                       const char* hint) {
  OfxPropertySetHandle props = nullptr;
  if (s.param->paramDefine(param_set, param_type, name, &props) != kOfxStatOK ||
      props == nullptr) {
    return nullptr;
  }
  s.prop->propSetString(props, kOfxPropLabel, 0, label);
  if (parent != nullptr) {
    s.prop->propSetString(props, kOfxParamPropParent, 0, parent);
  }
  if (hint != nullptr) {
    s.prop->propSetString(props, kOfxParamPropHint, 0, hint);
  }
  return props;
}

void DefineGroupParam(const HostSuites& s, OfxParamSetHandle param_set,
                      const char* name, const char* label, bool open_default) {
  OfxPropertySetHandle props = DefineParamCommon(
      s, param_set, kOfxParamTypeGroup, name, label, nullptr, nullptr);
  if (props != nullptr) {
    s.prop->propSetInt(props, kOfxParamPropGroupOpen, 0, open_default ? 1 : 0);
  }
}

void DefineBoolParam(const HostSuites& s, OfxParamSetHandle param_set,
                     const char* name, const char* label, const char* parent,
                     const char* hint, bool def_val) {
  OfxPropertySetHandle props = DefineParamCommon(
      s, param_set, kOfxParamTypeBoolean, name, label, parent, hint);
  if (props != nullptr) {
    s.prop->propSetInt(props, kOfxParamPropDefault, 0, def_val ? 1 : 0);
  }
}

void DefineIntParam(const HostSuites& s, OfxParamSetHandle param_set,
                    const char* name, const char* label, const char* parent,
                    const char* hint, int def_val, int min_val, int max_val,
                    int dmin, int dmax) {
  OfxPropertySetHandle props = DefineParamCommon(
      s, param_set, kOfxParamTypeInteger, name, label, parent, hint);
  if (props != nullptr) {
    s.prop->propSetInt(props, kOfxParamPropDefault, 0, def_val);
    s.prop->propSetInt(props, kOfxParamPropMin, 0, min_val);
    s.prop->propSetInt(props, kOfxParamPropMax, 0, max_val);
    s.prop->propSetInt(props, kOfxParamPropDisplayMin, 0, dmin);
    s.prop->propSetInt(props, kOfxParamPropDisplayMax, 0, dmax);
  }
}

void DefineDoubleParam(const HostSuites& s, OfxParamSetHandle param_set,
                       const char* name, const char* label, const char* parent,
                       const char* hint, const char* double_type,
                       double def_val, double min_val, double max_val,
                       double dmin, double dmax) {
  OfxPropertySetHandle props = DefineParamCommon(
      s, param_set, kOfxParamTypeDouble, name, label, parent, hint);
  if (props == nullptr) {
    return;
  }
  if (double_type != nullptr) {
    s.prop->propSetString(props, kOfxParamPropDoubleType, 0, double_type);
  }
  s.prop->propSetDouble(props, kOfxParamPropDefault, 0, def_val);
  s.prop->propSetDouble(props, kOfxParamPropMin, 0, min_val);
  s.prop->propSetDouble(props, kOfxParamPropMax, 0, max_val);
  s.prop->propSetDouble(props, kOfxParamPropDisplayMin, 0, dmin);
  s.prop->propSetDouble(props, kOfxParamPropDisplayMax, 0, dmax);
}

void DefineDouble2DParam(const HostSuites& s, OfxParamSetHandle param_set,
                         const char* name, const char* label,
                         const char* parent, const char* hint, double def_x,
                         double def_y) {
  OfxPropertySetHandle props = DefineParamCommon(
      s, param_set, kOfxParamTypeDouble2D, name, label, parent, hint);
  if (props == nullptr) {
    return;
  }
  s.prop->propSetString(props, kOfxParamPropDoubleType, 0,
                        kOfxParamDoubleTypePlain);
  const double defs[2] = {def_x, def_y};
  const double mins[2] = {-1.0, -1.0};
  const double maxs[2] = {2.0, 2.0};
  const double dmins[2] = {0.0, 0.0};
  const double dmaxs[2] = {1.0, 1.0};
  s.prop->propSetDoubleN(props, kOfxParamPropDefault, 2, defs);
  s.prop->propSetDoubleN(props, kOfxParamPropMin, 2, mins);
  s.prop->propSetDoubleN(props, kOfxParamPropMax, 2, maxs);
  s.prop->propSetDoubleN(props, kOfxParamPropDisplayMin, 2, dmins);
  s.prop->propSetDoubleN(props, kOfxParamPropDisplayMax, 2, dmaxs);
}

void DefineRgbParam(const HostSuites& s, OfxParamSetHandle param_set,
                    const char* name, const char* label, const char* parent,
                    const char* hint, double r, double g, double b) {
  OfxPropertySetHandle props = DefineParamCommon(s, param_set, kOfxParamTypeRGB,
                                                 name, label, parent, hint);
  if (props == nullptr) {
    return;
  }
  const double defs[3] = {r, g, b};
  const double mins[3] = {0.0, 0.0, 0.0};
  const double maxs[3] = {1.0, 1.0, 1.0};
  s.prop->propSetDoubleN(props, kOfxParamPropDefault, 3, defs);
  s.prop->propSetDoubleN(props, kOfxParamPropMin, 3, mins);
  s.prop->propSetDoubleN(props, kOfxParamPropMax, 3, maxs);
  s.prop->propSetDoubleN(props, kOfxParamPropDisplayMin, 3, mins);
  s.prop->propSetDoubleN(props, kOfxParamPropDisplayMax, 3, maxs);
}

template <std::size_t N>
void DefineChoiceParam(const HostSuites& s, OfxParamSetHandle param_set,
                       const char* name, const char* label, const char* parent,
                       const char* hint, int def_val,
                       const std::array<const char*, N>& options) {
  OfxPropertySetHandle props = DefineParamCommon(
      s, param_set, kOfxParamTypeChoice, name, label, parent, hint);
  if (props != nullptr) {
    s.prop->propSetInt(props, kOfxParamPropDefault, 0, def_val);
    s.prop->propSetStringN(props, kOfxParamPropChoiceOption,
                           static_cast<int>(N), options.data());
  }
}

void DefineCutoutParameters(const HostSuites& s, OfxParamSetHandle param_set) {
  DefineGroupParam(s, param_set, "MediaSourceGroup",
                   "1. Media Source & Placement", true);
  constexpr std::array<const char*, 2> kSourceModes = {
      "Timeline Clip", "Built-in Paper Sticker"};
  DefineChoiceParam(s, param_set, "sourceMode", "Source Mode",
                    "MediaSourceGroup",
                    "Use the timeline clip or a procedural demo sticker.",
                    0, kSourceModes);
  DefineBoolParam(s, param_set, "sourcePremult", "Premultiplied Source Alpha",
                  "MediaSourceGroup",
                  "Disable only if footage has straight/unpremultiplied alpha.",
                  true);
  DefineDoubleParam(s, param_set, "mediaScale", "Media Scale",
                    "MediaSourceGroup", "Scale of the subject inside frame.",
                    kOfxParamDoubleTypeScale, 0.90, 0.05, 5.0, 0.1, 2.5);
  DefineDouble2DParam(
      s, param_set, "mediaOffset", "Media Position", "MediaSourceGroup",
      "Normalized X/Y center placement of the subject.", 0.5, 0.5);
  DefineDoubleParam(s, param_set, "mediaAngle", "Media Rotation",
                    "MediaSourceGroup", "Rotate the subject in degrees.",
                    kOfxParamDoubleTypeAngle, 0.0, -3600.0, 3600.0, -180.0,
                    180.0);

  DefineGroupParam(s, param_set, "KeyerGroup",
                   "2. Green Screen & Matte Cleanup", false);
  DefineBoolParam(s, param_set, "enableChromaKey", "Enable Green Screen Key",
                  "KeyerGroup", "Extract subject alpha from a solid backdrop.",
                  false);
  DefineRgbParam(s, param_set, "keyColor", "Screen Color", "KeyerGroup",
                 "Pick the backdrop color to remove.", 0.0, 1.0, 0.0);
  DefineDoubleParam(s, param_set, "keyGain", "Key Gain", "KeyerGroup",
                    "Matte separation strength against the keyed background.",
                    kOfxParamDoubleTypePlain, 1.0, 0.0, 3.0, 0.5, 2.0);
  DefineDoubleParam(s, param_set, "keyBalance", "Key Balance", "KeyerGroup",
                    "Soft matte threshold balance.", kOfxParamDoubleTypePlain,
                    0.5, 0.0, 1.0, 0.0, 1.0);
  DefineDoubleParam(s, param_set, "edgeChokePx", "Edge Choke (px)",
                    "KeyerGroup", "Shrink (<0) or expand (>0) subject matte.",
                    kOfxParamDoubleTypePlain, 0.0, -32.0, 32.0, -12.0, 12.0);

  DefineGroupParam(s, param_set, "PaperBorderGroup",
                   "3. Torn Paper Cutout Border", true);
  DefineBoolParam(
      s, param_set, "enableBorder", "Enable Paper Border", "PaperBorderGroup",
      "Generate a torn cardstock silhouette around the subject.", true);
  DefineDoubleParam(s, param_set, "borderWidth", "Border Width",
                    "PaperBorderGroup", "Cardstock collar width around matte.",
                    kOfxParamDoubleTypePlain, 0.018, 0.0, 0.15, 0.0, 0.06);
  DefineDoubleParam(s, param_set, "tearRoughness", "Torn Edge Roughness",
                    "PaperBorderGroup",
                    "Amplitude of ragged torn paper fibers.",
                    kOfxParamDoubleTypePlain, 0.012, 0.0, 0.20, 0.0, 0.06);
  DefineDoubleParam(s, param_set, "tearFrequency", "Tear Frequency",
                    "PaperBorderGroup", "Spatial density of paper edge tears.",
                    kOfxParamDoubleTypePlain, 24.0, 1.0, 120.0, 2.0, 60.0);
  DefineIntParam(s, param_set, "tearDetail", "Tear Fiber Detail",
                 "PaperBorderGroup", "Octaves of torn paper micro-fibers.", 5,
                 1, 8, 1, 8);
  DefineDoubleParam(
      s, param_set, "edgeBoil", "Edge Boil Speed", "PaperBorderGroup",
      "Subtle frame-by-frame edge variation on stop-motion steps.",
      kOfxParamDoubleTypePlain, 0.15, 0.0, 5.0, 0.0, 1.0);
  DefineRgbParam(s, param_set, "paperColor", "Paper Cardstock Color",
                 "PaperBorderGroup", "Base tone of the torn cardstock collar.",
                 0.965, 0.945, 0.895);

  DefineGroupParam(s, param_set, "TextureGroup",
                   "4. Paper Grain & Fold Creases", false);
  DefineDoubleParam(s, param_set, "grainStrength", "Paper Grain Strength",
                    "TextureGroup", "Cardstock fiber grain depth.",
                    kOfxParamDoubleTypePlain, 0.35, 0.0, 1.0, 0.0, 1.0);
  DefineDoubleParam(s, param_set, "grainScale", "Grain Scale", "TextureGroup",
                    "Scale of cardstock surface fibers.",
                    kOfxParamDoubleTypeScale, 1.0, 0.1, 10.0, 0.2, 3.0);
  DefineDoubleParam(s, param_set, "creaseStrength", "Fold Crease Strength",
                    "TextureGroup", "Ridged fold crease contrast.",
                    kOfxParamDoubleTypePlain, 0.40, 0.0, 1.0, 0.0, 1.0);
  DefineDoubleParam(s, param_set, "creaseScale", "Crease Scale", "TextureGroup",
                    "Spatial size of folded paper creases.",
                    kOfxParamDoubleTypePlain, 3.2, 0.2, 30.0, 0.8, 12.0);
  DefineDoubleParam(s, param_set, "mediaTextureOverlay",
                    "Media Texture Overlay", "TextureGroup",
                    "Subtle paper crease blending across the subject ink.",
                    kOfxParamDoubleTypePlain, 0.22, 0.0, 1.0, 0.0, 1.0);
  DefineIntParam(s, param_set, "randomSeed", "Texture Seed", "TextureGroup",
                 "Seed controlling tear, crease, and jitter variations.", 7, 0,
                 1000, 0, 100);

  DefineGroupParam(s, param_set, "TransitionGroup",
                   "5. Intro & Outro Paper Animations", true);
  constexpr std::array<const char*, 5> kAnimStyles = {
      "None", "Paper Unfold (3D)", "Crumple Ball", "Paper Airplane Glide",
      "Flat Cutout Pop"};
  DefineChoiceParam(s, param_set, "introStyle", "Intro Style",
                    "TransitionGroup", "Opening paper animation at clip start.",
                    0, kAnimStyles);
  DefineIntParam(s, param_set, "introFrames", "Intro Duration (Frames)",
                 "TransitionGroup", "Length of opening animation in frames.",
                 14, 0, 240, 0, 60);
  DefineChoiceParam(s, param_set, "outroStyle", "Outro Style",
                    "TransitionGroup",
                    "Closing paper animation before clip end.", 0, kAnimStyles);
  DefineIntParam(s, param_set, "outroFrames", "Outro Duration (Frames)",
                 "TransitionGroup", "Length of closing animation in frames.",
                 14, 0, 240, 0, 60);
  DefineDoubleParam(
      s, param_set, "transitionAngle", "Glide Angle", "TransitionGroup",
      "Incoming/outgoing glide direction in degrees.", kOfxParamDoubleTypeAngle,
      -22.0, -360.0, 360.0, -180.0, 180.0);
  DefineDoubleParam(s, param_set, "glideDistance", "Glide Distance",
                    "TransitionGroup", "Travel distance for glide entrance.",
                    kOfxParamDoubleTypePlain, 0.65, 0.0, 3.0, 0.0, 1.5);
  DefineBoolParam(s, param_set, "mirrorTransH", "Mirror Animation H",
                  "TransitionGroup", "Flip intro/outro motion horizontally.",
                  false);
  DefineBoolParam(s, param_set, "mirrorTransV", "Mirror Animation V",
                  "TransitionGroup", "Flip intro/outro motion vertically.",
                  false);

  DefineGroupParam(s, param_set, "ShadowGroup", "6. 3D Drop Shadow", false);
  DefineDoubleParam(s, param_set, "shadowOpacity", "Shadow Opacity",
                    "ShadowGroup", "Drop shadow density behind cutout.",
                    kOfxParamDoubleTypePlain, 0.62, 0.0, 1.0, 0.0, 1.0);
  DefineDoubleParam(s, param_set, "shadowAngle", "Shadow Angle", "ShadowGroup",
                    "Shadow throw angle in degrees.", kOfxParamDoubleTypeAngle,
                    -45.0, -360.0, 360.0, -180.0, 180.0);
  DefineDoubleParam(s, param_set, "shadowDistance", "Shadow Distance",
                    "ShadowGroup", "Drop shadow offset distance.",
                    kOfxParamDoubleTypePlain, 0.016, 0.0, 0.20, 0.0, 0.08);
  DefineDoubleParam(s, param_set, "shadowSoftness", "Shadow Softness",
                    "ShadowGroup", "Feather falloff of the drop shadow.",
                    kOfxParamDoubleTypePlain, 0.018, 0.0, 0.20, 0.0, 0.08);

  DefineGroupParam(s, param_set, "StopMotionMasterGroup",
                   "7. Stop-Motion & Master Transform", true);
  DefineBoolParam(s, param_set, "enableStopMotion", "Enable Stop-Motion",
                  "StopMotionMasterGroup",
                  "Quantize motion into stepped stop-motion frame holds.",
                  true);
  DefineIntParam(s, param_set, "holdFrames", "Hold Frames (Step)",
                 "StopMotionMasterGroup",
                 "Frames held per stop-motion step (e.g. 3 for 8 fps look).", 3,
                 1, 30, 1, 12);
  DefineDoubleParam(s, param_set, "jitterPos", "Position Jitter",
                    "StopMotionMasterGroup",
                    "Hand-placed stop-motion position jitter amplitude.",
                    kOfxParamDoubleTypePlain, 0.0025, 0.0, 0.1, 0.0, 0.03);
  DefineDoubleParam(s, param_set, "jitterRot", "Rotation Jitter",
                    "StopMotionMasterGroup",
                    "Hand-placed stop-motion rotational wobble in degrees.",
                    kOfxParamDoubleTypeAngle, 0.75, 0.0, 30.0, 0.0, 8.0);
  DefineDouble2DParam(
      s, param_set, "masterPos", "Master Position", "StopMotionMasterGroup",
      "Normalized X/Y position of finished paper cutout.", 0.5, 0.5);
  DefineDoubleParam(s, param_set, "masterScale", "Master Scale",
                    "StopMotionMasterGroup", "Uniform scale of final cutout.",
                    kOfxParamDoubleTypeScale, 1.0, 0.01, 10.0, 0.05, 3.0);
  DefineDoubleParam(s, param_set, "masterAngle", "Master Rotation",
                    "StopMotionMasterGroup", "Final cutout rotation.",
                    kOfxParamDoubleTypeAngle, 0.0, -3600.0, 3600.0, -180.0,
                    180.0);
  DefineBoolParam(s, param_set, "flipH", "Flip Horizontal",
                  "StopMotionMasterGroup", "Mirror finished cutout left/right.",
                  false);
  DefineBoolParam(s, param_set, "flipV", "Flip Vertical",
                  "StopMotionMasterGroup", "Mirror finished cutout top/bottom.",
                  false);
}

void DefineFoldTransitionParameters(const HostSuites& s,
                                    OfxParamSetHandle param_set) {
  // Mandatory OpenFX transition progress, animated 0 -> 1 by the host.
  DefineDoubleParam(s, param_set, kOfxImageEffectTransitionParamName,
                    "Progress", nullptr,
                    "Transition progress driven by the timeline (0 to 1).",
                    kOfxParamDoubleTypePlain, 0.0, 0.0, 1.0, 0.0, 1.0);

  DefineGroupParam(s, param_set, "FoldGroup", "1. Fold & Unfold", true);
  constexpr std::array<const char*, 4> kActions = {
      "Auto (Unfold In / Fold Out)", "Unfold Incoming Clip",
      "Fold Outgoing Clip", "Fold Out, Then Unfold In"};
  DefineChoiceParam(
      s, param_set, "foldAction", "Fold Action", "FoldGroup",
      "Auto unfolds at the start of a clip and folds at its end.", 0,
      kActions);
  constexpr std::array<const char*, 4> kPatterns = {
      "Craft Flaps", "Map Grid", "Accordion", "Origami Corners"};
  DefineChoiceParam(s, param_set, "foldPattern", "Fold Pattern", "FoldGroup",
                    "How the paper is folded into a small packet.", 0,
                    kPatterns);
  DefineIntParam(s, param_set, "foldStages", "Fold Stages", "FoldGroup",
                 "Folds that open one after another.", 4, 2, 5, 2, 5);
  DefineDoubleParam(s, param_set, "foldDirection", "Fold Direction",
                    "FoldGroup", "Rotates every fold line, in degrees.",
                    kOfxParamDoubleTypeAngle, 0.0, -360.0, 360.0, -180.0,
                    180.0);
  DefineDoubleParam(s, param_set, "foldDepth", "3D Depth", "FoldGroup",
                    "Perspective of the lifted flaps (0 = flat).",
                    kOfxParamDoubleTypePlain, 0.55, 0.0, 2.0, 0.0, 1.5);
  DefineDoubleParam(
      s, param_set, "slideDistance", "Packet Slide Distance", "FoldGroup",
      "How far the folded packet slides in or out (0 = stays in place).",
      kOfxParamDoubleTypePlain, 0.6, 0.0, 2.0, 0.0, 1.5);
  DefineBoolParam(s, param_set, "mirrorTransH", "Mirror Horizontal",
                  "FoldGroup", "Mirror the fold layout left/right.", false);
  DefineBoolParam(s, param_set, "mirrorTransV", "Mirror Vertical",
                  "FoldGroup", "Mirror the fold layout top/bottom.", false);

  DefineGroupParam(s, param_set, "PaperGroup", "2. Paper Look", true);
  DefineBoolParam(s, param_set, "enableBorder", "Torn Paper Border",
                  "PaperGroup",
                  "Torn cardstock collar shown while the sheet is folded.",
                  true);
  DefineDoubleParam(s, param_set, "borderWidth", "Border Width", "PaperGroup",
                    "Cardstock collar width around the clip.",
                    kOfxParamDoubleTypePlain, 0.018, 0.0, 0.15, 0.0, 0.06);
  DefineDoubleParam(s, param_set, "tearRoughness", "Torn Edge Roughness",
                    "PaperGroup", "Amplitude of the ragged paper fibers.",
                    kOfxParamDoubleTypePlain, 0.012, 0.0, 0.20, 0.0, 0.06);
  DefineDoubleParam(s, param_set, "tearFrequency", "Tear Frequency",
                    "PaperGroup", "Spatial density of the paper tears.",
                    kOfxParamDoubleTypePlain, 24.0, 1.0, 120.0, 2.0, 60.0);
  DefineRgbParam(s, param_set, "paperColor", "Paper Color", "PaperGroup",
                 "Color of the paper back and collar.", 0.965, 0.945, 0.895);
  DefineDoubleParam(s, param_set, "grainStrength", "Paper Grain",
                    "PaperGroup", "Cardstock fiber grain depth.",
                    kOfxParamDoubleTypePlain, 0.38, 0.0, 1.0, 0.0, 1.0);
  DefineDoubleParam(s, param_set, "creaseStrength", "Crease Strength",
                    "PaperGroup", "Crumple texture and fold-line creases.",
                    kOfxParamDoubleTypePlain, 0.52, 0.0, 1.0, 0.0, 1.0);
  DefineIntParam(s, param_set, "randomSeed", "Paper Seed", "PaperGroup",
                 "Varies tears, creases, jitter, and fold angles.", 7, 0, 1000,
                 0, 100);
  DefineBoolParam(s, param_set, "sourcePremult", "Premultiplied Source Alpha",
                  "PaperGroup",
                  "Disable only if the clips have straight alpha.", true);

  DefineGroupParam(s, param_set, "MotionGroup", "3. Stop-Motion & Shadow",
                   true);
  DefineBoolParam(s, param_set, "enableStopMotion", "Enable Stop-Motion",
                  "MotionGroup", "Hold each pose like a stop-motion camera.",
                  true);
  DefineIntParam(s, param_set, "stopMotionSteps", "Stop-Motion Steps",
                 "MotionGroup", "Distinct poses across the transition.", 10, 2,
                 60, 4, 30);
  DefineDoubleParam(s, param_set, "jitterAmount", "Hand Jitter", "MotionGroup",
                    "Small per-step position and rotation wobble.",
                    kOfxParamDoubleTypePlain, 0.25, 0.0, 1.0, 0.0, 1.0);
  DefineDoubleParam(s, param_set, "shadowOpacity", "Shadow Opacity",
                    "MotionGroup", "Drop shadow and flap shadow density.",
                    kOfxParamDoubleTypePlain, 0.62, 0.0, 1.0, 0.0, 1.0);
  DefineDoubleParam(s, param_set, "shadowAngle", "Shadow Angle", "MotionGroup",
                    "Shadow direction; also sets the light on the flaps.",
                    kOfxParamDoubleTypeAngle, -45.0, -360.0, 360.0, -180.0,
                    180.0);
  DefineDoubleParam(s, param_set, "shadowDistance", "Shadow Distance",
                    "MotionGroup", "Drop shadow offset.",
                    kOfxParamDoubleTypePlain, 0.014, 0.0, 0.20, 0.0, 0.08);
  DefineDoubleParam(s, param_set, "shadowSoftness", "Shadow Softness",
                    "MotionGroup", "Feather of the drop shadow.",
                    kOfxParamDoubleTypePlain, 0.020, 0.0, 0.20, 0.0, 0.08);
}

OfxStatus HandleLoad(PluginRole role) {
  HostSuites& s = MutableSuites(role);
  if (s.host == nullptr || s.host->fetchSuite == nullptr) {
    return kOfxStatErrMissingHostFeature;
  }
  s.prop = static_cast<const OfxPropertySuiteV1*>(
      s.host->fetchSuite(s.host->host, kOfxPropertySuite, 1));
  s.param = static_cast<const OfxParameterSuiteV1*>(
      s.host->fetchSuite(s.host->host, kOfxParameterSuite, 1));
  s.effect = static_cast<const OfxImageEffectSuiteV1*>(
      s.host->fetchSuite(s.host->host, kOfxImageEffectSuite, 1));
  s.thread = static_cast<const OfxMultiThreadSuiteV1*>(
      s.host->fetchSuite(s.host->host, kOfxMultiThreadSuite, 1));
  if (s.prop == nullptr || s.param == nullptr || s.effect == nullptr) {
    return kOfxStatErrMissingHostFeature;
  }
  return kOfxStatOK;
}

OfxStatus HandleUnload(PluginRole role) {
  HostSuites& s = MutableSuites(role);
  s.prop = nullptr;
  s.param = nullptr;
  s.effect = nullptr;
  s.thread = nullptr;
  return kOfxStatOK;
}

OfxStatus HandleDescribe(PluginRole role, OfxImageEffectHandle effect) {
  const HostSuites& s = MutableSuites(role);
  OfxPropertySetHandle props = nullptr;
  if (s.effect->getPropertySet(effect, &props) != kOfxStatOK ||
      props == nullptr) {
    return kOfxStatFailed;
  }

  const bool is_transition = role == PluginRole::kFoldTransition;
  const char* label = is_transition ? "Paper Fold & Unfold" : "Paper Cutout FX";
  const char* short_label = is_transition ? "PaperFold" : "PaperCutout";
  const char* long_label =
      is_transition ? "MTTN PaperCraft Studio: Paper Fold & Unfold"
                    : "MTTN PaperCraft Studio: Paper Cutout FX";
  const char* desc =
      is_transition
          ? "Stop-motion paper transition: clips unfold from a folded packet "
            "or fold away, with paper backside, creases, torn edges, and "
            "shadows."
          : "Real-time torn paper cutout border, fiber texture, fold creases, "
            "green screen key, intro/outro paper animations, and stop-motion "
            "jitter.";
  s.prop->propSetString(props, kOfxPropLabel, 0, label);
  s.prop->propSetString(props, kOfxPropShortLabel, 0, short_label);
  s.prop->propSetString(props, kOfxPropLongLabel, 0, long_label);
  s.prop->propSetString(props, kOfxImageEffectPluginPropGrouping, 0,
                        "MTTN PaperCraft Studio");
  s.prop->propSetString(props, kOfxPropPluginDescription, 0, desc);

  const int version[2] = {kVersionMajor, kVersionMinor};
  s.prop->propSetIntN(props, kOfxPropVersion, 2, version);
  s.prop->propSetString(props, kOfxPropVersionLabel, 0, kVersionLabel);

  s.prop->propSetString(props, kOfxImageEffectPropSupportedContexts, 0,
                        is_transition ? kOfxImageEffectContextTransition
                                      : kOfxImageEffectContextFilter);

  const char* depths[3] = {kOfxBitDepthFloat, kOfxBitDepthShort,
                           kOfxBitDepthByte};
  s.prop->propSetStringN(props, kOfxImageEffectPropSupportedPixelDepths, 3,
                         depths);

  s.prop->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0,
                        kOfxImageEffectRenderFullySafe);
  s.prop->propSetInt(props, kOfxImageEffectPluginPropHostFrameThreading, 0, 1);
  s.prop->propSetInt(props, kOfxImageEffectPropSupportsMultiResolution, 0, 1);
  // Folds and torn collars move pixels across the whole frame.
  s.prop->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
  return kOfxStatOK;
}

// Defines an RGBA clip that always arrives as one full-frame buffer.
bool DefineRgbaClip(const HostSuites& s, OfxImageEffectHandle effect,
                    const char* name, bool optional) {
  OfxPropertySetHandle props = nullptr;
  if (s.effect->clipDefine(effect, name, &props) != kOfxStatOK ||
      props == nullptr) {
    return false;
  }
  s.prop->propSetString(props, kOfxImageEffectPropSupportedComponents, 0,
                        kOfxImageComponentRGBA);
  if (optional) {
    s.prop->propSetInt(props, kOfxImageClipPropOptional, 0, 1);
  }
  s.prop->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
  return true;
}

OfxStatus HandleDescribeInContext(PluginRole role, OfxImageEffectHandle effect,
                                  OfxPropertySetHandle in_args) {
  const HostSuites& s = MutableSuites(role);
  char* context = nullptr;
  if (in_args == nullptr ||
      s.prop->propGetString(in_args, kOfxImageEffectPropContext, 0, &context) !=
          kOfxStatOK) {
    return kOfxStatFailed;
  }

  const bool is_transition = role == PluginRole::kFoldTransition;
  if (!StrEq(context, is_transition ? kOfxImageEffectContextTransition
                                    : kOfxImageEffectContextFilter)) {
    return kOfxStatErrMissingHostFeature;
  }

  if (!DefineRgbaClip(s, effect, kOfxImageEffectOutputClipName, false)) {
    return kOfxStatFailed;
  }
  const bool clips_ok =
      is_transition
          ? DefineRgbaClip(s, effect,
                           kOfxImageEffectTransitionSourceFromClipName,
                           false) &&
                DefineRgbaClip(s, effect,
                               kOfxImageEffectTransitionSourceToClipName, false)
          : DefineRgbaClip(s, effect, kOfxImageEffectSimpleSourceClipName,
                           true);
  if (!clips_ok) {
    return kOfxStatFailed;
  }

  OfxParamSetHandle param_set = nullptr;
  if (s.effect->getParamSet(effect, &param_set) != kOfxStatOK ||
      param_set == nullptr) {
    return kOfxStatFailed;
  }
  if (is_transition) {
    DefineFoldTransitionParameters(s, param_set);
  } else {
    DefineCutoutParameters(s, param_set);
  }
  return kOfxStatOK;
}

OfxStatus HandleCreateInstance(PluginRole role, OfxImageEffectHandle effect) {
  const HostSuites& s = MutableSuites(role);
  OfxPropertySetHandle props = nullptr;
  OfxParamSetHandle param_set = nullptr;
  if (s.effect->getPropertySet(effect, &props) != kOfxStatOK ||
      s.effect->getParamSet(effect, &param_set) != kOfxStatOK ||
      props == nullptr || param_set == nullptr) {
    return kOfxStatFailed;
  }

  auto inst = std::make_unique<PluginInstance>();
  inst->role = role;
  s.effect->clipGetHandle(effect, kOfxImageEffectOutputClipName,
                          &inst->output_clip, nullptr);
  auto bind = [&](const char* name, OfxParamHandle* out) {
    s.param->paramGetHandle(param_set, name, out, nullptr);
  };
  // Parameters shared by both plugins.
  bind("sourcePremult", &inst->source_premult);
  bind("enableBorder", &inst->enable_border);
  bind("borderWidth", &inst->border_width);
  bind("tearRoughness", &inst->tear_roughness);
  bind("tearFrequency", &inst->tear_frequency);
  bind("paperColor", &inst->paper_color);
  bind("grainStrength", &inst->grain_strength);
  bind("creaseStrength", &inst->crease_strength);
  bind("randomSeed", &inst->random_seed);
  bind("mirrorTransH", &inst->mirror_trans_h);
  bind("mirrorTransV", &inst->mirror_trans_v);
  bind("shadowOpacity", &inst->shadow_opacity);
  bind("shadowAngle", &inst->shadow_angle);
  bind("shadowDistance", &inst->shadow_distance);
  bind("shadowSoftness", &inst->shadow_softness);
  bind("enableStopMotion", &inst->enable_stop_motion);

  if (role == PluginRole::kFoldTransition) {
    s.effect->clipGetHandle(effect, kOfxImageEffectTransitionSourceFromClipName,
                            &inst->source_from_clip, nullptr);
    s.effect->clipGetHandle(effect, kOfxImageEffectTransitionSourceToClipName,
                            &inst->source_to_clip, nullptr);
    bind(kOfxImageEffectTransitionParamName, &inst->transition_progress);
    bind("foldAction", &inst->fold_action);
    bind("foldPattern", &inst->fold_pattern);
    bind("foldStages", &inst->fold_stages);
    bind("foldDirection", &inst->fold_direction);
    bind("foldDepth", &inst->fold_depth);
    bind("slideDistance", &inst->slide_distance);
    bind("stopMotionSteps", &inst->stop_motion_steps);
    bind("jitterAmount", &inst->jitter_amount);
  } else {
    s.effect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName,
                            &inst->source_clip, nullptr);
    bind("sourceMode", &inst->source_mode);
    bind("mediaScale", &inst->media_scale);
    bind("mediaOffset", &inst->media_offset);
    bind("mediaAngle", &inst->media_angle);
    bind("enableChromaKey", &inst->enable_chroma_key);
    bind("keyColor", &inst->key_color);
    bind("keyGain", &inst->key_gain);
    bind("keyBalance", &inst->key_balance);
    bind("edgeChokePx", &inst->edge_choke_px);
    bind("tearDetail", &inst->tear_detail);
    bind("edgeBoil", &inst->edge_boil);
    bind("grainScale", &inst->grain_scale);
    bind("creaseScale", &inst->crease_scale);
    bind("mediaTextureOverlay", &inst->media_texture_overlay);
    bind("introStyle", &inst->intro_style);
    bind("introFrames", &inst->intro_frames);
    bind("outroStyle", &inst->outro_style);
    bind("outroFrames", &inst->outro_frames);
    bind("transitionAngle", &inst->transition_angle);
    bind("glideDistance", &inst->glide_distance);
    bind("holdFrames", &inst->hold_frames);
    bind("jitterPos", &inst->jitter_pos);
    bind("jitterRot", &inst->jitter_rot);
    bind("masterPos", &inst->master_pos);
    bind("masterScale", &inst->master_scale);
    bind("masterAngle", &inst->master_angle);
    bind("flipH", &inst->flip_h);
    bind("flipV", &inst->flip_v);
  }

  if (s.prop->propSetPointer(props, kOfxPropInstanceData, 0, inst.get()) !=
      kOfxStatOK) {
    return kOfxStatFailed;
  }
  inst.release();
  return kOfxStatOK;
}

PluginInstance* GetInstance(const HostSuites& s, OfxImageEffectHandle effect) {
  OfxPropertySetHandle props = nullptr;
  if (effect == nullptr || s.effect == nullptr ||
      s.effect->getPropertySet(effect, &props) != kOfxStatOK ||
      props == nullptr) {
    return nullptr;
  }
  void* raw = nullptr;
  if (s.prop->propGetPointer(props, kOfxPropInstanceData, 0, &raw) !=
      kOfxStatOK) {
    return nullptr;
  }
  return static_cast<PluginInstance*>(raw);
}

OfxStatus HandleDestroyInstance(PluginRole role, OfxImageEffectHandle effect) {
  const HostSuites& s = MutableSuites(role);
  OfxPropertySetHandle props = nullptr;
  if (s.effect->getPropertySet(effect, &props) != kOfxStatOK ||
      props == nullptr) {
    return kOfxStatReplyDefault;
  }
  void* raw = nullptr;
  if (s.prop->propGetPointer(props, kOfxPropInstanceData, 0, &raw) ==
          kOfxStatOK &&
      raw != nullptr) {
    delete static_cast<PluginInstance*>(raw);
    s.prop->propSetPointer(props, kOfxPropInstanceData, 0, nullptr);
  }
  return kOfxStatOK;
}

const char* PickPreferredDepth(const HostSuites& s, OfxImageClipHandle clip) {
  if (clip == nullptr) {
    return nullptr;
  }
  OfxPropertySetHandle clip_props = nullptr;
  if (s.effect->clipGetPropertySet(clip, &clip_props) != kOfxStatOK ||
      clip_props == nullptr) {
    return nullptr;
  }
  int connected = 0;
  if (s.prop->propGetInt(clip_props, kOfxImageClipPropConnected, 0,
                         &connected) != kOfxStatOK ||
      connected == 0) {
    return nullptr;
  }
  char* unmapped_depth = nullptr;
  if (s.prop->propGetString(clip_props, kOfxImageClipPropUnmappedPixelDepth, 0,
                            &unmapped_depth) != kOfxStatOK ||
      unmapped_depth == nullptr) {
    return kOfxBitDepthFloat;
  }
  if (StrEq(unmapped_depth, kOfxBitDepthByte)) {
    return kOfxBitDepthByte;
  }
  if (StrEq(unmapped_depth, kOfxBitDepthShort)) {
    return kOfxBitDepthShort;
  }
  return kOfxBitDepthFloat;
}

OfxStatus HandleGetClipPreferences(PluginRole role, OfxImageEffectHandle effect,
                                   OfxPropertySetHandle out_args) {
  const HostSuites& s = MutableSuites(role);
  PluginInstance* inst = GetInstance(s, effect);
  if (inst == nullptr || out_args == nullptr) {
    return kOfxStatFailed;
  }

  // Inputs keep their own depth when they agree; mixed inputs go float.
  struct InputClip {
    OfxImageClipHandle handle;
    const char* components_prop;
    const char* depth_prop;
  };
  std::array<InputClip, 2> inputs = {
      InputClip{inst->source_clip, "OfxImageClipPropComponents_Source",
                "OfxImageClipPropDepth_Source"},
      InputClip{nullptr, nullptr, nullptr}};
  if (role == PluginRole::kFoldTransition) {
    inputs = {InputClip{inst->source_from_clip,
                        "OfxImageClipPropComponents_SourceFrom",
                        "OfxImageClipPropDepth_SourceFrom"},
              InputClip{inst->source_to_clip,
                        "OfxImageClipPropComponents_SourceTo",
                        "OfxImageClipPropDepth_SourceTo"}};
  }
  const char* depth = nullptr;
  for (const InputClip& in : inputs) {
    const char* d = PickPreferredDepth(s, in.handle);
    if (d != nullptr) {
      depth = (depth == nullptr || StrEq(depth, d)) ? d : kOfxBitDepthFloat;
    }
  }
  const bool any_connected = depth != nullptr;
  if (!any_connected) {
    depth = kOfxBitDepthFloat;
  }
  s.prop->propSetString(out_args, "OfxImageClipPropComponents_Output", 0,
                        kOfxImageComponentRGBA);
  s.prop->propSetString(out_args, "OfxImageClipPropDepth_Output", 0, depth);
  for (const InputClip& in : inputs) {
    if (any_connected && PickPreferredDepth(s, in.handle) != nullptr) {
      s.prop->propSetString(out_args, in.components_prop, 0,
                            kOfxImageComponentRGBA);
      s.prop->propSetString(out_args, in.depth_prop, 0, depth);
    }
  }
  s.prop->propSetInt(out_args, kOfxImageEffectFrameVarying, 0, 1);

  s.prop->propSetString(out_args, kOfxImageEffectPropPreMultiplication, 0,
                        kOfxImagePreMultiplied);
  return kOfxStatOK;
}

OfxStatus HandleGetRegionsOfInterest(PluginRole role,
                                     OfxImageEffectHandle effect,
                                     OfxPropertySetHandle in_args,
                                     OfxPropertySetHandle out_args) {
  const HostSuites& s = MutableSuites(role);
  PluginInstance* inst = GetInstance(s, effect);
  if (inst == nullptr || in_args == nullptr || out_args == nullptr) {
    return kOfxStatReplyDefault;
  }

  double time = 0.0;
  s.prop->propGetDouble(in_args, kOfxPropTime, 0, &time);

  bool set_any = false;
  auto request_full_clip = [&](OfxImageClipHandle clip, const char* roi_prop) {
    if (clip == nullptr) {
      return;
    }
    OfxRectD rod = {0.0, 0.0, 0.0, 0.0};
    if (s.effect->clipGetRegionOfDefinition(clip, time, &rod) != kOfxStatOK) {
      return;
    }
    const double w = rod.x2 - rod.x1;
    const double h = rod.y2 - rod.y1;
    if (w > 0.0 && h > 0.0 && w <= 16384.0 && h <= 16384.0 && rod.x1 > -1e7 &&
        rod.y1 > -1e7 && rod.x2 < 1e7 && rod.y2 < 1e7) {
      const double vals[4] = {rod.x1, rod.y1, rod.x2, rod.y2};
      if (s.prop->propSetDoubleN(out_args, roi_prop, 4, vals) == kOfxStatOK) {
        set_any = true;
      }
    }
  };

  if (role == PluginRole::kFoldTransition) {
    request_full_clip(inst->source_from_clip, "OfxImageClipPropRoI_SourceFrom");
    request_full_clip(inst->source_to_clip, "OfxImageClipPropRoI_SourceTo");
  } else {
    request_full_clip(inst->source_clip, "OfxImageClipPropRoI_Source");
  }
  return set_any ? kOfxStatOK : kOfxStatReplyDefault;
}

int ReadIntParam(const HostSuites& s, OfxParamHandle param, OfxTime time,
                 int def_val) {
  if (param == nullptr) {
    return def_val;
  }
  int val = def_val;
  return s.param->paramGetValueAtTime(param, time, &val) == kOfxStatOK
             ? val
             : def_val;
}

bool ReadBoolParam(const HostSuites& s, OfxParamHandle param, OfxTime time,
                   bool def_val) {
  return ReadIntParam(s, param, time, def_val ? 1 : 0) != 0;
}

float ReadFloatParam(const HostSuites& s, OfxParamHandle param, OfxTime time,
                     float def_val) {
  if (param == nullptr) {
    return def_val;
  }
  double val = static_cast<double>(def_val);
  return s.param->paramGetValueAtTime(param, time, &val) == kOfxStatOK
             ? static_cast<float>(val)
             : def_val;
}

void ReadDouble2DParam(const HostSuites& s, OfxParamHandle param, OfxTime time,
                       float* out_x, float* out_y, float def_x, float def_y) {
  if (param == nullptr) {
    *out_x = def_x;
    *out_y = def_y;
    return;
  }
  double x = def_x;
  double y = def_y;
  if (s.param->paramGetValueAtTime(param, time, &x, &y) == kOfxStatOK) {
    *out_x = static_cast<float>(x);
    *out_y = static_cast<float>(y);
  } else {
    *out_x = def_x;
    *out_y = def_y;
  }
}

void ReadRgbParam(const HostSuites& s, OfxParamHandle param, OfxTime time,
                  float* out_r, float* out_g, float* out_b, float def_r,
                  float def_g, float def_b) {
  if (param == nullptr) {
    *out_r = def_r;
    *out_g = def_g;
    *out_b = def_b;
    return;
  }
  double r = def_r;
  double g = def_g;
  double b = def_b;
  if (s.param->paramGetValueAtTime(param, time, &r, &g, &b) == kOfxStatOK) {
    *out_r = static_cast<float>(r);
    *out_g = static_cast<float>(g);
    *out_b = static_cast<float>(b);
  } else {
    *out_r = def_r;
    *out_g = def_g;
    *out_b = def_b;
  }
}

CutoutParams ReadCutoutParams(const HostSuites& s, const PluginInstance& inst,
                              OfxTime time, double render_scale_x) {
  CutoutParams cp;
  cp.source_mode = ReadIntParam(s, inst.source_mode, time, 0);
  cp.source_is_premultiplied =
      ReadBoolParam(s, inst.source_premult, time, true);
  cp.media_scale = ReadFloatParam(s, inst.media_scale, time, 0.90f);
  ReadDouble2DParam(s, inst.media_offset, time, &cp.media_offset_x,
                    &cp.media_offset_y, 0.5f, 0.5f);
  cp.media_angle_deg = ReadFloatParam(s, inst.media_angle, time, 0.0f);

  cp.enable_chroma_key = ReadBoolParam(s, inst.enable_chroma_key, time, false);
  ReadRgbParam(s, inst.key_color, time, &cp.key_r, &cp.key_g, &cp.key_b, 0.0f,
               1.0f, 0.0f);
  cp.key_gain = ReadFloatParam(s, inst.key_gain, time, 1.0f);
  cp.key_balance = ReadFloatParam(s, inst.key_balance, time, 0.5f);
  cp.edge_choke_px = ReadFloatParam(s, inst.edge_choke_px, time, 0.0f) *
                     static_cast<float>(std::max(0.05, render_scale_x));

  cp.enable_border = ReadBoolParam(s, inst.enable_border, time, true);
  cp.border_width_norm = ReadFloatParam(s, inst.border_width, time, 0.018f);
  cp.tear_roughness_norm = ReadFloatParam(s, inst.tear_roughness, time, 0.012f);
  cp.tear_frequency = ReadFloatParam(s, inst.tear_frequency, time, 24.0f);
  cp.tear_detail = ReadIntParam(s, inst.tear_detail, time, 5);
  cp.edge_boil = ReadFloatParam(s, inst.edge_boil, time, 0.15f);
  ReadRgbParam(s, inst.paper_color, time, &cp.paper_r, &cp.paper_g, &cp.paper_b,
               0.965f, 0.945f, 0.895f);

  cp.grain_strength = ReadFloatParam(s, inst.grain_strength, time, 0.35f);
  cp.grain_scale = ReadFloatParam(s, inst.grain_scale, time, 1.0f);
  cp.crease_strength = ReadFloatParam(s, inst.crease_strength, time, 0.40f);
  cp.crease_scale = ReadFloatParam(s, inst.crease_scale, time, 3.2f);
  cp.media_texture_overlay =
      ReadFloatParam(s, inst.media_texture_overlay, time, 0.22f);
  cp.random_seed = ReadIntParam(s, inst.random_seed, time, 7);

  cp.intro_style = ReadIntParam(s, inst.intro_style, time, 0);
  cp.intro_frames = ReadIntParam(s, inst.intro_frames, time, 14);
  cp.outro_style = ReadIntParam(s, inst.outro_style, time, 0);
  cp.outro_frames = ReadIntParam(s, inst.outro_frames, time, 14);
  cp.transition_angle_deg =
      ReadFloatParam(s, inst.transition_angle, time, -22.0f);
  cp.glide_distance = ReadFloatParam(s, inst.glide_distance, time, 0.65f);
  cp.mirror_transition_h = ReadBoolParam(s, inst.mirror_trans_h, time, false);
  cp.mirror_transition_v = ReadBoolParam(s, inst.mirror_trans_v, time, false);

  cp.shadow_opacity = ReadFloatParam(s, inst.shadow_opacity, time, 0.62f);
  cp.shadow_angle_deg = ReadFloatParam(s, inst.shadow_angle, time, -45.0f);
  cp.shadow_distance_norm =
      ReadFloatParam(s, inst.shadow_distance, time, 0.016f);
  cp.shadow_softness_norm =
      ReadFloatParam(s, inst.shadow_softness, time, 0.018f);

  cp.enable_stop_motion = ReadBoolParam(s, inst.enable_stop_motion, time, true);
  cp.hold_frames = ReadIntParam(s, inst.hold_frames, time, 3);
  cp.jitter_pos_norm = ReadFloatParam(s, inst.jitter_pos, time, 0.0025f);
  cp.jitter_rot_deg = ReadFloatParam(s, inst.jitter_rot, time, 0.75f);
  ReadDouble2DParam(s, inst.master_pos, time, &cp.master_pos_x,
                    &cp.master_pos_y, 0.5f, 0.5f);
  cp.master_scale = ReadFloatParam(s, inst.master_scale, time, 1.0f);
  cp.master_angle_deg = ReadFloatParam(s, inst.master_angle, time, 0.0f);
  cp.flip_h = ReadBoolParam(s, inst.flip_h, time, false);
  cp.flip_v = ReadBoolParam(s, inst.flip_v, time, false);

  cp.current_time = time;
  cp.frame_start = 0.0;
  cp.frame_end = 120.0;

  auto try_range = [&](OfxImageClipHandle clip) {
    if (clip == nullptr) {
      return false;
    }
    OfxPropertySetHandle cprops = nullptr;
    if (s.effect->clipGetPropertySet(clip, &cprops) != kOfxStatOK ||
        cprops == nullptr) {
      return false;
    }
    double range[2] = {0.0, 0.0};
    if (s.prop->propGetDoubleN(cprops, kOfxImageEffectPropFrameRange, 2,
                               range) != kOfxStatOK) {
      return false;
    }
    const double span = range[1] - range[0];
    if (span > 0.0 && span < 1e7) {
      cp.frame_start = range[0];
      cp.frame_end = range[1];
      return true;
    }
    return false;
  };

  if (!try_range(inst.source_clip)) {
    try_range(inst.output_clip);
  }
  return cp;
}

PaperFoldTransitionParams ReadFoldTransitionParams(const HostSuites& s,
                                                   const PluginInstance& inst,
                                                   OfxTime time) {
  PaperFoldTransitionParams tp;
  tp.progress = ReadFloatParam(s, inst.transition_progress, time, 0.0f);
  tp.fold_action = ReadIntParam(s, inst.fold_action, time, 0);
  tp.fold_pattern = ReadIntParam(s, inst.fold_pattern, time, 0);
  tp.fold_stages = ReadIntParam(s, inst.fold_stages, time, 4);
  tp.direction_angle_deg = ReadFloatParam(s, inst.fold_direction, time, 0.0f);
  tp.fold_depth = ReadFloatParam(s, inst.fold_depth, time, 0.55f);
  tp.slide_distance = ReadFloatParam(s, inst.slide_distance, time, 0.6f);
  tp.mirror_h = ReadBoolParam(s, inst.mirror_trans_h, time, false);
  tp.mirror_v = ReadBoolParam(s, inst.mirror_trans_v, time, false);

  tp.enable_border = ReadBoolParam(s, inst.enable_border, time, true);
  tp.border_width_norm = ReadFloatParam(s, inst.border_width, time, 0.018f);
  tp.tear_roughness_norm =
      ReadFloatParam(s, inst.tear_roughness, time, 0.012f);
  tp.tear_frequency = ReadFloatParam(s, inst.tear_frequency, time, 24.0f);
  ReadRgbParam(s, inst.paper_color, time, &tp.paper_r, &tp.paper_g,
               &tp.paper_b, 0.965f, 0.945f, 0.895f);
  tp.grain_strength = ReadFloatParam(s, inst.grain_strength, time, 0.38f);
  tp.crease_strength = ReadFloatParam(s, inst.crease_strength, time, 0.52f);
  tp.random_seed = ReadIntParam(s, inst.random_seed, time, 7);
  tp.source_is_premultiplied =
      ReadBoolParam(s, inst.source_premult, time, true);

  tp.enable_stop_motion = ReadBoolParam(s, inst.enable_stop_motion, time, true);
  tp.stop_motion_steps = ReadIntParam(s, inst.stop_motion_steps, time, 10);
  tp.jitter_amount = ReadFloatParam(s, inst.jitter_amount, time, 0.25f);
  tp.shadow_opacity = ReadFloatParam(s, inst.shadow_opacity, time, 0.62f);
  tp.shadow_angle_deg = ReadFloatParam(s, inst.shadow_angle, time, -45.0f);
  tp.shadow_distance_norm =
      ReadFloatParam(s, inst.shadow_distance, time, 0.014f);
  tp.shadow_softness_norm =
      ReadFloatParam(s, inst.shadow_softness, time, 0.020f);
  return tp;
}

// The first and last transition frames are the outgoing and incoming clips
// unchanged, so the host can skip rendering them.
OfxStatus HandleIsIdentity(PluginRole role, OfxImageEffectHandle effect,
                           OfxPropertySetHandle in_args,
                           OfxPropertySetHandle out_args) {
  if (role != PluginRole::kFoldTransition) {
    return kOfxStatReplyDefault;
  }
  const HostSuites& s = MutableSuites(role);
  PluginInstance* inst = GetInstance(s, effect);
  if (inst == nullptr || in_args == nullptr || out_args == nullptr) {
    return kOfxStatReplyDefault;
  }
  double time = 0.0;
  s.prop->propGetDouble(in_args, kOfxPropTime, 0, &time);
  const float p = ComputeSteppedTransitionProgress(
      ReadFoldTransitionParams(s, *inst, time));
  if (p > 0.0f && p < 1.0f) {
    return kOfxStatReplyDefault;
  }
  s.prop->propSetString(out_args, kOfxPropName, 0,
                        p <= 0.0f ? kOfxImageEffectTransitionSourceFromClipName
                                  : kOfxImageEffectTransitionSourceToClipName);
  s.prop->propSetDouble(out_args, kOfxPropTime, 0, time);
  return kOfxStatOK;
}

AlphaMode ParseAlphaMode(const HostSuites& s, OfxImageClipHandle clip,
                         OfxPropertySetHandle img_handle) {
  char* premult = nullptr;
  if (s.prop->propGetString(img_handle, kOfxImageEffectPropPreMultiplication, 0,
                            &premult) != kOfxStatOK ||
      premult == nullptr) {
    OfxPropertySetHandle clip_props = nullptr;
    if (clip != nullptr &&
        s.effect->clipGetPropertySet(clip, &clip_props) == kOfxStatOK &&
        clip_props != nullptr) {
      s.prop->propGetString(clip_props, kOfxImageEffectPropPreMultiplication, 0,
                            &premult);
    }
  }
  if (StrEq(premult, kOfxImageOpaque)) {
    return AlphaMode::kOpaque;
  }
  if (StrEq(premult, kOfxImageUnPreMultiplied)) {
    return AlphaMode::kUnPremultiplied;
  }
  return AlphaMode::kPremultiplied;
}

bool ExtractImageView(const HostSuites& s, OfxImageClipHandle clip,
                      OfxPropertySetHandle img_handle, ImageView* out_view,
                      bool* out_wrong_format) {
  *out_wrong_format = false;
  if (img_handle == nullptr) {
    return false;
  }

  char* comps = nullptr;
  char* depth_str = nullptr;
  if (s.prop->propGetString(img_handle, kOfxImageEffectPropComponents, 0,
                            &comps) == kOfxStatOK &&
      comps != nullptr && !StrEq(comps, kOfxImageComponentRGBA)) {
    *out_wrong_format = true;
    return false;
  }

  PixelDepth depth = PixelDepth::kFloat;
  if (s.prop->propGetString(img_handle, kOfxImageEffectPropPixelDepth, 0,
                            &depth_str) == kOfxStatOK &&
      depth_str != nullptr) {
    if (StrEq(depth_str, kOfxBitDepthFloat)) {
      depth = PixelDepth::kFloat;
    } else if (StrEq(depth_str, kOfxBitDepthShort)) {
      depth = PixelDepth::kShort;
    } else if (StrEq(depth_str, kOfxBitDepthByte)) {
      depth = PixelDepth::kByte;
    } else {
      *out_wrong_format = true;
      return false;
    }
  }

  void* data = nullptr;
  int bounds[4] = {0, 0, 0, 0};
  int row_bytes = 0;
  if (s.prop->propGetPointer(img_handle, kOfxImagePropData, 0, &data) !=
          kOfxStatOK ||
      s.prop->propGetIntN(img_handle, kOfxImagePropBounds, 4, bounds) !=
          kOfxStatOK ||
      s.prop->propGetInt(img_handle, kOfxImagePropRowBytes, 0, &row_bytes) !=
          kOfxStatOK) {
    return false;
  }

  out_view->data = data;
  out_view->bounds = {bounds[0], bounds[1], bounds[2], bounds[3]};
  out_view->row_bytes = static_cast<std::ptrdiff_t>(row_bytes);
  out_view->depth = depth;
  out_view->components = 4;
  out_view->alpha_mode = ParseAlphaMode(s, clip, img_handle);

  int rod[4] = {0, 0, 0, 0};
  if (s.prop->propGetIntN(img_handle, kOfxImagePropRegionOfDefinition, 4,
                          rod) == kOfxStatOK &&
      rod[2] > rod[0] && rod[3] > rod[1]) {
    out_view->rod = {rod[0], rod[1], rod[2], rod[3]};
  } else {
    out_view->rod = out_view->bounds;
  }
  return IsValidRgbaView(*out_view);
}

OfxStatus HandleRender(PluginRole role, OfxImageEffectHandle effect,
                       OfxPropertySetHandle in_args) {
  const HostSuites& s = MutableSuites(role);
  PluginInstance* inst = GetInstance(s, effect);
  if (inst == nullptr || in_args == nullptr || inst->output_clip == nullptr) {
    return kOfxStatFailed;
  }

  double time = 0.0;
  s.prop->propGetDouble(in_args, kOfxPropTime, 0, &time);

  double render_scale[2] = {1.0, 1.0};
  s.prop->propGetDoubleN(in_args, kOfxImageEffectPropRenderScale, 2,
                         render_scale);

  ScopedOfxImage out_guard(s.effect);
  if (s.effect->clipGetImage(inst->output_clip, time, nullptr,
                             &out_guard.handle) != kOfxStatOK) {
    return kOfxStatFailed;
  }

  ImageView out_view;
  bool wrong_format = false;
  if (!ExtractImageView(s, inst->output_clip, out_guard.handle, &out_view,
                        &wrong_format)) {
    return wrong_format ? kOfxStatErrImageFormat : kOfxStatFailed;
  }
  const PixelRect canvas_bounds = out_view.rod;

  PixelRect render_window = out_view.bounds;
  int win_vals[4] = {0, 0, 0, 0};
  if (s.prop->propGetIntN(in_args, kOfxImageEffectPropRenderWindow, 4,
                          win_vals) == kOfxStatOK &&
      win_vals[2] > win_vals[0] && win_vals[3] > win_vals[1]) {
    render_window = {win_vals[0], win_vals[1], win_vals[2], win_vals[3]};
  }

  ParallelRowsFn parallel_for = [&](int y_start, int y_end,
                                    const std::function<void(int y)>& row_fn) {
    const int total = y_end - y_start;
    if (total <= 0) {
      return;
    }
    const bool already_spawned =
        s.thread != nullptr &&
        s.thread->multiThreadIsSpawnedThread != nullptr &&
        s.thread->multiThreadIsSpawnedThread() != 0;
    unsigned int num_cpus = 1;
    if (s.thread != nullptr && s.thread->multiThreadNumCPUs != nullptr) {
      s.thread->multiThreadNumCPUs(&num_cpus);
    }
    const unsigned int workers = std::min<unsigned int>(
        std::max<unsigned int>(1, num_cpus), static_cast<unsigned int>(total));
    if (s.thread == nullptr || s.thread->multiThread == nullptr ||
        already_spawned || total < 16 || workers <= 1) {
      for (int y = y_start; y < y_end; ++y) {
        row_fn(y);
      }
      return;
    }
    ParallelDispatchContext ctx;
    ctx.next_row.store(y_start, std::memory_order_relaxed);
    ctx.y_end = y_end;
    ctx.chunk = std::max(1, total / static_cast<int>(workers * 8));
    ctx.row_fn = &row_fn;
    if (s.thread->multiThread(ParallelThreadWorker, workers, &ctx) !=
        kOfxStatOK) {
      for (int y = y_start; y < y_end; ++y) {
        row_fn(y);
      }
    }
  };

  if (role == PluginRole::kFoldTransition) {
    const PaperFoldTransitionParams tp =
        ReadFoldTransitionParams(s, *inst, time);
    const float p = ComputeSteppedTransitionProgress(tp);
    // A missing image is the empty side of a clip edge, not an error.
    auto fetch = [&](OfxImageClipHandle clip, ScopedOfxImage* guard,
                     ImageView* view) {
      if (clip == nullptr || s.effect->clipGetImage(clip, time, nullptr,
                                                    &guard->handle) !=
                                 kOfxStatOK) {
        return true;
      }
      bool wrong_fmt = false;
      if (!ExtractImageView(s, clip, guard->handle, view, &wrong_fmt)) {
        *view = ImageView{};
        return !wrong_fmt;
      }
      if (!tp.source_is_premultiplied &&
          view->alpha_mode != AlphaMode::kOpaque) {
        view->alpha_mode = AlphaMode::kUnPremultiplied;
      }
      return true;
    };
    ScopedOfxImage from_guard(s.effect);
    ScopedOfxImage to_guard(s.effect);
    ImageView from_view;
    ImageView to_view;
    if ((p < 1.0f &&
         !fetch(inst->source_from_clip, &from_guard, &from_view)) ||
        (p > 0.0f && !fetch(inst->source_to_clip, &to_guard, &to_view))) {
      return kOfxStatErrImageFormat;
    }
    RenderPaperFoldTransition(tp, from_view, to_view, out_view, canvas_bounds,
                              render_window, parallel_for);
    return kOfxStatOK;
  }

  CutoutParams cp = ReadCutoutParams(s, *inst, time, render_scale[0]);
  ScopedOfxImage src_guard(s.effect);
  ImageView src_view;
  if (inst->source_clip != nullptr && cp.source_mode == 0) {
    if (s.effect->clipGetImage(inst->source_clip, time, nullptr,
                               &src_guard.handle) == kOfxStatOK) {
      bool src_wrong_fmt = false;
      if (ExtractImageView(s, inst->source_clip, src_guard.handle, &src_view,
                           &src_wrong_fmt)) {
        if (!cp.source_is_premultiplied &&
            src_view.alpha_mode != AlphaMode::kOpaque) {
          src_view.alpha_mode = AlphaMode::kUnPremultiplied;
        }
      } else if (src_wrong_fmt) {
        return kOfxStatErrImageFormat;
      }
    }
  }
  RenderCutoutFrame(cp, src_view, out_view, canvas_bounds, render_window,
                    parallel_for);
  return kOfxStatOK;
}

OfxStatus DispatchAction(PluginRole role, const char* action,
                         const void* handle, OfxPropertySetHandle in_args,
                         OfxPropertySetHandle out_args) {
  if (StrEq(action, kOfxActionLoad)) {
    return HandleLoad(role);
  }
  if (StrEq(action, kOfxActionUnload)) {
    return HandleUnload(role);
  }

  auto effect = static_cast<OfxImageEffectHandle>(const_cast<void*>(handle));
  if (StrEq(action, kOfxActionDescribe)) {
    return HandleDescribe(role, effect);
  }
  if (StrEq(action, kOfxImageEffectActionDescribeInContext)) {
    return HandleDescribeInContext(role, effect, in_args);
  }
  if (StrEq(action, kOfxActionCreateInstance)) {
    return HandleCreateInstance(role, effect);
  }
  if (StrEq(action, kOfxActionDestroyInstance)) {
    return HandleDestroyInstance(role, effect);
  }
  if (StrEq(action, kOfxImageEffectActionGetClipPreferences)) {
    return HandleGetClipPreferences(role, effect, out_args);
  }
  if (StrEq(action, kOfxImageEffectActionGetRegionsOfInterest)) {
    return HandleGetRegionsOfInterest(role, effect, in_args, out_args);
  }
  if (StrEq(action, kOfxImageEffectActionIsIdentity)) {
    return HandleIsIdentity(role, effect, in_args, out_args);
  }
  if (StrEq(action, kOfxImageEffectActionRender)) {
    return HandleRender(role, effect, in_args);
  }
  if (StrEq(action, kOfxImageEffectActionBeginSequenceRender) ||
      StrEq(action, kOfxImageEffectActionEndSequenceRender) ||
      StrEq(action, kOfxActionSyncPrivateData) ||
      StrEq(action, kOfxActionPurgeCaches) ||
      StrEq(action, kOfxActionBeginInstanceChanged) ||
      StrEq(action, kOfxActionEndInstanceChanged) ||
      StrEq(action, kOfxActionInstanceChanged) ||
      StrEq(action, kOfxActionBeginInstanceEdit) ||
      StrEq(action, kOfxActionEndInstanceEdit)) {
    return kOfxStatOK;
  }
  return kOfxStatReplyDefault;
}

void SetHostCutout(OfxHost* host) {
  MutableSuites(PluginRole::kCutout).host = host;
}

void SetHostFoldTransition(OfxHost* host) {
  MutableSuites(PluginRole::kFoldTransition).host = host;
}

OfxStatus MainEntryCutout(const char* action, const void* handle,
                          OfxPropertySetHandle in_args,
                          OfxPropertySetHandle out_args) {
  return DispatchAction(PluginRole::kCutout, action, handle, in_args,
                        out_args);
}

OfxStatus MainEntryFoldTransition(const char* action, const void* handle,
                                  OfxPropertySetHandle in_args,
                                  OfxPropertySetHandle out_args) {
  return DispatchAction(PluginRole::kFoldTransition, action, handle, in_args,
                        out_args);
}

OfxPlugin gPlugins[kPluginCount] = {
    {
        kOfxImageEffectPluginApi,
        kOfxImageEffectPluginApiVersion,
        "com.papercraft.CutoutFX",
        kVersionMajor,
        kVersionMinor,
        SetHostCutout,
        MainEntryCutout,
    },
    {
        kOfxImageEffectPluginApi,
        kOfxImageEffectPluginApiVersion,
        "com.papercraft.PaperFoldTransition",
        kVersionMajor,
        kVersionMinor,
        SetHostFoldTransition,
        MainEntryFoldTransition,
    },
};

}  // namespace
}  // namespace ofx
}  // namespace papercraft

extern "C" {

__attribute__((visibility("default"))) int OfxGetNumberOfPlugins(void) {
  return papercraft::ofx::kPluginCount;
}

__attribute__((visibility("default"))) OfxPlugin* OfxGetPlugin(int nth) {
  if (nth < 0 || nth >= papercraft::ofx::kPluginCount) {
    return nullptr;
  }
  return &papercraft::ofx::gPlugins[nth];
}

}  // extern "C"
