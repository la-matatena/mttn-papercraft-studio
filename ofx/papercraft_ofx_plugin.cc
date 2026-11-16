// MTTN PaperCraft Studio: OpenFX host glue and exported entry points.
//
// This file wraps the host-independent rendering kernel (papercraft_kernel.h)
// in a binary OpenFX bundle that DaVinci Resolve (or any OpenFX image-effect
// host) can load. One binary registers two plugins:
//
//   com.papercraft.CutoutFX            Filter context: torn-paper cutout,
//                                      chroma key, stop-motion jitter.
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
#include <iterator>
#include <memory>
#include <vector>

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
// the host and stay valid for the instance's lifetime. A parameter handle
// that stays nullptr (group entry, or the host did not return it) makes the
// readers use the table default. The struct is read-only after creation, so
// concurrent render calls can share it without locking.
struct PluginInstance {
  PluginRole role = PluginRole::kCutout;
  OfxImageClipHandle output_clip = nullptr;
  // Cutout filter only: the "Source" clip (optional in the filter context).
  OfxImageClipHandle source_clip = nullptr;
  // Fold transition only: outgoing ("SourceFrom") and incoming ("SourceTo").
  OfxImageClipHandle source_from_clip = nullptr;
  OfxImageClipHandle source_to_clip = nullptr;

  // One handle per entry of the role's parameter table (kCutoutParamSpecs or
  // kFoldTransitionParamSpecs), at the same index.
  std::vector<OfxParamHandle> params;
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
// Parameter tables.
//
// Every OpenFX parameter of both plugins is described exactly once, in
// kCutoutParamSpecs and kFoldTransitionParamSpecs below: persistent ID, type,
// label, parent group, hint, default, hard limits, slider range, choice
// options and the kernel struct field it feeds. HandleDescribeInContext()
// defines the parameters from the table (in table order, which is also the
// Inspector order), HandleCreateInstance() binds one handle per entry, and
// ReadCutoutParams() / ReadFoldTransitionParams() copy each value into the
// matching CutoutParams / PaperFoldTransitionParams field, falling back to
// the same default the host was given.
//
// IDs are persistent: the host saves keyframes and values in projects under
// them, so renaming an ID silently resets that control in existing projects.
// Labels, hints and group order may change freely. Parameters that exist in
// both plugins deliberately use the same ID (for example "borderWidth",
// "paperColor", "shadowAngle", "mirrorTransH"). Group labels start with a
// number so the Inspector order is obvious. "_norm" style values (border
// width, roughness, shadow distance/softness, jitter) are fractions of the
// frame size, so the look does not depend on resolution.
//
// Retired controls stay in the table as secret (hidden) entries without a
// kernel field: the host still defines them, so projects saved with them
// open without errors, but they are never bound or read (see
// ParamSpec::Secret()).
//
// Plugin defaults match the kernel struct defaults in papercraft_kernel.h;
// papercraft_ofx_test.cc checks that, and lists the intentional exceptions
// (only the transition's progress, which starts at 0 instead of 0.5).
// ---------------------------------------------------------------------------

// OpenFX parameter type of one table entry.
enum class ParamKind {
  kGroup,     // Collapsible group; not a value, has no kernel field.
  kBoolean,   // Checkbox -> bool field.
  kInteger,   // Integer -> int field.
  kChoice,    // Drop-down -> int field (option index).
  kDouble,    // Scalar double -> float field.
  kDouble2D,  // 2D position in normalized frame units -> two float fields.
  kRgb,       // RGB color, channels 0..1 -> three float fields.
};

// One parameter of a plugin, used both to define it at describe time and to
// read it at render time. `KernelParams` is the kernel struct the value is
// copied into (CutoutParams or PaperFoldTransitionParams). Build entries with
// the static factories, which set only the fields that matter for the kind.
template <typename KernelParams>
struct ParamSpec {
  ParamKind kind = ParamKind::kGroup;
  // Persistent parameter ID (see the section comment).
  const char* id = nullptr;
  const char* label = nullptr;
  // Parent group ID, or nullptr for top level.
  const char* parent = nullptr;
  // Tooltip, or nullptr.
  const char* hint = nullptr;
  // kOfxParamPropDoubleType for kDouble / kDouble2D (display only), or
  // nullptr to keep the host default.
  const char* double_type = nullptr;
  // Default per component (x/y for 2D, r/g/b for RGB). Booleans store 0/1,
  // groups store whether they start expanded.
  double def[3] = {0.0, 0.0, 0.0};
  // Hard limits enforced by the host, applied to every component.
  double min = 0.0;
  double max = 0.0;
  // Slider range only: users can still type values up to the hard limits.
  double display_min = 0.0;
  double display_max = 0.0;
  // Hidden legacy parameter (kOfxParamPropSecret = 1): defined so old
  // projects load, but top level, never bound, never read, and without a
  // kernel field. Set with Secret().
  bool secret = false;
  // kChoice entries. The host saves the selected index, not the label, so
  // new options must be appended at the end; reordering changes existing
  // projects. The index maps directly to kernel enums (FoldTransitionAction,
  // FoldPattern) or to CutoutParams::source_mode.
  const char* const* options = nullptr;
  int option_count = 0;
  // Kernel destination; exactly the one matching `kind` is set.
  bool KernelParams::* bool_field = nullptr;
  int KernelParams::* int_field = nullptr;
  float KernelParams::* float_fields[3] = {nullptr, nullptr, nullptr};

  // Collapsible group (a twirl-down section in Resolve's Inspector). Child
  // parameters join it by passing `id` as their `parent`; `open` sets
  // whether the group starts expanded.
  static constexpr ParamSpec Group(const char* id, const char* label,
                                   bool open) {
    ParamSpec spec;
    spec.kind = ParamKind::kGroup;
    spec.id = id;
    spec.label = label;
    spec.def[0] = open ? 1.0 : 0.0;
    return spec;
  }

