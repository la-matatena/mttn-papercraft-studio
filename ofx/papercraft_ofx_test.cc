// Integration tests for the PaperCraft OpenFX binary (papercraft_ofx_plugin.cc
// plus papercraft_kernel.cc), driven the same way a host such as DaVinci
// Resolve drives it.
//
// Approach: the tests do not link the plugin sources. The PaperCraftOfxTest
// fixture dlopen()s the built PaperCraftStudioPro.ofx (see
// ResolvePluginBinaryPath()) and builds a fresh MockHost for every test;
// tests reach the plugin only through the exported OfxGetNumberOfPlugins() /
// OfxGetPlugin() entry points. That way the real build flags, symbol
// visibility and the full action sequence (setHost, Load, Describe,
// DescribeInContext, CreateInstance, GetClipPreferences,
// GetRegionsOfInterest, IsIdentity, Render, DestroyInstance, Unload) are all
// under test.
//
// The mock host implements the four suites the plugin fetches:
//   - Property suite: MockPropertySet, a mutex-guarded map from property name
//     to a typed value vector (int, double, string or pointer).
//   - Parameter suite: MockParamSet / MockParam. Values are not animated
//     (GetValueAtTime ignores time); tests set them with SetIntParam() /
//     SetDoubleParam(), which fail the test on an unknown ID or wrong type.
//   - Image effect suite: MockEffectObject (a descriptor or an instance) with
//     MockClip entries. A clip returns the image property set bound to it by
//     ConfigureClipAndImage(), and MockHost counts leased images so tests can
//     assert that every clipGetImage() was released. Each clip also counts
//     clipGetImage() calls, and RequestRegionsOfInterest() drives the RoI
//     action with host-style default RoIs.
//   - Multithread suite: real pthreads (at most 4) with thread-local
//     "spawned" flags, so the plugin's parallel row dispatch runs truly
//     concurrently.
// Mock OpenFX handles are reinterpret_cast pointers to these structs.
// CloneDescriptorToInstance() mimics the host creating an instance from a
// described context.
//
// papercraft_kernel.h is included only for the default member values of
// CutoutParams / PaperFoldTransitionParams, which the parameter-default
// drift test compares against what the plugin describes; no kernel code is
// linked.
//
// Run with Bazel (`papercraft_ofx_test` in BUILD) or as in CI: build the
// bundle, compile this file against gtest, and set PAPERCRAFT_OFX_PATH.

#include <dlfcn.h>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#if defined(PAPERCRAFT_STANDALONE_OFX)
#include <gtest/gtest.h>

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
#include "testing/base/public/gunit.h"
#endif

namespace papercraft {
namespace ofx_test {
namespace {

// ---------------------------------------------------------------------------
// Locating the plugin binary.
// ---------------------------------------------------------------------------

// Signatures of the two C entry points exported by the plugin.
using GetNumPluginsFn = int (*)();
using GetPluginFn = OfxPlugin* (*)(int);

// Returns the path of the plugin binary to dlopen: $PAPERCRAFT_OFX_PATH if
// set, else the first existing candidate under Bazel's $TEST_SRCDIR runfiles
// or the working directory. If none exists, returns the first candidate so
// the dlopen failure message names a meaningful path.
std::string ResolvePluginBinaryPath() {
  if (const char* explicit_path = std::getenv("PAPERCRAFT_OFX_PATH");
      explicit_path != nullptr && explicit_path[0] != '\0') {
    return explicit_path;
  }
  const char* test_srcdir = std::getenv("TEST_SRCDIR");
  const std::string base = test_srcdir != nullptr ? test_srcdir : "";
  const std::vector<std::string> candidates = {
      base +
          "/google3/experimental/users/joseluiszapata/davinci_resolve_plugins/"
          "papercraft_pro/PaperCraftStudioPro.ofx",
      base +
          "/experimental/users/joseluiszapata/davinci_resolve_plugins/"
          "papercraft_pro/PaperCraftStudioPro.ofx",
      base + "/papercraft_pro/PaperCraftStudioPro.ofx",
      "./PaperCraftStudioPro.ofx",
  };
  for (const auto& p : candidates) {
    if (FILE* f = std::fopen(p.c_str(), "rb")) {
      std::fclose(f);
      return p;
    }
  }
  return candidates.front();
}

// ---------------------------------------------------------------------------
// Mock property suite.
// ---------------------------------------------------------------------------

// Minimal non-copyable pthread mutex wrapper used by the mock host.
class PthreadMutex {
 public:
  PthreadMutex() { pthread_mutex_init(&mu_, nullptr); }
  ~PthreadMutex() { pthread_mutex_destroy(&mu_); }
  PthreadMutex(const PthreadMutex&) = delete;
  PthreadMutex& operator=(const PthreadMutex&) = delete;

  void Lock() { pthread_mutex_lock(&mu_); }
  void Unlock() { pthread_mutex_unlock(&mu_); }

 private:
  pthread_mutex_t mu_;
};

// RAII lock for PthreadMutex.
class ScopedMutexLock {
 public:
  explicit ScopedMutexLock(PthreadMutex& mu) : mu_(mu) { mu_.Lock(); }
  ~ScopedMutexLock() { mu_.Unlock(); }
  ScopedMutexLock(const ScopedMutexLock&) = delete;
  ScopedMutexLock& operator=(const ScopedMutexLock&) = delete;

 private:
  PthreadMutex& mu_;
};

// One OpenFX property value. Only the vector matching `kind` is meaningful;
// a property keeps the type of its last write, like a real host's.
struct PropValue {
  enum class Kind { kUnset, kInt, kDouble, kString, kPointer };
  Kind kind = Kind::kUnset;
  std::vector<int> ints;
  std::vector<double> doubles;
  std::vector<std::string> strings;
  std::vector<void*> ptrs;
};

// Property set behind every mock OfxPropertySetHandle. Guarded by `mu`
// because the plugin's render workers may read properties concurrently.
struct MockPropertySet {
  PthreadMutex mu;
  std::map<std::string, PropValue> map;
};

// Converts between opaque OpenFX property handles and MockPropertySet.
MockPropertySet* AsProps(OfxPropertySetHandle h) {
  return reinterpret_cast<MockPropertySet*>(h);
}

OfxPropertySetHandle ToPropHandle(MockPropertySet* p) {
  return reinterpret_cast<OfxPropertySetHandle>(p);
}

// Replaces the whole value of property `name` with `count` items of type T.
// `Field` selects the PropValue vector that stores T.
template <typename T, PropValue::Kind K, std::vector<T> PropValue::* Field>
OfxStatus SetPropN(OfxPropertySetHandle h, const char* name, int count,
                   const T* vals) {
  if (h == nullptr || name == nullptr || count < 0) {
    return kOfxStatErrBadHandle;
  }
  auto* ps = AsProps(h);
  ScopedMutexLock lock(ps->mu);
  PropValue& pv = ps->map[name];
  pv.kind = K;
  auto& vec = pv.*Field;
  vec.assign(vals, vals + count);
  return kOfxStatOK;
}

// Sets element `index` of property `name`, growing the vector as needed.
template <typename T, PropValue::Kind K, std::vector<T> PropValue::* Field>
OfxStatus SetPropOne(OfxPropertySetHandle h, const char* name, int index,
                     const T& val) {
  if (h == nullptr || name == nullptr || index < 0) {
    return kOfxStatErrBadHandle;
  }
  auto* ps = AsProps(h);
  ScopedMutexLock lock(ps->mu);
  PropValue& pv = ps->map[name];
  pv.kind = K;
  auto& vec = pv.*Field;
  if (static_cast<int>(vec.size()) <= index) {
    vec.resize(index + 1);
  }
  vec[index] = val;
  return kOfxStatOK;
}

// Copies the first `count` items of property `name` into `out`. Fails with
// kOfxStatErrUnknown if the property is missing or has another type, which
// is how the plugin's fallback paths get exercised.
template <typename T, PropValue::Kind K, std::vector<T> PropValue::* Field>
OfxStatus GetPropN(OfxPropertySetHandle h, const char* name, int count,
                   T* out) {
  if (h == nullptr || name == nullptr || out == nullptr || count < 0) {
    return kOfxStatErrBadHandle;
  }
  auto* ps = AsProps(h);
  ScopedMutexLock lock(ps->mu);
  auto it = ps->map.find(name);
  if (it == ps->map.end() || it->second.kind != K) {
    return kOfxStatErrUnknown;
  }
  const auto& vec = it->second.*Field;
  if (static_cast<int>(vec.size()) < count) {
    return kOfxStatErrBadIndex;
  }
  for (int i = 0; i < count; ++i) {
    out[i] = vec[i];
  }
  return kOfxStatOK;
}

// OfxPropertySuiteV1 entry points. Each one forwards to the SetProp*/GetProp*
// templates above. Strings returned by getters point into the property set
// and stay valid until the property is next written.
OfxStatus MockPropSetPointer(OfxPropertySetHandle h, const char* p, int idx,
                             void* v) {
  return SetPropOne<void*, PropValue::Kind::kPointer, &PropValue::ptrs>(h, p,
                                                                        idx, v);
}
OfxStatus MockPropSetString(OfxPropertySetHandle h, const char* p, int idx,
                            const char* v) {
  return SetPropOne<std::string, PropValue::Kind::kString, &PropValue::strings>(
      h, p, idx, v != nullptr ? std::string(v) : std::string());
}
OfxStatus MockPropSetDouble(OfxPropertySetHandle h, const char* p, int idx,
                            double v) {
  return SetPropOne<double, PropValue::Kind::kDouble, &PropValue::doubles>(
      h, p, idx, v);
}
OfxStatus MockPropSetInt(OfxPropertySetHandle h, const char* p, int idx,
                         int v) {
  return SetPropOne<int, PropValue::Kind::kInt, &PropValue::ints>(h, p, idx, v);
}
OfxStatus MockPropSetPointerN(OfxPropertySetHandle h, const char* p, int count,
                              void* const* v) {
  return SetPropN<void*, PropValue::Kind::kPointer, &PropValue::ptrs>(h, p,
                                                                      count, v);
}
OfxStatus MockPropSetStringN(OfxPropertySetHandle h, const char* p, int count,
                             const char* const* v) {
  if (h == nullptr || p == nullptr || count < 0) {
    return kOfxStatErrBadHandle;
  }
  auto* ps = AsProps(h);
  ScopedMutexLock lock(ps->mu);
  PropValue& pv = ps->map[p];
  pv.kind = PropValue::Kind::kString;
  pv.strings.clear();
  for (int i = 0; i < count; ++i) {
    pv.strings.push_back(v[i] != nullptr ? std::string(v[i]) : std::string());
  }
  return kOfxStatOK;
}
OfxStatus MockPropSetDoubleN(OfxPropertySetHandle h, const char* p, int count,
                             const double* v) {
  return SetPropN<double, PropValue::Kind::kDouble, &PropValue::doubles>(
      h, p, count, v);
}
OfxStatus MockPropSetIntN(OfxPropertySetHandle h, const char* p, int count,
                          const int* v) {
  return SetPropN<int, PropValue::Kind::kInt, &PropValue::ints>(h, p, count, v);
}
OfxStatus MockPropGetPointer(OfxPropertySetHandle h, const char* p, int idx,
                             void** v) {
  if (idx != 0) {
    return kOfxStatErrBadIndex;
  }
  return GetPropN<void*, PropValue::Kind::kPointer, &PropValue::ptrs>(h, p, 1,
                                                                      v);
}
OfxStatus MockPropGetString(OfxPropertySetHandle h, const char* p, int idx,
                            char** v) {
  if (h == nullptr || p == nullptr || v == nullptr || idx < 0) {
    return kOfxStatErrBadHandle;
  }
  auto* ps = AsProps(h);
  ScopedMutexLock lock(ps->mu);
  auto it = ps->map.find(p);
  if (it == ps->map.end() || it->second.kind != PropValue::Kind::kString) {
    return kOfxStatErrUnknown;
  }
  if (idx >= static_cast<int>(it->second.strings.size())) {
    return kOfxStatErrBadIndex;
  }
  *v = const_cast<char*>(it->second.strings[idx].c_str());
  return kOfxStatOK;
}
OfxStatus MockPropGetDouble(OfxPropertySetHandle h, const char* p, int idx,
                            double* v) {
  if (idx != 0) {
    return kOfxStatErrBadIndex;
  }
  return GetPropN<double, PropValue::Kind::kDouble, &PropValue::doubles>(h, p,
                                                                         1, v);
}
OfxStatus MockPropGetInt(OfxPropertySetHandle h, const char* p, int idx,
                         int* v) {
  if (idx != 0) {
    return kOfxStatErrBadIndex;
  }
  return GetPropN<int, PropValue::Kind::kInt, &PropValue::ints>(h, p, 1, v);
}
OfxStatus MockPropGetPointerN(OfxPropertySetHandle h, const char* p, int count,
                              void** v) {
  return GetPropN<void*, PropValue::Kind::kPointer, &PropValue::ptrs>(h, p,
                                                                      count, v);
}
OfxStatus MockPropGetStringN(OfxPropertySetHandle h, const char* p, int count,
                             char** v) {
  if (h == nullptr || p == nullptr || v == nullptr || count < 0) {
    return kOfxStatErrBadHandle;
  }
  auto* ps = AsProps(h);
  ScopedMutexLock lock(ps->mu);
  auto it = ps->map.find(p);
  if (it == ps->map.end() || it->second.kind != PropValue::Kind::kString) {
    return kOfxStatErrUnknown;
  }
  if (static_cast<int>(it->second.strings.size()) < count) {
    return kOfxStatErrBadIndex;
  }
  for (int i = 0; i < count; ++i) {
    v[i] = const_cast<char*>(it->second.strings[i].c_str());
  }
  return kOfxStatOK;
}
OfxStatus MockPropGetDoubleN(OfxPropertySetHandle h, const char* p, int count,
                             double* v) {
  return GetPropN<double, PropValue::Kind::kDouble, &PropValue::doubles>(
      h, p, count, v);
}
OfxStatus MockPropGetIntN(OfxPropertySetHandle h, const char* p, int count,
                          int* v) {
  return GetPropN<int, PropValue::Kind::kInt, &PropValue::ints>(h, p, count, v);
}
OfxStatus MockPropReset(OfxPropertySetHandle h, const char* p) {
  if (h == nullptr || p == nullptr) {
    return kOfxStatErrBadHandle;
  }
  auto* ps = AsProps(h);
  ScopedMutexLock lock(ps->mu);
  ps->map.erase(p);
  return kOfxStatOK;
}
OfxStatus MockPropGetDimension(OfxPropertySetHandle h, const char* p,
                               int* count) {
  if (h == nullptr || p == nullptr || count == nullptr) {
    return kOfxStatErrBadHandle;
  }
  auto* ps = AsProps(h);
  ScopedMutexLock lock(ps->mu);
  auto it = ps->map.find(p);
  if (it == ps->map.end()) {
    *count = 0;
    return kOfxStatOK;
  }
  switch (it->second.kind) {
    case PropValue::Kind::kInt:
      *count = static_cast<int>(it->second.ints.size());
      break;
    case PropValue::Kind::kDouble:
      *count = static_cast<int>(it->second.doubles.size());
      break;
    case PropValue::Kind::kString:
      *count = static_cast<int>(it->second.strings.size());
      break;
    case PropValue::Kind::kPointer:
      *count = static_cast<int>(it->second.ptrs.size());
      break;
    case PropValue::Kind::kUnset:
      *count = 0;
      break;
  }
  return kOfxStatOK;
}

// Property suite table, in OfxPropertySuiteV1 member order.
const OfxPropertySuiteV1 kPropSuite = {
    MockPropSetPointer,  MockPropSetString,   MockPropSetDouble,
    MockPropSetInt,      MockPropSetPointerN, MockPropSetStringN,
    MockPropSetDoubleN,  MockPropSetIntN,     MockPropGetPointer,
    MockPropGetString,   MockPropGetDouble,   MockPropGetInt,
    MockPropGetPointerN, MockPropGetStringN,  MockPropGetDoubleN,
    MockPropGetIntN,     MockPropReset,       MockPropGetDimension,
};

// ---------------------------------------------------------------------------
// Mock parameter suite.
// ---------------------------------------------------------------------------

// One defined parameter. `props` holds what the plugin set at describe time
// (label, default, limits); the current value lives in `int_val` (int, bool,
// choice) or `double_vals` (double, 2D, RGB) and is not animated.
struct MockParam {
  std::string type;
  std::string name;
  MockPropertySet props;
  int int_val = 0;
  double double_vals[3] = {0.0, 0.0, 0.0};
};

// All parameters of one descriptor or instance, keyed by parameter ID.
// `looked_up` records every ID paramGetHandle() resolved successfully, so
// tests can check that CreateInstance binds every parameter it describes.
// `define_order` lists IDs in first paramDefine() order (= Inspector order).
struct MockParamSet {
  MockPropertySet owner_props;
  std::map<std::string, std::unique_ptr<MockParam>> params;
  std::vector<std::string> define_order;
  PthreadMutex looked_up_mu;
  std::set<std::string> looked_up;
};

// paramDefine: creates (or replaces) parameter `name` and returns its
// property set so the plugin can fill in label, default and limits.
OfxStatus MockParamDefine(OfxParamSetHandle param_set, const char* param_type,
                          const char* name, OfxPropertySetHandle* prop_set) {
  if (param_set == nullptr || param_type == nullptr || name == nullptr) {
    return kOfxStatErrBadHandle;
  }
  auto* ps = reinterpret_cast<MockParamSet*>(param_set);
  auto param = std::make_unique<MockParam>();
  param->type = param_type;
  param->name = name;
  if (prop_set != nullptr) {
    *prop_set = ToPropHandle(&param->props);
  }
  if (ps->params.count(name) == 0) {
    ps->define_order.push_back(name);
  }
  ps->params[name] = std::move(param);
  return kOfxStatOK;
}

// paramGetHandle: looks a parameter up by ID; kOfxStatErrUnknown if absent.
// Successful lookups are recorded in MockParamSet::looked_up.
OfxStatus MockParamGetHandle(OfxParamSetHandle param_set, const char* name,
                             OfxParamHandle* param_out,
                             OfxPropertySetHandle* prop_set) {
  if (param_set == nullptr || name == nullptr || param_out == nullptr) {
    return kOfxStatErrBadHandle;
  }
  auto* ps = reinterpret_cast<MockParamSet*>(param_set);
  auto it = ps->params.find(name);
  if (it == ps->params.end()) {
    return kOfxStatErrUnknown;
  }
  *param_out = reinterpret_cast<OfxParamHandle>(it->second.get());
  if (prop_set != nullptr) {
    *prop_set = ToPropHandle(&it->second->props);
  }
  {
    ScopedMutexLock lock(ps->looked_up_mu);
    ps->looked_up.insert(name);
  }
  return kOfxStatOK;
}

// Property-set accessors for a parameter set and for a single parameter.
OfxStatus MockParamSetGetPropertySet(OfxParamSetHandle param_set,
                                     OfxPropertySetHandle* prop_out) {
  auto* ps = reinterpret_cast<MockParamSet*>(param_set);
  *prop_out = ToPropHandle(&ps->owner_props);
  return kOfxStatOK;
}

OfxStatus MockParamGetPropertySet(OfxParamHandle param_h,
                                  OfxPropertySetHandle* prop_out) {
  auto* p = reinterpret_cast<MockParam*>(param_h);
  *prop_out = ToPropHandle(&p->props);
  return kOfxStatOK;
}

// Writes the current value of `param_h` through the variadic out-pointers in
// `ap`, following the OpenFX convention: one int* for int/bool/choice, one,
// two or three double* for double/2D/RGB.
OfxStatus ReadParamVaList(OfxParamHandle param_h, va_list ap) {
  if (param_h == nullptr) {
    return kOfxStatErrBadHandle;
  }
  auto* p = reinterpret_cast<MockParam*>(param_h);
  if (p->type == kOfxParamTypeInteger || p->type == kOfxParamTypeBoolean ||
      p->type == kOfxParamTypeChoice) {
    int* out = va_arg(ap, int*);
    *out = p->int_val;
    return kOfxStatOK;
  }
  if (p->type == kOfxParamTypeDouble) {
    double* out = va_arg(ap, double*);
    *out = p->double_vals[0];
    return kOfxStatOK;
  }
  if (p->type == kOfxParamTypeDouble2D) {
    double* ox = va_arg(ap, double*);
    double* oy = va_arg(ap, double*);
    *ox = p->double_vals[0];
    *oy = p->double_vals[1];
    return kOfxStatOK;
  }
  if (p->type == kOfxParamTypeRGB) {
    double* or_c = va_arg(ap, double*);
    double* og_c = va_arg(ap, double*);
    double* ob_c = va_arg(ap, double*);
    *or_c = p->double_vals[0];
    *og_c = p->double_vals[1];
    *ob_c = p->double_vals[2];
    return kOfxStatOK;
  }
  return kOfxStatErrUnsupported;
}

// paramGetValue / paramGetValueAtTime. Time is ignored: the mock has no
// keyframes, so every frame sees the last value set by the test.
OfxStatus MockParamGetValue(OfxParamHandle param_h, ...) {
  va_list ap;
  va_start(ap, param_h);
  const OfxStatus st = ReadParamVaList(param_h, ap);
  va_end(ap);
  return st;
}

OfxStatus MockParamGetValueAtTime(OfxParamHandle param_h, OfxTime time, ...) {
  va_list ap;
  va_start(ap, time);
  const OfxStatus st = ReadParamVaList(param_h, ap);
  va_end(ap);
  return st;
}

// Parameter-suite entry points the plugin never uses: unsupported or no-op
// stubs. MockParamSetValue() is the exception: tests call it directly to set
// a parameter (int for int/bool/choice, doubles for double/2D/RGB).
OfxStatus MockParamGetDerivative(OfxParamHandle /*p*/, OfxTime /*t*/, ...) {
  return kOfxStatErrUnsupported;
}
OfxStatus MockParamGetIntegral(OfxParamHandle /*p*/, OfxTime /*t1*/,
                               OfxTime /*t2*/, ...) {
  return kOfxStatErrUnsupported;
}
OfxStatus MockParamSetValue(OfxParamHandle param_h, ...) {
  if (param_h == nullptr) {
    return kOfxStatErrBadHandle;
  }
  auto* p = reinterpret_cast<MockParam*>(param_h);
  va_list ap;
  va_start(ap, param_h);
  if (p->type == kOfxParamTypeInteger || p->type == kOfxParamTypeBoolean ||
      p->type == kOfxParamTypeChoice) {
    p->int_val = va_arg(ap, int);
  } else if (p->type == kOfxParamTypeDouble) {
    p->double_vals[0] = va_arg(ap, double);
  } else if (p->type == kOfxParamTypeDouble2D) {
    p->double_vals[0] = va_arg(ap, double);
    p->double_vals[1] = va_arg(ap, double);
  } else if (p->type == kOfxParamTypeRGB) {
    p->double_vals[0] = va_arg(ap, double);
    p->double_vals[1] = va_arg(ap, double);
    p->double_vals[2] = va_arg(ap, double);
  }
  va_end(ap);
  return kOfxStatOK;
}
OfxStatus MockParamSetValueAtTime(OfxParamHandle /*p*/, OfxTime /*t*/, ...) {
  return kOfxStatOK;
}
OfxStatus MockParamGetNumKeys(OfxParamHandle /*p*/, unsigned int* n) {
  *n = 0;
  return kOfxStatOK;
}
OfxStatus MockParamGetKeyTime(OfxParamHandle /*p*/, unsigned int /*k*/,
                              OfxTime* /*t*/) {
  return kOfxStatErrBadIndex;
}
OfxStatus MockParamGetKeyIndex(OfxParamHandle /*p*/, OfxTime /*t*/, int /*d*/,
                               int* idx) {
  *idx = -1;
  return kOfxStatFailed;
}
OfxStatus MockParamDeleteKey(OfxParamHandle /*p*/, OfxTime /*t*/) {
  return kOfxStatErrBadIndex;
}
OfxStatus MockParamDeleteAllKeys(OfxParamHandle /*p*/) { return kOfxStatOK; }
OfxStatus MockParamCopy(OfxParamHandle /*to*/, OfxParamHandle /*from*/,
                        OfxTime /*off*/, const OfxRangeD* /*r*/) {
  return kOfxStatOK;
}
OfxStatus MockParamEditBegin(OfxParamSetHandle /*s*/, const char* /*n*/) {
  return kOfxStatOK;
}
OfxStatus MockParamEditEnd(OfxParamSetHandle /*s*/) { return kOfxStatOK; }

// Parameter suite table, in OfxParameterSuiteV1 member order.
const OfxParameterSuiteV1 kParamSuite = {
    MockParamDefine,         MockParamGetHandle,   MockParamSetGetPropertySet,
    MockParamGetPropertySet, MockParamGetValue,    MockParamGetValueAtTime,
    MockParamGetDerivative,  MockParamGetIntegral, MockParamSetValue,
    MockParamSetValueAtTime, MockParamGetNumKeys,  MockParamGetKeyTime,
    MockParamGetKeyIndex,    MockParamDeleteKey,   MockParamDeleteAllKeys,
    MockParamCopy,           MockParamEditBegin,   MockParamEditEnd,
};

// ---------------------------------------------------------------------------
// Mock host, clips and effects.
// ---------------------------------------------------------------------------

struct MockHost;

// One clip ("Output", "Source", "SourceFrom" or "SourceTo"). `props` holds
// clip properties (connected, depth, frame range, plus the test-only
// "MockRoD"); `bound_image` is the image returned by clipGetImage(), or
// nullptr to simulate "no image at this time". `get_image_calls` counts
// clipGetImage() calls (successful or not), so tests can check that render
// skips clips the RoI action declared unused; reset it before a render.
struct MockClip {
  std::string name;
  MockHost* host = nullptr;
  MockPropertySet props;
  MockPropertySet* bound_image = nullptr;
  int get_image_calls = 0;  // Guarded by host->lease_mu.
};

// An effect descriptor or instance: its properties, parameters and clips.
// OfxImageEffectHandle values given to the plugin point at one of these.
// `looked_up_clips` records every clip name clipGetHandle() resolved.
struct MockEffectObject {
  MockHost* host = nullptr;
  MockPropertySet props;
  MockParamSet param_set;
  std::map<std::string, std::unique_ptr<MockClip>> clips;
  PthreadMutex looked_up_clips_mu;
  std::set<std::string> looked_up_clips;
};

// The host itself. `ofx_host` is what setHost receives. `lease_mu` guards
// `active_leased_images`, the count of images fetched and not yet released;
// tests expect it back at 0 after every render.
struct MockHost {
  MockPropertySet host_props;
  OfxHost ofx_host;
  PthreadMutex lease_mu;
  int active_leased_images = 0;

