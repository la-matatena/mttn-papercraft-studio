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
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>

namespace papercraft {
namespace ofx {
namespace {

constexpr int kPluginCount = 4;
constexpr int kVersionMajor = 2;
constexpr int kVersionMinor = 0;

enum class PluginRole {
  kCutout = 0,
  kUnfoldTransition = 1,
  kCrumpleTransition = 2,
  kRipWipeTransition = 3,
};

struct HostSuites {
  OfxHost* host = nullptr;
  const OfxPropertySuiteV1* prop = nullptr;
  const OfxParameterSuiteV1* param = nullptr;
  const OfxImageEffectSuiteV1* effect = nullptr;
  const OfxMultiThreadSuiteV1* thread = nullptr;
};

HostSuites& MutableSuites(PluginRole role) {
  static std::array<HostSuites, kPluginCount> kSuitesByRole;
  return kSuitesByRole[static_cast<std::size_t>(role)];
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
  bool is_generator = false;

  OfxImageClipHandle output_clip = nullptr;
  OfxImageClipHandle source_clip = nullptr;
  OfxImageClipHandle source_from_clip = nullptr;
  OfxImageClipHandle source_to_clip = nullptr;

  // Cutout FX parameters.
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

  // Shared Transition parameters.
  OfxParamHandle transition_progress = nullptr;
  OfxParamHandle stop_motion_steps = nullptr;
  OfxParamHandle reverse_direction = nullptr;
  OfxParamHandle mirror_h = nullptr;
  OfxParamHandle mirror_v = nullptr;
  OfxParamHandle fold_tilt = nullptr;
  OfxParamHandle slide_distance = nullptr;
  OfxParamHandle crumple_intensity = nullptr;
  OfxParamHandle spin_angle = nullptr;
  OfxParamHandle rip_angle = nullptr;
};

struct ParallelDispatchContext {
  int y_start = 0;
  int y_end = 0;
  const std::function<void(int y)>* row_fn = nullptr;
};

void ParallelThreadWorker(unsigned int thread_index, unsigned int thread_max,
                          void* custom_arg) {
  auto* ctx = static_cast<ParallelDispatchContext*>(custom_arg);
  if (ctx == nullptr || ctx->row_fn == nullptr || thread_max == 0) {
    return;
  }
  const int total = ctx->y_end - ctx->y_start;
  const int begin = ctx->y_start + (total * static_cast<int>(thread_index)) /
                                       static_cast<int>(thread_max);
  const int end = ctx->y_start + (total * static_cast<int>(thread_index + 1)) /
                                     static_cast<int>(thread_max);
  for (int y = begin; y < end; ++y) {
    (*ctx->row_fn)(y);
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

void DefineCutoutParameters(const HostSuites& s, OfxParamSetHandle param_set,
                            bool is_generator) {
  DefineGroupParam(s, param_set, "MediaSourceGroup",
                   "1. Media Source & Placement", true);
  constexpr std::array<const char*, 2> kSourceModes = {
      "Timeline Clip", "Built-in Paper Sticker"};
  DefineChoiceParam(s, param_set, "sourceMode", "Source Mode",
                    "MediaSourceGroup",
                    "Use the timeline clip or a procedural demo sticker.",
                    is_generator ? 1 : 0, kSourceModes);
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
                    2, kAnimStyles);
  DefineIntParam(s, param_set, "introFrames", "Intro Duration (Frames)",
                 "TransitionGroup", "Length of opening animation in frames.",
                 14, 0, 240, 0, 60);
  DefineChoiceParam(s, param_set, "outroStyle", "Outro Style",
                    "TransitionGroup",
                    "Closing paper animation before clip end.", 1, kAnimStyles);
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

void DefineTransitionParameters(const HostSuites& s,
                                OfxParamSetHandle param_set, PluginRole role) {
  // Mandated OpenFX transition progress parameter.
  DefineDoubleParam(s, param_set, kOfxImageEffectTransitionParamName,
                    "Progress", nullptr,
                    "Host-animated normalized [0, 1] transition progress.",
                    kOfxParamDoubleTypePlain, 0.0, 0.0, 1.0, 0.0, 1.0);

  DefineGroupParam(s, param_set, "StyleGroup", "1. Paper Transition Style",
                   true);
  if (role == PluginRole::kUnfoldTransition) {
    DefineDoubleParam(s, param_set, "foldTilt", "Fold Tilt Angle", "StyleGroup",
                      "3D vertical skew angle during sheet unfold.",
                      kOfxParamDoubleTypeAngle, 22.0, -90.0, 90.0, -60.0, 60.0);
    DefineDoubleParam(s, param_set, "slideDistance", "Slide Distance",
                      "StyleGroup", "Horizontal slide sweep during unfold.",
                      kOfxParamDoubleTypePlain, 0.55, 0.0, 3.0, 0.0, 1.5);
    DefineDoubleParam(s, param_set, "creaseScale", "Fold Crease Scale",
                      "StyleGroup",
                      "Spatial scale of crease texture highlights.",
                      kOfxParamDoubleTypePlain, 4.0, 0.2, 30.0, 0.8, 15.0);
  } else if (role == PluginRole::kCrumpleTransition) {
    DefineDoubleParam(s, param_set, "crumpleIntensity", "Crumple Intensity",
                      "StyleGroup", "Faceted paper warp distortion amount.",
                      kOfxParamDoubleTypePlain, 0.22, 0.0, 1.0, 0.0, 0.5);
    DefineDoubleParam(s, param_set, "creaseScale", "Crumple Facet Scale",
                      "StyleGroup", "Spatial density of paper ball facets.",
                      kOfxParamDoubleTypePlain, 4.0, 0.5, 30.0, 1.0, 15.0);
    DefineDoubleParam(s, param_set, "spinAngle", "Paper Ball Spin",
                      "StyleGroup", "Twist rotation at transition midpoint.",
                      kOfxParamDoubleTypeAngle, 35.0, -360.0, 360.0, -180.0,
                      180.0);
  } else {
    DefineDoubleParam(s, param_set, "ripAngle", "Rip Angle", "StyleGroup",
                      "Direction of the torn paper wipe edge in degrees.",
                      kOfxParamDoubleTypeAngle, 12.0, -180.0, 180.0, -180.0,
                      180.0);
    DefineDoubleParam(s, param_set, "tearWidth", "Torn White Edge Width",
                      "StyleGroup", "Width of the exposed cardstock fiber rim.",
                      kOfxParamDoubleTypePlain, 0.028, 0.0, 0.20, 0.0, 0.08);
    DefineDoubleParam(s, param_set, "tearRoughness", "Rip Jaggedness",
                      "StyleGroup", "Amplitude of torn edge teeth.",
                      kOfxParamDoubleTypePlain, 0.032, 0.0, 0.25, 0.0, 0.10);
    DefineDoubleParam(s, param_set, "tearFrequency", "Tear Frequency",
                      "StyleGroup", "Density of ragged paper teeth along rip.",
                      kOfxParamDoubleTypePlain, 24.0, 1.0, 120.0, 2.0, 60.0);
    DefineRgbParam(s, param_set, "paperColor", "Torn Edge Paper Color",
                   "StyleGroup", "Color of the torn paper fiber rim.", 0.960,
                   0.940, 0.890);
  }

  DefineGroupParam(s, param_set, "StopMotionGroup",
                   "2. Stop-Motion & Orientation", true);
  DefineBoolParam(s, param_set, "enableStopMotion", "Enable Stop-Motion",
                  "StopMotionGroup",
                  "Step transition progress like a stop-motion camera.", true);
  DefineIntParam(s, param_set, "stopMotionSteps", "Stop-Motion Steps",
                 "StopMotionGroup", "Number of discrete hold steps over wipe.",
                 12, 2, 60, 4, 30);
  DefineBoolParam(s, param_set, "reverseDirection", "Reverse Direction",
                  "StopMotionGroup", "Invert wipe progress timing.", false);
  DefineBoolParam(s, param_set, "mirrorH", "Mirror Horizontal",
                  "StopMotionGroup", "Flip transition motion horizontally.",
                  false);
  DefineBoolParam(s, param_set, "mirrorV", "Mirror Vertical", "StopMotionGroup",
                  "Flip transition motion vertically.", false);

  DefineGroupParam(s, param_set, "ShadowGroup", "3. Shadow & Paper Creases",
                   true);
  DefineDoubleParam(s, param_set, "shadowOpacity", "Shadow Opacity",
                    "ShadowGroup", "Drop shadow intensity under moving sheet.",
                    kOfxParamDoubleTypePlain, 0.60, 0.0, 1.0, 0.0, 1.0);
  DefineDoubleParam(s, param_set, "shadowSoftness", "Shadow Softness",
                    "ShadowGroup", "Drop shadow blur width.",
                    kOfxParamDoubleTypePlain, 0.020, 0.0, 0.20, 0.0, 0.08);
  DefineDoubleParam(s, param_set, "creaseIntensity", "Crease Texture Blend",
                    "ShadowGroup", "Strength of folded paper crease shading.",
                    kOfxParamDoubleTypePlain, 0.45, 0.0, 1.0, 0.0, 1.0);
  DefineIntParam(s, param_set, "randomSeed", "Texture Seed", "ShadowGroup",
                 "Seed controlling tear and crease pattern.", 7, 0, 1000, 0,
                 100);
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

  const char* label = "Paper Cutout FX";
  const char* short_label = "PaperCutout";
  const char* long_label = "PaperCraft Studio: Paper Cutout FX";
  const char* desc =
      "Real-time torn paper cutout border, fiber texture, fold creases, "
      "green screen key, intro/outro paper animations, and stop-motion jitter.";
  switch (role) {
    case PluginRole::kCutout:
      break;
    case PluginRole::kUnfoldTransition:
      label = "Paper Unfold Transition";
      short_label = "PaperUnfold";
      long_label = "PaperCraft Studio: Paper Unfold Transition";
      desc =
          "3D folded paper sheet transition with directional drop shadow and "
          "stop-motion frame stepping.";
      break;
    case PluginRole::kCrumpleTransition:
      label = "Paper Crumple Transition";
      short_label = "PaperCrumple";
      long_label = "PaperCraft Studio: Paper Crumple Transition";
      desc =
          "Faceted paper crumple-ball and uncrumple transition for Edit-tab "
          "timelines.";
      break;
    case PluginRole::kRipWipeTransition:
      label = "Paper Rip Wipe Transition";
      short_label = "PaperRipWipe";
      long_label = "PaperCraft Studio: Paper Rip Wipe Transition";
      desc =
          "Jagged torn-paper reveal wipe with exposed cardstock rim and "
          "cast shadow.";
      break;
  }

  s.prop->propSetString(props, kOfxPropLabel, 0, label);
  s.prop->propSetString(props, kOfxPropShortLabel, 0, short_label);
  s.prop->propSetString(props, kOfxPropLongLabel, 0, long_label);
  s.prop->propSetString(props, kOfxImageEffectPluginPropGrouping, 0,
                        "PaperCraft Studio");
  s.prop->propSetString(props, kOfxPropPluginDescription, 0, desc);

  const int version[2] = {kVersionMajor, kVersionMinor};
  s.prop->propSetIntN(props, kOfxPropVersion, 2, version);
  s.prop->propSetString(props, kOfxPropVersionLabel, 0, "2.0.0");

  if (role == PluginRole::kCutout) {
    const char* contexts[2] = {kOfxImageEffectContextFilter,
                               kOfxImageEffectContextGenerator};
    s.prop->propSetStringN(props, kOfxImageEffectPropSupportedContexts, 2,
                           contexts);
  } else {
    const char* contexts[1] = {kOfxImageEffectContextTransition};
    s.prop->propSetStringN(props, kOfxImageEffectPropSupportedContexts, 1,
                           contexts);
  }

  const char* depths[3] = {kOfxBitDepthFloat, kOfxBitDepthShort,
                           kOfxBitDepthByte};
  s.prop->propSetStringN(props, kOfxImageEffectPropSupportedPixelDepths, 3,
                         depths);

  s.prop->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0,
                        kOfxImageEffectRenderFullySafe);
  s.prop->propSetInt(props, kOfxImageEffectPluginPropHostFrameThreading, 0, 1);
  s.prop->propSetInt(props, kOfxImageEffectPropSupportsMultiResolution, 0, 1);
  s.prop->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0,
                     role == PluginRole::kCutout ? 0 : 1);
  return kOfxStatOK;
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

  const bool is_filter = StrEq(context, kOfxImageEffectContextFilter);
  const bool is_generator = StrEq(context, kOfxImageEffectContextGenerator);
  const bool is_transition = StrEq(context, kOfxImageEffectContextTransition);
  if ((role == PluginRole::kCutout && !is_filter && !is_generator) ||
      (role != PluginRole::kCutout && !is_transition)) {
    return kOfxStatErrMissingHostFeature;
  }

  OfxPropertySetHandle out_clip_props = nullptr;
  if (s.effect->clipDefine(effect, kOfxImageEffectOutputClipName,
                           &out_clip_props) != kOfxStatOK ||
      out_clip_props == nullptr) {
    return kOfxStatFailed;
  }
  s.prop->propSetString(out_clip_props, kOfxImageEffectPropSupportedComponents,
                        0, kOfxImageComponentRGBA);
  if (role == PluginRole::kCutout) {
    s.prop->propSetInt(out_clip_props, kOfxImageEffectPropSupportsTiles, 0, 0);
  }

  if (role == PluginRole::kCutout && is_filter) {
    OfxPropertySetHandle src_props = nullptr;
    if (s.effect->clipDefine(effect, kOfxImageEffectSimpleSourceClipName,
                             &src_props) != kOfxStatOK ||
        src_props == nullptr) {
      return kOfxStatFailed;
    }
    s.prop->propSetString(src_props, kOfxImageEffectPropSupportedComponents, 0,
                          kOfxImageComponentRGBA);
    s.prop->propSetInt(src_props, kOfxImageClipPropOptional, 0, 1);
    s.prop->propSetInt(src_props, kOfxImageEffectPropSupportsTiles, 0, 0);
  } else if (role != PluginRole::kCutout) {
    OfxPropertySetHandle from_props = nullptr;
    OfxPropertySetHandle to_props = nullptr;
    if (s.effect->clipDefine(effect,
                             kOfxImageEffectTransitionSourceFromClipName,
                             &from_props) != kOfxStatOK ||
        from_props == nullptr) {
      return kOfxStatFailed;
    }
    if (s.effect->clipDefine(effect, kOfxImageEffectTransitionSourceToClipName,
                             &to_props) != kOfxStatOK ||
        to_props == nullptr) {
      return kOfxStatFailed;
    }
    s.prop->propSetString(from_props, kOfxImageEffectPropSupportedComponents, 0,
                          kOfxImageComponentRGBA);
    s.prop->propSetInt(from_props, kOfxImageEffectPropSupportsTiles, 0, 0);
    s.prop->propSetString(to_props, kOfxImageEffectPropSupportedComponents, 0,
                          kOfxImageComponentRGBA);
    s.prop->propSetInt(to_props, kOfxImageEffectPropSupportsTiles, 0, 0);
  }

  OfxParamSetHandle param_set = nullptr;
  if (s.effect->getParamSet(effect, &param_set) != kOfxStatOK ||
      param_set == nullptr) {
    return kOfxStatFailed;
  }
  if (role == PluginRole::kCutout) {
    DefineCutoutParameters(s, param_set, is_generator);
  } else {
    DefineTransitionParameters(s, param_set, role);
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

  char* context = nullptr;
  s.prop->propGetString(props, kOfxImageEffectPropContext, 0, &context);

  auto inst = std::make_unique<PluginInstance>();
  inst->role = role;
  inst->is_generator = StrEq(context, kOfxImageEffectContextGenerator);

  s.effect->clipGetHandle(effect, kOfxImageEffectOutputClipName,
                          &inst->output_clip, nullptr);
  if (role == PluginRole::kCutout) {
    if (!inst->is_generator) {
      s.effect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName,
                              &inst->source_clip, nullptr);
    }
    auto bind = [&](const char* name, OfxParamHandle* out) {
      s.param->paramGetHandle(param_set, name, out, nullptr);
    };
    bind("sourceMode", &inst->source_mode);
    bind("sourcePremult", &inst->source_premult);
    bind("mediaScale", &inst->media_scale);
    bind("mediaOffset", &inst->media_offset);
    bind("mediaAngle", &inst->media_angle);
    bind("enableChromaKey", &inst->enable_chroma_key);
    bind("keyColor", &inst->key_color);
    bind("keyGain", &inst->key_gain);
    bind("keyBalance", &inst->key_balance);
    bind("edgeChokePx", &inst->edge_choke_px);
    bind("enableBorder", &inst->enable_border);
    bind("borderWidth", &inst->border_width);
    bind("tearRoughness", &inst->tear_roughness);
    bind("tearFrequency", &inst->tear_frequency);
    bind("tearDetail", &inst->tear_detail);
    bind("edgeBoil", &inst->edge_boil);
    bind("paperColor", &inst->paper_color);
    bind("grainStrength", &inst->grain_strength);
    bind("grainScale", &inst->grain_scale);
    bind("creaseStrength", &inst->crease_strength);
    bind("creaseScale", &inst->crease_scale);
    bind("mediaTextureOverlay", &inst->media_texture_overlay);
    bind("randomSeed", &inst->random_seed);
    bind("introStyle", &inst->intro_style);
    bind("introFrames", &inst->intro_frames);
    bind("outroStyle", &inst->outro_style);
    bind("outroFrames", &inst->outro_frames);
    bind("transitionAngle", &inst->transition_angle);
    bind("glideDistance", &inst->glide_distance);
    bind("mirrorTransH", &inst->mirror_trans_h);
    bind("mirrorTransV", &inst->mirror_trans_v);
    bind("shadowOpacity", &inst->shadow_opacity);
    bind("shadowAngle", &inst->shadow_angle);
    bind("shadowDistance", &inst->shadow_distance);
    bind("shadowSoftness", &inst->shadow_softness);
    bind("enableStopMotion", &inst->enable_stop_motion);
    bind("holdFrames", &inst->hold_frames);
    bind("jitterPos", &inst->jitter_pos);
    bind("jitterRot", &inst->jitter_rot);
    bind("masterPos", &inst->master_pos);
    bind("masterScale", &inst->master_scale);
    bind("masterAngle", &inst->master_angle);
    bind("flipH", &inst->flip_h);
    bind("flipV", &inst->flip_v);
  } else {
    s.effect->clipGetHandle(effect, kOfxImageEffectTransitionSourceFromClipName,
                            &inst->source_from_clip, nullptr);
    s.effect->clipGetHandle(effect, kOfxImageEffectTransitionSourceToClipName,
                            &inst->source_to_clip, nullptr);
    auto bind = [&](const char* name, OfxParamHandle* out) {
      s.param->paramGetHandle(param_set, name, out, nullptr);
    };
    bind(kOfxImageEffectTransitionParamName, &inst->transition_progress);
    bind("enableStopMotion", &inst->enable_stop_motion);
    bind("stopMotionSteps", &inst->stop_motion_steps);
    bind("reverseDirection", &inst->reverse_direction);
    bind("mirrorH", &inst->mirror_h);
    bind("mirrorV", &inst->mirror_v);
    bind("shadowOpacity", &inst->shadow_opacity);
    bind("shadowSoftness", &inst->shadow_softness);
    bind("creaseIntensity", &inst->crease_strength);
    bind("randomSeed", &inst->random_seed);
    if (role == PluginRole::kUnfoldTransition) {
      bind("creaseScale", &inst->crease_scale);
      bind("foldTilt", &inst->fold_tilt);
      bind("slideDistance", &inst->slide_distance);
    } else if (role == PluginRole::kCrumpleTransition) {
      bind("creaseScale", &inst->crease_scale);
      bind("crumpleIntensity", &inst->crumple_intensity);
      bind("spinAngle", &inst->spin_angle);
    } else {
      bind("ripAngle", &inst->rip_angle);
      bind("tearWidth", &inst->border_width);
      bind("tearRoughness", &inst->tear_roughness);
      bind("tearFrequency", &inst->tear_frequency);
      bind("paperColor", &inst->paper_color);
    }
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

  const char* depth = kOfxBitDepthFloat;
  if (role == PluginRole::kCutout) {
    const char* src_depth = PickPreferredDepth(s, inst->source_clip);
    if (src_depth != nullptr) {
      depth = src_depth;
    }
    s.prop->propSetString(out_args, "OfxImageClipPropComponents_Output", 0,
                          kOfxImageComponentRGBA);
    s.prop->propSetString(out_args, "OfxImageClipPropDepth_Output", 0, depth);
    if (src_depth != nullptr) {
      s.prop->propSetString(out_args, "OfxImageClipPropComponents_Source", 0,
                            kOfxImageComponentRGBA);
      s.prop->propSetString(out_args, "OfxImageClipPropDepth_Source", 0, depth);
    }
    s.prop->propSetInt(out_args, kOfxImageEffectFrameVarying, 0, 1);
  } else {
    const char* from_depth = PickPreferredDepth(s, inst->source_from_clip);
    const char* to_depth = PickPreferredDepth(s, inst->source_to_clip);
    if (StrEq(from_depth, kOfxBitDepthByte) &&
        StrEq(to_depth, kOfxBitDepthByte)) {
      depth = kOfxBitDepthByte;
    } else if (StrEq(from_depth, kOfxBitDepthShort) &&
               StrEq(to_depth, kOfxBitDepthShort)) {
      depth = kOfxBitDepthShort;
    }
    s.prop->propSetString(out_args, "OfxImageClipPropComponents_Output", 0,
                          kOfxImageComponentRGBA);
    s.prop->propSetString(out_args, "OfxImageClipPropDepth_Output", 0, depth);
    if (from_depth != nullptr) {
      s.prop->propSetString(out_args, "OfxImageClipPropComponents_SourceFrom",
                            0, kOfxImageComponentRGBA);
      s.prop->propSetString(out_args, "OfxImageClipPropDepth_SourceFrom", 0,
                            depth);
    }
    if (to_depth != nullptr) {
      s.prop->propSetString(out_args, "OfxImageClipPropComponents_SourceTo", 0,
                            kOfxImageComponentRGBA);
      s.prop->propSetString(out_args, "OfxImageClipPropDepth_SourceTo", 0,
                            depth);
    }
  }

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

  if (role == PluginRole::kCutout) {
    request_full_clip(inst->source_clip, "OfxImageClipPropRoI_Source");
  } else {
    request_full_clip(inst->source_from_clip, "OfxImageClipPropRoI_SourceFrom");
    request_full_clip(inst->source_to_clip, "OfxImageClipPropRoI_SourceTo");
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

TransitionParams ReadTransitionParams(const HostSuites& s,
                                      const PluginInstance& inst,
                                      OfxTime time) {
  TransitionParams tp;
  if (inst.role == PluginRole::kUnfoldTransition) {
    tp.kind = TransitionKind::kUnfold;
  } else if (inst.role == PluginRole::kCrumpleTransition) {
    tp.kind = TransitionKind::kCrumple;
  } else {
    tp.kind = TransitionKind::kRipWipe;
  }

  tp.progress = ReadFloatParam(s, inst.transition_progress, time, 0.5f);
  tp.enable_stop_motion = ReadBoolParam(s, inst.enable_stop_motion, time, true);
  tp.stop_motion_steps = ReadIntParam(s, inst.stop_motion_steps, time, 12);
  tp.reverse_direction = ReadBoolParam(s, inst.reverse_direction, time, false);
  tp.mirror_h = ReadBoolParam(s, inst.mirror_h, time, false);
  tp.mirror_v = ReadBoolParam(s, inst.mirror_v, time, false);
  tp.shadow_opacity = ReadFloatParam(s, inst.shadow_opacity, time, 0.60f);
  tp.shadow_softness_norm =
      ReadFloatParam(s, inst.shadow_softness, time, 0.020f);
  tp.crease_intensity = ReadFloatParam(s, inst.crease_strength, time, 0.45f);
  tp.crease_scale = ReadFloatParam(s, inst.crease_scale, time, 4.0f);
  tp.random_seed = ReadIntParam(s, inst.random_seed, time, 7);

  if (tp.kind == TransitionKind::kUnfold) {
    tp.fold_tilt_deg = ReadFloatParam(s, inst.fold_tilt, time, 22.0f);
    tp.slide_distance = ReadFloatParam(s, inst.slide_distance, time, 0.55f);
  } else if (tp.kind == TransitionKind::kCrumple) {
    tp.crumple_intensity =
        ReadFloatParam(s, inst.crumple_intensity, time, 0.22f);
    tp.spin_angle_deg = ReadFloatParam(s, inst.spin_angle, time, 35.0f);
  } else {
    tp.rip_angle_deg = ReadFloatParam(s, inst.rip_angle, time, 12.0f);
    tp.tear_width_norm = ReadFloatParam(s, inst.border_width, time, 0.028f);
    tp.tear_roughness_norm =
        ReadFloatParam(s, inst.tear_roughness, time, 0.032f);
    tp.tear_frequency = ReadFloatParam(s, inst.tear_frequency, time, 24.0f);
    ReadRgbParam(s, inst.paper_color, time, &tp.paper_r, &tp.paper_g,
                 &tp.paper_b, 0.960f, 0.940f, 0.890f);
  }
  return tp;
}

CutoutParams ReadCutoutParams(const HostSuites& s, const PluginInstance& inst,
                              OfxTime time, double render_scale_x) {
  CutoutParams cp;
  cp.source_mode =
      ReadIntParam(s, inst.source_mode, time, inst.is_generator ? 1 : 0);
  if (inst.is_generator) {
    cp.source_mode = 1;
  }
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

  cp.intro_style = ReadIntParam(s, inst.intro_style, time, 2);
  cp.intro_frames = ReadIntParam(s, inst.intro_frames, time, 14);
  cp.outro_style = ReadIntParam(s, inst.outro_style, time, 1);
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

OfxStatus HandleIsIdentity(PluginRole role, OfxImageEffectHandle effect,
                           OfxPropertySetHandle in_args,
                           OfxPropertySetHandle out_args) {
  const HostSuites& s = MutableSuites(role);
  PluginInstance* inst = GetInstance(s, effect);
  if (inst == nullptr || in_args == nullptr || out_args == nullptr) {
    return kOfxStatReplyDefault;
  }
  if (role == PluginRole::kCutout) {
    return kOfxStatReplyDefault;
  }

  double time = 0.0;
  s.prop->propGetDouble(in_args, kOfxPropTime, 0, &time);
  const TransitionParams tp = ReadTransitionParams(s, *inst, time);
  const float effective_p = ComputeEffectiveTransitionProgress(tp);
  if (effective_p <= 1e-5f) {
    s.prop->propSetString(out_args, kOfxPropName, 0,
                          kOfxImageEffectTransitionSourceFromClipName);
    s.prop->propSetDouble(out_args, kOfxPropTime, 0, time);
    return kOfxStatOK;
  }
  if (effective_p >= 1.0f - 1e-5f) {
    s.prop->propSetString(out_args, kOfxPropName, 0,
                          kOfxImageEffectTransitionSourceToClipName);
    s.prop->propSetDouble(out_args, kOfxPropTime, 0, time);
    return kOfxStatOK;
  }
  return kOfxStatReplyDefault;
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
    ParallelDispatchContext ctx{y_start, y_end, &row_fn};
    if (s.thread->multiThread(ParallelThreadWorker, workers, &ctx) !=
        kOfxStatOK) {
      for (int y = y_start; y < y_end; ++y) {
        row_fn(y);
      }
    }
  };

  if (role == PluginRole::kCutout) {
    CutoutParams cp = ReadCutoutParams(s, *inst, time, render_scale[0]);
    ScopedOfxImage src_guard(s.effect);
    ImageView src_view;
    if (!inst->is_generator && inst->source_clip != nullptr &&
        cp.source_mode == 0) {
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

  TransitionParams tp = ReadTransitionParams(s, *inst, time);
  ScopedOfxImage from_guard(s.effect);
  ScopedOfxImage to_guard(s.effect);
  ImageView from_view;
  ImageView to_view;

  if (inst->source_from_clip != nullptr &&
      s.effect->clipGetImage(inst->source_from_clip, time, nullptr,
                             &from_guard.handle) == kOfxStatOK) {
    bool fmt_err = false;
    if (!ExtractImageView(s, inst->source_from_clip, from_guard.handle,
                          &from_view, &fmt_err) &&
        fmt_err) {
      return kOfxStatErrImageFormat;
    }
  }
  if (inst->source_to_clip != nullptr &&
      s.effect->clipGetImage(inst->source_to_clip, time, nullptr,
                             &to_guard.handle) == kOfxStatOK) {
    bool fmt_err = false;
    if (!ExtractImageView(s, inst->source_to_clip, to_guard.handle, &to_view,
                          &fmt_err) &&
        fmt_err) {
      return kOfxStatErrImageFormat;
    }
  }

  RenderTransitionFrame(tp, from_view, to_view, out_view, canvas_bounds,
                        render_window, parallel_for);
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

template <PluginRole kRole>
struct RoleCallbacks {
  static void SetHost(OfxHost* host) { MutableSuites(kRole).host = host; }
  static OfxStatus MainEntry(const char* action, const void* handle,
                             OfxPropertySetHandle in_args,
                             OfxPropertySetHandle out_args) {
    return DispatchAction(kRole, action, handle, in_args, out_args);
  }
};

OfxPlugin gPlugins[kPluginCount] = {
    {
        kOfxImageEffectPluginApi,
        kOfxImageEffectPluginApiVersion,
        "com.papercraft.CutoutFX",
        kVersionMajor,
        kVersionMinor,
        RoleCallbacks<PluginRole::kCutout>::SetHost,
        RoleCallbacks<PluginRole::kCutout>::MainEntry,
    },
    {
        kOfxImageEffectPluginApi,
        kOfxImageEffectPluginApiVersion,
        "com.papercraft.UnfoldTransition",
        kVersionMajor,
        kVersionMinor,
        RoleCallbacks<PluginRole::kUnfoldTransition>::SetHost,
        RoleCallbacks<PluginRole::kUnfoldTransition>::MainEntry,
    },
    {
        kOfxImageEffectPluginApi,
        kOfxImageEffectPluginApiVersion,
        "com.papercraft.CrumpleTransition",
        kVersionMajor,
        kVersionMinor,
        RoleCallbacks<PluginRole::kCrumpleTransition>::SetHost,
        RoleCallbacks<PluginRole::kCrumpleTransition>::MainEntry,
    },
    {
        kOfxImageEffectPluginApi,
        kOfxImageEffectPluginApiVersion,
        "com.papercraft.RipWipeTransition",
        kVersionMajor,
        kVersionMinor,
        RoleCallbacks<PluginRole::kRipWipeTransition>::SetHost,
        RoleCallbacks<PluginRole::kRipWipeTransition>::MainEntry,
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
