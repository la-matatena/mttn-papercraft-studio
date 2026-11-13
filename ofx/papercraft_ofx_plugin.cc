// MTTN PaperCraft Studio: OpenFX host glue and exported entry points.
//
// This file wraps the host-independent rendering kernel (papercraft_kernel.h)
// in a binary OpenFX bundle that DaVinci Resolve (or any OpenFX image-effect
// host) can load. One binary registers two plugins:
//
//   com.papercraft.CutoutFX            Filter context: torn-paper cutout,
//                                      chroma key, intro/outro animations.
//   com.papercraft.PaperFoldTransition Transition context: stop-motion paper
//                                      fold & unfold between two clips.
//
// The only exported symbols are OfxGetNumberOfPlugins() and OfxGetPlugin()
// (see the bottom of the file). Each plugin's mainEntry forwards host actions
// to DispatchAction(), which calls one Handle* function per action. This file
// never touches pixels itself: it translates OpenFX properties, parameters,
// clips and images into kernel structs (CutoutParams,
// PaperFoldTransitionParams, ImageView, PixelRect) and then calls
// RenderCutoutFrame() or RenderPaperFoldTransition().
//
// OpenFX lifecycle, in the order the host drives it:
//   setHost                   -> SetHostCutout / SetHostFoldTransition
//   kOfxActionLoad            -> HandleLoad (fetch host suites)
//   kOfxActionDescribe        -> HandleDescribe (labels, context, threading)
//   ...DescribeInContext      -> HandleDescribeInContext (clips, parameters)
//   kOfxActionCreateInstance  -> HandleCreateInstance (new PluginInstance)
//   then, repeatedly per frame:
//     ...GetClipPreferences   -> HandleGetClipPreferences
//     ...GetRegionsOfInterest -> HandleGetRegionsOfInterest
//     ...IsIdentity           -> HandleIsIdentity
//     ...Render               -> HandleRender
//   kOfxActionDestroyInstance -> HandleDestroyInstance (delete instance)
//   kOfxActionUnload          -> HandleUnload (drop suite pointers)
//
// Build: compiled together with papercraft_kernel.cc. The standalone build
// (Install_PaperCraft_OpenFX_macOS.command) defines PAPERCRAFT_STANDALONE_OFX
// and uses flat include paths; the internal build uses full repo paths.

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
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>