  static const void* FetchSuite(OfxPropertySetHandle /*host*/,
                                const char* suite_name, int suite_version);
};

const OfxMultiThreadSuiteV1* GetMultiThreadSuite();
const OfxImageEffectSuiteV1* GetImageEffectSuite();

// OfxHost::fetchSuite: returns the mock property, parameter, image-effect and
// multithread suites (version 1 only); nullptr for anything else.
const void* MockHost::FetchSuite(OfxPropertySetHandle /*host*/,
                                 const char* suite_name, int suite_version) {
  if (suite_version != 1 || suite_name == nullptr) {
    return nullptr;
  }
  if (std::strcmp(suite_name, kOfxPropertySuite) == 0) {
    return &kPropSuite;
  }
  if (std::strcmp(suite_name, kOfxParameterSuite) == 0) {
    return &kParamSuite;
  }
  if (std::strcmp(suite_name, kOfxImageEffectSuite) == 0) {
    return GetImageEffectSuite();
  }
  if (std::strcmp(suite_name, kOfxMultiThreadSuite) == 0) {
    return GetMultiThreadSuite();
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Mock multithread suite.
// ---------------------------------------------------------------------------

// Per-thread state reported by multiThreadIsSpawnedThread() and
// multiThreadIndex(). The plugin must not nest multiThread() calls from a
// spawned thread, and these flags let it detect that.
thread_local bool g_is_spawned_thread = false;
thread_local unsigned int g_spawned_thread_index = 0;

// Arguments for one worker started by MockMultiThread().
struct WorkerThreadArgs {
  OfxThreadFunctionV1* func = nullptr;
  unsigned int index = 0;
  unsigned int total = 0;
  void* custom_arg = nullptr;
};

// pthread entry point: marks the thread as spawned, runs the plugin's
// thread function, then clears the marks.
void* RunMockWorkerThread(void* raw) {
  auto* args = static_cast<WorkerThreadArgs*>(raw);
  g_is_spawned_thread = true;
  g_spawned_thread_index = args->index;
  args->func(args->index, args->total, args->custom_arg);
  g_is_spawned_thread = false;
  g_spawned_thread_index = 0;
  return nullptr;
}

// Test-only failure injection for MockMultiThread(), to exercise the
// plugin's serial fallback. kBeforeRun fails without running any worker;
// kAfterRun runs and joins every worker, calls g_multithread_after_run_hook
// (if set), then still reports failure. Only touched by the test's main
// thread outside a dispatch; reset by ScopedMultiThreadFailure.
enum class MultiThreadFailMode { kNone, kBeforeRun, kAfterRun };
MultiThreadFailMode g_multithread_fail_mode = MultiThreadFailMode::kNone;
std::function<void()> g_multithread_after_run_hook;

// RAII guard that enables a MultiThreadFailMode (and optional after-run
// hook) for one scope and restores normal multiThread() behavior on exit.
class ScopedMultiThreadFailure {
 public:
  explicit ScopedMultiThreadFailure(MultiThreadFailMode mode,
                                    std::function<void()> hook = nullptr) {
    g_multithread_fail_mode = mode;
    g_multithread_after_run_hook = std::move(hook);
  }
  ~ScopedMultiThreadFailure() {
    g_multithread_fail_mode = MultiThreadFailMode::kNone;
    g_multithread_after_run_hook = nullptr;
  }
  ScopedMultiThreadFailure(const ScopedMultiThreadFailure&) = delete;
  ScopedMultiThreadFailure& operator=(const ScopedMultiThreadFailure&) = delete;
};

// multiThread: runs `func` on min(n_threads, 4) real pthreads and joins them
// all before returning, as the OpenFX spec requires. If a thread cannot be
// started, joins the ones already running and fails. Honors
// g_multithread_fail_mode for failure-injection tests.
OfxStatus MockMultiThread(OfxThreadFunctionV1 func, unsigned int n_threads,
                          void* custom_arg) {
  if (func == nullptr || n_threads == 0) {
    return kOfxStatFailed;
  }
  if (g_multithread_fail_mode == MultiThreadFailMode::kBeforeRun) {
    return kOfxStatFailed;
  }
  const unsigned int workers = std::min(n_threads, 4u);
  std::vector<WorkerThreadArgs> args(workers);
  std::vector<pthread_t> threads(workers);
  unsigned int started = 0;
  for (unsigned int i = 0; i < workers; ++i) {
    args[i] = {func, i, workers, custom_arg};
    if (pthread_create(&threads[i], nullptr, RunMockWorkerThread, &args[i]) !=
        0) {
      for (unsigned int j = 0; j < started; ++j) {
        pthread_join(threads[j], nullptr);
      }
      return kOfxStatFailed;
    }
    ++started;
  }
  for (unsigned int j = 0; j < started; ++j) {
    pthread_join(threads[j], nullptr);
  }
  if (g_multithread_fail_mode == MultiThreadFailMode::kAfterRun) {
    if (g_multithread_after_run_hook) {
      g_multithread_after_run_hook();
    }
    return kOfxStatFailed;
  }
  return kOfxStatOK;
}

// multiThreadNumCPUs: always 4, so tests behave the same on any machine.
OfxStatus MockMultiThreadNumCPUs(unsigned int* n_cpus) {
  *n_cpus = 4;
  return kOfxStatOK;
}

// multiThreadIndex / multiThreadIsSpawnedThread, backed by the thread-locals.
OfxStatus MockMultiThreadIndex(unsigned int* idx) {
  *idx = g_spawned_thread_index;
  return kOfxStatOK;
}

int MockMultiThreadIsSpawnedThread(void) { return g_is_spawned_thread ? 1 : 0; }

// Mutex functions of the multithread suite, backed by heap-allocated
// pthread mutexes. The plugin does not use them today.
OfxStatus MockMutexCreate(OfxMutexHandle* m, int /*lock_count*/) {
  auto* pm = new pthread_mutex_t;
  pthread_mutex_init(pm, nullptr);
  *m = reinterpret_cast<OfxMutexHandle>(pm);
  return kOfxStatOK;
}
OfxStatus MockMutexDestroy(const OfxMutexHandle m) {
  auto* pm = reinterpret_cast<pthread_mutex_t*>(m);
  pthread_mutex_destroy(pm);
  delete pm;
  return kOfxStatOK;
}
OfxStatus MockMutexLock(const OfxMutexHandle m) {
  pthread_mutex_lock(reinterpret_cast<pthread_mutex_t*>(m));
  return kOfxStatOK;
}
OfxStatus MockMutexUnLock(const OfxMutexHandle m) {
  pthread_mutex_unlock(reinterpret_cast<pthread_mutex_t*>(m));
  return kOfxStatOK;
}
OfxStatus MockMutexTryLock(const OfxMutexHandle m) {
  return pthread_mutex_trylock(reinterpret_cast<pthread_mutex_t*>(m)) == 0
             ? kOfxStatOK
             : kOfxStatFailed;
}

// Returns the multithread suite table, in OfxMultiThreadSuiteV1 order.
const OfxMultiThreadSuiteV1* GetMultiThreadSuite() {
  static const OfxMultiThreadSuiteV1 kSuite = {
      MockMultiThread,      MockMultiThreadNumCPUs,
      MockMultiThreadIndex, MockMultiThreadIsSpawnedThread,
      MockMutexCreate,      MockMutexDestroy,
      MockMutexLock,        MockMutexUnLock,
      MockMutexTryLock,
  };
  return &kSuite;
}

// ---------------------------------------------------------------------------
// Mock image effect suite.
// ---------------------------------------------------------------------------

// getPropertySet / getParamSet for an effect handle (a MockEffectObject*).
OfxStatus MockEffectGetPropertySet(OfxImageEffectHandle effect,
                                   OfxPropertySetHandle* prop_set) {
  auto* obj = reinterpret_cast<MockEffectObject*>(effect);
  *prop_set = ToPropHandle(&obj->props);
  return kOfxStatOK;
}

OfxStatus MockEffectGetParamSet(OfxImageEffectHandle effect,
                                OfxParamSetHandle* param_set) {
  auto* obj = reinterpret_cast<MockEffectObject*>(effect);
  *param_set = reinterpret_cast<OfxParamSetHandle>(&obj->param_set);
  return kOfxStatOK;
}

// clipDefine: adds clip `name` to a descriptor and returns its properties.
OfxStatus MockClipDefine(OfxImageEffectHandle effect, const char* name,
                         OfxPropertySetHandle* prop_set) {
  auto* obj = reinterpret_cast<MockEffectObject*>(effect);
  auto clip = std::make_unique<MockClip>();
  clip->name = name;
  clip->host = obj->host;
  if (prop_set != nullptr) {
    *prop_set = ToPropHandle(&clip->props);
  }
  obj->clips[name] = std::move(clip);
  return kOfxStatOK;
}

// clipGetHandle: looks a clip up by name; kOfxStatErrUnknown if absent.
// Successful lookups are recorded in MockEffectObject::looked_up_clips.
OfxStatus MockClipGetHandle(OfxImageEffectHandle effect, const char* name,
                            OfxImageClipHandle* clip_out,
                            OfxPropertySetHandle* prop_set) {
  auto* obj = reinterpret_cast<MockEffectObject*>(effect);
  auto it = obj->clips.find(name);
  if (it == obj->clips.end()) {
    return kOfxStatErrUnknown;
  }
  *clip_out = reinterpret_cast<OfxImageClipHandle>(it->second.get());
  if (prop_set != nullptr) {
    *prop_set = ToPropHandle(&it->second->props);
  }
  {
    ScopedMutexLock lock(obj->looked_up_clips_mu);
    obj->looked_up_clips.insert(name);
  }
  return kOfxStatOK;
}

// clipGetPropertySet: returns the clip's own properties.
OfxStatus MockClipGetPropertySet(OfxImageClipHandle clip_h,
                                 OfxPropertySetHandle* prop_set) {
  auto* clip = reinterpret_cast<MockClip*>(clip_h);
  *prop_set = ToPropHandle(&clip->props);
  return kOfxStatOK;
}

// clipGetImage: returns the clip's bound image (time and region ignored) and
// counts one lease. Fails when nothing is bound, which is how tests simulate
// a missing image at a clip edge. Every call, failed or not, increments
// MockClip::get_image_calls.
OfxStatus MockClipGetImage(OfxImageClipHandle clip_h, OfxTime /*time*/,
                           const OfxRectD* /*region*/,
                           OfxPropertySetHandle* image_handle) {
  auto* clip = reinterpret_cast<MockClip*>(clip_h);
  if (clip == nullptr) {
    return kOfxStatFailed;
  }
  {
    ScopedMutexLock lock(clip->host->lease_mu);
    ++clip->get_image_calls;
  }
  if (clip->bound_image == nullptr) {
    return kOfxStatFailed;
  }
  {
    ScopedMutexLock lock(clip->host->lease_mu);
    ++clip->host->active_leased_images;
  }
  // Return the bound image property set directly (release tracks the lease).
  *image_handle = ToPropHandle(clip->bound_image);
  return kOfxStatOK;
}

// clipReleaseImage: ends one lease. The owning host is found through the
// test-only "MockHostPtr" property that ConfigureClipAndImage() stores on
// every image.
OfxStatus MockClipReleaseImage(OfxPropertySetHandle image_handle) {
  auto* ps = AsProps(image_handle);
  void* host_ptr = nullptr;
  if (MockPropGetPointer(ToPropHandle(ps), "MockHostPtr", 0, &host_ptr) ==
          kOfxStatOK &&
      host_ptr != nullptr) {
    auto* host = static_cast<MockHost*>(host_ptr);
    ScopedMutexLock lock(host->lease_mu);
    --host->active_leased_images;
  }
  return kOfxStatOK;
}

// clipGetRegionOfDefinition: returns the clip's test-only "MockRoD" property
// (x1, y1, x2, y2), or fails if the test did not set it.
OfxStatus MockClipGetRod(OfxImageClipHandle clip_h, OfxTime /*time*/,
                         OfxRectD* bounds) {
  auto* clip = reinterpret_cast<MockClip*>(clip_h);
  double rod[4] = {0.0, 0.0, 0.0, 0.0};
  if (MockPropGetDoubleN(ToPropHandle(&clip->props), "MockRoD", 4, rod) !=
      kOfxStatOK) {
    return kOfxStatFailed;
  }
  bounds->x1 = rod[0];
  bounds->y1 = rod[1];
  bounds->x2 = rod[2];
  bounds->y2 = rod[3];
  return kOfxStatOK;
}

// abort (never requested) and image-memory functions (plain malloc/free).
// The plugin does not use them today.
int MockAbort(OfxImageEffectHandle /*effect*/) { return 0; }
OfxStatus MockImageMemoryAlloc(OfxImageEffectHandle /*effect*/, size_t bytes,
                               OfxImageMemoryHandle* mem) {
  *mem = reinterpret_cast<OfxImageMemoryHandle>(std::malloc(bytes));
  return *mem != nullptr ? kOfxStatOK : kOfxStatErrMemory;
}
OfxStatus MockImageMemoryFree(OfxImageMemoryHandle mem) {
  std::free(mem);
  return kOfxStatOK;
}
OfxStatus MockImageMemoryLock(OfxImageMemoryHandle mem, void** ptr) {
  *ptr = reinterpret_cast<void*>(mem);
  return kOfxStatOK;
}
OfxStatus MockImageMemoryUnlock(OfxImageMemoryHandle /*mem*/) {
  return kOfxStatOK;
}

// Returns the image effect suite table, in OfxImageEffectSuiteV1 order.
const OfxImageEffectSuiteV1* GetImageEffectSuite() {
  static const OfxImageEffectSuiteV1 kSuite = {
      MockEffectGetPropertySet, MockEffectGetParamSet,  MockClipDefine,
      MockClipGetHandle,        MockClipGetPropertySet, MockClipGetImage,
      MockClipReleaseImage,     MockClipGetRod,         MockAbort,
      MockImageMemoryAlloc,     MockImageMemoryFree,    MockImageMemoryLock,
      MockImageMemoryUnlock,
  };
  return &kSuite;
}

// ---------------------------------------------------------------------------
// Test helpers.
// ---------------------------------------------------------------------------

// Mimics the host turning a described context into an instance: copies the
// clips (with their properties) and the parameters from `context_desc` into
// `inst`, seeds each parameter's value from its kOfxParamPropDefault, and
// records `context` on the instance.
void CloneDescriptorToInstance(const MockEffectObject& context_desc,
                               MockEffectObject* inst, const char* context) {
  inst->host = context_desc.host;
  MockPropSetString(ToPropHandle(&inst->props), kOfxImageEffectPropContext, 0,
                    context);
  for (const auto& [clip_name, clip_ptr] : context_desc.clips) {
    auto c = std::make_unique<MockClip>();
    c->name = clip_name;
    c->host = inst->host;
    c->props.map = clip_ptr->props.map;
    inst->clips[clip_name] = std::move(c);
  }
  for (const auto& [p_name, p_ptr] : context_desc.param_set.params) {
    auto p = std::make_unique<MockParam>();
    p->type = p_ptr->type;
    p->name = p_ptr->name;
    p->props.map = p_ptr->props.map;
    auto it_def = p->props.map.find(kOfxParamPropDefault);
    if (it_def != p->props.map.end()) {
      if (!it_def->second.ints.empty()) {
        p->int_val = it_def->second.ints[0];
      }
      for (std::size_t i = 0;
           i < std::min<std::size_t>(3, it_def->second.doubles.size()); ++i) {
        p->double_vals[i] = it_def->second.doubles[i];
      }
    }
    inst->param_set.params[p_name] = std::move(p);
  }
}

// Marks `clip` as connected with pixel depth `depth` and the given frame
// range, sets its RoD to (0, 0, width, height), and binds `img_props` as the
// image clipGetImage() will return. The image wraps the caller-owned RGBA
// buffer `data` (premultiplied, bounds = RoD) with `row_bytes` per row; a
// negative `row_bytes` with `data` pointing at the last row models bottom-up
// storage. Tests may override individual image properties afterwards.
void ConfigureClipAndImage(MockHost* host, MockClip* clip,
                           MockPropertySet* img_props, void* data, int width,
                           int height, int row_bytes, const char* depth,
                           const double frame_range[2]) {
  MockPropSetInt(ToPropHandle(&clip->props), kOfxImageClipPropConnected, 0, 1);
  MockPropSetString(ToPropHandle(&clip->props),
                    kOfxImageClipPropUnmappedPixelDepth, 0, depth);
  MockPropSetDoubleN(ToPropHandle(&clip->props), kOfxImageEffectPropFrameRange,
                     2, frame_range);
  const double rod_d[4] = {0.0, 0.0, static_cast<double>(width),
                           static_cast<double>(height)};
  MockPropSetDoubleN(ToPropHandle(&clip->props), "MockRoD", 4, rod_d);

  MockPropSetPointer(ToPropHandle(img_props), "MockHostPtr", 0, host);
  MockPropSetPointer(ToPropHandle(img_props), kOfxImagePropData, 0, data);
  MockPropSetString(ToPropHandle(img_props), kOfxImageEffectPropComponents, 0,
                    kOfxImageComponentRGBA);
  MockPropSetString(ToPropHandle(img_props), kOfxImageEffectPropPixelDepth, 0,
                    depth);
  const int bounds[4] = {0, 0, width, height};
  MockPropSetIntN(ToPropHandle(img_props), kOfxImagePropBounds, 4, bounds);
  MockPropSetIntN(ToPropHandle(img_props), kOfxImagePropRegionOfDefinition, 4,
                  bounds);
  MockPropSetInt(ToPropHandle(img_props), kOfxImagePropRowBytes, 0, row_bytes);
  MockPropSetString(ToPropHandle(img_props),
                    kOfxImageEffectPropPreMultiplication, 0,
                    kOfxImagePreMultiplied);
  clip->bound_image = img_props;
}

// RAII wrapper around dlopen()/dlclose() for the plugin binary. raw() is
// nullptr if loading failed; Sym() looks up an exported symbol.
class LoadedOfxBundle {
 public:
  explicit LoadedOfxBundle(const std::string& path) {
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  }
  ~LoadedOfxBundle() {
    if (handle_ != nullptr) {
      dlclose(handle_);
    }
  }
  void* raw() const { return handle_; }

  template <typename Fn>
  Fn Sym(const char* name) const {
    return reinterpret_cast<Fn>(dlsym(handle_, name));
  }

 private:
  void* handle_ = nullptr;
};

// Returns the plugin whose identifier is `id`, or nullptr.
const OfxPlugin* FindPluginById(GetPluginFn get_plugin, int count,
                                const char* id) {
  for (int i = 0; i < count; ++i) {
    const OfxPlugin* p = get_plugin(i);
    if (p != nullptr && std::strcmp(p->pluginIdentifier, id) == 0) {
      return p;
    }
  }
  return nullptr;
}

// Owns one plugin instance for a test. The destructor sends DestroyInstance
// and Unload so every test cleans up even when an ASSERT returns early.
struct ScopedPluginInstance {
  const OfxPlugin* plugin = nullptr;
  MockHost* host = nullptr;
  MockEffectObject desc;
  MockEffectObject inst;

  ~ScopedPluginInstance() {
    if (plugin != nullptr) {
      plugin->mainEntry(kOfxActionDestroyInstance, &inst, nullptr, nullptr);
      plugin->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    }
  }
};

// Runs setHost, Load, Describe and DescribeInContext(`context`) for plugin
// `id`, then clones the descriptor and sends CreateInstance, leaving a ready
// instance in `out`. A failed ASSERT returns from this helper only, not from
// the calling test, so always call it through ASSERT_NO_FATAL_FAILURE() to
// stop the test instead of rendering with a half-initialized instance.
void InitPluginInstance(LoadedOfxBundle* bundle, const char* id,
                        const char* context, MockHost* host,
                        ScopedPluginInstance* out) {
  auto get_count = bundle->Sym<GetNumPluginsFn>("OfxGetNumberOfPlugins");
  auto get_plugin = bundle->Sym<GetPluginFn>("OfxGetPlugin");
  out->plugin = FindPluginById(get_plugin, get_count(), id);
  ASSERT_NE(out->plugin, nullptr) << id;
  out->host = host;
  out->desc.host = host;

  out->plugin->setHost(&host->ofx_host);
  ASSERT_EQ(out->plugin->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr),
            kOfxStatOK);
  ASSERT_EQ(
      out->plugin->mainEntry(kOfxActionDescribe, &out->desc, nullptr, nullptr),
      kOfxStatOK);

  MockPropertySet ctx_in;
  MockPropSetString(ToPropHandle(&ctx_in), kOfxImageEffectPropContext, 0,
                    context);
  ASSERT_EQ(out->plugin->mainEntry(kOfxImageEffectActionDescribeInContext,
                                   &out->desc, ToPropHandle(&ctx_in), nullptr),
            kOfxStatOK);
  CloneDescriptorToInstance(out->desc, &out->inst, context);
  ASSERT_EQ(out->plugin->mainEntry(kOfxActionCreateInstance, &out->inst,
                                   nullptr, nullptr),
            kOfxStatOK);
}

// Sends one Render action at `time` with render scale (`scale_x`, `scale_y`)
// and no explicit render window (so the plugin renders the whole output
// buffer). Buffers keep their size; only the plugin's interpretation of
// pixel-unit parameters changes with the scale.
OfxStatus RenderFrameAtScale(const ScopedPluginInstance& pi, OfxTime time,
                             double scale_x, double scale_y) {
  MockPropertySet render_in;
  MockPropSetDouble(ToPropHandle(&render_in), kOfxPropTime, 0, time);
  const double rs[2] = {scale_x, scale_y};
  MockPropSetDoubleN(ToPropHandle(&render_in), kOfxImageEffectPropRenderScale,
                     2, rs);
  return pi.plugin->mainEntry(kOfxImageEffectActionRender,
                              const_cast<MockEffectObject*>(&pi.inst),
                              ToPropHandle(&render_in), nullptr);
}

// Sends one Render action at `time` with render scale 1 and no explicit
// render window (so the plugin renders the whole output buffer).
OfxStatus RenderFrame(const ScopedPluginInstance& pi, OfxTime time) {
  return RenderFrameAtScale(pi, time, 1.0, 1.0);
}

// Sets an int, bool or choice parameter of `inst` by ID. Fails the test if
// the plugin did not define that ID or gave it another type, instead of
// silently creating a new parameter (as params[name] would) or passing the
// variadic value with the wrong type.
void SetIntParam(MockEffectObject* inst, const char* name, int value) {
  const auto it = inst->param_set.params.find(name);
  ASSERT_TRUE(it != inst->param_set.params.end())
      << "unknown parameter " << name;
  const std::string& type = it->second->type;
  ASSERT_TRUE(type == kOfxParamTypeInteger || type == kOfxParamTypeBoolean ||
              type == kOfxParamTypeChoice)
      << name << " has type " << type;
  MockParamSetValue(reinterpret_cast<OfxParamHandle>(it->second.get()), value);
}

// Sets a scalar double parameter of `inst` by ID, with the same checks as
// SetIntParam().
void SetDoubleParam(MockEffectObject* inst, const char* name, double value) {
  const auto it = inst->param_set.params.find(name);
  ASSERT_TRUE(it != inst->param_set.params.end())
      << "unknown parameter " << name;
  ASSERT_EQ(it->second->type, kOfxParamTypeDouble) << name;
  MockParamSetValue(reinterpret_cast<OfxParamHandle>(it->second.get()), value);
}

// Overloads that set a parameter on a ScopedPluginInstance's instance.
void SetIntParam(ScopedPluginInstance* pi, const char* name, int value) {
  SetIntParam(&pi->inst, name, value);
}

void SetDoubleParam(ScopedPluginInstance* pi, const char* name, double value) {
  SetDoubleParam(&pi->inst, name, value);
}

// One GetRegionsOfInterest reply: the action status and, per input clip, the
// RoI (x1, y1, x2, y2 in canonical coordinates) left in outArgs.
struct RoiReply {
  OfxStatus status = kOfxStatFailed;
  std::map<std::string, std::array<double, 4>> rois;
};

// Sends GetRegionsOfInterest at `time` the way the OpenFX spec says a host
// does: inArgs carries the time, render scale 1 and the region to render
// (the whole `w` x `h` output), and every input clip's
// "OfxImageClipPropRoI_<clip>" in outArgs is set to that default region
// before the call. An input the plugin does not touch therefore reads back
// as the full render region, which is what a real host would fetch.
RoiReply RequestRegionsOfInterest(const ScopedPluginInstance& pi, OfxTime time,
                                  int w, int h) {
  MockPropertySet in_args, out_args;
  MockPropSetDouble(ToPropHandle(&in_args), kOfxPropTime, 0, time);
  const double rs[2] = {1.0, 1.0};
  MockPropSetDoubleN(ToPropHandle(&in_args), kOfxImageEffectPropRenderScale, 2,
                     rs);
  const double region[4] = {0.0, 0.0, static_cast<double>(w),
                            static_cast<double>(h)};
  MockPropSetDoubleN(ToPropHandle(&in_args),
                     kOfxImageEffectPropRegionOfInterest, 4, region);
  std::vector<std::string> inputs;
  for (const auto& [name, clip] : pi.inst.clips) {
    if (name != kOfxImageEffectOutputClipName) {
      inputs.push_back(name);
      MockPropSetDoubleN(ToPropHandle(&out_args),
                         ("OfxImageClipPropRoI_" + name).c_str(), 4, region);
    }
  }
  RoiReply reply;
  reply.status =
      pi.plugin->mainEntry(kOfxImageEffectActionGetRegionsOfInterest,
                           const_cast<MockEffectObject*>(&pi.inst),
                           ToPropHandle(&in_args), ToPropHandle(&out_args));
  for (const std::string& name : inputs) {
    std::array<double, 4> roi = {0.0, 0.0, 0.0, 0.0};
    MockPropGetDoubleN(ToPropHandle(&out_args),
                       ("OfxImageClipPropRoI_" + name).c_str(), 4, roi.data());
    reply.rois[name] = roi;
  }
  return reply;
}

// Returns whether `roi` (x1, y1, x2, y2) covers no area.
bool IsEmptyRoi(const std::array<double, 4>& roi) {
  return roi[2] <= roi[0] || roi[3] <= roi[1];
}

// Returns string property `name` of a described parameter (for example its
// label or parent group ID), or "" when the plugin did not set it.
std::string ParamStringProp(const MockParam& param, const char* name) {
  const auto it = param.props.map.find(name);
  if (it == param.props.map.end() ||
      it->second.kind != PropValue::Kind::kString ||
      it->second.strings.empty()) {
    return "";
  }
  return it->second.strings[0];
}

// Returns integer property `name` of a described parameter, or `fallback`
// when the plugin did not set it.
int ParamIntProp(const MockParam& param, const char* name, int fallback) {
  const auto it = param.props.map.find(name);
  if (it == param.props.map.end() || it->second.kind != PropValue::Kind::kInt ||
      it->second.ints.empty()) {
    return fallback;
  }
  return it->second.ints[0];
}

// Returns whether the plugin hid `param` (kOfxParamPropSecret = 1).
bool IsSecretParam(const MockParam& param) {
  return ParamIntProp(param, kOfxParamPropSecret, 0) != 0;
}

// Retired Paper Cutout FX "Intro & Outro Paper Animations" parameter IDs.
// They stay defined (hidden) so old projects open, but are never read.
constexpr const char* kLegacyCutoutParamIds[] = {
    "introStyle",      "introFrames",   "outroStyle",   "outroFrames",
    "transitionAngle", "glideDistance", "mirrorTransH", "mirrorTransV",
};

// ---------------------------------------------------------------------------
// Test fixture.
// ---------------------------------------------------------------------------

constexpr char kCutoutId[] = "com.papercraft.CutoutFX";
constexpr char kFoldTransitionId[] = "com.papercraft.PaperFoldTransition";

// Shared per-test setup: dlopen()s the plugin binary and builds a fresh mock
// host whose fetchSuite serves the mock suites. SetUp() stops the test when
// the binary cannot be loaded. Declaration order matters for teardown:
// ScopedPluginInstance locals of a test body (which send DestroyInstance and
// Unload into the plugin) go first, then `host_`, and `bundle_` unloads the
// plugin last.
class PaperCraftOfxTest : public ::testing::Test {
 protected:
  PaperCraftOfxTest()
      : bundle_path_(ResolvePluginBinaryPath()), bundle_(bundle_path_) {
    if (bundle_.raw() == nullptr) {
      // Read dlerror() once: a second call returns nullptr.
      const char* err = dlerror();
      load_error_ = err != nullptr ? err : "unknown";
    }
    host_.ofx_host.host = ToPropHandle(&host_.host_props);
    host_.ofx_host.fetchSuite = &MockHost::FetchSuite;
  }

  void SetUp() override {
    ASSERT_NE(bundle_.raw(), nullptr)
        << "dlopen failed for " << bundle_path_ << ": " << load_error_;
  }

  // Returns the plugin whose identifier is `id`, or nullptr.
  const OfxPlugin* FindPlugin(const char* id) const {
    auto get_count = bundle_.Sym<GetNumPluginsFn>("OfxGetNumberOfPlugins");
    auto get_plugin = bundle_.Sym<GetPluginFn>("OfxGetPlugin");
    if (get_count == nullptr || get_plugin == nullptr) {
      return nullptr;
    }
    return FindPluginById(get_plugin, get_count(), id);
  }

  // Creates a ready instance of plugin `id` in `context` on `host_` (see
  // InitPluginInstance()). Call through ASSERT_NO_FATAL_FAILURE().
  void InitInstance(const char* id, const char* context,
                    ScopedPluginInstance* out) {
    InitPluginInstance(&bundle_, id, context, &host_, out);
  }

  // Shortcuts for the two plugins in their only supported contexts.
  void InitCutout(ScopedPluginInstance* out) {
    InitInstance(kCutoutId, kOfxImageEffectContextFilter, out);
  }

  void InitFoldTransition(ScopedPluginInstance* out) {
    InitInstance(kFoldTransitionId, kOfxImageEffectContextTransition, out);
  }

  std::string bundle_path_;
  LoadedOfxBundle bundle_;
  std::string load_error_;  // dlerror() text when bundle_ failed to load.
  MockHost host_;
};

// Verifies the exported plugin table and that internal symbols stay hidden.
TEST_F(PaperCraftOfxTest,
       ExportsCutoutFilterAndFoldTransitionAndHidesInternals) {
  auto get_count = bundle_.Sym<GetNumPluginsFn>("OfxGetNumberOfPlugins");
  auto get_plugin = bundle_.Sym<GetPluginFn>("OfxGetPlugin");
  ASSERT_NE(get_count, nullptr);
  ASSERT_NE(get_plugin, nullptr);

  EXPECT_EQ(get_count(), 2);
  EXPECT_EQ(get_plugin(-1), nullptr);
  EXPECT_EQ(get_plugin(2), nullptr);

  std::vector<std::string> ids;
  for (int i = 0; i < 2; ++i) {
    OfxPlugin* p = get_plugin(i);
    ASSERT_NE(p, nullptr);
    EXPECT_STREQ(p->pluginApi, kOfxImageEffectPluginApi);
    EXPECT_EQ(p->apiVersion, 1);
    EXPECT_EQ(p->pluginVersionMajor, 3u);
    EXPECT_NE(p->setHost, nullptr);
    EXPECT_NE(p->mainEntry, nullptr);
    ids.emplace_back(p->pluginIdentifier);
  }
  EXPECT_EQ(ids, (std::vector<std::string>{
                     "com.papercraft.CutoutFX",
                     "com.papercraft.PaperFoldTransition",
                 }));
  EXPECT_NE(get_plugin(0)->mainEntry, get_plugin(1)->mainEntry);

  // Internal symbols must stay hidden via version script + -fvisibility=hidden.
  EXPECT_EQ(dlsym(bundle_.raw(), "RenderCutoutFrame"), nullptr);
  EXPECT_EQ(dlsym(bundle_.raw(), "RenderPaperFoldTransition"), nullptr);
  EXPECT_EQ(dlsym(bundle_.raw(), "CutoutMainEntry"), nullptr);
}

// Verifies Cutout lifecycle: keyed render + collar, contexts, sticker mode.
TEST_F(PaperCraftOfxTest,
       CutoutFilterLifecycleRendersTornPaperChromaKeyAndSticker) {
  const OfxPlugin* plugin = FindPlugin(kCutoutId);
  ASSERT_NE(plugin, nullptr);

  plugin->setHost(&host_.ofx_host);

  ASSERT_EQ(plugin->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr),
            kOfxStatOK);

  MockEffectObject global_desc;
  global_desc.host = &host_;
  ASSERT_EQ(
      plugin->mainEntry(kOfxActionDescribe, &global_desc, nullptr, nullptr),
      kOfxStatOK);

  char* grouping = nullptr;
  ASSERT_EQ(MockPropGetString(ToPropHandle(&global_desc.props),
                              kOfxImageEffectPluginPropGrouping, 0, &grouping),
            kOfxStatOK);
  EXPECT_STREQ(grouping, "MTTN PaperCraft Studio");

  // 1) Filter context: green-screen subject inside solid green backdrop.
  {
    MockEffectObject filter_desc;
    filter_desc.host = &host_;
    MockPropertySet ctx_in;
    MockPropSetString(ToPropHandle(&ctx_in), kOfxImageEffectPropContext, 0,
                      kOfxImageEffectContextFilter);
    ASSERT_EQ(plugin->mainEntry(kOfxImageEffectActionDescribeInContext,
                                &filter_desc, ToPropHandle(&ctx_in), nullptr),
              kOfxStatOK);

    MockEffectObject inst;
    CloneDescriptorToInstance(filter_desc, &inst, kOfxImageEffectContextFilter);
    ASSERT_EQ(
        plugin->mainEntry(kOfxActionCreateInstance, &inst, nullptr, nullptr),
        kOfxStatOK);

    constexpr int kW = 128;
    constexpr int kH = 96;
    std::vector<float> src_buf(kW * kH * 4);
    std::vector<float> out_buf(kW * kH * 4, -1.0f);
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < kW; ++x) {
        const bool in_subject = (x >= 44 && x < 84 && y >= 28 && y < 68);
        const std::size_t idx = static_cast<std::size_t>(y * kW + x) * 4;
        if (in_subject) {
          src_buf[idx + 0] = 0.85f;
          src_buf[idx + 1] = 0.25f;
          src_buf[idx + 2] = 0.15f;
          src_buf[idx + 3] = 1.0f;
        } else {
          src_buf[idx + 0] = 0.0f;
          src_buf[idx + 1] = 1.0f;
          src_buf[idx + 2] = 0.0f;
          src_buf[idx + 3] = 1.0f;
        }
      }
    }