  // Checkbox with default `def`.
  static constexpr ParamSpec Bool(const char* id, const char* label,
                                  const char* parent, const char* hint,
                                  bool def, bool KernelParams::* field) {
    ParamSpec spec = Common(ParamKind::kBoolean, id, label, parent, hint);
    spec.def[0] = def ? 1.0 : 0.0;
    spec.bool_field = field;
    return spec;
  }

  // Integer with default `def`, hard limits [min, max] and slider range
  // [dmin, dmax].
  static constexpr ParamSpec Int(const char* id, const char* label,
                                 const char* parent, const char* hint, int def,
                                 int min, int max, int dmin, int dmax,
                                 int KernelParams::* field) {
    ParamSpec spec = Common(ParamKind::kInteger, id, label, parent, hint);
    spec.def[0] = def;
    spec.min = min;
    spec.max = max;
    spec.display_min = dmin;
    spec.display_max = dmax;
    spec.int_field = field;
    return spec;
  }

  // Drop-down over `options` with default index `def`.
  template <std::size_t N>
  static constexpr ParamSpec Choice(const char* id, const char* label,
                                    const char* parent, const char* hint,
                                    int def, const char* const (&options)[N],
                                    int KernelParams::* field) {
    ParamSpec spec = Common(ParamKind::kChoice, id, label, parent, hint);
    spec.def[0] = def;
    spec.options = options;
    spec.option_count = static_cast<int>(N);
    spec.int_field = field;
    return spec;
  }

  // Scalar double. `double_type` (for example kOfxParamDoubleTypeAngle for
  // degrees or kOfxParamDoubleTypeScale) only changes how the host displays
  // the value. Limits work as in Int().
  static constexpr ParamSpec Double(const char* id, const char* label,
                                    const char* parent, const char* hint,
                                    const char* double_type, double def,
                                    double min, double max, double dmin,
                                    double dmax, float KernelParams::* field) {
    ParamSpec spec = Common(ParamKind::kDouble, id, label, parent, hint);
    spec.double_type = double_type;
    spec.def[0] = def;
    spec.min = min;
    spec.max = max;
    spec.display_min = dmin;
    spec.display_max = dmax;
    spec.float_fields[0] = field;
    return spec;
  }

  // 2D position in normalized frame units (0..1 = frame edges). It uses the
  // plain double type, not an OpenFX normalized/canonical XY type, so the
  // host passes the values through unchanged and the kernel does the mapping
  // to pixels. Limits apply to both axes.
  static constexpr ParamSpec Double2D(const char* id, const char* label,
                                      const char* parent, const char* hint,
                                      double def_x, double def_y, double min,
                                      double max, double dmin, double dmax,
                                      float KernelParams::* field_x,
                                      float KernelParams::* field_y) {
    ParamSpec spec = Common(ParamKind::kDouble2D, id, label, parent, hint);
    spec.double_type = kOfxParamDoubleTypePlain;
    spec.def[0] = def_x;
    spec.def[1] = def_y;
    spec.min = min;
    spec.max = max;
    spec.display_min = dmin;
    spec.display_max = dmax;
    spec.float_fields[0] = field_x;
    spec.float_fields[1] = field_y;
    return spec;
  }

  // RGB color picker with default (r, g, b); every channel is limited to
  // 0..1 (hard limits and slider range).
  static constexpr ParamSpec Rgb(const char* id, const char* label,
                                 const char* parent, const char* hint, double r,
                                 double g, double b,
                                 float KernelParams::* field_r,
                                 float KernelParams::* field_g,
                                 float KernelParams::* field_b) {
    ParamSpec spec = Common(ParamKind::kRgb, id, label, parent, hint);
    spec.def[0] = r;
    spec.def[1] = g;
    spec.def[2] = b;
    spec.min = 0.0;
    spec.max = 1.0;
    spec.display_min = 0.0;
    spec.display_max = 1.0;
    spec.float_fields[0] = field_r;
    spec.float_fields[1] = field_g;
    spec.float_fields[2] = field_b;
    return spec;
  }

  // Returns a copy of this value parameter marked secret, for retired
  // controls: build it with a nullptr parent and nullptr kernel field(s),
  // e.g. Bool("oldId", "Old Label", nullptr, hint, false, nullptr).Secret().
  // IsValidParamTable() enforces that shape.
  constexpr ParamSpec Secret() const {
    ParamSpec spec = *this;
    spec.secret = true;
    return spec;
  }