namespace papercraft {
namespace ofx {
namespace {

// ---------------------------------------------------------------------------
// Plugin constants and host suites.
// ---------------------------------------------------------------------------

// Number of entries in gPlugins (Cutout filter + Fold transition).
constexpr int kPluginCount = 2;
// Reported to the host through OfxPlugin and kOfxPropVersion. Hosts treat a
// new major version as a different plugin for saved projects, so bump the
// major only for changes that break existing parameter values.
constexpr int kVersionMajor = 3;
constexpr int kVersionMinor = 1;
constexpr const char* kVersionLabel = "3.1.0";

// Each registered OpenFX plugin gets its own entry points and host suites.
// The enum value doubles as the index into the MutableSuites() table.
enum class PluginRole {
  kCutout = 0,
  kFoldTransition = 1,
};

// Host suite pointers fetched in HandleLoad(). The memory belongs to the host
// and stays valid between kOfxActionLoad and kOfxActionUnload.
struct HostSuites {
  // Set by setHost before kOfxActionLoad.
  OfxHost* host = nullptr;
  const OfxPropertySuiteV1* prop = nullptr;
  const OfxParameterSuiteV1* param = nullptr;
  const OfxImageEffectSuiteV1* effect = nullptr;
  // Optional: when nullptr, HandleRender renders all rows on one thread.
  const OfxMultiThreadSuiteV1* thread = nullptr;
};

// Returns the process-wide suite table for `role`. There is one entry per
// plugin because the host calls setHost and kOfxActionLoad separately for
// each plugin, even though both live in this binary. The function-local
// static avoids static initialization order problems. The table is written
// only in setHost/Load/Unload and is read-only while instances render.
HostSuites& MutableSuites(PluginRole role) {
  static std::array<HostSuites, kPluginCount> suites;
  return suites[static_cast<std::size_t>(role)];
}

// Null-safe C string equality. OpenFX action names, contexts and property
// values are C strings that can be nullptr when a property is missing.
bool StrEq(const char* a, const char* b) {
  return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

// RAII guard for an image fetched with clipGetImage(). The destructor calls
// clipReleaseImage() so that every early return in HandleRender hands the
// image back to the host; hosts can stall or leak frames otherwise (the tests
// check that the mock host has zero leased images after each render).
// `handle` is public so it can be passed straight to clipGetImage() as the
// output argument.
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

// ---------------------------------------------------------------------------
// Per-instance state and multithreaded row dispatch.
// ---------------------------------------------------------------------------

// State of one effect instance (one clip or transition on the timeline).
//
// Ownership: allocated in HandleCreateInstance(), stored as a raw pointer in
// the host's kOfxPropInstanceData property, and deleted in
// HandleDestroyInstance(). Everything else only borrows it via GetInstance().
//
// Clip and parameter handles are looked up by name once at creation, so
// render does not repeat string lookups every frame. The handles belong to
// the host and stay valid for the instance's lifetime. A handle that stays
// nullptr (the parameter does not exist for this role) makes the Read*Param
// helpers return their default. The struct is read-only after creation, so
// concurrent render calls can share it without locking.
struct PluginInstance {
  PluginRole role = PluginRole::kCutout;
  OfxImageClipHandle output_clip = nullptr;
  // Cutout filter only: the "Source" clip (optional in the filter context).
  OfxImageClipHandle source_clip = nullptr;
  // Fold transition only: outgoing ("SourceFrom") and incoming ("SourceTo").
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
//
// Threading: the context lives on the stack of the parallel_for lambda in
// HandleRender(). multiThread() blocks until every worker returns, so the
// pointer stays valid for the whole dispatch. Workers only modify `next_row`
// (atomically) and their own claimed entries of `chunk_done`; the other
// fields are read-only once dispatch starts.
struct ParallelDispatchContext {
  // Next row not yet claimed by any worker.
  std::atomic<int> next_row{0};
  // First row of the dispatch; chunk k covers rows
  // [y_start + k * chunk, y_start + (k + 1) * chunk).
  int y_start = 0;
  // Exclusive end row of the dispatch.
  int y_end = 0;
  // Rows claimed per fetch_add.
  int chunk = 1;
  // One flag per chunk, set after every row of that chunk has run. If
  // multiThread() fails midway, the serial fallback skips finished chunks
  // and redoes the rest (including chunks a worker claimed but did not
  // finish). Owned by the caller of parallel_for.
  std::atomic<bool>* chunk_done = nullptr;
  // Per-row kernel callback. Must be safe to run concurrently for different
  // rows; it is owned by the caller of parallel_for.
  const std::function<void(int y)>* row_fn = nullptr;
};

// OfxThreadFunctionV1 run on each thread spawned by the host's multiThread().
// Repeatedly claims `chunk` rows and runs `row_fn` on them until all rows are
// taken, marking each chunk in `chunk_done` once it is complete.
// `thread_index` is unused because work is pulled dynamically instead of
// split into fixed slices. Relaxed ordering is enough for fetch_add, which
// only has to hand out disjoint row ranges; the release store on
// `chunk_done` pairs with the fallback's acquire load (multiThread() also
// joins all workers before the caller reads the output image).
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
    if (ctx->chunk_done != nullptr) {
      ctx->chunk_done[(begin - ctx->y_start) / ctx->chunk].store(
          true, std::memory_order_release);
    }
  }
}

// ---------------------------------------------------------------------------
// Parameter definition helpers.
//
// Called only from HandleDescribeInContext(). Every `name` argument is the
// parameter's persistent ID: the host saves keyframes and values in projects
// under it, and HandleCreateInstance() looks the handle up by the same
// string. Renaming an ID silently resets that control in existing projects,
// so IDs must stay stable; labels, hints and group order may change freely.
// If the host rejects a definition the helper does nothing; the instance then
// gets a nullptr handle and the Read*Param helpers use their defaults.
// ---------------------------------------------------------------------------

// Defines a parameter of OpenFX type `param_type` and sets the properties
// shared by all types: label, optional parent group ID (`parent`) and
// optional tooltip (`hint`). Returns the parameter's property set so callers
// can add type-specific properties, or nullptr if paramDefine failed.
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

// Defines a collapsible parameter group (a twirl-down section in Resolve's
// Inspector). Child parameters join it by passing `name` as their `parent`.
// `open_default` sets whether the group starts expanded.
void DefineGroupParam(const HostSuites& s, OfxParamSetHandle param_set,
                      const char* name, const char* label, bool open_default) {
  OfxPropertySetHandle props = DefineParamCommon(
      s, param_set, kOfxParamTypeGroup, name, label, nullptr, nullptr);
  if (props != nullptr) {
    s.prop->propSetInt(props, kOfxParamPropGroupOpen, 0, open_default ? 1 : 0);
  }
}

// Defines a checkbox parameter with default `def_val`. Read back with
// ReadBoolParam().
void DefineBoolParam(const HostSuites& s, OfxParamSetHandle param_set,
                     const char* name, const char* label, const char* parent,
                     const char* hint, bool def_val) {
  OfxPropertySetHandle props = DefineParamCommon(
      s, param_set, kOfxParamTypeBoolean, name, label, parent, hint);
  if (props != nullptr) {
    s.prop->propSetInt(props, kOfxParamPropDefault, 0, def_val ? 1 : 0);
  }
}

// Defines an integer parameter. [min_val, max_val] is the hard limit the
// host enforces; [dmin, dmax] is only the slider range, so users can still
// type values outside it up to the hard limit.
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

// Defines a scalar double parameter. `double_type` (for example
// kOfxParamDoubleTypeAngle for degrees or kOfxParamDoubleTypeScale) only
// changes how the host displays the value; nullptr keeps the host default.
// Hard limits and slider range work as in DefineIntParam().
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

// Defines a 2D position parameter in normalized frame units (0..1 = frame
// edges). It uses the plain double type, not an OpenFX normalized/canonical
// XY type, so the host passes the values through unchanged and the kernel
// does the mapping to pixels. The hard limit -1..2 allows placing the subject
// partly off screen; the slider covers the visible frame 0..1.
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

// Defines an RGB color picker with default (r, g, b), each channel limited to
// 0..1.
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

// Defines a drop-down whose entries are `options`, with default index
// `def_val`. The host saves the selected index, not the label, so new
// options must be appended at the end; reordering changes existing projects.
// The index maps directly to kernel enums (CutoutAnimStyle,
// FoldTransitionAction, FoldPattern) or to CutoutParams::source_mode.
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

// ---------------------------------------------------------------------------
// Parameter layouts.
//
// Group IDs ("MediaSourceGroup", "FoldGroup", ...) and parameter IDs are
// persistent (see the helper section above). Parameters that exist in both
// plugins deliberately use the same ID (for example "borderWidth",
// "paperColor", "shadowAngle", "mirrorTransH") so HandleCreateInstance() can
// bind them in one shared block. Keep the IDs here, the bind() calls in
// HandleCreateInstance() and the defaults in Read*Params() in sync.
// Group labels start with a number so the Inspector order is obvious.
// "_norm" style values (border width, roughness, shadow distance/softness,
// jitter) are fractions of the frame size, so the look does not depend on
// resolution.
// ---------------------------------------------------------------------------

// Defines the Paper Cutout FX controls in seven groups: media source and
// placement, green screen key, torn border, paper texture, intro/outro
// animations, drop shadow, and stop-motion plus master transform.
void DefineCutoutParameters(const HostSuites& s, OfxParamSetHandle param_set) {
  DefineGroupParam(s, param_set, "MediaSourceGroup",
                   "1. Media Source & Placement", true);
  constexpr std::array<const char*, 2> kSourceModes = {
      "Timeline Clip", "Built-in Paper Sticker"};
  DefineChoiceParam(
      s, param_set, "sourceMode", "Source Mode", "MediaSourceGroup",
      "Use the timeline clip or a procedural demo sticker.", 0, kSourceModes);
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

// Defines the Paper Fold & Unfold controls: the mandatory transition progress
// parameter plus three groups (fold behavior, paper look, stop-motion and
// shadow). Shared IDs reuse the Cutout names, but some defaults differ (for
// example grainStrength 0.38 vs 0.35), so ReadFoldTransitionParams() keeps
// its own default values.
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
  DefineChoiceParam(s, param_set, "foldAction", "Fold Action", "FoldGroup",
                    "Auto unfolds at the start of a clip and folds at its end.",
                    0, kActions);
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
  DefineBoolParam(s, param_set, "mirrorTransV", "Mirror Vertical", "FoldGroup",
                  "Mirror the fold layout top/bottom.", false);

  DefineGroupParam(s, param_set, "PaperGroup", "2. Paper Look", true);
  DefineBoolParam(
      s, param_set, "enableBorder", "Torn Paper Border", "PaperGroup",
      "Torn cardstock collar shown while the sheet is folded.", true);
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
  DefineDoubleParam(s, param_set, "grainStrength", "Paper Grain", "PaperGroup",
                    "Cardstock fiber grain depth.", kOfxParamDoubleTypePlain,
                    0.38, 0.0, 1.0, 0.0, 1.0);
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

// ---------------------------------------------------------------------------
// Plugin-level actions: load, unload, describe.
// ---------------------------------------------------------------------------

// kOfxActionLoad: first action after setHost. Fetches the V1 property,
// parameter, image-effect and multithread suites from the host. The first
// three are required (kOfxStatErrMissingHostFeature makes the host skip the
// plugin); the multithread suite is optional.
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

// kOfxActionUnload: last action before the host drops the plugin. Clears the
// suite pointers so nothing can use them after the host frees them. The
// OfxHost pointer is kept; the host resets it via setHost before reloading.
OfxStatus HandleUnload(PluginRole role) {
  HostSuites& s = MutableSuites(role);
  s.prop = nullptr;
  s.param = nullptr;
  s.effect = nullptr;
  s.thread = nullptr;
  return kOfxStatOK;
}

// kOfxActionDescribe: writes plugin-wide properties into the global
// descriptor `effect`: labels, the "MTTN PaperCraft Studio" grouping (the
// folder shown under Open FX in Resolve), version, the single supported
// context, pixel depths, and threading/tiling capabilities. No clips or
// parameters exist yet; those come in HandleDescribeInContext().
OfxStatus HandleDescribe(PluginRole role, OfxImageEffectHandle effect) {
  const HostSuites& s = MutableSuites(role);
  if (s.effect == nullptr || s.prop == nullptr) {
    return kOfxStatErrMissingHostFeature;
  }
  OfxPropertySetHandle props = nullptr;
  if (s.effect->getPropertySet(effect, &props) != kOfxStatOK ||
      props == nullptr) {
    return kOfxStatFailed;
  }

  const bool is_transition = role == PluginRole::kFoldTransition;
  const char* label = is_transition ? "Paper Fold & Unfold" : "Paper Cutout FX";
  const char* short_label = is_transition ? "PaperFold" : "PaperCutout";
  const char* long_label = is_transition
                               ? "MTTN PaperCraft Studio: Paper Fold & Unfold"
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

  // The kernel reads and writes all three depths natively.
  const char* depths[3] = {kOfxBitDepthFloat, kOfxBitDepthShort,
                           kOfxBitDepthByte};
  s.prop->propSetStringN(props, kOfxImageEffectPropSupportedPixelDepths, 3,
                         depths);

  // Render keeps no shared mutable state, so the host may run any number of
  // render calls at once and may split frames across its own threads. Inputs
  // and output may differ in size (multi-resolution), e.g. cropped clips.
  s.prop->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0,
                        kOfxImageEffectRenderFullySafe);
  s.prop->propSetInt(props, kOfxImageEffectPluginPropHostFrameThreading, 0, 1);
  s.prop->propSetInt(props, kOfxImageEffectPropSupportsMultiResolution, 0, 1);
  // Folds and torn collars move pixels across the whole frame.
  s.prop->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
  return kOfxStatOK;
}

// Defines an RGBA clip that always arrives as one full-frame buffer.
// Used by HandleDescribeInContext(); `name` must be one of the clip names the
// OpenFX spec fixes for the context ("Output", "Source", "SourceFrom",
// "SourceTo"). Returns false if the host refused the clip.
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

// kOfxImageEffectActionDescribeInContext: called once per context the host
// wants to use, with the context name in `in_args`. Each plugin accepts only
// its own context (Filter for Cutout, Transition for Fold) and answers
// kOfxStatErrMissingHostFeature for any other, so Resolve lists each effect
// in exactly one category. On success defines the clips and all parameters.
OfxStatus HandleDescribeInContext(PluginRole role, OfxImageEffectHandle effect,
                                  OfxPropertySetHandle in_args) {
  const HostSuites& s = MutableSuites(role);
  if (s.effect == nullptr || s.prop == nullptr || s.param == nullptr) {
    return kOfxStatErrMissingHostFeature;
  }
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
  // The filter's Source clip is optional so the built-in sticker source mode
  // works without any input. Transitions always need both clips.
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

// ---------------------------------------------------------------------------
// Instance lifetime.
// ---------------------------------------------------------------------------

// kOfxActionCreateInstance: allocates a PluginInstance for the new effect
// instance `effect` and caches its clip and parameter handles. Only the
// Output clip is required; any other handle the host does not return stays
// nullptr and falls back to defaults at render time. The unique_ptr gives up
// ownership only after the host has stored the pointer in
// kOfxPropInstanceData, so failure paths do not leak.
OfxStatus HandleCreateInstance(PluginRole role, OfxImageEffectHandle effect) {
  const HostSuites& s = MutableSuites(role);
  if (s.effect == nullptr || s.prop == nullptr || s.param == nullptr) {
    return kOfxStatErrMissingHostFeature;
  }
  OfxPropertySetHandle props = nullptr;
  OfxParamSetHandle param_set = nullptr;
  if (s.effect->getPropertySet(effect, &props) != kOfxStatOK ||
      s.effect->getParamSet(effect, &param_set) != kOfxStatOK ||
      props == nullptr || param_set == nullptr) {
    return kOfxStatFailed;
  }

  auto inst = std::make_unique<PluginInstance>();
  inst->role = role;
  if (s.effect->clipGetHandle(effect, kOfxImageEffectOutputClipName,
                              &inst->output_clip, nullptr) != kOfxStatOK ||
      inst->output_clip == nullptr) {
    return kOfxStatFailed;
  }
  // Looks up a parameter by its persistent ID. Errors are ignored on purpose:
  // a missing parameter leaves *out as nullptr, which readers treat as
  // "use the default".
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

// Returns the PluginInstance stored on `effect` by HandleCreateInstance(), or
// nullptr if there is none or the suites are not loaded. The caller borrows
// the pointer; it stays valid until kOfxActionDestroyInstance.
PluginInstance* GetInstance(const HostSuites& s, OfxImageEffectHandle effect) {
  OfxPropertySetHandle props = nullptr;
  if (effect == nullptr || s.effect == nullptr || s.prop == nullptr ||
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

// kOfxActionDestroyInstance: deletes the PluginInstance and clears
// kOfxPropInstanceData so a repeated destroy is harmless. Returns
// kOfxStatReplyDefault when there is nothing this plugin can clean up.
OfxStatus HandleDestroyInstance(PluginRole role, OfxImageEffectHandle effect) {
  const HostSuites& s = MutableSuites(role);
  if (s.effect == nullptr || s.prop == nullptr) {
    return kOfxStatReplyDefault;
  }
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

// ---------------------------------------------------------------------------
// Clip preferences.
// ---------------------------------------------------------------------------

// Returns the depth this plugin would like for input `clip`: the clip's
// native (unmapped) depth if it is byte or short, float otherwise. Returns
// nullptr when the clip is missing or not connected, so it does not take part
// in the depth decision.
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

// kOfxImageEffectActionGetClipPreferences: tells the host which format to
// deliver. Output is always RGBA and premultiplied. All connected inputs and
// the output share one depth so the kernel never mixes formats. The output is
// marked frame-varying because animations, edge boil and stop-motion jitter
// change it every frame even when the input is a still. Per-clip properties
// follow the OpenFX naming pattern "OfxImageClipPropDepth_<clip name>".
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
    inputs = {
        InputClip{inst->source_from_clip,
                  "OfxImageClipPropComponents_SourceFrom",
                  "OfxImageClipPropDepth_SourceFrom"},
        InputClip{inst->source_to_clip, "OfxImageClipPropComponents_SourceTo",
                  "OfxImageClipPropDepth_SourceTo"}};
  }
  std::array<const char*, 2> clip_depths = {nullptr, nullptr};
  const char* depth = nullptr;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const char* d = PickPreferredDepth(s, inputs[i].handle);
    clip_depths[i] = d;
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
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    if (any_connected && clip_depths[i] != nullptr) {
      s.prop->propSetString(out_args, inputs[i].components_prop, 0,
                            kOfxImageComponentRGBA);
      s.prop->propSetString(out_args, inputs[i].depth_prop, 0, depth);
    }
  }
  s.prop->propSetInt(out_args, kOfxImageEffectFrameVarying, 0, 1);

  s.prop->propSetString(out_args, kOfxImageEffectPropPreMultiplication, 0,
                        kOfxImagePreMultiplied);
  return kOfxStatOK;
}

// ---------------------------------------------------------------------------
// Parameter readers.
//
// Each reader samples a parameter at host time `time` (frames) with
// paramGetValueAtTime(), so keyframed values animate. They return the given
// default when the handle is nullptr (parameter not bound) or the host call
// fails, so render never sees uninitialized values. Called from render,
// isIdentity and getRegionsOfInterest, which may run on several host threads
// at once; the parameter suite allows concurrent reads during render.
// ---------------------------------------------------------------------------

// Reads an integer, boolean or choice parameter (all are ints in OpenFX).
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

// Reads a boolean parameter (stored as int 0/1).
bool ReadBoolParam(const HostSuites& s, OfxParamHandle param, OfxTime time,
                   bool def_val) {
  return ReadIntParam(s, param, time, def_val ? 1 : 0) != 0;
}

// Reads a scalar double parameter and narrows it to float for the kernel.
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

// Reads a 2D double parameter into *out_x / *out_y (normalized frame units
// for the position parameters defined by DefineDouble2DParam()).
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

// Reads an RGB color parameter into *out_r / *out_g / *out_b (0..1).
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

// Builds the kernel's CutoutParams for frame `time`. The defaults passed to
// each reader must match DefineCutoutParameters(). `render_scale_x` and
// `render_scale_y` are the host's proxy scale per axis (1.0 = full
// resolution, 0.5 = half-res preview); they convert pixel-unit parameters so
// the look matches across preview and final renders. Also reads the clip's
// frame range, which the kernel needs to time intro/outro animations.
CutoutParams ReadCutoutParams(const HostSuites& s, const PluginInstance& inst,
                              OfxTime time, double render_scale_x,
                              double render_scale_y) {
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
  // Edge choke is entered in full-resolution pixels; scale it to the render
  // resolution. The choke is a single isotropic radius, so with a non-uniform
  // render scale (X != Y) use the geometric mean of both axes: it keeps the
  // eroded area proportional to the proxy's pixel area and equals the common
  // scale when X == Y. Each axis gets a 0.05 floor first, which guards
  // against a zero, negative or NaN scale (std::max returns 0.05 for NaN).
  const double choke_scale = std::sqrt(std::max(0.05, render_scale_x) *
                                       std::max(0.05, render_scale_y));
  cp.edge_choke_px = ReadFloatParam(s, inst.edge_choke_px, time, 0.0f) *
                     static_cast<float>(choke_scale);

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
  // Fallback range when no clip reports a usable one.
  cp.frame_start = 0.0;
  cp.frame_end = 120.0;

  // Copies a clip's kOfxImageEffectPropFrameRange (host frames) into cp if it
  // is a sane positive span; returns whether it did.
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

  // Prefer the source clip's range; fall back to the output clip's range (for
  // example in sticker mode with no input connected).
  if (!try_range(inst.source_clip)) {
    try_range(inst.output_clip);
  }
  return cp;
}

// Builds the kernel's PaperFoldTransitionParams for frame `time`. The
// defaults must match DefineFoldTransitionParameters(). `progress` is the
// host-animated 0..1 transition parameter; stop-motion quantization of it
// happens later in ComputeSteppedTransitionProgress().
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
  tp.tear_roughness_norm = ReadFloatParam(s, inst.tear_roughness, time, 0.012f);
  tp.tear_frequency = ReadFloatParam(s, inst.tear_frequency, time, 24.0f);
  ReadRgbParam(s, inst.paper_color, time, &tp.paper_r, &tp.paper_g, &tp.paper_b,
               0.965f, 0.945f, 0.895f);
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

// ---------------------------------------------------------------------------
// Regions of interest.
// ---------------------------------------------------------------------------

// Returns whether the Cutout filter reads its Source clip for `source_mode`
// (the "sourceMode" choice). Only Timeline Clip (0) samples the source; the
// Built-in Paper Sticker (1) is fully procedural, including its SDF, because
// the kernel's BuildSubjectTransform() switches to the demo sticker. Shared
// by HandleGetRegionsOfInterest and HandleRender so the RoI never declares a
// clip unused that render would still fetch.
bool CutoutReadsSourceClip(int source_mode) { return source_mode == 0; }

// Which transition inputs one frame reads.
struct TransitionClipUse {
  bool from = true;  // SourceFrom (outgoing clip).
  bool to = true;    // SourceTo (incoming clip).
};

// Returns which inputs RenderPaperFoldTransition() can read for `tp` at
// stepped progress `p` (from ComputeSteppedTransitionProgress(tp)). Mirrors
// the kernel exactly:
//   - p == 0 copies SourceFrom unchanged; p == 1 copies SourceTo.
//   - A forced Fold & Unfold (Fold Action 3, clamped like the kernel) has no
//     `under` clip: the first half (p < 0.5) folds SourceFrom away, the
//     second half (p >= 0.5) unfolds SourceTo.
//   - Auto inspects both clips to pick an action, and forced Unfold In /
//     Fold Out draw the other clip underneath, so both are needed.
// Shared by HandleGetRegionsOfInterest and HandleRender.
TransitionClipUse TransitionClipsUsed(const PaperFoldTransitionParams& tp,
                                      float p) {
  TransitionClipUse use;
  if (p <= 0.0f) {
    use.to = false;
  } else if (p >= 1.0f) {
    use.from = false;
  } else if (std::min(3, std::max(0, tp.fold_action)) ==
             static_cast<int>(FoldTransitionAction::kFoldAndUnfold)) {
    use.from = p < 0.5f;
    use.to = !use.from;
  }
  return use;
}

// kOfxImageEffectActionGetRegionsOfInterest: asks for the whole region of
// definition of every input the frame at kOfxPropTime will read, and an
// empty RoI for inputs it will not read.
//
// Used inputs get their full frame, not a window around the render window:
// the plugin declares kOfxImageEffectPropSupportsTiles = 0, so hosts always
// render the whole output, and the kernel's algorithms are global anyway
// (matte SDF, InspectClipContent, BuildCleanCutoutAlpha, and folds, torn
// collars, shadows and the media/master transforms can sample any source
// pixel for any output pixel). RoDs (canonical coordinates) that look
// degenerate or unbounded are skipped: empty, larger than 16384 per side, or
// beyond +/-1e7 (what hosts report for infinite generators).
//
// Unused inputs (Cutout in Built-in Paper Sticker mode; the hidden side of a
// transition at p == 0 / p == 1 or in either half of a forced Fold &
// Unfold) get a zero-area RoI (0, 0, 0, 0). The OpenFX headers do not define
// a special "not needed" value, but an empty region is the convention hosts
// use to skip fetching, decoding and rendering the upstream nodes of that
// input. Render never fetches those clips (see CutoutReadsSourceClip and
// TransitionClipsUsed), and it treats a missing image as transparent, so a
// host that ignores the empty RoI is still handled. Parameters are read at
// the action's time with the same readers HandleRender uses. If no clip
// gets a RoI, kOfxStatReplyDefault lets the host use its default.
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
  auto request_empty_clip = [&](OfxImageClipHandle clip, const char* roi_prop) {
    if (clip == nullptr) {
      return;
    }
    const double vals[4] = {0.0, 0.0, 0.0, 0.0};
    if (s.prop->propSetDoubleN(out_args, roi_prop, 4, vals) == kOfxStatOK) {
      set_any = true;
    }
  };
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

  // Picks the full or the empty request for one input.
  auto request_clip = [&](bool used, OfxImageClipHandle clip,
                          const char* roi_prop) {
    if (used) {
      request_full_clip(clip, roi_prop);
    } else {
      request_empty_clip(clip, roi_prop);
    }
  };

  if (role == PluginRole::kFoldTransition) {
    const PaperFoldTransitionParams tp =
        ReadFoldTransitionParams(s, *inst, time);
    const TransitionClipUse use =
        TransitionClipsUsed(tp, ComputeSteppedTransitionProgress(tp));
    request_clip(use.from, inst->source_from_clip,
                 "OfxImageClipPropRoI_SourceFrom");
    request_clip(use.to, inst->source_to_clip, "OfxImageClipPropRoI_SourceTo");
  } else {
    // Same reader and default as ReadCutoutParams().
    const int source_mode = ReadIntParam(s, inst->source_mode, time, 0);
    request_clip(CutoutReadsSourceClip(source_mode), inst->source_clip,
                 "OfxImageClipPropRoI_Source");
  }
  return set_any ? kOfxStatOK : kOfxStatReplyDefault;
}

// ---------------------------------------------------------------------------
// Identity, image access and render.
// ---------------------------------------------------------------------------

// kOfxImageEffectActionIsIdentity. Answers for the transition only; the cutout
// always returns kOfxStatReplyDefault (it changes every frame).
// The first and last transition frames are the outgoing and incoming clips
// unchanged, so the host can skip rendering them. For those frames this
// writes the clip to pass through (kOfxPropName) and the time into
// `out_args`. The check uses the stepped progress, so it matches what
// HandleRender would draw.
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

// Returns how the pixels in `img_handle` store alpha. Reads the image's
// kOfxImageEffectPropPreMultiplication, falls back to the clip's property,
// and assumes premultiplied when neither is set (the OpenFX default).
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

// Fills `out_view` (a non-owning kernel ImageView) from a host image: data
// pointer, pixel bounds, signed row bytes (negative for bottom-up storage),
// depth and alpha mode. `bounds` is the part of the image actually in memory;
// `rod` is the full frame it belongs to, which can be larger when the host
// delivers a cropped tile. A missing RoD falls back to the bounds.
// Returns false on failure. *out_wrong_format is set only when the image is
// not RGBA or has an unknown depth, so the caller can answer
// kOfxStatErrImageFormat instead of a generic failure.
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

// kOfxImageEffectActionRender: renders one frame (or one render window of
// it) at host time `time`.
//
// Steps: fetch the output image; use its RoD as the canvas (full-frame
// geometry) and the host's render window as the rows/columns to fill; read
// parameters; fetch inputs; call the kernel. All images are held by
// ScopedOfxImage guards, so they are released on every return path.
//
// Threading: the plugin declares itself fully thread safe, so the host may
// call this concurrently for different frames or windows of one instance.
// Each call only reads PluginInstance and keeps its state on the stack.
OfxStatus HandleRender(PluginRole role, OfxImageEffectHandle effect,
                       OfxPropertySetHandle in_args) {
  const HostSuites& s = MutableSuites(role);
  PluginInstance* inst = GetInstance(s, effect);
  if (inst == nullptr || in_args == nullptr || inst->output_clip == nullptr) {
    return kOfxStatFailed;
  }

  double time = 0.0;
  s.prop->propGetDouble(in_args, kOfxPropTime, 0, &time);

  // Render scale < 1 means a proxy/preview render at reduced resolution.
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

  // Default to the whole output buffer if the host sends no valid window.
  PixelRect render_window = out_view.bounds;
  int win_vals[4] = {0, 0, 0, 0};
  if (s.prop->propGetIntN(in_args, kOfxImageEffectPropRenderWindow, 4,
                          win_vals) == kOfxStatOK &&
      win_vals[2] > win_vals[0] && win_vals[3] > win_vals[1]) {
    render_window = {win_vals[0], win_vals[1], win_vals[2], win_vals[3]};
  }

  // ParallelRowsFn handed to the kernel: runs row_fn(y) for every y in
  // [y_start, y_end) and returns only when all rows are done. Uses the host's
  // multiThread() with ParallelThreadWorker when possible. Runs inline
  // instead when there is no thread suite, we are already on a thread the
  // host spawned (nested multiThread calls are not allowed), the job is
  // under 16 rows (thread start-up would cost more), or only one worker
  // would be used. If multiThread() fails (possibly after some workers ran),
  // only chunks not marked complete are run serially; row_fn only writes its
  // own output row, so redoing rows of a partly finished chunk is safe.
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
    ctx.y_start = y_start;
    ctx.y_end = y_end;
    // About 8 chunks per worker: small enough to balance uneven rows, large
    // enough to keep atomic contention low.
    ctx.chunk = std::max(1, total / static_cast<int>(workers * 8));
    const int num_chunks = (total + ctx.chunk - 1) / ctx.chunk;
    // make_unique<T[]> value-initializes, so every flag starts false.
    auto chunk_done = std::make_unique<std::atomic<bool>[]>(num_chunks);
    ctx.chunk_done = chunk_done.get();
    ctx.row_fn = &row_fn;
    if (s.thread->multiThread(ParallelThreadWorker, workers, &ctx) !=
        kOfxStatOK) {
      for (int c = 0; c < num_chunks; ++c) {
        if (chunk_done[c].load(std::memory_order_acquire)) {
          continue;
        }
        const int begin = y_start + c * ctx.chunk;
        const int end = std::min(y_end, begin + ctx.chunk);
        for (int y = begin; y < end; ++y) {
          row_fn(y);
        }
      }
    }
  };

  if (role == PluginRole::kFoldTransition) {
    const PaperFoldTransitionParams tp =
        ReadFoldTransitionParams(s, *inst, time);
    const float p = ComputeSteppedTransitionProgress(tp);
    // A missing image is the empty side of a clip edge, not an error.
    // Returns false only for a wrong pixel format. Applies the "Premultiplied
    // Source Alpha" override for clips that are really straight alpha.
    auto fetch = [&](OfxImageClipHandle clip, ScopedOfxImage* guard,
                     ImageView* view) {
      if (clip == nullptr ||
          s.effect->clipGetImage(clip, time, nullptr, &guard->handle) !=
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
    // Skip fetching a clip the frame does not read (the hidden side at
    // p == 0 / p == 1 or in either half of a forced Fold & Unfold); the RoI
    // action already told the host it is not needed.
    const TransitionClipUse use = TransitionClipsUsed(tp, p);
    if ((use.from && !fetch(inst->source_from_clip, &from_guard, &from_view)) ||
        (use.to && !fetch(inst->source_to_clip, &to_guard, &to_view))) {
      return kOfxStatErrImageFormat;
    }
    RenderPaperFoldTransition(tp, from_view, to_view, out_view, canvas_bounds,
                              render_window, parallel_for);
    return kOfxStatOK;
  }

  CutoutParams cp =
      ReadCutoutParams(s, *inst, time, render_scale[0], render_scale[1]);
  ScopedOfxImage src_guard(s.effect);
  // Stays an empty view when there is no source image, when the image is
  // unusable, or in sticker mode (see CutoutReadsSourceClip; its RoI was
  // empty); RenderCutoutFrame() accepts an empty source.
  ImageView src_view;
  if (inst->source_clip != nullptr && CutoutReadsSourceClip(cp.source_mode)) {
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
      } else {
        // ExtractImageView() may have filled some fields before failing.
        src_view = ImageView{};
      }
    }
  }
  RenderCutoutFrame(cp, src_view, out_view, canvas_bounds, render_window,
                    parallel_for);
  return kOfxStatOK;
}

// ---------------------------------------------------------------------------
// Action dispatch and exported plugin table.
// ---------------------------------------------------------------------------

// Routes one host action for plugin `role` to its Handle* function. `handle`
// is the effect handle for every action except Load/Unload (where it is
// nullptr). Actions this plugin has no work for but must acknowledge return
// kOfxStatOK; anything else returns kOfxStatReplyDefault so the host applies
// its default behavior.
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

// OfxPlugin::setHost callbacks. The host calls them before kOfxActionLoad to
// hand over its OfxHost (used to fetch suites); one per plugin role.
void SetHostCutout(OfxHost* host) {
  MutableSuites(PluginRole::kCutout).host = host;
}

void SetHostFoldTransition(OfxHost* host) {
  MutableSuites(PluginRole::kFoldTransition).host = host;
}

// OfxPlugin::mainEntry callbacks. mainEntry has no user-data argument, so a
// separate function per plugin is the only way to know which role an action
// is for.
OfxStatus MainEntryCutout(const char* action, const void* handle,
                          OfxPropertySetHandle in_args,
                          OfxPropertySetHandle out_args) {
  return DispatchAction(PluginRole::kCutout, action, handle, in_args, out_args);
}

OfxStatus MainEntryFoldTransition(const char* action, const void* handle,
                                  OfxPropertySetHandle in_args,
                                  OfxPropertySetHandle out_args) {
  return DispatchAction(PluginRole::kFoldTransition, action, handle, in_args,
                        out_args);
}

// Plugin table returned by OfxGetPlugin(); the array index is the `nth`
// argument. The identifiers ("com.papercraft.CutoutFX",
// "com.papercraft.PaperFoldTransition") are permanent: hosts save them in
// projects, so changing one makes every existing use show as missing.
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

// The two C entry points every OpenFX binary must export. Everything else is
// hidden by -fvisibility=hidden plus the exported-symbol list (macOS) or
// openfx_sym.lds (Linux); the tests check that internals are not visible.
extern "C" {

// Returns how many plugins this binary contains.
__attribute__((visibility("default"))) int OfxGetNumberOfPlugins(void) {
  return papercraft::ofx::kPluginCount;
}

// Returns plugin `nth` (0-based), or nullptr if `nth` is out of range.
__attribute__((visibility("default"))) OfxPlugin* OfxGetPlugin(int nth) {
  if (nth < 0 || nth >= papercraft::ofx::kPluginCount) {
    return nullptr;
  }
  return &papercraft::ofx::gPlugins[nth];
}

}  // extern "C"