    const double range[2] = {0.0, 120.0};
    MockPropertySet src_img, out_img;
    ConfigureClipAndImage(&host_, inst.clips.at("Source").get(), &src_img,
                          src_buf.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);
    ConfigureClipAndImage(&host_, inst.clips.at("Output").get(), &out_img,
                          out_buf.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);

    // Enable ChromaKey and hold the resting pose cleanly.
    SetIntParam(&inst, "enableChromaKey", 1);
    SetDoubleParam(&inst, "mediaScale", 1.0);
    SetDoubleParam(&inst, "shadowOpacity", 0.0);
    SetDoubleParam(&inst, "borderWidth", 0.06);

    MockPropertySet pref_out;
    ASSERT_EQ(plugin->mainEntry(kOfxImageEffectActionGetClipPreferences, &inst,
                                nullptr, ToPropHandle(&pref_out)),
              kOfxStatOK);
    int frame_varying = 0;
    EXPECT_EQ(MockPropGetInt(ToPropHandle(&pref_out),
                             kOfxImageEffectFrameVarying, 0, &frame_varying),
              kOfxStatOK);
    EXPECT_EQ(frame_varying, 1);

    MockPropertySet roi_in, roi_out;
    MockPropSetDouble(ToPropHandle(&roi_in), kOfxPropTime, 0, 30.0);
    ASSERT_EQ(
        plugin->mainEntry(kOfxImageEffectActionGetRegionsOfInterest, &inst,
                          ToPropHandle(&roi_in), ToPropHandle(&roi_out)),
        kOfxStatOK);
    double roi[4] = {0, 0, 0, 0};
    EXPECT_EQ(MockPropGetDoubleN(ToPropHandle(&roi_out),
                                 "OfxImageClipPropRoI_Source", 4, roi),
              kOfxStatOK);
    EXPECT_DOUBLE_EQ(roi[2], static_cast<double>(kW));
    EXPECT_DOUBLE_EQ(roi[3], static_cast<double>(kH));