 private:
  // Fills the fields shared by every value parameter.
  static constexpr ParamSpec Common(ParamKind kind, const char* id,
                                    const char* label, const char* parent,
                                    const char* hint) {
    ParamSpec spec;
    spec.kind = kind;
    spec.id = id;
    spec.label = label;
    spec.parent = parent;
    spec.hint = hint;
    return spec;
  }
};

using CutoutSpec = ParamSpec<CutoutParams>;
using FoldSpec = ParamSpec<PaperFoldTransitionParams>;

// Choice options (index = saved value; append only, never reorder).
constexpr const char* kSourceModeOptions[] = {"Timeline Clip",
                                              "Built-in Paper Sticker"};
// Options of the retired, hidden Cutout introStyle / outroStyle choices.
// Kept so saved indices stay valid when old projects load.
constexpr const char* kAnimStyleOptions[] = {
    "None", "Paper Unfold (3D)", "Crumple Ball", "Paper Airplane Glide",
    "Flat Cutout Pop"};
constexpr const char* kFoldActionOptions[] = {
    "Auto (Unfold In / Fold Out)", "Unfold Incoming Clip", "Fold Outgoing Clip",
    "Fold Out, Then Unfold In"};
constexpr const char* kFoldPatternOptions[] = {"Craft Flaps", "Map Grid",
                                               "Accordion", "Origami Corners"};

// Paper Cutout FX controls in seven groups: media placement, green screen
// key, torn border, paper texture, drop shadow, stop-motion plus master
// transform, and a closed "Advanced" group with the technical controls
// (source mode and alpha, tear detail, texture scales, seed). The retired
// intro/outro controls follow as secret entries.
constexpr CutoutSpec kCutoutParamSpecs[] = {
    CutoutSpec::Group("MediaSourceGroup", "1. Media Source & Placement", true),
    CutoutSpec::Double("mediaScale", "Media Scale", "MediaSourceGroup",
                       "Scale of the subject inside frame.",
                       kOfxParamDoubleTypeScale, 0.90, 0.05, 5.0, 0.1, 2.5,
                       &CutoutParams::media_scale),
    CutoutSpec::Double2D(
        "mediaOffset", "Media Position", "MediaSourceGroup",
        "Normalized X/Y center placement of the subject.", 0.5, 0.5, -1.0, 2.0,
        0.0, 1.0, &CutoutParams::media_offset_x, &CutoutParams::media_offset_y),
    CutoutSpec::Double("mediaAngle", "Media Rotation", "MediaSourceGroup",
                       "Rotate the subject in degrees.",
                       kOfxParamDoubleTypeAngle, 0.0, -3600.0, 3600.0, -180.0,
                       180.0, &CutoutParams::media_angle_deg),

    CutoutSpec::Group("KeyerGroup", "2. Green Screen & Matte Cleanup", false),
    CutoutSpec::Bool("enableChromaKey", "Enable Green Screen Key", "KeyerGroup",
                     "Extract subject alpha from a solid backdrop.", false,
                     &CutoutParams::enable_chroma_key),
    CutoutSpec::Rgb("keyColor", "Screen Color", "KeyerGroup",
                    "Pick the backdrop color to remove.", 0.0, 1.0, 0.0,
                    &CutoutParams::key_r, &CutoutParams::key_g,
                    &CutoutParams::key_b),
    CutoutSpec::Double(
        "keyGain", "Key Gain", "KeyerGroup",
        "Matte separation strength against the keyed background.",
        kOfxParamDoubleTypePlain, 1.0, 0.0, 3.0, 0.5, 2.0,
        &CutoutParams::key_gain),
    CutoutSpec::Double("keyBalance", "Key Balance", "KeyerGroup",
                       "Soft matte threshold balance.",
                       kOfxParamDoubleTypePlain, 0.5, 0.0, 1.0, 0.0, 1.0,
                       &CutoutParams::key_balance),
    // Full-resolution pixels; ReadCutoutParams() applies the render scale.
    CutoutSpec::Double("edgeChokePx", "Edge Choke (px)", "KeyerGroup",
                       "Shrink (<0) or expand (>0) subject matte.",
                       kOfxParamDoubleTypePlain, 0.0, -32.0, 32.0, -12.0, 12.0,
                       &CutoutParams::edge_choke_px),

    CutoutSpec::Group("PaperBorderGroup", "3. Torn Paper Cutout Border", true),
    CutoutSpec::Bool("enableBorder", "Enable Paper Border", "PaperBorderGroup",
                     "Generate a torn cardstock silhouette around the "
                     "subject.",
                     true, &CutoutParams::enable_border),
    CutoutSpec::Double("borderWidth", "Border Width", "PaperBorderGroup",
                       "Cardstock collar width around matte.",
                       kOfxParamDoubleTypePlain, 0.018, 0.0, 0.15, 0.0, 0.06,
                       &CutoutParams::border_width_norm),
    CutoutSpec::Double(
        "tearRoughness", "Torn Edge Roughness", "PaperBorderGroup",
        "Amplitude of ragged torn paper fibers.", kOfxParamDoubleTypePlain,
        0.012, 0.0, 0.20, 0.0, 0.06, &CutoutParams::tear_roughness_norm),
    CutoutSpec::Double("tearFrequency", "Tear Frequency", "PaperBorderGroup",
                       "Spatial density of paper edge tears.",
                       kOfxParamDoubleTypePlain, 24.0, 1.0, 120.0, 2.0, 60.0,
                       &CutoutParams::tear_frequency),
    CutoutSpec::Double(
        "edgeBoil", "Edge Boil Speed", "PaperBorderGroup",
        "Subtle frame-by-frame edge variation on stop-motion steps.",
        kOfxParamDoubleTypePlain, 0.15, 0.0, 5.0, 0.0, 1.0,
        &CutoutParams::edge_boil),
    CutoutSpec::Rgb("paperColor", "Paper Cardstock Color", "PaperBorderGroup",
                    "Base tone of the torn cardstock collar.", 0.965, 0.945,
                    0.895, &CutoutParams::paper_r, &CutoutParams::paper_g,
                    &CutoutParams::paper_b),

    CutoutSpec::Group("TextureGroup", "4. Paper Grain & Fold Creases", false),
    CutoutSpec::Double("grainStrength", "Paper Grain Strength", "TextureGroup",
                       "Cardstock fiber grain depth.", kOfxParamDoubleTypePlain,
                       0.35, 0.0, 1.0, 0.0, 1.0, &CutoutParams::grain_strength),
    CutoutSpec::Double("creaseStrength", "Fold Crease Strength", "TextureGroup",
                       "Ridged fold crease contrast.", kOfxParamDoubleTypePlain,
                       0.40, 0.0, 1.0, 0.0, 1.0,
                       &CutoutParams::crease_strength),
    CutoutSpec::Double("mediaTextureOverlay", "Media Texture Overlay",
                       "TextureGroup",
                       "Subtle paper crease blending across the subject ink.",
                       kOfxParamDoubleTypePlain, 0.22, 0.0, 1.0, 0.0, 1.0,
                       &CutoutParams::media_texture_overlay),

    CutoutSpec::Group("ShadowGroup", "5. 3D Drop Shadow", false),
    CutoutSpec::Double("shadowOpacity", "Shadow Opacity", "ShadowGroup",
                       "Drop shadow density behind cutout.",
                       kOfxParamDoubleTypePlain, 0.62, 0.0, 1.0, 0.0, 1.0,
                       &CutoutParams::shadow_opacity),
    CutoutSpec::Double("shadowAngle", "Shadow Angle", "ShadowGroup",
                       "Shadow throw angle in degrees.",
                       kOfxParamDoubleTypeAngle, -45.0, -360.0, 360.0, -180.0,
                       180.0, &CutoutParams::shadow_angle_deg),
    CutoutSpec::Double("shadowDistance", "Shadow Distance", "ShadowGroup",
                       "Drop shadow offset distance.", kOfxParamDoubleTypePlain,
                       0.016, 0.0, 0.20, 0.0, 0.08,
                       &CutoutParams::shadow_distance_norm),
    CutoutSpec::Double("shadowSoftness", "Shadow Softness", "ShadowGroup",
                       "Feather falloff of the drop shadow.",
                       kOfxParamDoubleTypePlain, 0.018, 0.0, 0.20, 0.0, 0.08,
                       &CutoutParams::shadow_softness_norm),

    CutoutSpec::Group("StopMotionMasterGroup",
                      "6. Stop-Motion & Master Transform", true),
    CutoutSpec::Bool("enableStopMotion", "Enable Stop-Motion",
                     "StopMotionMasterGroup",
                     "Quantize motion into stepped stop-motion frame holds.",
                     true, &CutoutParams::enable_stop_motion),
    CutoutSpec::Int(
        "holdFrames", "Hold Frames (Step)", "StopMotionMasterGroup",
        "Frames held per stop-motion step (e.g. 6 for a 4 fps look at 24 fps).",
        6, 1, 30, 1, 12, &CutoutParams::hold_frames),
    CutoutSpec::Double("jitterPos", "Position Jitter", "StopMotionMasterGroup",
                       "Hand-placed stop-motion position jitter amplitude.",
                       kOfxParamDoubleTypePlain, 0.001, 0.0, 0.1, 0.0, 0.03,
                       &CutoutParams::jitter_pos_norm),
    CutoutSpec::Double("jitterRot", "Rotation Jitter", "StopMotionMasterGroup",
                       "Hand-placed stop-motion rotational wobble in degrees.",
                       kOfxParamDoubleTypeAngle, 0.3, 0.0, 30.0, 0.0, 8.0,
                       &CutoutParams::jitter_rot_deg),
    CutoutSpec::Double2D(
        "masterPos", "Master Position", "StopMotionMasterGroup",
        "Normalized X/Y position of finished paper cutout.", 0.5, 0.5, -1.0,
        2.0, 0.0, 1.0, &CutoutParams::master_pos_x,
        &CutoutParams::master_pos_y),
    CutoutSpec::Double("masterScale", "Master Scale", "StopMotionMasterGroup",
                       "Uniform scale of final cutout.",
                       kOfxParamDoubleTypeScale, 1.0, 0.01, 10.0, 0.05, 3.0,
                       &CutoutParams::master_scale),
    CutoutSpec::Double("masterAngle", "Master Rotation",
                       "StopMotionMasterGroup", "Final cutout rotation.",
                       kOfxParamDoubleTypeAngle, 0.0, -3600.0, 3600.0, -180.0,
                       180.0, &CutoutParams::master_angle_deg),
    CutoutSpec::Bool("flipH", "Flip Horizontal", "StopMotionMasterGroup",
                     "Mirror finished cutout left/right.", false,
                     &CutoutParams::flip_h),
    CutoutSpec::Bool("flipV", "Flip Vertical", "StopMotionMasterGroup",
                     "Mirror finished cutout top/bottom.", false,
                     &CutoutParams::flip_v),

    CutoutSpec::Group("AdvancedGroup", "7. Advanced", false),
    CutoutSpec::Choice("sourceMode", "Source Mode", "AdvancedGroup",
                       "Use the timeline clip or a procedural demo sticker.", 0,
                       kSourceModeOptions, &CutoutParams::source_mode),
    CutoutSpec::Bool(
        "sourcePremult", "Premultiplied Source Alpha", "AdvancedGroup",
        "Disable only if footage has straight/unpremultiplied alpha.", true,
        &CutoutParams::source_is_premultiplied),
    CutoutSpec::Int("tearDetail", "Tear Fiber Detail", "AdvancedGroup",
                    "Octaves of torn paper micro-fibers.", 5, 1, 8, 1, 8,
                    &CutoutParams::tear_detail),
    CutoutSpec::Double("grainScale", "Grain Scale", "AdvancedGroup",
                       "Scale of cardstock surface fibers.",
                       kOfxParamDoubleTypeScale, 1.0, 0.1, 10.0, 0.2, 3.0,
                       &CutoutParams::grain_scale),
    CutoutSpec::Double("creaseScale", "Crease Scale", "AdvancedGroup",
                       "Spatial size of folded paper creases.",
                       kOfxParamDoubleTypePlain, 3.2, 0.2, 30.0, 0.8, 12.0,
                       &CutoutParams::crease_scale),
    CutoutSpec::Int("randomSeed", "Texture Seed", "AdvancedGroup",
                    "Seed controlling tear, crease, and jitter variations.", 7,
                    0, 1000, 0, 100, &CutoutParams::random_seed),

    // Retired "Intro & Outro Paper Animations" controls: secret, top level,
    // never read (the cutout always renders at rest). IDs, options, and
    // ranges are unchanged so saved values still load.
    CutoutSpec::Choice("introStyle", "Intro Style", nullptr,
                       "Retired: intro animations were removed.", 0,
                       kAnimStyleOptions, nullptr)
        .Secret(),
    CutoutSpec::Int("introFrames", "Intro Duration (Frames)", nullptr,
                    "Retired: intro animations were removed.", 14, 0, 240, 0,
                    60, nullptr)
        .Secret(),
    CutoutSpec::Choice("outroStyle", "Outro Style", nullptr,
                       "Retired: outro animations were removed.", 0,
                       kAnimStyleOptions, nullptr)
        .Secret(),
    CutoutSpec::Int("outroFrames", "Outro Duration (Frames)", nullptr,
                    "Retired: outro animations were removed.", 14, 0, 240, 0,
                    60, nullptr)
        .Secret(),
    CutoutSpec::Double("transitionAngle", "Glide Angle", nullptr,
                       "Retired: intro/outro animations were removed.",
                       kOfxParamDoubleTypeAngle, -22.0, -360.0, 360.0, -180.0,
                       180.0, nullptr)
        .Secret(),
    CutoutSpec::Double("glideDistance", "Glide Distance", nullptr,
                       "Retired: intro/outro animations were removed.",
                       kOfxParamDoubleTypePlain, 0.65, 0.0, 3.0, 0.0, 1.5,
                       nullptr)
        .Secret(),
    CutoutSpec::Bool("mirrorTransH", "Mirror Animation H", nullptr,
                     "Retired: intro/outro animations were removed.", false,
                     nullptr)
        .Secret(),
    CutoutSpec::Bool("mirrorTransV", "Mirror Animation V", nullptr,
                     "Retired: intro/outro animations were removed.", false,
                     nullptr)
        .Secret(),
};

// Paper Fold & Unfold controls: the mandatory transition progress parameter
// plus three groups (fold behavior, paper look, stop-motion and shadow).
// Shared IDs reuse the Cutout names, but some defaults differ (for example
// grainStrength 0.38 vs 0.35), following PaperFoldTransitionParams.
constexpr FoldSpec kFoldTransitionParamSpecs[] = {
    // Mandatory OpenFX transition progress, animated 0 -> 1 by the host.
    FoldSpec::Double(kOfxImageEffectTransitionParamName, "Progress", nullptr,
                     "Transition progress driven by the timeline (0 to 1).",
                     kOfxParamDoubleTypePlain, 0.0, 0.0, 1.0, 0.0, 1.0,
                     &PaperFoldTransitionParams::progress),

    FoldSpec::Group("FoldGroup", "1. Fold & Unfold", true),
    FoldSpec::Choice(
        "foldAction", "Fold Action", "FoldGroup",
        "Auto unfolds at the start of a clip and folds at its end.", 0,
        kFoldActionOptions, &PaperFoldTransitionParams::fold_action),
    FoldSpec::Choice("foldPattern", "Fold Pattern", "FoldGroup",
                     "How the paper is folded into a small packet.", 0,
                     kFoldPatternOptions,
                     &PaperFoldTransitionParams::fold_pattern),
    FoldSpec::Int("foldStages", "Fold Stages", "FoldGroup",
                  "Folds that open one after another.", 4, 2, 5, 2, 5,
                  &PaperFoldTransitionParams::fold_stages),
    FoldSpec::Double("foldDirection", "Fold Direction", "FoldGroup",
                     "Rotates every fold line, in degrees.",
                     kOfxParamDoubleTypeAngle, 0.0, -360.0, 360.0, -180.0,
                     180.0, &PaperFoldTransitionParams::direction_angle_deg),
    FoldSpec::Double("foldDepth", "3D Depth", "FoldGroup",
                     "Perspective of the lifted flaps (0 = flat).",
                     kOfxParamDoubleTypePlain, 0.55, 0.0, 2.0, 0.0, 1.5,
                     &PaperFoldTransitionParams::fold_depth),
    FoldSpec::Double(
        "slideDistance", "Packet Slide Distance", "FoldGroup",
        "How far the folded packet slides in or out (0 = stays in place).",
        kOfxParamDoubleTypePlain, 0.6, 0.0, 2.0, 0.0, 1.5,
        &PaperFoldTransitionParams::slide_distance),
    FoldSpec::Bool("mirrorTransH", "Mirror Horizontal", "FoldGroup",
                   "Mirror the fold layout left/right.", false,
                   &PaperFoldTransitionParams::mirror_h),
    FoldSpec::Bool("mirrorTransV", "Mirror Vertical", "FoldGroup",
                   "Mirror the fold layout top/bottom.", false,
                   &PaperFoldTransitionParams::mirror_v),

    FoldSpec::Group("PaperGroup", "2. Paper Look", true),
    FoldSpec::Bool("enableBorder", "Torn Paper Border", "PaperGroup",
                   "Torn cardstock collar shown while the sheet is folded.",
                   true, &PaperFoldTransitionParams::enable_border),
    FoldSpec::Double("borderWidth", "Border Width", "PaperGroup",
                     "Cardstock collar width around the clip.",
                     kOfxParamDoubleTypePlain, 0.018, 0.0, 0.15, 0.0, 0.06,
                     &PaperFoldTransitionParams::border_width_norm),
    FoldSpec::Double("tearRoughness", "Torn Edge Roughness", "PaperGroup",
                     "Amplitude of the ragged paper fibers.",
                     kOfxParamDoubleTypePlain, 0.012, 0.0, 0.20, 0.0, 0.06,
                     &PaperFoldTransitionParams::tear_roughness_norm),
    FoldSpec::Double("tearFrequency", "Tear Frequency", "PaperGroup",
                     "Spatial density of the paper tears.",
                     kOfxParamDoubleTypePlain, 24.0, 1.0, 120.0, 2.0, 60.0,
                     &PaperFoldTransitionParams::tear_frequency),
    FoldSpec::Rgb("paperColor", "Paper Color", "PaperGroup",
                  "Color of the paper back and collar.", 0.965, 0.945, 0.895,
                  &PaperFoldTransitionParams::paper_r,
                  &PaperFoldTransitionParams::paper_g,
                  &PaperFoldTransitionParams::paper_b),
    FoldSpec::Double("grainStrength", "Paper Grain", "PaperGroup",
                     "Cardstock fiber grain depth.", kOfxParamDoubleTypePlain,
                     0.38, 0.0, 1.0, 0.0, 1.0,
                     &PaperFoldTransitionParams::grain_strength),
    FoldSpec::Double("creaseStrength", "Crease Strength", "PaperGroup",
                     "Crumple texture and fold-line creases.",
                     kOfxParamDoubleTypePlain, 0.52, 0.0, 1.0, 0.0, 1.0,
                     &PaperFoldTransitionParams::crease_strength),
    FoldSpec::Int("randomSeed", "Paper Seed", "PaperGroup",
                  "Varies tears, creases, jitter, and fold angles.", 7, 0, 1000,
                  0, 100, &PaperFoldTransitionParams::random_seed),
    FoldSpec::Bool("sourcePremult", "Premultiplied Source Alpha", "PaperGroup",
                   "Disable only if the clips have straight alpha.", true,
                   &PaperFoldTransitionParams::source_is_premultiplied),

    FoldSpec::Group("MotionGroup", "3. Stop-Motion & Shadow", true),
    FoldSpec::Bool("enableStopMotion", "Enable Stop-Motion", "MotionGroup",
                   "Hold each pose like a stop-motion camera.", true,
                   &PaperFoldTransitionParams::enable_stop_motion),
    FoldSpec::Int("stopMotionSteps", "Stop-Motion Steps", "MotionGroup",
                  "Distinct poses across the transition.", 10, 2, 60, 4, 30,
                  &PaperFoldTransitionParams::stop_motion_steps),
    FoldSpec::Double("jitterAmount", "Hand Jitter", "MotionGroup",
                     "Small per-step position and rotation wobble.",
                     kOfxParamDoubleTypePlain, 0.25, 0.0, 1.0, 0.0, 1.0,
                     &PaperFoldTransitionParams::jitter_amount),
    FoldSpec::Double("shadowOpacity", "Shadow Opacity", "MotionGroup",
                     "Drop shadow and flap shadow density.",
                     kOfxParamDoubleTypePlain, 0.62, 0.0, 1.0, 0.0, 1.0,
                     &PaperFoldTransitionParams::shadow_opacity),
    FoldSpec::Double("shadowAngle", "Shadow Angle", "MotionGroup",
                     "Shadow direction; also sets the light on the flaps.",
                     kOfxParamDoubleTypeAngle, -45.0, -360.0, 360.0, -180.0,
                     180.0, &PaperFoldTransitionParams::shadow_angle_deg),
    FoldSpec::Double("shadowDistance", "Shadow Distance", "MotionGroup",
                     "Drop shadow offset.", kOfxParamDoubleTypePlain, 0.014,
                     0.0, 0.20, 0.0, 0.08,
                     &PaperFoldTransitionParams::shadow_distance_norm),
    FoldSpec::Double("shadowSoftness", "Shadow Softness", "MotionGroup",
                     "Feather of the drop shadow.", kOfxParamDoubleTypePlain,
                     0.020, 0.0, 0.20, 0.0, 0.08,
                     &PaperFoldTransitionParams::shadow_softness_norm),
};

// Compile-time C string equality (std::strcmp is not constexpr).
constexpr bool ConstexprStrEq(const char* a, const char* b) {
  while (*a != '\0' && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

// Returns the index of parameter `id` in `specs`, or N when it is missing.
template <typename P, std::size_t N>
constexpr std::size_t FindParamIndex(const ParamSpec<P> (&specs)[N],
                                     const char* id) {
  for (std::size_t i = 0; i < N; ++i) {
    if (ConstexprStrEq(specs[i].id, id)) {
      return i;
    }
  }
  return N;
}

// Compile-time sanity check of one table: every entry has an ID and a label,
// IDs are unique, every parent is a group defined earlier in the table, each
// visible value parameter has exactly the kernel field(s) its kind needs,
// secret (legacy) entries are top-level values with no kernel field,
// choices have options, and every default lies inside the hard limits and
// slider range.
template <typename P, std::size_t N>
constexpr bool IsValidParamTable(const ParamSpec<P> (&specs)[N]) {
  for (std::size_t i = 0; i < N; ++i) {
    const ParamSpec<P>& spec = specs[i];
    if (spec.id == nullptr || spec.label == nullptr ||
        FindParamIndex(specs, spec.id) != i) {
      return false;
    }
    if (spec.parent != nullptr) {
      const std::size_t parent = FindParamIndex(specs, spec.parent);
      if (parent >= i || specs[parent].kind != ParamKind::kGroup) {
        return false;
      }
    }
    const bool has_field =
        spec.bool_field != nullptr || spec.int_field != nullptr ||
        spec.float_fields[0] != nullptr || spec.float_fields[1] != nullptr ||
        spec.float_fields[2] != nullptr;
    // Secret entries are only ever defined, never read, so a kernel field
    // would be dead (and a parent group would show them again).
    if (spec.secret && (spec.kind == ParamKind::kGroup ||
                        spec.parent != nullptr || has_field)) {
      return false;
    }
    int components = 0;
    switch (spec.kind) {
      case ParamKind::kGroup:
        if (has_field) {
          return false;
        }
        continue;
      case ParamKind::kBoolean:
        if (!spec.secret && spec.bool_field == nullptr) {
          return false;
        }
        continue;
      case ParamKind::kChoice:
        if ((!spec.secret && spec.int_field == nullptr) ||
            spec.option_count <= 0 || spec.def[0] < 0.0 ||
            spec.def[0] >= spec.option_count) {
          return false;
        }
        continue;
      case ParamKind::kInteger:
        if (!spec.secret && spec.int_field == nullptr) {
          return false;
        }
        components = 1;
        break;
      case ParamKind::kDouble:
        components = 1;
        break;
      case ParamKind::kDouble2D:
        components = 2;
        break;
      case ParamKind::kRgb:
        components = 3;
        break;
    }
    for (int c = 0; c < components; ++c) {
      const bool needs_float = !spec.secret && spec.kind != ParamKind::kInteger;
      if (needs_float != (spec.float_fields[c] != nullptr) ||
          spec.def[c] < spec.min || spec.def[c] > spec.max ||
          spec.def[c] < spec.display_min || spec.def[c] > spec.display_max) {
        return false;
      }
    }
  }
  return true;
}

static_assert(IsValidParamTable(kCutoutParamSpecs),
              "kCutoutParamSpecs has an invalid entry");
static_assert(IsValidParamTable(kFoldTransitionParamSpecs),
              "kFoldTransitionParamSpecs has an invalid entry");

// Index of "sourceMode", which HandleGetRegionsOfInterest() reads on its own.
constexpr std::size_t kCutoutSourceModeIndex =
    FindParamIndex(kCutoutParamSpecs, "sourceMode");
static_assert(kCutoutSourceModeIndex < std::size(kCutoutParamSpecs),
              "sourceMode is missing from kCutoutParamSpecs");

// ---------------------------------------------------------------------------
// Parameter definition.
//
// Called only from HandleDescribeInContext(). If the host rejects a
// definition the parameter is skipped; the instance then gets a nullptr
// handle and the readers use the table default.
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

// Sets the default, hard limits and slider range of a 2D or RGB parameter
// (`n` components). Limits are the same for every component.
template <typename P>
void SetDoubleNProps(const HostSuites& s, OfxPropertySetHandle props,
                     const ParamSpec<P>& spec, int n) {
  const double mins[3] = {spec.min, spec.min, spec.min};
  const double maxs[3] = {spec.max, spec.max, spec.max};
  const double dmins[3] = {spec.display_min, spec.display_min,
                           spec.display_min};
  const double dmaxs[3] = {spec.display_max, spec.display_max,
                           spec.display_max};
  s.prop->propSetDoubleN(props, kOfxParamPropDefault, n, spec.def);
  s.prop->propSetDoubleN(props, kOfxParamPropMin, n, mins);
  s.prop->propSetDoubleN(props, kOfxParamPropMax, n, maxs);
  s.prop->propSetDoubleN(props, kOfxParamPropDisplayMin, n, dmins);
  s.prop->propSetDoubleN(props, kOfxParamPropDisplayMax, n, dmaxs);
}

// Defines the OpenFX parameter described by `spec` in `param_set`.
template <typename P>
void DefineParamFromSpec(const HostSuites& s, OfxParamSetHandle param_set,
                         const ParamSpec<P>& spec) {
  const char* param_type = kOfxParamTypeGroup;
  switch (spec.kind) {
    case ParamKind::kGroup:
      param_type = kOfxParamTypeGroup;
      break;
    case ParamKind::kBoolean:
      param_type = kOfxParamTypeBoolean;
      break;
    case ParamKind::kInteger:
      param_type = kOfxParamTypeInteger;
      break;
    case ParamKind::kChoice:
      param_type = kOfxParamTypeChoice;
      break;
    case ParamKind::kDouble:
      param_type = kOfxParamTypeDouble;
      break;
    case ParamKind::kDouble2D:
      param_type = kOfxParamTypeDouble2D;
      break;
    case ParamKind::kRgb:
      param_type = kOfxParamTypeRGB;
      break;
  }
  OfxPropertySetHandle props = DefineParamCommon(
      s, param_set, param_type, spec.id, spec.label, spec.parent, spec.hint);
  if (props == nullptr) {
    return;
  }
  if (spec.secret) {
    s.prop->propSetInt(props, kOfxParamPropSecret, 0, 1);
  }
  switch (spec.kind) {
    case ParamKind::kGroup:
      s.prop->propSetInt(props, kOfxParamPropGroupOpen, 0,
                         spec.def[0] != 0.0 ? 1 : 0);
      break;
    case ParamKind::kBoolean:
      s.prop->propSetInt(props, kOfxParamPropDefault, 0,
                         spec.def[0] != 0.0 ? 1 : 0);
      break;
    case ParamKind::kInteger:
      s.prop->propSetInt(props, kOfxParamPropDefault, 0,
                         static_cast<int>(spec.def[0]));
      s.prop->propSetInt(props, kOfxParamPropMin, 0,
                         static_cast<int>(spec.min));
      s.prop->propSetInt(props, kOfxParamPropMax, 0,
                         static_cast<int>(spec.max));
      s.prop->propSetInt(props, kOfxParamPropDisplayMin, 0,
                         static_cast<int>(spec.display_min));
      s.prop->propSetInt(props, kOfxParamPropDisplayMax, 0,
                         static_cast<int>(spec.display_max));
      break;
    case ParamKind::kChoice:
      s.prop->propSetInt(props, kOfxParamPropDefault, 0,
                         static_cast<int>(spec.def[0]));
      s.prop->propSetStringN(props, kOfxParamPropChoiceOption,
                             spec.option_count, spec.options);
      break;
    case ParamKind::kDouble:
      if (spec.double_type != nullptr) {
        s.prop->propSetString(props, kOfxParamPropDoubleType, 0,
                              spec.double_type);
      }
      s.prop->propSetDouble(props, kOfxParamPropDefault, 0, spec.def[0]);
      s.prop->propSetDouble(props, kOfxParamPropMin, 0, spec.min);
      s.prop->propSetDouble(props, kOfxParamPropMax, 0, spec.max);
      s.prop->propSetDouble(props, kOfxParamPropDisplayMin, 0,
                            spec.display_min);
      s.prop->propSetDouble(props, kOfxParamPropDisplayMax, 0,
                            spec.display_max);
      break;
    case ParamKind::kDouble2D:
      s.prop->propSetString(props, kOfxParamPropDoubleType, 0,
                            spec.double_type);
      SetDoubleNProps(s, props, spec, 2);
      break;
    case ParamKind::kRgb:
      SetDoubleNProps(s, props, spec, 3);
      break;
  }
}

// Defines every parameter of `specs` in table order (= Inspector order).
template <typename P, std::size_t N>
void DefineParamTable(const HostSuites& s, OfxParamSetHandle param_set,
                      const ParamSpec<P> (&specs)[N]) {
  for (const ParamSpec<P>& spec : specs) {
    DefineParamFromSpec(s, param_set, spec);
  }
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
            "green screen key, 3D drop shadow, and stop-motion jitter.";
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
    DefineParamTable(s, param_set, kFoldTransitionParamSpecs);
  } else {
    DefineParamTable(s, param_set, kCutoutParamSpecs);
  }
  return kOfxStatOK;
}

// ---------------------------------------------------------------------------
// Instance lifetime.
// ---------------------------------------------------------------------------

// Looks up one handle per entry of `specs` by its persistent ID and stores it
// at the same index of `*handles`. Groups and secret (legacy) entries are
// skipped, so their handles stay nullptr. Errors are ignored on purpose: a
// missing parameter leaves its handle nullptr, which the readers treat as
// "use the table default".
template <typename P, std::size_t N>
void BindParamTable(const HostSuites& s, OfxParamSetHandle param_set,
                    const ParamSpec<P> (&specs)[N],
                    std::vector<OfxParamHandle>* handles) {
  handles->assign(N, nullptr);
  for (std::size_t i = 0; i < N; ++i) {
    if (specs[i].kind != ParamKind::kGroup && !specs[i].secret) {
      s.param->paramGetHandle(param_set, specs[i].id, &(*handles)[i], nullptr);
    }
  }
}

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
  if (role == PluginRole::kFoldTransition) {
    s.effect->clipGetHandle(effect, kOfxImageEffectTransitionSourceFromClipName,
                            &inst->source_from_clip, nullptr);
    s.effect->clipGetHandle(effect, kOfxImageEffectTransitionSourceToClipName,
                            &inst->source_to_clip, nullptr);
    BindParamTable(s, param_set, kFoldTransitionParamSpecs, &inst->params);
  } else {
    s.effect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName,
                            &inst->source_clip, nullptr);
    BindParamTable(s, param_set, kCutoutParamSpecs, &inst->params);
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
// for the position parameters described by ParamSpec::Double2D()).
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

// Reads every visible value parameter of `specs` at `time` into the matching
// field of `*out`; secret (legacy) entries have no field and are skipped, so
// their saved values never reach the kernel. `handles` holds one handle per
// table entry (see BindParamTable()); a nullptr or missing handle yields the
// table default, narrowed to the kernel field's type exactly as the host
// default is.
template <typename P, std::size_t N>
void ReadParamTable(const HostSuites& s, const ParamSpec<P> (&specs)[N],
                    const std::vector<OfxParamHandle>& handles, OfxTime time,
                    P* out) {
  for (std::size_t i = 0; i < N; ++i) {
    const ParamSpec<P>& spec = specs[i];
    if (spec.secret) {
      continue;
    }
    const OfxParamHandle param = i < handles.size() ? handles[i] : nullptr;
    switch (spec.kind) {
      case ParamKind::kGroup:
        break;
      case ParamKind::kBoolean:
        out->*spec.bool_field =
            ReadBoolParam(s, param, time, spec.def[0] != 0.0);
        break;
      case ParamKind::kInteger:
      case ParamKind::kChoice:
        out->*spec.int_field =
            ReadIntParam(s, param, time, static_cast<int>(spec.def[0]));
        break;
      case ParamKind::kDouble:
        out->*spec.float_fields[0] =
            ReadFloatParam(s, param, time, static_cast<float>(spec.def[0]));
        break;
      case ParamKind::kDouble2D:
        ReadDouble2DParam(s, param, time, &(out->*spec.float_fields[0]),
                          &(out->*spec.float_fields[1]),
                          static_cast<float>(spec.def[0]),
                          static_cast<float>(spec.def[1]));
        break;
      case ParamKind::kRgb:
        ReadRgbParam(
            s, param, time, &(out->*spec.float_fields[0]),
            &(out->*spec.float_fields[1]), &(out->*spec.float_fields[2]),
            static_cast<float>(spec.def[0]), static_cast<float>(spec.def[1]),
            static_cast<float>(spec.def[2]));
        break;
    }
  }
}

// Reads the Cutout "sourceMode" choice alone, with the same handle and
// default ReadCutoutParams() uses (for HandleGetRegionsOfInterest()).
int ReadCutoutSourceMode(const HostSuites& s, const PluginInstance& inst,
                         OfxTime time) {
  const OfxParamHandle param = kCutoutSourceModeIndex < inst.params.size()
                                   ? inst.params[kCutoutSourceModeIndex]
                                   : nullptr;
  return ReadIntParam(
      s, param, time,
      static_cast<int>(kCutoutParamSpecs[kCutoutSourceModeIndex].def[0]));
}

// Builds the kernel's CutoutParams for frame `time` from kCutoutParamSpecs.
// `render_scale_x` and `render_scale_y` are the host's proxy scale per axis
// (1.0 = full resolution, 0.5 = half-res preview); they convert pixel-unit
// parameters so the look matches across preview and final renders. Also
// reads the clip's frame range, which the kernel needs to time stop-motion
// steps.
CutoutParams ReadCutoutParams(const HostSuites& s, const PluginInstance& inst,
                              OfxTime time, double render_scale_x,
                              double render_scale_y) {
  CutoutParams cp;
  ReadParamTable(s, kCutoutParamSpecs, inst.params, time, &cp);
  // Edge choke is entered in full-resolution pixels; scale it to the render
  // resolution. The choke is a single isotropic radius, so with a non-uniform
  // render scale (X != Y) use the geometric mean of both axes: it keeps the
  // eroded area proportional to the proxy's pixel area and equals the common
  // scale when X == Y. Each axis gets a 0.05 floor first, which guards
  // against a zero, negative or NaN scale (std::max returns 0.05 for NaN).
  const double choke_scale = std::sqrt(std::max(0.05, render_scale_x) *
                                       std::max(0.05, render_scale_y));
  cp.edge_choke_px *= static_cast<float>(choke_scale);

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

// Builds the kernel's PaperFoldTransitionParams for frame `time` from
// kFoldTransitionParamSpecs. `progress` is the host-animated 0..1
// transition parameter; stop-motion quantization of it happens later in
// ComputeSteppedTransitionProgress().
PaperFoldTransitionParams ReadFoldTransitionParams(const HostSuites& s,
                                                   const PluginInstance& inst,
                                                   OfxTime time) {
  PaperFoldTransitionParams tp;
  ReadParamTable(s, kFoldTransitionParamSpecs, inst.params, time, &tp);
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
    const int source_mode = ReadCutoutSourceMode(s, *inst, time);
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