    MockPropertySet render_in;
    MockPropSetDouble(ToPropHandle(&render_in), kOfxPropTime, 0, 30.0);
    const double rs[2] = {1.0, 1.0};
    MockPropSetDoubleN(ToPropHandle(&render_in), kOfxImageEffectPropRenderScale,
                       2, rs);
    const int win[4] = {0, 0, kW, kH};
    MockPropSetIntN(ToPropHandle(&render_in), kOfxImageEffectPropRenderWindow,
                    4, win);
    ASSERT_EQ(plugin->mainEntry(kOfxImageEffectActionRender, &inst,
                                ToPropHandle(&render_in), nullptr),
              kOfxStatOK);
    EXPECT_EQ(host_.active_leased_images, 0);

    // Corners must be transparent (green backdrop keyed out), center opaque
    // red subject, and collar pixel just outside subject must have cream paper
    // border.
    EXPECT_LT(out_buf[3], 0.02f);
    const std::size_t center_idx =
        static_cast<std::size_t>((kH / 2) * kW + kW / 2) * 4;
    EXPECT_GT(out_buf[center_idx + 3], 0.9f);
    EXPECT_GT(out_buf[center_idx + 0], out_buf[center_idx + 1]);
    const std::size_t collar_idx =
        static_cast<std::size_t>((kH / 2) * kW + 41) * 4;
    EXPECT_GT(out_buf[collar_idx + 3], 0.5f);
    EXPECT_GT(out_buf[collar_idx + 2], 0.4f);

    ASSERT_EQ(
        plugin->mainEntry(kOfxActionDestroyInstance, &inst, nullptr, nullptr),
        kOfxStatOK);
  }

  // 2) Only the Filter context is supported: Generator and Transition must be
  // rejected so Resolve does not list Paper Cutout FX under those categories.
  for (const char* ctx :
       {kOfxImageEffectContextGenerator, kOfxImageEffectContextTransition}) {
    MockEffectObject bad_desc;
    bad_desc.host = &host_;
    MockPropertySet ctx_in;
    MockPropSetString(ToPropHandle(&ctx_in), kOfxImageEffectPropContext, 0,
                      ctx);
    EXPECT_EQ(plugin->mainEntry(kOfxImageEffectActionDescribeInContext,
                                &bad_desc, ToPropHandle(&ctx_in), nullptr),
              kOfxStatErrMissingHostFeature)
        << ctx;
  }

  // 3) Filter context with Source Mode = Built-in Paper Sticker renders the
  // procedural sticker without reading the source clip.
  {
    MockEffectObject sticker_desc;
    sticker_desc.host = &host_;
    MockPropertySet ctx_in;
    MockPropSetString(ToPropHandle(&ctx_in), kOfxImageEffectPropContext, 0,
                      kOfxImageEffectContextFilter);
    ASSERT_EQ(plugin->mainEntry(kOfxImageEffectActionDescribeInContext,
                                &sticker_desc, ToPropHandle(&ctx_in), nullptr),
              kOfxStatOK);

    MockEffectObject inst;
    CloneDescriptorToInstance(sticker_desc, &inst,
                              kOfxImageEffectContextFilter);
    ASSERT_EQ(
        plugin->mainEntry(kOfxActionCreateInstance, &inst, nullptr, nullptr),
        kOfxStatOK);

    constexpr int kW = 96;
    constexpr int kH = 96;
    std::vector<float> out_buf(kW * kH * 4, 0.0f);
    const double range[2] = {0.0, 200.0};
    MockPropertySet out_img;
    ConfigureClipAndImage(&host_, inst.clips.at("Output").get(), &out_img,
                          out_buf.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);
    SetIntParam(&inst, "sourceMode", 1);

    MockPropertySet render_in;
    MockPropSetDouble(ToPropHandle(&render_in), kOfxPropTime, 0, 50.0);
    const double rs[2] = {1.0, 1.0};
    MockPropSetDoubleN(ToPropHandle(&render_in), kOfxImageEffectPropRenderScale,
                       2, rs);
    ASSERT_EQ(plugin->mainEntry(kOfxImageEffectActionRender, &inst,
                                ToPropHandle(&render_in), nullptr),
              kOfxStatOK);
    EXPECT_EQ(host_.active_leased_images, 0);

    const std::size_t center =
        static_cast<std::size_t>((kH / 2) * kW + kW / 2) * 4;
    EXPECT_GT(out_buf[center + 3], 0.9f);
    EXPECT_LT(out_buf[3], 0.02f);

    ASSERT_EQ(
        plugin->mainEntry(kOfxActionDestroyInstance, &inst, nullptr, nullptr),
        kOfxStatOK);
  }

  ASSERT_EQ(plugin->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr),
            kOfxStatOK);
}

// Verifies negative-rowBytes 8-bit output matches and keeps canary rows.
TEST_F(PaperCraftOfxTest,
       NegativeRowBytesByteBufferPreservesCanaryGuardRowsAndMatchesPositive) {
  constexpr int kW = 72;
  constexpr int kH = 40;
  constexpr int kStride = kW * 4;
  constexpr std::uint8_t kCanary = 0xA5;
  const double range[2] = {0.0, 24.0};

  MockPropertySet render_in;
  MockPropSetDouble(ToPropHandle(&render_in), kOfxPropTime, 0, 10.0);
  const double rs[2] = {1.0, 1.0};
  MockPropSetDoubleN(ToPropHandle(&render_in), kOfxImageEffectPropRenderScale,
                     2, rs);

  // CutoutFX with 8-bit negative rowBytes and canary guard rows.
  const OfxPlugin* cutout = FindPlugin(kCutoutId);
  ASSERT_NE(cutout, nullptr);
  cutout->setHost(&host_.ofx_host);
  ASSERT_EQ(cutout->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr),
            kOfxStatOK);
  MockEffectObject c_desc;
  c_desc.host = &host_;
  ASSERT_EQ(cutout->mainEntry(kOfxActionDescribe, &c_desc, nullptr, nullptr),
            kOfxStatOK);
  MockPropertySet c_ctx;
  MockPropSetString(ToPropHandle(&c_ctx), kOfxImageEffectPropContext, 0,
                    kOfxImageEffectContextFilter);
  ASSERT_EQ(cutout->mainEntry(kOfxImageEffectActionDescribeInContext, &c_desc,
                              ToPropHandle(&c_ctx), nullptr),
            kOfxStatOK);
  MockEffectObject c_inst;
  CloneDescriptorToInstance(c_desc, &c_inst, kOfxImageEffectContextFilter);
  ASSERT_EQ(
      cutout->mainEntry(kOfxActionCreateInstance, &c_inst, nullptr, nullptr),
      kOfxStatOK);
  SetIntParam(&c_inst, "sourceMode", 1);

  std::vector<std::uint8_t> c_pos(kH * kStride, 0);
  std::vector<std::uint8_t> c_neg((kH + 2) * kStride, kCanary);
  {
    MockPropertySet o_img;
    ConfigureClipAndImage(&host_, c_inst.clips.at("Output").get(), &o_img,
                          c_pos.data(), kW, kH, kStride, kOfxBitDepthByte,
                          range);
    ASSERT_EQ(cutout->mainEntry(kOfxImageEffectActionRender, &c_inst,
                                ToPropHandle(&render_in), nullptr),
              kOfxStatOK);
  }
  {
    MockPropertySet o_img;
    ConfigureClipAndImage(&host_, c_inst.clips.at("Output").get(), &o_img,
                          &c_neg[kH * kStride], kW, kH, -kStride,
                          kOfxBitDepthByte, range);
    ASSERT_EQ(cutout->mainEntry(kOfxImageEffectActionRender, &c_inst,
                                ToPropHandle(&render_in), nullptr),
              kOfxStatOK);
  }
  for (int x = 0; x < kStride; ++x) {
    EXPECT_EQ(c_neg[x], kCanary);
    EXPECT_EQ(c_neg[(kH + 1) * kStride + x], kCanary);
  }
  for (int y = 0; y < kH; ++y) {
    const int phys_y = kH - 1 - y;
    EXPECT_EQ(std::memcmp(&c_neg[(phys_y + 1) * kStride], &c_pos[y * kStride],
                          kStride),
              0)
        << "Cutout mismatch at logical row " << y;
  }
  ASSERT_EQ(
      cutout->mainEntry(kOfxActionDestroyInstance, &c_inst, nullptr, nullptr),
      kOfxStatOK);
  ASSERT_EQ(cutout->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr),
            kOfxStatOK);
}

// Verifies an opaque source with all-zero alpha still renders as opaque.
TEST_F(PaperCraftOfxTest, OpaqueInputClipRendersCleanly) {
  // 1) kOfxImageOpaque camera source whose raw alpha slot is 0.0f everywhere.
  {
    ScopedPluginInstance pi;
    ASSERT_NO_FATAL_FAILURE(InitCutout(&pi));

    constexpr int kW = 96;
    constexpr int kH = 72;
    const double range[2] = {0.0, 120.0};
    std::vector<float> src_buf(kW * kH * 4, 0.0f);
    std::vector<float> out_buf(kW * kH * 4, 0.0f);
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < kW; ++x) {
        const std::size_t idx = static_cast<std::size_t>(y * kW + x) * 4;
        const bool in_subj = (x >= 32 && x < 64 && y >= 20 && y < 52);
        src_buf[idx + 0] = in_subj ? 0.85f : 0.0f;
        src_buf[idx + 1] = in_subj ? 0.25f : 1.0f;
        src_buf[idx + 2] = in_subj ? 0.15f : 0.0f;
        src_buf[idx + 3] =
            0.0f;  // Uninitialized/zero alpha slot on opaque RGB.
      }
    }
    MockPropertySet s_img, o_img;
    ConfigureClipAndImage(&host_, pi.inst.clips.at("Source").get(), &s_img,
                          src_buf.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);
    MockPropSetString(ToPropHandle(&s_img),
                      kOfxImageEffectPropPreMultiplication, 0, kOfxImageOpaque);
    ConfigureClipAndImage(&host_, pi.inst.clips.at("Output").get(), &o_img,
                          out_buf.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);
    SetIntParam(&pi, "enableChromaKey", 1);
    SetDoubleParam(&pi, "shadowOpacity", 0.0);

    ASSERT_EQ(RenderFrame(pi, 30.0), kOfxStatOK);
    const std::size_t center =
        static_cast<std::size_t>((kH / 2) * kW + kW / 2) * 4;
    EXPECT_GT(out_buf[center + 3], 0.9f);
    EXPECT_LT(out_buf[3], 0.02f);
  }
}

// Verifies a cropped source tile lands at its bounds, not stretched to RoD.
TEST_F(PaperCraftOfxTest, CroppedInputTileMapsViaRegionOfDefinition) {
  ScopedPluginInstance pi;
  ASSERT_NO_FATAL_FAILURE(InitCutout(&pi));

  constexpr int kCanvasW = 128;
  constexpr int kCanvasH = 96;
  constexpr int kTileX1 = 32;
  constexpr int kTileY1 = 24;
  constexpr int kTileW = 64;
  constexpr int kTileH = 48;
  const double range[2] = {0.0, 120.0};

  // Fill only the central [32, 96) x [24, 72) tile with bright cyan.
  std::vector<float> tile_buf(kTileW * kTileH * 4, 0.0f);
  for (int i = 0; i < kTileW * kTileH; ++i) {
    tile_buf[i * 4 + 0] = 0.1f;
    tile_buf[i * 4 + 1] = 0.8f;
    tile_buf[i * 4 + 2] = 0.9f;
    tile_buf[i * 4 + 3] = 1.0f;
  }
  std::vector<float> out_buf(kCanvasW * kCanvasH * 4, 0.0f);

  MockPropertySet s_img, o_img;
  ConfigureClipAndImage(&host_, pi.inst.clips.at("Source").get(), &s_img,
                        tile_buf.data(), kTileW, kTileH, kTileW * 16,
                        kOfxBitDepthFloat, range);
  const int tile_bounds[4] = {kTileX1, kTileY1, kTileX1 + kTileW,
                              kTileY1 + kTileH};
  const int full_rod[4] = {0, 0, kCanvasW, kCanvasH};
  MockPropSetIntN(ToPropHandle(&s_img), kOfxImagePropBounds, 4, tile_bounds);
  MockPropSetIntN(ToPropHandle(&s_img), kOfxImagePropRegionOfDefinition, 4,
                  full_rod);
  ConfigureClipAndImage(&host_, pi.inst.clips.at("Output").get(), &o_img,
                        out_buf.data(), kCanvasW, kCanvasH, kCanvasW * 16,
                        kOfxBitDepthFloat, range);

  SetDoubleParam(&pi, "mediaScale", 1.0);
  SetIntParam(&pi, "enableBorder", 0);
  SetDoubleParam(&pi, "shadowOpacity", 0.0);

  ASSERT_EQ(RenderFrame(pi, 30.0), kOfxStatOK);

  // Corner (4, 4) is outside tile_bounds and must stay transparent instead of
  // having the cropped tile stretched across the whole canvas.
  const std::size_t corner_idx = static_cast<std::size_t>(4 * kCanvasW + 4) * 4;
  EXPECT_LT(out_buf[corner_idx + 3], 0.02f);
  const std::size_t center_idx =
      static_cast<std::size_t>((kCanvasH / 2) * kCanvasW + kCanvasW / 2) * 4;
  EXPECT_GT(out_buf[center_idx + 3], 0.95f);
  EXPECT_GT(out_buf[center_idx + 2], 0.5f);
}

// ---------------------------------------------------------------------------
// Paper Fold & Unfold transition.
// ---------------------------------------------------------------------------

// Returns a w x h premultiplied float RGBA buffer filled with (r, g, b, a).
std::vector<float> SolidRgba(int w, int h, float r, float g, float b, float a) {
  std::vector<float> buf(static_cast<std::size_t>(w) * h * 4);
  for (std::size_t i = 0; i < buf.size(); i += 4) {
    buf[i + 0] = r * a;
    buf[i + 1] = g * a;
    buf[i + 2] = b * a;
    buf[i + 3] = a;
  }
  return buf;
}

// Returns the mean alpha of an RGBA float buffer (0 = empty, 1 = covered).
double MeanAlpha(const std::vector<float>& buf) {
  double sum = 0.0;
  for (std::size_t i = 3; i < buf.size(); i += 4) {
    sum += buf[i];
  }
  return sum / static_cast<double>(buf.size() / 4);
}

// Renders the transition at `progress` into a fresh float buffer.
// The buffer starts at -1 so unwritten pixels are detectable. The Output
// clip's binding to the stack-local `out_img` is cleared before returning so
// it never dangles.
std::vector<float> RenderTransitionAt(MockHost* host, ScopedPluginInstance* pi,
                                      int w, int h, double progress) {
  std::vector<float> out(static_cast<std::size_t>(w) * h * 4, -1.0f);
  const double range[2] = {0.0, 24.0};
  MockPropertySet out_img;
  ConfigureClipAndImage(host, pi->inst.clips.at("Output").get(), &out_img,
                        out.data(), w, h, w * 16, kOfxBitDepthFloat, range);
  SetDoubleParam(pi, kOfxImageEffectTransitionParamName, progress);
  EXPECT_EQ(RenderFrame(*pi, 10.0), kOfxStatOK);
  pi->inst.clips.at("Output")->bound_image = nullptr;
  return out;
}

// Verifies the transition describes only its context, clips and parameters.
TEST_F(PaperCraftOfxTest, FoldTransitionDescribesTransitionContextOnly) {
  const OfxPlugin* plugin = FindPlugin(kFoldTransitionId);
  ASSERT_NE(plugin, nullptr);

  plugin->setHost(&host_.ofx_host);
  ASSERT_EQ(plugin->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr),
            kOfxStatOK);

  MockEffectObject desc;
  desc.host = &host_;
  ASSERT_EQ(plugin->mainEntry(kOfxActionDescribe, &desc, nullptr, nullptr),
            kOfxStatOK);
  char* value = nullptr;
  ASSERT_EQ(MockPropGetString(ToPropHandle(&desc.props),
                              kOfxImageEffectPluginPropGrouping, 0, &value),
            kOfxStatOK);
  EXPECT_STREQ(value, "MTTN PaperCraft Studio");
  ASSERT_EQ(
      MockPropGetString(ToPropHandle(&desc.props), kOfxPropLabel, 0, &value),
      kOfxStatOK);
  EXPECT_STREQ(value, "Paper Fold & Unfold");
  ASSERT_EQ(MockPropGetString(ToPropHandle(&desc.props),
                              kOfxImageEffectPropSupportedContexts, 0, &value),
            kOfxStatOK);
  EXPECT_STREQ(value, kOfxImageEffectContextTransition);

  for (const char* ctx :
       {kOfxImageEffectContextFilter, kOfxImageEffectContextGenerator}) {
    MockEffectObject bad_desc;
    bad_desc.host = &host_;
    MockPropertySet ctx_in;
    MockPropSetString(ToPropHandle(&ctx_in), kOfxImageEffectPropContext, 0,
                      ctx);
    EXPECT_EQ(plugin->mainEntry(kOfxImageEffectActionDescribeInContext,
                                &bad_desc, ToPropHandle(&ctx_in), nullptr),
              kOfxStatErrMissingHostFeature)
        << ctx;
  }

  MockPropertySet ctx_in;
  MockPropSetString(ToPropHandle(&ctx_in), kOfxImageEffectPropContext, 0,
                    kOfxImageEffectContextTransition);
  ASSERT_EQ(plugin->mainEntry(kOfxImageEffectActionDescribeInContext, &desc,
                              ToPropHandle(&ctx_in), nullptr),
            kOfxStatOK);
  for (const char* clip : {kOfxImageEffectOutputClipName,
                           kOfxImageEffectTransitionSourceFromClipName,
                           kOfxImageEffectTransitionSourceToClipName}) {
    EXPECT_EQ(desc.clips.count(clip), 1u) << clip;
  }
  for (const char* param :
       {kOfxImageEffectTransitionParamName, "foldAction", "foldPattern",
        "foldStages", "foldDirection", "slideDistance", "enableBorder",
        "paperColor", "stopMotionSteps", "shadowOpacity"}) {
    EXPECT_EQ(desc.param_set.params.count(param), 1u) << param;
  }
  ASSERT_EQ(plugin->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr),
            kOfxStatOK);
}

// Verifies p=0/1 render the clips unchanged and isIdentity names them.
TEST_F(PaperCraftOfxTest, FoldTransitionEndpointsMatchClipsAndReportIdentity) {
  ScopedPluginInstance pi;
  ASSERT_NO_FATAL_FAILURE(InitFoldTransition(&pi));

  constexpr int kW = 80;
  constexpr int kH = 48;
  const double range[2] = {0.0, 24.0};
  std::vector<float> from = SolidRgba(kW, kH, 0.1f, 0.2f, 0.9f, 1.0f);
  std::vector<float> to = SolidRgba(kW, kH, 0.9f, 0.2f, 0.1f, 1.0f);
  MockPropertySet from_img, to_img;
  ConfigureClipAndImage(&host_, pi.inst.clips.at("SourceFrom").get(), &from_img,
                        from.data(), kW, kH, kW * 16, kOfxBitDepthFloat, range);
  ConfigureClipAndImage(&host_, pi.inst.clips.at("SourceTo").get(), &to_img,
                        to.data(), kW, kH, kW * 16, kOfxBitDepthFloat, range);

  EXPECT_EQ(RenderTransitionAt(&host_, &pi, kW, kH, 0.0), from);
  EXPECT_EQ(RenderTransitionAt(&host_, &pi, kW, kH, 1.0), to);
  const std::vector<float> mid = RenderTransitionAt(&host_, &pi, kW, kH, 0.5);
  EXPECT_NE(mid, from);
  EXPECT_NE(mid, to);
  EXPECT_EQ(host_.active_leased_images, 0);

  auto identity_clip = [&](double progress) -> std::string {
    SetDoubleParam(&pi, kOfxImageEffectTransitionParamName, progress);
    MockPropertySet in_args, out_args;
    MockPropSetDouble(ToPropHandle(&in_args), kOfxPropTime, 0, 10.0);
    const OfxStatus st =
        pi.plugin->mainEntry(kOfxImageEffectActionIsIdentity, &pi.inst,
                             ToPropHandle(&in_args), ToPropHandle(&out_args));
    if (st != kOfxStatOK) {
      return "";
    }
    char* name = nullptr;
    MockPropGetString(ToPropHandle(&out_args), kOfxPropName, 0, &name);
    return name != nullptr ? name : "";
  };
  EXPECT_EQ(identity_clip(0.0), kOfxImageEffectTransitionSourceFromClipName);
  EXPECT_EQ(identity_clip(1.0), kOfxImageEffectTransitionSourceToClipName);
  EXPECT_EQ(identity_clip(0.5), "");
}

// Verifies Auto unfolds over empty at clip start and folds away at clip end.
TEST_F(PaperCraftOfxTest, FoldTransitionAutoUnfoldsAtClipStartAndFoldsAtEnd) {
  constexpr int kW = 96;
  constexpr int kH = 64;
  const double range[2] = {0.0, 24.0};
  std::vector<float> media = SolidRgba(kW, kH, 0.1f, 0.3f, 0.9f, 1.0f);
  std::vector<float> empty(static_cast<std::size_t>(kW) * kH * 4, 0.0f);

  // Start of a clip: transparent outgoing side, media incoming.
  {
    ScopedPluginInstance pi;
    ASSERT_NO_FATAL_FAILURE(InitFoldTransition(&pi));
    SetIntParam(&pi, "enableStopMotion", 0);
    MockPropertySet from_img, to_img;
    ConfigureClipAndImage(&host_, pi.inst.clips.at("SourceFrom").get(),
                          &from_img, empty.data(), kW, kH, kW * 16,
                          kOfxBitDepthFloat, range);
    ConfigureClipAndImage(&host_, pi.inst.clips.at("SourceTo").get(), &to_img,
                          media.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);
    double prev = 0.0;
    for (double p : {0.3, 0.55, 0.8}) {
      const std::vector<float> out = RenderTransitionAt(&host_, &pi, kW, kH, p);
      EXPECT_GE(*std::min_element(out.begin(), out.end()), 0.0f)
          << "unwritten pixels at " << p;
      const double cov = MeanAlpha(out);
      EXPECT_GT(cov, prev + 0.02) << "unfold coverage must grow at " << p;
      EXPECT_LT(cov, 0.999) << p;
      prev = cov;
    }
  }

  // End of a clip: the host has no incoming image at all.
  {
    ScopedPluginInstance pi;
    ASSERT_NO_FATAL_FAILURE(InitFoldTransition(&pi));
    SetIntParam(&pi, "enableStopMotion", 0);
    MockPropertySet from_img;
    ConfigureClipAndImage(&host_, pi.inst.clips.at("SourceFrom").get(),
                          &from_img, media.data(), kW, kH, kW * 16,
                          kOfxBitDepthFloat, range);
    double prev = 1.0;
    for (double p : {0.2, 0.45, 0.7}) {
      const double cov = MeanAlpha(RenderTransitionAt(&host_, &pi, kW, kH, p));
      EXPECT_LT(cov, prev - 0.02) << "fold coverage must shrink at " << p;
      EXPECT_GT(cov, 0.0) << p;
      prev = cov;
    }
    EXPECT_EQ(MeanAlpha(RenderTransitionAt(&host_, &pi, kW, kH, 1.0)), 0.0);
  }
  EXPECT_EQ(host_.active_leased_images, 0);
}

// Verifies folded flaps show the cream paper back, with or without border.
TEST_F(PaperCraftOfxTest, FoldTransitionShowsPaperBacksideWhileFolded) {
  ScopedPluginInstance pi;
  ASSERT_NO_FATAL_FAILURE(InitFoldTransition(&pi));

  constexpr int kW = 96;
  constexpr int kH = 64;
  const double range[2] = {0.0, 24.0};
  // Pure blue media can never produce warm cream pixels by itself.
  std::vector<float> blue = SolidRgba(kW, kH, 0.0f, 0.0f, 1.0f, 1.0f);
  std::vector<float> empty(static_cast<std::size_t>(kW) * kH * 4, 0.0f);
  MockPropertySet from_img, to_img;
  ConfigureClipAndImage(&host_, pi.inst.clips.at("SourceFrom").get(), &from_img,
                        empty.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                        range);
  ConfigureClipAndImage(&host_, pi.inst.clips.at("SourceTo").get(), &to_img,
                        blue.data(), kW, kH, kW * 16, kOfxBitDepthFloat, range);

  for (int border : {1, 0}) {
    SetIntParam(&pi, "enableBorder", border);
    // Early in the unfold the top half is still folded over, back side up.
    const std::vector<float> out = RenderTransitionAt(&host_, &pi, kW, kH, 0.3);
    int paper_pixels = 0;
    for (std::size_t i = 0; i < out.size(); i += 4) {
      if (out[i + 3] > 0.9f && out[i] > 0.45f && out[i + 1] > 0.45f &&
          out[i] > out[i + 2]) {
        ++paper_pixels;
      }
    }
    EXPECT_GT(paper_pixels, kW * kH / 20) << "border=" << border;
  }
}

// Verifies stop-motion holds one pose within a step and changes across it.
TEST_F(PaperCraftOfxTest, FoldTransitionStopMotionHoldsPosesBetweenSteps) {
  ScopedPluginInstance pi;
  ASSERT_NO_FATAL_FAILURE(InitFoldTransition(&pi));
  SetIntParam(&pi, "stopMotionSteps", 4);

  constexpr int kW = 64;
  constexpr int kH = 48;
  const double range[2] = {0.0, 24.0};
  std::vector<float> from = SolidRgba(kW, kH, 0.1f, 0.2f, 0.9f, 1.0f);
  std::vector<float> to = SolidRgba(kW, kH, 0.9f, 0.6f, 0.1f, 1.0f);
  MockPropertySet from_img, to_img;
  ConfigureClipAndImage(&host_, pi.inst.clips.at("SourceFrom").get(), &from_img,
                        from.data(), kW, kH, kW * 16, kOfxBitDepthFloat, range);
  ConfigureClipAndImage(&host_, pi.inst.clips.at("SourceTo").get(), &to_img,
                        to.data(), kW, kH, kW * 16, kOfxBitDepthFloat, range);

  const std::vector<float> a = RenderTransitionAt(&host_, &pi, kW, kH, 0.30);
  const std::vector<float> b = RenderTransitionAt(&host_, &pi, kW, kH, 0.45);
  const std::vector<float> c = RenderTransitionAt(&host_, &pi, kW, kH, 0.55);
  EXPECT_EQ(a, b) << "progress 0.30 and 0.45 share one stop-motion step";
  EXPECT_NE(b, c);
}

// Verifies all patterns/stages/actions render, plus 8-bit negative rowBytes.
TEST_F(PaperCraftOfxTest, FoldTransitionPatternsActionsAndByteDepthRender) {
  ScopedPluginInstance pi;
  ASSERT_NO_FATAL_FAILURE(InitFoldTransition(&pi));

  constexpr int kW = 72;
  constexpr int kH = 48;
  const double range[2] = {0.0, 24.0};
  std::vector<float> from = SolidRgba(kW, kH, 0.1f, 0.2f, 0.9f, 1.0f);
  std::vector<float> to = SolidRgba(kW, kH, 0.9f, 0.2f, 0.1f, 1.0f);
  MockPropertySet from_img, to_img;
  ConfigureClipAndImage(&host_, pi.inst.clips.at("SourceFrom").get(), &from_img,
                        from.data(), kW, kH, kW * 16, kOfxBitDepthFloat, range);
  ConfigureClipAndImage(&host_, pi.inst.clips.at("SourceTo").get(), &to_img,
                        to.data(), kW, kH, kW * 16, kOfxBitDepthFloat, range);

  for (int pattern = 0; pattern < 4; ++pattern) {
    for (int stages : {2, 5}) {
      for (int action : {1, 2, 3}) {
        SetIntParam(&pi, "foldPattern", pattern);
        SetIntParam(&pi, "foldStages", stages);
        SetIntParam(&pi, "foldAction", action);
        const std::vector<float> out =
            RenderTransitionAt(&host_, &pi, kW, kH, 0.6);
        EXPECT_GE(*std::min_element(out.begin(), out.end()), 0.0f);
        EXPECT_GT(MeanAlpha(out), 0.05)
            << "pattern=" << pattern << " stages=" << stages
            << " action=" << action;
      }
    }
  }

  // 8-bit clips with a negative rowBytes output buffer.
  SetIntParam(&pi, "foldAction", 1);
  std::vector<std::uint8_t> from8(static_cast<std::size_t>(kW) * kH * 4, 255);
  std::vector<std::uint8_t> out8((kH + 2) * kW * 4, 0xA5);
  for (std::size_t i = 0; i < from8.size(); i += 4) {
    from8[i] = 20;
    from8[i + 1] = 40;
  }
  MockPropertySet from8_img, to8_img, out8_img;
  ConfigureClipAndImage(&host_, pi.inst.clips.at("SourceFrom").get(),
                        &from8_img, from8.data(), kW, kH, kW * 4,
                        kOfxBitDepthByte, range);
  ConfigureClipAndImage(&host_, pi.inst.clips.at("SourceTo").get(), &to8_img,
                        nullptr, kW, kH, kW * 4, kOfxBitDepthByte, range);
  pi.inst.clips.at("SourceTo")->bound_image = nullptr;  // No incoming image.
  ConfigureClipAndImage(&host_, pi.inst.clips.at("Output").get(), &out8_img,
                        &out8[kH * kW * 4], kW, kH, -kW * 4, kOfxBitDepthByte,
                        range);
  SetDoubleParam(&pi, kOfxImageEffectTransitionParamName, 0.5);
  ASSERT_EQ(RenderFrame(pi, 10.0), kOfxStatOK);
  for (int x = 0; x < kW * 4; ++x) {
    EXPECT_EQ(out8[x], 0xA5);
    EXPECT_EQ(out8[(kH + 1) * kW * 4 + x], 0xA5);
  }
  EXPECT_EQ(host_.active_leased_images, 0);
}

// Verifies Cutout output feeds the transition without double collar or pops.
TEST_F(PaperCraftOfxTest,
       CutoutEffectAndFoldTransitionCombineSeamlesslyOnSameClip) {
  constexpr int kW = 192;
  constexpr int kH = 128;
  const double range[2] = {0.0, 48.0};

  // 1. Default CutoutFX renders a resting cutout + drop shadow on frame 0
  //    (the cutout has no intro/outro, so timeline transitions never
  //    collide).
  std::vector<float> cutout_buf(static_cast<std::size_t>(kW) * kH * 4, 0.0f);
  {
    ScopedPluginInstance cutout_pi;
    ASSERT_NO_FATAL_FAILURE(InitCutout(&cutout_pi));
    SetIntParam(&cutout_pi, "sourceMode", 1);  // Built-in Paper Sticker.
    SetDoubleParam(&cutout_pi, "shadowDistance", 0.06);
    SetDoubleParam(&cutout_pi, "shadowSoftness", 0.03);
    MockPropertySet out_img;
    ConfigureClipAndImage(&host_, cutout_pi.inst.clips.at("Output").get(),
                          &out_img, cutout_buf.data(), kW, kH, kW * 16,
                          kOfxBitDepthFloat, range);
    ASSERT_EQ(RenderFrame(cutout_pi, 0.0), kOfxStatOK);
  }
  EXPECT_GT(MeanAlpha(cutout_buf), 0.08)
      << "Default CutoutFX must not collapse frame 0 to transparent black";

  // Count pure-black shadow pixels vs bright cream pixels in cutout_buf.
  auto count_shadow_px = [](const std::vector<float>& buf) {
    int n = 0;
    for (std::size_t i = 0; i < buf.size(); i += 4) {
      const float max_c = std::max({buf[i], buf[i + 1], buf[i + 2]});
      if (buf[i + 3] >= 0.05f && buf[i + 3] <= 0.90f && max_c <= 0.01f) {
        ++n;
      }
    }
    return n;
  };
  const int resting_shadow_px = count_shadow_px(cutout_buf);
  ASSERT_GT(resting_shadow_px, 40);

  // 2. Feed CutoutFX output into PaperFoldTransition on the same clip:
  //    - No second cream collar wraps around the drop shadow.
  //    - Shadow stays alive at p = 0.96 instead of fading to 0 via settle.
  //    - Endpoints p = 0 and p = 1 match empty and cutout_buf 1:1.
  std::vector<float> empty(static_cast<std::size_t>(kW) * kH * 4, 0.0f);
  {
    ScopedPluginInstance fold_pi;
    ASSERT_NO_FATAL_FAILURE(InitFoldTransition(&fold_pi));
    SetIntParam(&fold_pi, "enableStopMotion", 0);
    SetDoubleParam(&fold_pi, "slideDistance", 0.0);
    MockPropertySet from_img, to_img;
    ConfigureClipAndImage(&host_, fold_pi.inst.clips.at("SourceFrom").get(),
                          &from_img, empty.data(), kW, kH, kW * 16,
                          kOfxBitDepthFloat, range);
    ConfigureClipAndImage(&host_, fold_pi.inst.clips.at("SourceTo").get(),
                          &to_img, cutout_buf.data(), kW, kH, kW * 16,
                          kOfxBitDepthFloat, range);

    EXPECT_EQ(RenderTransitionAt(&host_, &fold_pi, kW, kH, 0.0), empty);
    EXPECT_EQ(RenderTransitionAt(&host_, &fold_pi, kW, kH, 1.0), cutout_buf);

    // At p = 0.80, pixels where cutout_buf had ONLY pure-black shadow (far
    // outside the sticker's stationary lower-right shadow skirt) must never be
    // turned into opaque cream cardstock by a second border around the shadow.
    const std::vector<float> near_open =
        RenderTransitionAt(&host_, &fold_pi, kW, kH, 0.82);
    int cream_over_shadow_skirt = 0;
    for (std::size_t i = 0; i < cutout_buf.size(); i += 4) {
      const float c_max =
          std::max({cutout_buf[i], cutout_buf[i + 1], cutout_buf[i + 2]});
      if (cutout_buf[i + 3] >= 0.15f && cutout_buf[i + 3] <= 0.65f &&
          c_max <= 1e-3f) {
        if (near_open[i + 3] > 0.9f && near_open[i] > 0.7f) {
          ++cream_over_shadow_skirt;
        }
      }
    }
    EXPECT_EQ(cream_over_shadow_skirt, 0)
        << "Drop shadow must not be wrapped in a second cream paper border";

    // At p = 0.96 (settle near 0), the cutout's shadow must remain present
    // rather than vanishing before p = 1.0.
    const std::vector<float> landing =
        RenderTransitionAt(&host_, &fold_pi, kW, kH, 0.96);
    EXPECT_GT(count_shadow_px(landing), resting_shadow_px / 2)
        << "Cutout shadow must hand off continuously instead of fading to 0";
  }

  // 3. A raw graphic with anti-aliased black strokes + color (and NO drop
  //    shadow) must NOT be misclassified as a shadowed cutout: enabling the
  //    transition border still wraps a temporary cardstock collar around it.
  {
    std::vector<float> raw_logo(static_cast<std::size_t>(kW) * kH * 4, 0.0f);
    for (int y = 40; y < 88; ++y) {
      for (int x = 56; x < 136; ++x) {
        const std::size_t i =
            (static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)) *
            4;
        const bool edge = (x == 56 || x == 135 || y == 40 || y == 87);
        const float a = edge ? 0.5f : 1.0f;
        // Left half: pure black with 1-px AA edge; right half: purple.
        if (x < 96) {
          raw_logo[i + 3] = a;
        } else {
          raw_logo[i + 0] = 0.5f * a;
          raw_logo[i + 1] = 0.3f * a;
          raw_logo[i + 2] = 0.8f * a;
          raw_logo[i + 3] = a;
        }
      }
    }
    ScopedPluginInstance fold_pi;
    ASSERT_NO_FATAL_FAILURE(InitFoldTransition(&fold_pi));
    SetIntParam(&fold_pi, "enableStopMotion", 0);
    SetDoubleParam(&fold_pi, "slideDistance", 0.0);
    MockPropertySet from_img, to_img;
    ConfigureClipAndImage(&host_, fold_pi.inst.clips.at("SourceFrom").get(),
                          &from_img, empty.data(), kW, kH, kW * 16,
                          kOfxBitDepthFloat, range);
    ConfigureClipAndImage(&host_, fold_pi.inst.clips.at("SourceTo").get(),
                          &to_img, raw_logo.data(), kW, kH, kW * 16,
                          kOfxBitDepthFloat, range);

    SetIntParam(&fold_pi, "enableBorder", 1);
    const std::vector<float> with_border =
        RenderTransitionAt(&host_, &fold_pi, kW, kH, 0.65);
    SetIntParam(&fold_pi, "enableBorder", 0);
    const std::vector<float> no_border =
        RenderTransitionAt(&host_, &fold_pi, kW, kH, 0.65);
    EXPECT_GT(MeanAlpha(with_border), MeanAlpha(no_border) + 0.01)
        << "Raw black+color logo without shadow must still get transition "
           "collar";
  }
}

// ---------------------------------------------------------------------------
// Regression tests: binding, render scale, threading fallback, Auto action.
// ---------------------------------------------------------------------------

// Verifies CreateInstance looks up every described parameter and clip, so a
// typo in a parameter ID cannot silently fall back to the default value.
// Secret (retired) parameters are the exception: they must stay unbound so
// their saved values can never reach the kernel.
TEST_F(PaperCraftOfxTest, CreateInstanceBindsEveryDescribedParameterAndClip) {
  const std::pair<const char*, const char*> plugins[] = {
      {"com.papercraft.CutoutFX", kOfxImageEffectContextFilter},
      {kFoldTransitionId, kOfxImageEffectContextTransition},
  };
  for (const auto& plugin : plugins) {
    const char* id = plugin.first;
    ScopedPluginInstance pi;
    ASSERT_NO_FATAL_FAILURE(InitInstance(id, plugin.second, &pi));
    int value_params = 0;
    for (const auto& [name, param] : pi.inst.param_set.params) {
      // Groups and pages only organize the UI; they have no value to read.
      if (param->type == kOfxParamTypeGroup ||
          param->type == kOfxParamTypePage) {
        continue;
      }
      if (IsSecretParam(*param)) {
        EXPECT_EQ(pi.inst.param_set.looked_up.count(name), 0u)
            << id << ": secret parameter \"" << name << "\" is bound";
        continue;
      }
      ++value_params;
      EXPECT_EQ(pi.inst.param_set.looked_up.count(name), 1u)
          << id << ": parameter \"" << name
          << "\" is described but never bound in CreateInstance";
    }
    EXPECT_GT(value_params, 10) << id;
    for (const auto& [name, clip] : pi.inst.clips) {
      EXPECT_EQ(pi.inst.looked_up_clips.count(name), 1u)
          << id << ": clip \"" << name
          << "\" is described but never bound in CreateInstance";
    }
  }
}

// Expected default of one described value parameter, taken from the kernel
// struct. `values` has one entry per component (1 for bool/int/choice/double,
// 2 for 2D, 3 for RGB); booleans are 0/1.
struct KernelDefault {
  const char* id;
  std::vector<double> values;
};

// One intentional difference between a plugin default and the kernel struct
// default: the value the host is given and the kernel struct's value.
struct DefaultException {
  const char* id;
  double plugin_value;
  double kernel_value;
};

// Converts a kernel bool default to the OpenFX 0/1 form.
double BoolDefault(bool b) { return b ? 1.0 : 0.0; }

// Expected defaults of every visible Paper Cutout FX value parameter, read
// from a default-constructed CutoutParams. The secret legacy intro/outro
// parameters have no kernel field (see kLegacyCutoutParamIds).
std::vector<KernelDefault> CutoutKernelDefaults() {
  const CutoutParams k;
  return {
      {"sourceMode", {static_cast<double>(k.source_mode)}},
      {"sourcePremult", {BoolDefault(k.source_is_premultiplied)}},
      {"mediaScale", {k.media_scale}},
      {"mediaOffset", {k.media_offset_x, k.media_offset_y}},
      {"mediaAngle", {k.media_angle_deg}},
      {"enableChromaKey", {BoolDefault(k.enable_chroma_key)}},
      {"keyColor", {k.key_r, k.key_g, k.key_b}},
      {"keyGain", {k.key_gain}},
      {"keyBalance", {k.key_balance}},
      {"edgeChokePx", {k.edge_choke_px}},
      {"enableBorder", {BoolDefault(k.enable_border)}},
      {"borderWidth", {k.border_width_norm}},
      {"tearRoughness", {k.tear_roughness_norm}},
      {"tearFrequency", {k.tear_frequency}},
      {"tearDetail", {static_cast<double>(k.tear_detail)}},
      {"edgeBoil", {k.edge_boil}},
      {"paperColor", {k.paper_r, k.paper_g, k.paper_b}},
      {"grainStrength", {k.grain_strength}},
      {"grainScale", {k.grain_scale}},
      {"creaseStrength", {k.crease_strength}},
      {"creaseScale", {k.crease_scale}},
      {"mediaTextureOverlay", {k.media_texture_overlay}},
      {"randomSeed", {static_cast<double>(k.random_seed)}},
      {"shadowOpacity", {k.shadow_opacity}},
      {"shadowAngle", {k.shadow_angle_deg}},
      {"shadowDistance", {k.shadow_distance_norm}},
      {"shadowSoftness", {k.shadow_softness_norm}},
      {"enableStopMotion", {BoolDefault(k.enable_stop_motion)}},
      {"holdFrames", {static_cast<double>(k.hold_frames)}},
      {"jitterPos", {k.jitter_pos_norm}},
      {"jitterRot", {k.jitter_rot_deg}},
      {"masterPos", {k.master_pos_x, k.master_pos_y}},
      {"masterScale", {k.master_scale}},
      {"masterAngle", {k.master_angle_deg}},
      {"flipH", {BoolDefault(k.flip_h)}},
      {"flipV", {BoolDefault(k.flip_v)}},
  };
}

// Expected defaults of every Paper Fold & Unfold value parameter, read from a
// default-constructed PaperFoldTransitionParams.
std::vector<KernelDefault> FoldTransitionKernelDefaults() {
  const PaperFoldTransitionParams k;
  return {
      {kOfxImageEffectTransitionParamName, {k.progress}},
      {"foldAction", {static_cast<double>(k.fold_action)}},
      {"foldPattern", {static_cast<double>(k.fold_pattern)}},
      {"foldStages", {static_cast<double>(k.fold_stages)}},
      {"foldDirection", {k.direction_angle_deg}},
      {"foldDepth", {k.fold_depth}},
      {"slideDistance", {k.slide_distance}},
      {"mirrorTransH", {BoolDefault(k.mirror_h)}},
      {"mirrorTransV", {BoolDefault(k.mirror_v)}},
      {"enableBorder", {BoolDefault(k.enable_border)}},
      {"borderWidth", {k.border_width_norm}},
      {"tearRoughness", {k.tear_roughness_norm}},
      {"tearFrequency", {k.tear_frequency}},
      {"paperColor", {k.paper_r, k.paper_g, k.paper_b}},
      {"grainStrength", {k.grain_strength}},
      {"creaseStrength", {k.crease_strength}},
      {"randomSeed", {static_cast<double>(k.random_seed)}},
      {"sourcePremult", {BoolDefault(k.source_is_premultiplied)}},
      {"enableStopMotion", {BoolDefault(k.enable_stop_motion)}},
      {"stopMotionSteps", {static_cast<double>(k.stop_motion_steps)}},
      {"jitterAmount", {k.jitter_amount}},
      {"shadowOpacity", {k.shadow_opacity}},
      {"shadowAngle", {k.shadow_angle_deg}},
      {"shadowDistance", {k.shadow_distance_norm}},
      {"shadowSoftness", {k.shadow_softness_norm}},
  };
}

// Returns property `name` of a described parameter as doubles (int-valued
// properties are converted), or an empty vector when it is not set.
std::vector<double> ParamPropAsDoubles(const MockParam& param,
                                       const char* name) {
  const auto it = param.props.map.find(name);
  if (it == param.props.map.end()) {
    return {};
  }
  if (it->second.kind == PropValue::Kind::kInt) {
    return std::vector<double>(it->second.ints.begin(), it->second.ints.end());
  }
  return it->second.doubles;
}

// Verifies the plugin's parameter tables cannot drift from the kernel
// structs: every visible described value parameter has a kernel
// counterpart (secret legacy parameters have none), its
// default equals the CutoutParams / PaperFoldTransitionParams default (as
// the float/int the kernel stores), and that default lies inside the hard
// limits, the slider range and the choice options. Intentional differences
// are listed in kExceptions and checked on both sides, so the list stays
// accurate:
//   - Transition progress ("Transition"): the kernel's 0.5 is a mid-transition
//     value convenient for direct kernel calls; the host-animated OpenFX
//     progress parameter starts at 0 like any OpenFX transition.
TEST_F(PaperCraftOfxTest, DescribedParameterDefaultsMatchKernelStructs) {
  const DefaultException kExceptions[] = {
      {kOfxImageEffectTransitionParamName, 0.0, 0.5},
  };
  struct PluginCase {
    const char* id;
    const char* context;
    std::vector<KernelDefault> expected;
    bool transition;
  };
  const PluginCase cases[] = {
      {kCutoutId, kOfxImageEffectContextFilter, CutoutKernelDefaults(), false},
      {kFoldTransitionId, kOfxImageEffectContextTransition,
       FoldTransitionKernelDefaults(), true},
  };
  for (const PluginCase& c : cases) {
    SCOPED_TRACE(c.id);
    ScopedPluginInstance pi;
    ASSERT_NO_FATAL_FAILURE(InitInstance(c.id, c.context, &pi));
    const auto& params = pi.desc.param_set.params;

    std::set<std::string> expected_ids;
    for (const KernelDefault& kd : c.expected) {
      expected_ids.insert(kd.id);
    }
    for (const auto& [name, param] : params) {
      if (param->type != kOfxParamTypeGroup &&
          param->type != kOfxParamTypePage && !IsSecretParam(*param)) {
        EXPECT_EQ(expected_ids.count(name), 1u)
            << "parameter \"" << name
            << "\" has no kernel default in this test; add it";
      }
    }

    for (const KernelDefault& kd : c.expected) {
      SCOPED_TRACE(kd.id);
      const auto it = params.find(kd.id);
      ASSERT_TRUE(it != params.end()) << "not described";
      const MockParam& param = *it->second;
      const std::vector<double> def =
          ParamPropAsDoubles(param, kOfxParamPropDefault);
      ASSERT_EQ(def.size(), kd.values.size()) << "component count";

      const DefaultException* exception = nullptr;
      for (const DefaultException& e : kExceptions) {
        if (c.transition && std::strcmp(e.id, kd.id) == 0) {
          exception = &e;
        }
      }
      const bool is_int = param.type == kOfxParamTypeInteger ||
                          param.type == kOfxParamTypeBoolean ||
                          param.type == kOfxParamTypeChoice;
      for (std::size_t i = 0; i < def.size(); ++i) {
        if (exception != nullptr) {
          EXPECT_EQ(def[i], exception->plugin_value) << "documented exception";
          EXPECT_EQ(kd.values[i], exception->kernel_value)
              << "documented exception (kernel side)";
          continue;
        }
        // Compare in the type the kernel stores: the plugin narrows doubles
        // to float when it fills the struct.
        if (is_int) {
          EXPECT_EQ(static_cast<int>(def[i]), static_cast<int>(kd.values[i]))
              << "component " << i;
        } else {
          EXPECT_EQ(static_cast<float>(def[i]),
                    static_cast<float>(kd.values[i]))
              << "component " << i;
        }
      }

      // The kernel default must be reachable from the UI.
      const std::vector<double> mins =
          ParamPropAsDoubles(param, kOfxParamPropMin);
      const std::vector<double> maxs =
          ParamPropAsDoubles(param, kOfxParamPropMax);
      const std::vector<double> dmins =
          ParamPropAsDoubles(param, kOfxParamPropDisplayMin);
      const std::vector<double> dmaxs =
          ParamPropAsDoubles(param, kOfxParamPropDisplayMax);
      if (param.type == kOfxParamTypeBoolean) {
        EXPECT_TRUE(mins.empty() && maxs.empty());
      } else if (param.type == kOfxParamTypeChoice) {
        const auto opt = param.props.map.find(kOfxParamPropChoiceOption);
        ASSERT_TRUE(opt != param.props.map.end()) << "no choice options";
        EXPECT_GE(kd.values[0], 0.0);
        EXPECT_LT(kd.values[0],
                  static_cast<double>(opt->second.strings.size()));
      } else {
        ASSERT_EQ(mins.size(), def.size());
        ASSERT_EQ(maxs.size(), def.size());
        ASSERT_EQ(dmins.size(), def.size());
        ASSERT_EQ(dmaxs.size(), def.size());
        for (std::size_t i = 0; i < def.size(); ++i) {
          EXPECT_LE(mins[i], kd.values[i]) << "component " << i;
          EXPECT_GE(maxs[i], kd.values[i]) << "component " << i;
          EXPECT_LE(dmins[i], kd.values[i]) << "component " << i;
          EXPECT_GE(dmaxs[i], kd.values[i]) << "component " << i;
        }
      }
    }
  }
}

// Verifies the retired intro/outro parameters are ignored: setting all of
// them on an instance (values that used to collapse, slide, or mirror the
// cutout at these frames) leaves every rendered frame byte-identical.
TEST_F(PaperCraftOfxTest, CutoutIgnoresLegacyIntroOutroParams) {
  ScopedPluginInstance pi;
  ASSERT_NO_FATAL_FAILURE(InitCutout(&pi));

  constexpr int kW = 96;
  constexpr int kH = 64;
  const double range[2] = {0.0, 40.0};
  SetIntParam(&pi, "sourceMode", 1);  // Built-in Paper Sticker.

  // Frames at the clip start, inside the old intro/outro windows, and at
  // the clip end.
  const double kTimes[] = {0.0, 3.0, 20.0, 37.0, 40.0};
  auto render_all = [&]() {
    std::vector<std::vector<float>> frames;
    for (double t : kTimes) {
      std::vector<float> buf(static_cast<std::size_t>(kW) * kH * 4, -1.0f);
      MockPropertySet o_img;
      ConfigureClipAndImage(&host_, pi.inst.clips.at("Output").get(), &o_img,
                            buf.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                            range);
      EXPECT_EQ(RenderFrame(pi, t), kOfxStatOK) << "t=" << t;
      frames.push_back(std::move(buf));
    }
    return frames;
  };

  const std::vector<std::vector<float>> resting = render_all();
  for (std::size_t i = 0; i < resting.size(); ++i) {
    EXPECT_GT(MeanAlpha(resting[i]), 0.05) << "t=" << kTimes[i];
  }

  SetIntParam(&pi, "introStyle", 1);  // Was Paper Unfold (3D).
  SetIntParam(&pi, "introFrames", 30);
  SetIntParam(&pi, "outroStyle", 2);  // Was Crumple Ball.
  SetIntParam(&pi, "outroFrames", 30);
  SetDoubleParam(&pi, "transitionAngle", 45.0);
  SetDoubleParam(&pi, "glideDistance", 1.2);
  SetIntParam(&pi, "mirrorTransH", 1);
  SetIntParam(&pi, "mirrorTransV", 1);
  const std::vector<std::vector<float>> with_legacy = render_all();
  ASSERT_EQ(with_legacy.size(), resting.size());
  for (std::size_t i = 0; i < resting.size(); ++i) {
    EXPECT_TRUE(with_legacy[i] == resting[i]) << "t=" << kTimes[i];
  }
}

// Verifies the Cutout describes the 8 retired intro/outro IDs as secret,
// top-level parameters (in no group), that they are the only secret ones,
// and that every visible value parameter sits in a group. The Paper Fold &
// Unfold transition keeps its own mirrorTransH / mirrorTransV visible.
TEST_F(PaperCraftOfxTest, CutoutLegacyIntroOutroParamsAreSecretAndUngrouped) {
  ScopedPluginInstance pi;
  ASSERT_NO_FATAL_FAILURE(InitCutout(&pi));
  const auto& params = pi.desc.param_set.params;
  const std::set<std::string> legacy(std::begin(kLegacyCutoutParamIds),
                                     std::end(kLegacyCutoutParamIds));
  EXPECT_EQ(legacy.size(), 8u);
  for (const std::string& id : legacy) {
    SCOPED_TRACE(id);
    const auto it = params.find(id);
    ASSERT_TRUE(it != params.end()) << "legacy ID must stay defined";
    EXPECT_TRUE(IsSecretParam(*it->second));
    EXPECT_EQ(ParamStringProp(*it->second, kOfxParamPropParent), "");
  }
  for (const auto& [name, param] : params) {
    SCOPED_TRACE(name);
    EXPECT_EQ(IsSecretParam(*param), legacy.count(name) == 1);
    if (param->type != kOfxParamTypeGroup && legacy.count(name) == 0) {
      const std::string parent = ParamStringProp(*param, kOfxParamPropParent);
      const auto group = params.find(parent);
      ASSERT_TRUE(group != params.end()) << "visible parameter has no group";
      EXPECT_EQ(group->second->type, kOfxParamTypeGroup);
      EXPECT_FALSE(IsSecretParam(*group->second));
    }
  }
  // The retired "5. Intro & Outro Paper Animations" group is gone.
  EXPECT_EQ(params.count("TransitionGroup"), 0u);

  ScopedPluginInstance fold;
  ASSERT_NO_FATAL_FAILURE(InitFoldTransition(&fold));
  for (const char* id : {"mirrorTransH", "mirrorTransV"}) {
    SCOPED_TRACE(id);
    const auto it = fold.desc.param_set.params.find(id);
    ASSERT_TRUE(it != fold.desc.param_set.params.end());
    EXPECT_FALSE(IsSecretParam(*it->second));
    EXPECT_EQ(ParamStringProp(*it->second, kOfxParamPropParent), "FoldGroup");
    EXPECT_EQ(fold.inst.param_set.looked_up.count(id), 1u);
  }
}

// Verifies the Cutout stop-motion defaults the host is given: Hold Frames 6
// (a 4 fps look at 24 fps), Rotation Jitter 0.3 degrees, and Position
// Jitter 0.001 of the frame, matching CutoutParams.
TEST_F(PaperCraftOfxTest, CutoutDescribesSubtleStopMotionDefaults) {
  ScopedPluginInstance pi;
  ASSERT_NO_FATAL_FAILURE(InitCutout(&pi));
  const auto& params = pi.desc.param_set.params;
  ASSERT_EQ(params.count("holdFrames"), 1u);
  ASSERT_EQ(params.count("jitterRot"), 1u);
  ASSERT_EQ(params.count("jitterPos"), 1u);
  EXPECT_EQ(ParamIntProp(*params.at("holdFrames"), kOfxParamPropDefault, -1),
            6);
  EXPECT_EQ(ParamPropAsDoubles(*params.at("jitterRot"), kOfxParamPropDefault),
            std::vector<double>{0.3});
  EXPECT_EQ(ParamPropAsDoubles(*params.at("jitterPos"), kOfxParamPropDefault),
            std::vector<double>{0.001});
  EXPECT_NE(ParamStringProp(*params.at("holdFrames"), kOfxParamPropHint)
                .find("6 for a 4 fps look at 24 fps"),
            std::string::npos);
  const CutoutParams k;
  EXPECT_EQ(k.hold_frames, 6);
  EXPECT_EQ(k.jitter_rot_deg, 0.3f);
  EXPECT_EQ(k.jitter_pos_norm, 0.001f);
}

// Verifies the Cutout "7. Advanced" group: defined, closed by default, the
// last visible group in Inspector order, and holding exactly the six moved
// technical controls in order.
TEST_F(PaperCraftOfxTest, CutoutAdvancedGroupIsClosedLastAndHoldsSixControls) {
  ScopedPluginInstance pi;
  ASSERT_NO_FATAL_FAILURE(InitCutout(&pi));
  const MockParamSet& ps = pi.desc.param_set;
  const auto it = ps.params.find("AdvancedGroup");
  ASSERT_TRUE(it != ps.params.end());
  const MockParam& group = *it->second;
  EXPECT_EQ(group.type, kOfxParamTypeGroup);
  EXPECT_EQ(ParamStringProp(group, kOfxPropLabel), "7. Advanced");
  EXPECT_EQ(ParamIntProp(group, kOfxParamPropGroupOpen, -1), 0);
  EXPECT_FALSE(IsSecretParam(group));

  std::string last_visible_group;
  std::vector<std::string> children;
  for (const std::string& name : ps.define_order) {
    const MockParam& param = *ps.params.at(name);
    if (IsSecretParam(param)) {
      continue;
    }
    if (param.type == kOfxParamTypeGroup) {
      last_visible_group = name;
    } else if (ParamStringProp(param, kOfxParamPropParent) == "AdvancedGroup") {
      children.push_back(name);
    }
  }
  EXPECT_EQ(last_visible_group, "AdvancedGroup");
  EXPECT_EQ(children, (std::vector<std::string>{"sourceMode", "sourcePremult",
                                                "tearDetail", "grainScale",
                                                "creaseScale", "randomSeed"}));
}

// Verifies both plugins number their visible group labels "1. ", "2. ", ...
// consecutively in Inspector order (seven Cutout groups, three transition
// groups), so the Inspector order is obvious and has no gaps.
TEST_F(PaperCraftOfxTest, VisibleGroupLabelsAreNumberedConsecutively) {
  const std::pair<const char*, int> cases[] = {{kCutoutId, 7},
                                               {kFoldTransitionId, 3}};
  for (const auto& c : cases) {
    const char* id = c.first;
    SCOPED_TRACE(id);
    const char* context = std::strcmp(id, kCutoutId) == 0
                              ? kOfxImageEffectContextFilter
                              : kOfxImageEffectContextTransition;
    ScopedPluginInstance pi;
    ASSERT_NO_FATAL_FAILURE(InitInstance(id, context, &pi));
    int number = 0;
    for (const std::string& name : pi.desc.param_set.define_order) {
      const MockParam& param = *pi.desc.param_set.params.at(name);
      if (param.type != kOfxParamTypeGroup || IsSecretParam(param)) {
        continue;
      }
      ++number;
      const std::string prefix = std::to_string(number) + ". ";
      EXPECT_EQ(ParamStringProp(param, kOfxPropLabel).rfind(prefix, 0), 0u)
          << name << " label \"" << ParamStringProp(param, kOfxPropLabel)
          << "\" should start with \"" << prefix << "\"";
    }
    EXPECT_EQ(number, c.second);
  }
}

// Verifies Edge Choke scales with both render-scale axes: swapping X and Y
// gives the same result, and a reduced Y scale changes the choke.
TEST_F(PaperCraftOfxTest, EdgeChokeUsesBothRenderScaleAxes) {
  ScopedPluginInstance pi;
  ASSERT_NO_FATAL_FAILURE(InitCutout(&pi));

  constexpr int kW = 128;
  constexpr int kH = 96;
  const double range[2] = {0.0, 120.0};
  // Opaque square subject on a transparent canvas.
  std::vector<float> src(static_cast<std::size_t>(kW) * kH * 4, 0.0f);
  for (int y = 24; y < 72; ++y) {
    for (int x = 32; x < 96; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * kW + x) * 4;
      src[i + 0] = 0.8f;
      src[i + 1] = 0.4f;
      src[i + 2] = 0.2f;
      src[i + 3] = 1.0f;
    }
  }
  MockPropertySet s_img;
  ConfigureClipAndImage(&host_, pi.inst.clips.at("Source").get(), &s_img,
                        src.data(), kW, kH, kW * 16, kOfxBitDepthFloat, range);
  SetIntParam(&pi, "enableBorder", 0);
  SetIntParam(&pi, "enableStopMotion", 0);
  SetDoubleParam(&pi, "shadowOpacity", 0.0);
  SetDoubleParam(&pi, "mediaScale", 1.0);
  // Negative choke shrinks the matte by 8 full-resolution pixels.
  SetDoubleParam(&pi, "edgeChokePx", -8.0);

  auto render_at_scale = [&](double sx, double sy) {
    std::vector<float> out(static_cast<std::size_t>(kW) * kH * 4, -1.0f);
    MockPropertySet o_img;
    ConfigureClipAndImage(&host_, pi.inst.clips.at("Output").get(), &o_img,
                          out.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);
    EXPECT_EQ(RenderFrameAtScale(pi, 30.0, sx, sy), kOfxStatOK);
    pi.inst.clips.at("Output")->bound_image = nullptr;
    return out;
  };
  const std::vector<float> full = render_at_scale(1.0, 1.0);
  const std::vector<float> half_y = render_at_scale(1.0, 0.5);
  const std::vector<float> half_x = render_at_scale(0.5, 1.0);
  EXPECT_EQ(half_y, half_x) << "choke must not depend on the axis order";
  EXPECT_NE(half_y, full) << "a reduced Y render scale must shrink the choke";
  EXPECT_GT(MeanAlpha(half_y), MeanAlpha(full))
      << "a smaller choke radius keeps more of the subject";
  EXPECT_EQ(host_.active_leased_images, 0);
}

// Verifies the serial fallback after a failed multiThread(): with no worker
// run it renders every row; after all workers ran it redoes none of them.
TEST_F(PaperCraftOfxTest, MultiThreadFailureFallbackRendersOnlyUnfinishedRows) {
  ScopedPluginInstance pi;
  ASSERT_NO_FATAL_FAILURE(InitFoldTransition(&pi));
  SetIntParam(&pi, "enableStopMotion", 0);

  constexpr int kW = 80;
  constexpr int kH = 64;
  const double range[2] = {0.0, 24.0};
  std::vector<float> from = SolidRgba(kW, kH, 0.1f, 0.2f, 0.9f, 1.0f);
  std::vector<float> to = SolidRgba(kW, kH, 0.9f, 0.6f, 0.1f, 1.0f);
  MockPropertySet from_img, to_img;
  ConfigureClipAndImage(&host_, pi.inst.clips.at("SourceFrom").get(), &from_img,
                        from.data(), kW, kH, kW * 16, kOfxBitDepthFloat, range);
  ConfigureClipAndImage(&host_, pi.inst.clips.at("SourceTo").get(), &to_img,
                        to.data(), kW, kH, kW * 16, kOfxBitDepthFloat, range);
  const std::vector<float> reference =
      RenderTransitionAt(&host_, &pi, kW, kH, 0.5);

  {
    // Host refuses to start threads: every row must still be rendered.
    ScopedMultiThreadFailure fail(MultiThreadFailMode::kBeforeRun);
    EXPECT_EQ(RenderTransitionAt(&host_, &pi, kW, kH, 0.5), reference);
  }
  {
    // Workers finish every chunk, then the host reports failure anyway.
    ScopedMultiThreadFailure fail(MultiThreadFailMode::kAfterRun);
    EXPECT_EQ(RenderTransitionAt(&host_, &pi, kW, kH, 0.5), reference);
  }
  {
    // p = 0 is a single row dispatch that copies SourceFrom to Output. The
    // hook overwrites Output after the workers finished; the fallback must
    // leave it alone because every chunk was already completed.
    constexpr float kSentinel = 7.0f;
    ScopedMultiThreadFailure fail(MultiThreadFailMode::kAfterRun, [&] {
      MockClip* out_clip = pi.inst.clips.at("Output").get();
      void* data = nullptr;
      if (out_clip->bound_image != nullptr &&
          MockPropGetPointer(ToPropHandle(out_clip->bound_image),
                             kOfxImagePropData, 0, &data) == kOfxStatOK) {
        std::fill_n(static_cast<float*>(data),
                    static_cast<std::size_t>(kW) * kH * 4, kSentinel);
      }
    });
    const std::vector<float> out = RenderTransitionAt(&host_, &pi, kW, kH, 0.0);
    EXPECT_EQ(std::count(out.begin(), out.end(), kSentinel),
              static_cast<std::ptrdiff_t>(out.size()))
        << "rows completed by workers were rendered again serially";
  }
  EXPECT_EQ(host_.active_leased_images, 0);
}

// Verifies Auto folds the outgoing clip away (like Fold Out) when the
// incoming side is an opaque black frame, the host's "empty" clip-end side.
TEST_F(PaperCraftOfxTest,
       FoldTransitionAutoTreatsOpaqueBlackIncomingAsFoldOut) {
  ScopedPluginInstance pi;
  ASSERT_NO_FATAL_FAILURE(InitFoldTransition(&pi));
  SetIntParam(&pi, "enableStopMotion", 0);

  constexpr int kW = 96;
  constexpr int kH = 64;
  const double range[2] = {0.0, 24.0};
  std::vector<float> from = SolidRgba(kW, kH, 0.1f, 0.3f, 0.9f, 1.0f);
  std::vector<float> black = SolidRgba(kW, kH, 0.0f, 0.0f, 0.0f, 1.0f);
  MockPropertySet from_img, to_img;
  ConfigureClipAndImage(&host_, pi.inst.clips.at("SourceFrom").get(), &from_img,
                        from.data(), kW, kH, kW * 16, kOfxBitDepthFloat, range);
  ConfigureClipAndImage(&host_, pi.inst.clips.at("SourceTo").get(), &to_img,
                        black.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                        range);
  for (MockPropertySet* img : {&from_img, &to_img}) {
    MockPropSetString(ToPropHandle(img), kOfxImageEffectPropPreMultiplication,
                      0, kOfxImageOpaque);
  }

  // Fold Action choice indices: 0 Auto, 1 Unfold In, 2 Fold Out.
  SetIntParam(&pi, "foldAction", 2);
  const std::vector<float> fold_out =
      RenderTransitionAt(&host_, &pi, kW, kH, 0.45);
  SetIntParam(&pi, "foldAction", 1);
  const std::vector<float> unfold_in =
      RenderTransitionAt(&host_, &pi, kW, kH, 0.45);
  SetIntParam(&pi, "foldAction", 0);
  const std::vector<float> auto_action =
      RenderTransitionAt(&host_, &pi, kW, kH, 0.45);

  ASSERT_NE(fold_out, unfold_in) << "test needs distinguishable actions";
  EXPECT_EQ(auto_action, fold_out)
      << "Auto must fold out when the incoming frame is opaque black";
  EXPECT_EQ(host_.active_leased_images, 0);
}

// ---------------------------------------------------------------------------
// Regions of interest: unused inputs get an empty RoI.
// ---------------------------------------------------------------------------

// Verifies the Cutout asks for the whole Source frame in Timeline Clip mode
// and an empty Source RoI in Built-in Paper Sticker mode, and that render
// then never fetches Source, even when the host has no image for it.
TEST_F(PaperCraftOfxTest,
       CutoutRoiIsEmptyForStickerModeAndFullForTimelineClip) {
  ScopedPluginInstance pi;
  ASSERT_NO_FATAL_FAILURE(InitCutout(&pi));

  constexpr int kW = 64;
  constexpr int kH = 48;
  const double range[2] = {0.0, 120.0};
  std::vector<float> src = SolidRgba(kW, kH, 0.8f, 0.4f, 0.2f, 1.0f);
  std::vector<float> out(static_cast<std::size_t>(kW) * kH * 4, 0.0f);
  MockClip* source = pi.inst.clips.at("Source").get();
  MockPropertySet s_img, o_img;
  ConfigureClipAndImage(&host_, source, &s_img, src.data(), kW, kH, kW * 16,
                        kOfxBitDepthFloat, range);
  ConfigureClipAndImage(&host_, pi.inst.clips.at("Output").get(), &o_img,
                        out.data(), kW, kH, kW * 16, kOfxBitDepthFloat, range);
  const std::array<double, 4> full = {0.0, 0.0, static_cast<double>(kW),
                                      static_cast<double>(kH)};

  // Source Mode choice indices: 0 Timeline Clip, 1 Built-in Paper Sticker.
  SetIntParam(&pi, "sourceMode", 0);
  RoiReply reply = RequestRegionsOfInterest(pi, 30.0, kW, kH);
  EXPECT_EQ(reply.status, kOfxStatOK);
  EXPECT_EQ(reply.rois.at("Source"), full);
  source->get_image_calls = 0;
  ASSERT_EQ(RenderFrame(pi, 30.0), kOfxStatOK);
  EXPECT_EQ(source->get_image_calls, 1);

  SetIntParam(&pi, "sourceMode", 1);
  reply = RequestRegionsOfInterest(pi, 30.0, kW, kH);
  EXPECT_EQ(reply.status, kOfxStatOK);
  EXPECT_TRUE(IsEmptyRoi(reply.rois.at("Source")))
      << "sticker mode never samples Source";
  source->bound_image = nullptr;  // A host honoring the empty RoI.
  source->get_image_calls = 0;
  std::fill(out.begin(), out.end(), 0.0f);
  ASSERT_EQ(RenderFrame(pi, 30.0), kOfxStatOK);
  EXPECT_EQ(source->get_image_calls, 0);
  EXPECT_GT(MeanAlpha(out), 0.05) << "the procedural sticker is drawn";
  EXPECT_EQ(host_.active_leased_images, 0);
}

// Verifies the transition asks for the whole frame of exactly the inputs a
// frame reads, an empty RoI for the other one, and that render fetches the
// same set: p = 0 reads only SourceFrom, p = 1 only SourceTo, a forced
// Fold & Unfold reads SourceFrom in its first half and SourceTo in its
// second, and Auto, Unfold In and Fold Out read both mid-transition.
TEST_F(PaperCraftOfxTest, FoldTransitionRoiRequestsOnlyClipsTheFrameReads) {
  ScopedPluginInstance pi;
  ASSERT_NO_FATAL_FAILURE(InitFoldTransition(&pi));
  SetIntParam(&pi, "enableStopMotion", 0);

  constexpr int kW = 64;
  constexpr int kH = 48;
  const double range[2] = {0.0, 24.0};
  std::vector<float> from = SolidRgba(kW, kH, 0.1f, 0.2f, 0.9f, 1.0f);
  std::vector<float> to = SolidRgba(kW, kH, 0.9f, 0.6f, 0.1f, 1.0f);
  MockClip* from_clip = pi.inst.clips.at("SourceFrom").get();
  MockClip* to_clip = pi.inst.clips.at("SourceTo").get();
  MockPropertySet from_img, to_img;
  ConfigureClipAndImage(&host_, from_clip, &from_img, from.data(), kW, kH,
                        kW * 16, kOfxBitDepthFloat, range);
  ConfigureClipAndImage(&host_, to_clip, &to_img, to.data(), kW, kH, kW * 16,
                        kOfxBitDepthFloat, range);
  const std::array<double, 4> full = {0.0, 0.0, static_cast<double>(kW),
                                      static_cast<double>(kH)};

  struct Case {
    int fold_action;  // 0 Auto, 1 Unfold In, 2 Fold Out, 3 Fold & Unfold.
    double progress;
    bool reads_from;
    bool reads_to;
  };
  const Case cases[] = {
      {0, 0.0, true, false},  {0, 1.0, false, true},  {0, 0.5, true, true},
      {3, 0.25, true, false}, {3, 0.75, false, true}, {1, 0.25, true, true},
      {2, 0.75, true, true},
  };
  for (const Case& c : cases) {
    SetIntParam(&pi, "foldAction", c.fold_action);
    SetDoubleParam(&pi, kOfxImageEffectTransitionParamName, c.progress);
    const std::string label = "foldAction=" + std::to_string(c.fold_action) +
                              " p=" + std::to_string(c.progress);
    const RoiReply reply = RequestRegionsOfInterest(pi, 10.0, kW, kH);
    EXPECT_EQ(reply.status, kOfxStatOK) << label;
    const std::pair<const char*, bool> inputs[] = {{"SourceFrom", c.reads_from},
                                                   {"SourceTo", c.reads_to}};
    for (const auto& [name, used] : inputs) {
      if (used) {
        EXPECT_EQ(reply.rois.at(name), full) << label << " " << name;
      } else {
        EXPECT_TRUE(IsEmptyRoi(reply.rois.at(name))) << label << " " << name;
      }
    }

    from_clip->get_image_calls = 0;
    to_clip->get_image_calls = 0;
    RenderTransitionAt(&host_, &pi, kW, kH, c.progress);
    EXPECT_EQ(from_clip->get_image_calls, c.reads_from ? 1 : 0) << label;
    EXPECT_EQ(to_clip->get_image_calls, c.reads_to ? 1 : 0) << label;
  }

  // A host that honors the empty RoI may have no image for the unused clip;
  // the frame must not change.
  SetIntParam(&pi, "foldAction", 3);
  const std::vector<float> with_to =
      RenderTransitionAt(&host_, &pi, kW, kH, 0.25);
  to_clip->bound_image = nullptr;
  EXPECT_EQ(RenderTransitionAt(&host_, &pi, kW, kH, 0.25), with_to);
  EXPECT_EQ(host_.active_leased_images, 0);
}

}  // namespace
}  // namespace ofx_test
}  // namespace papercraft
