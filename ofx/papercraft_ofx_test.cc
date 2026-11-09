#include <dlfcn.h>
#include <pthread.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#if defined(PAPERCRAFT_STANDALONE_OFX)
#include <gtest/gtest.h>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxMultiThread.h"
#include "ofxParam.h"
#include "ofxProperty.h"
#else
#include "experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro/ofx/openfx_include/ofxCore.h"
#include "experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro/ofx/openfx_include/ofxImageEffect.h"
#include "experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro/ofx/openfx_include/ofxMultiThread.h"
#include "experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro/ofx/openfx_include/ofxParam.h"
#include "experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro/ofx/openfx_include/ofxProperty.h"
#include "testing/base/public/gunit.h"
#endif

namespace papercraft {
namespace ofx_test {
namespace {

using GetNumPluginsFn = int (*)();
using GetPluginFn = OfxPlugin* (*)(int);

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

class ScopedMutexLock {
 public:
  explicit ScopedMutexLock(PthreadMutex& mu) : mu_(mu) { mu_.Lock(); }
  ~ScopedMutexLock() { mu_.Unlock(); }
  ScopedMutexLock(const ScopedMutexLock&) = delete;
  ScopedMutexLock& operator=(const ScopedMutexLock&) = delete;

 private:
  PthreadMutex& mu_;
};

struct PropValue {
  enum class Kind { kUnset, kInt, kDouble, kString, kPointer };
  Kind kind = Kind::kUnset;
  std::vector<int> ints;
  std::vector<double> doubles;
  std::vector<std::string> strings;
  std::vector<void*> ptrs;
};

struct MockPropertySet {
  PthreadMutex mu;
  std::map<std::string, PropValue> map;
};

MockPropertySet* AsProps(OfxPropertySetHandle h) {
  return reinterpret_cast<MockPropertySet*>(h);
}

OfxPropertySetHandle ToPropHandle(MockPropertySet* p) {
  return reinterpret_cast<OfxPropertySetHandle>(p);
}

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

const OfxPropertySuiteV1 kPropSuite = {
    MockPropSetPointer,  MockPropSetString,   MockPropSetDouble,
    MockPropSetInt,      MockPropSetPointerN, MockPropSetStringN,
    MockPropSetDoubleN,  MockPropSetIntN,     MockPropGetPointer,
    MockPropGetString,   MockPropGetDouble,   MockPropGetInt,
    MockPropGetPointerN, MockPropGetStringN,  MockPropGetDoubleN,
    MockPropGetIntN,     MockPropReset,       MockPropGetDimension,
};

struct MockParam {
  std::string type;
  std::string name;
  MockPropertySet props;
  int int_val = 0;
  double double_vals[3] = {0.0, 0.0, 0.0};
};

struct MockParamSet {
  MockPropertySet owner_props;
  std::map<std::string, std::unique_ptr<MockParam>> params;
};

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
  ps->params[name] = std::move(param);
  return kOfxStatOK;
}

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
  return kOfxStatOK;
}

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

const OfxParameterSuiteV1 kParamSuite = {
    MockParamDefine,         MockParamGetHandle,   MockParamSetGetPropertySet,
    MockParamGetPropertySet, MockParamGetValue,    MockParamGetValueAtTime,
    MockParamGetDerivative,  MockParamGetIntegral, MockParamSetValue,
    MockParamSetValueAtTime, MockParamGetNumKeys,  MockParamGetKeyTime,
    MockParamGetKeyIndex,    MockParamDeleteKey,   MockParamDeleteAllKeys,
    MockParamCopy,           MockParamEditBegin,   MockParamEditEnd,
};

struct MockHost;

struct MockClip {
  std::string name;
  MockHost* host = nullptr;
  MockPropertySet props;
  MockPropertySet* bound_image = nullptr;
};

struct MockEffectObject {
  MockHost* host = nullptr;
  MockPropertySet props;
  MockParamSet param_set;
  std::map<std::string, std::unique_ptr<MockClip>> clips;
};

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

thread_local bool g_is_spawned_thread = false;
thread_local unsigned int g_spawned_thread_index = 0;

struct WorkerThreadArgs {
  OfxThreadFunctionV1* func = nullptr;
  unsigned int index = 0;
  unsigned int total = 0;
  void* custom_arg = nullptr;
};

void* RunMockWorkerThread(void* raw) {
  auto* args = static_cast<WorkerThreadArgs*>(raw);
  g_is_spawned_thread = true;
  g_spawned_thread_index = args->index;
  args->func(args->index, args->total, args->custom_arg);
  g_is_spawned_thread = false;
  g_spawned_thread_index = 0;
  return nullptr;
}

OfxStatus MockMultiThread(OfxThreadFunctionV1 func, unsigned int n_threads,
                          void* custom_arg) {
  if (func == nullptr || n_threads == 0) {
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
  return kOfxStatOK;
}

OfxStatus MockMultiThreadNumCPUs(unsigned int* n_cpus) {
  *n_cpus = 4;
  return kOfxStatOK;
}

OfxStatus MockMultiThreadIndex(unsigned int* idx) {
  *idx = g_spawned_thread_index;
  return kOfxStatOK;
}

int MockMultiThreadIsSpawnedThread(void) { return g_is_spawned_thread ? 1 : 0; }

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
  return kOfxStatOK;
}

OfxStatus MockClipGetPropertySet(OfxImageClipHandle clip_h,
                                 OfxPropertySetHandle* prop_set) {
  auto* clip = reinterpret_cast<MockClip*>(clip_h);
  *prop_set = ToPropHandle(&clip->props);
  return kOfxStatOK;
}

OfxStatus MockClipGetImage(OfxImageClipHandle clip_h, OfxTime /*time*/,
                           const OfxRectD* /*region*/,
                           OfxPropertySetHandle* image_handle) {
  auto* clip = reinterpret_cast<MockClip*>(clip_h);
  if (clip == nullptr || clip->bound_image == nullptr) {
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

TEST(PaperCraftOfxTest, ExportsExactlyFourPluginsAndHidesInternals) {
  const std::string path = ResolvePluginBinaryPath();
  LoadedOfxBundle bundle(path);
  ASSERT_NE(bundle.raw(), nullptr) << "dlopen failed for " << path << ": "
                                   << (dlerror() ? dlerror() : "unknown");

  auto get_count = bundle.Sym<GetNumPluginsFn>("OfxGetNumberOfPlugins");
  auto get_plugin = bundle.Sym<GetPluginFn>("OfxGetPlugin");
  ASSERT_NE(get_count, nullptr);
  ASSERT_NE(get_plugin, nullptr);

  EXPECT_EQ(get_count(), 4);
  EXPECT_EQ(get_plugin(-1), nullptr);
  EXPECT_EQ(get_plugin(4), nullptr);

  std::vector<std::string> ids;
  for (int i = 0; i < 4; ++i) {
    OfxPlugin* p = get_plugin(i);
    ASSERT_NE(p, nullptr);
    EXPECT_STREQ(p->pluginApi, kOfxImageEffectPluginApi);
    EXPECT_EQ(p->apiVersion, 1);
    EXPECT_NE(p->setHost, nullptr);
    EXPECT_NE(p->mainEntry, nullptr);
    ids.emplace_back(p->pluginIdentifier);
  }
  EXPECT_EQ(ids, (std::vector<std::string>{
                     "com.papercraft.CutoutFX",
                     "com.papercraft.UnfoldTransition",
                     "com.papercraft.CrumpleTransition",
                     "com.papercraft.RipWipeTransition",
                 }));

  // Internal symbols must stay hidden via version script + -fvisibility=hidden.
  EXPECT_EQ(dlsym(bundle.raw(), "RenderCutoutFrame"), nullptr);
  EXPECT_EQ(dlsym(bundle.raw(), "CutoutMainEntry"), nullptr);
}

TEST(PaperCraftOfxTest,
     CutoutFilterAndGeneratorLifecycleRendersTornPaperAndChromaKey) {
  LoadedOfxBundle bundle(ResolvePluginBinaryPath());
  ASSERT_NE(bundle.raw(), nullptr);
  auto get_count = bundle.Sym<GetNumPluginsFn>("OfxGetNumberOfPlugins");
  auto get_plugin = bundle.Sym<GetPluginFn>("OfxGetPlugin");
  const OfxPlugin* plugin =
      FindPluginById(get_plugin, get_count(), "com.papercraft.CutoutFX");
  ASSERT_NE(plugin, nullptr);

  MockHost host;
  host.ofx_host.host = ToPropHandle(&host.host_props);
  host.ofx_host.fetchSuite = &MockHost::FetchSuite;
  plugin->setHost(&host.ofx_host);

  ASSERT_EQ(plugin->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr),
            kOfxStatOK);

  MockEffectObject global_desc;
  global_desc.host = &host;
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
    filter_desc.host = &host;
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
    ConfigureClipAndImage(&host, inst.clips["Source"].get(), &src_img,
                          src_buf.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);
    ConfigureClipAndImage(&host, inst.clips["Output"].get(), &out_img,
                          out_buf.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);

    // Enable ChromaKey, disable intro collapse, hold resting pose cleanly.
    MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                          inst.param_set.params["enableChromaKey"].get()),
                      1);
    MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                          inst.param_set.params["introStyle"].get()),
                      0);
    MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                          inst.param_set.params["mediaScale"].get()),
                      1.0);
    MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                          inst.param_set.params["shadowOpacity"].get()),
                      0.0);
    MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                          inst.param_set.params["borderWidth"].get()),
                      0.06);

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
    EXPECT_EQ(host.active_leased_images, 0);

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

  // 2) Generator context: no Source clip at all -> built-in demo sticker.
  {
    MockEffectObject gen_desc;
    gen_desc.host = &host;
    MockPropertySet ctx_in;
    MockPropSetString(ToPropHandle(&ctx_in), kOfxImageEffectPropContext, 0,
                      kOfxImageEffectContextGenerator);
    ASSERT_EQ(plugin->mainEntry(kOfxImageEffectActionDescribeInContext,
                                &gen_desc, ToPropHandle(&ctx_in), nullptr),
              kOfxStatOK);
    EXPECT_EQ(gen_desc.clips.count("Source"), 0u);

    MockEffectObject inst;
    CloneDescriptorToInstance(gen_desc, &inst, kOfxImageEffectContextGenerator);
    ASSERT_EQ(
        plugin->mainEntry(kOfxActionCreateInstance, &inst, nullptr, nullptr),
        kOfxStatOK);

    constexpr int kW = 96;
    constexpr int kH = 96;
    std::vector<float> out_buf(kW * kH * 4, 0.0f);
    const double range[2] = {0.0, 200.0};
    MockPropertySet out_img;
    ConfigureClipAndImage(&host, inst.clips["Output"].get(), &out_img,
                          out_buf.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);
    MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                          inst.param_set.params["introStyle"].get()),
                      0);

    MockPropertySet render_in;
    MockPropSetDouble(ToPropHandle(&render_in), kOfxPropTime, 0, 50.0);
    const double rs[2] = {1.0, 1.0};
    MockPropSetDoubleN(ToPropHandle(&render_in), kOfxImageEffectPropRenderScale,
                       2, rs);
    ASSERT_EQ(plugin->mainEntry(kOfxImageEffectActionRender, &inst,
                                ToPropHandle(&render_in), nullptr),
              kOfxStatOK);
    EXPECT_EQ(host.active_leased_images, 0);

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

TEST(PaperCraftOfxTest,
     AllThreeTransitionsRespectIdentityEndpointsAndRenderMidpoint) {
  LoadedOfxBundle bundle(ResolvePluginBinaryPath());
  ASSERT_NE(bundle.raw(), nullptr);
  auto get_count = bundle.Sym<GetNumPluginsFn>("OfxGetNumberOfPlugins");
  auto get_plugin = bundle.Sym<GetPluginFn>("OfxGetPlugin");

  const char* ids[3] = {
      "com.papercraft.UnfoldTransition",
      "com.papercraft.CrumpleTransition",
      "com.papercraft.RipWipeTransition",
  };
  constexpr int kW = 64;
  constexpr int kH = 48;
  const double range[2] = {0.0, 24.0};

  for (const char* id : ids) {
    const OfxPlugin* plugin = FindPluginById(get_plugin, get_count(), id);
    ASSERT_NE(plugin, nullptr) << id;

    MockHost host;
    host.ofx_host.host = ToPropHandle(&host.host_props);
    host.ofx_host.fetchSuite = &MockHost::FetchSuite;
    plugin->setHost(&host.ofx_host);
    ASSERT_EQ(plugin->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr),
              kOfxStatOK);

    MockEffectObject desc;
    desc.host = &host;
    ASSERT_EQ(plugin->mainEntry(kOfxActionDescribe, &desc, nullptr, nullptr),
              kOfxStatOK);

    MockPropertySet ctx_in;
    MockPropSetString(ToPropHandle(&ctx_in), kOfxImageEffectPropContext, 0,
                      kOfxImageEffectContextTransition);
    ASSERT_EQ(plugin->mainEntry(kOfxImageEffectActionDescribeInContext, &desc,
                                ToPropHandle(&ctx_in), nullptr),
              kOfxStatOK);

    MockEffectObject inst;
    CloneDescriptorToInstance(desc, &inst, kOfxImageEffectContextTransition);
    ASSERT_EQ(
        plugin->mainEntry(kOfxActionCreateInstance, &inst, nullptr, nullptr),
        kOfxStatOK);

    // Prepare 16-bit SourceFrom (solid warm coral) and SourceTo (solid teal).
    std::vector<std::uint16_t> from_buf(kW * kH * 4);
    std::vector<std::uint16_t> to_buf(kW * kH * 4);
    std::vector<std::uint16_t> out_buf(kW * kH * 4, 0);
    for (int i = 0; i < kW * kH; ++i) {
      from_buf[i * 4 + 0] = 60000;
      from_buf[i * 4 + 1] = 12000;
      from_buf[i * 4 + 2] = 10000;
      from_buf[i * 4 + 3] = 65535;
      to_buf[i * 4 + 0] = 8000;
      to_buf[i * 4 + 1] = 52000;
      to_buf[i * 4 + 2] = 48000;
      to_buf[i * 4 + 3] = 65535;
    }

    MockPropertySet from_img, to_img, out_img;
    ConfigureClipAndImage(&host, inst.clips["SourceFrom"].get(), &from_img,
                          from_buf.data(), kW, kH, kW * 8, kOfxBitDepthShort,
                          range);
    ConfigureClipAndImage(&host, inst.clips["SourceTo"].get(), &to_img,
                          to_buf.data(), kW, kH, kW * 8, kOfxBitDepthShort,
                          range);
    ConfigureClipAndImage(&host, inst.clips["Output"].get(), &out_img,
                          out_buf.data(), kW, kH, kW * 8, kOfxBitDepthShort,
                          range);

    OfxParamHandle trans_p = reinterpret_cast<OfxParamHandle>(
        inst.param_set.params[kOfxImageEffectTransitionParamName].get());

    // Endpoint 0.0 -> IsIdentity SourceFrom.
    MockParamSetValue(trans_p, 0.0);
    MockPropertySet id_in, id_out;
    MockPropSetDouble(ToPropHandle(&id_in), kOfxPropTime, 0, 0.0);
    ASSERT_EQ(plugin->mainEntry(kOfxImageEffectActionIsIdentity, &inst,
                                ToPropHandle(&id_in), ToPropHandle(&id_out)),
              kOfxStatOK);
    char* clip_id = nullptr;
    ASSERT_EQ(
        MockPropGetString(ToPropHandle(&id_out), kOfxPropName, 0, &clip_id),
        kOfxStatOK);
    EXPECT_STREQ(clip_id, kOfxImageEffectTransitionSourceFromClipName);

    // Endpoint 1.0 -> IsIdentity SourceTo.
    MockParamSetValue(trans_p, 1.0);
    MockPropReset(ToPropHandle(&id_out), kOfxPropName);
    ASSERT_EQ(plugin->mainEntry(kOfxImageEffectActionIsIdentity, &inst,
                                ToPropHandle(&id_in), ToPropHandle(&id_out)),
              kOfxStatOK);
    ASSERT_EQ(
        MockPropGetString(ToPropHandle(&id_out), kOfxPropName, 0, &clip_id),
        kOfxStatOK);
    EXPECT_STREQ(clip_id, kOfxImageEffectTransitionSourceToClipName);

    // Midpoint 0.5 -> not identity, renders distinct frame from both clips.
    MockParamSetValue(trans_p, 0.5);
    EXPECT_EQ(plugin->mainEntry(kOfxImageEffectActionIsIdentity, &inst,
                                ToPropHandle(&id_in), ToPropHandle(&id_out)),
              kOfxStatReplyDefault);

    MockPropertySet render_in;
    MockPropSetDouble(ToPropHandle(&render_in), kOfxPropTime, 0, 12.0);
    const double rs[2] = {1.0, 1.0};
    MockPropSetDoubleN(ToPropHandle(&render_in), kOfxImageEffectPropRenderScale,
                       2, rs);
    ASSERT_EQ(plugin->mainEntry(kOfxImageEffectActionRender, &inst,
                                ToPropHandle(&render_in), nullptr),
              kOfxStatOK);
    EXPECT_EQ(host.active_leased_images, 0);
    EXPECT_NE(out_buf, from_buf);
    EXPECT_NE(out_buf, to_buf);

    ASSERT_EQ(
        plugin->mainEntry(kOfxActionDestroyInstance, &inst, nullptr, nullptr),
        kOfxStatOK);
    ASSERT_EQ(plugin->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr),
              kOfxStatOK);
  }
}

TEST(PaperCraftOfxTest,
     NegativeRowBytesByteBufferPreservesCanaryGuardRowsAndMatchesPositive) {
  LoadedOfxBundle bundle(ResolvePluginBinaryPath());
  ASSERT_NE(bundle.raw(), nullptr);
  auto get_count = bundle.Sym<GetNumPluginsFn>("OfxGetNumberOfPlugins");
  auto get_plugin = bundle.Sym<GetPluginFn>("OfxGetPlugin");
  const OfxPlugin* plugin = FindPluginById(get_plugin, get_count(),
                                           "com.papercraft.RipWipeTransition");
  ASSERT_NE(plugin, nullptr);

  MockHost host;
  host.ofx_host.host = ToPropHandle(&host.host_props);
  host.ofx_host.fetchSuite = &MockHost::FetchSuite;
  plugin->setHost(&host.ofx_host);
  ASSERT_EQ(plugin->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr),
            kOfxStatOK);

  MockEffectObject desc;
  desc.host = &host;
  ASSERT_EQ(plugin->mainEntry(kOfxActionDescribe, &desc, nullptr, nullptr),
            kOfxStatOK);
  MockPropertySet ctx_in;
  MockPropSetString(ToPropHandle(&ctx_in), kOfxImageEffectPropContext, 0,
                    kOfxImageEffectContextTransition);
  ASSERT_EQ(plugin->mainEntry(kOfxImageEffectActionDescribeInContext, &desc,
                              ToPropHandle(&ctx_in), nullptr),
            kOfxStatOK);

  MockEffectObject inst;
  CloneDescriptorToInstance(desc, &inst, kOfxImageEffectContextTransition);
  ASSERT_EQ(
      plugin->mainEntry(kOfxActionCreateInstance, &inst, nullptr, nullptr),
      kOfxStatOK);

  constexpr int kW = 72;
  constexpr int kH = 40;
  constexpr int kStride = kW * 4;
  constexpr std::uint8_t kCanary = 0xA5;
  const double range[2] = {0.0, 24.0};

  std::vector<std::uint8_t> from_pos(kH * kStride);
  std::vector<std::uint8_t> to_pos(kH * kStride);
  std::vector<std::uint8_t> out_pos(kH * kStride, 0);
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      const int idx = y * kStride + x * 4;
      from_pos[idx + 0] = static_cast<std::uint8_t>((x * 3) & 255);
      from_pos[idx + 1] = static_cast<std::uint8_t>((y * 5) & 255);
      from_pos[idx + 2] = 40;
      from_pos[idx + 3] = 255;

      to_pos[idx + 0] = 220;
      to_pos[idx + 1] = static_cast<std::uint8_t>((x * 2) & 255);
      to_pos[idx + 2] = static_cast<std::uint8_t>((y * 4) & 255);
      to_pos[idx + 3] = 255;
    }
  }

  MockParamSetValue(
      reinterpret_cast<OfxParamHandle>(
          inst.param_set.params[kOfxImageEffectTransitionParamName].get()),
      0.47);
  MockPropertySet render_in;
  MockPropSetDouble(ToPropHandle(&render_in), kOfxPropTime, 0, 10.0);
  const double rs[2] = {1.0, 1.0};
  MockPropSetDoubleN(ToPropHandle(&render_in), kOfxImageEffectPropRenderScale,
                     2, rs);

  // Positive stride render.
  {
    MockPropertySet f_img, t_img, o_img;
    ConfigureClipAndImage(&host, inst.clips["SourceFrom"].get(), &f_img,
                          from_pos.data(), kW, kH, kStride, kOfxBitDepthByte,
                          range);
    ConfigureClipAndImage(&host, inst.clips["SourceTo"].get(), &t_img,
                          to_pos.data(), kW, kH, kStride, kOfxBitDepthByte,
                          range);
    ConfigureClipAndImage(&host, inst.clips["Output"].get(), &o_img,
                          out_pos.data(), kW, kH, kStride, kOfxBitDepthByte,
                          range);
    ASSERT_EQ(plugin->mainEntry(kOfxImageEffectActionRender, &inst,
                                ToPropHandle(&render_in), nullptr),
              kOfxStatOK);
  }

  // Negative stride render with canary rows above row 0 and below row kH - 1.
  std::vector<std::uint8_t> from_neg((kH + 2) * kStride, kCanary);
  std::vector<std::uint8_t> to_neg((kH + 2) * kStride, kCanary);
  std::vector<std::uint8_t> out_neg((kH + 2) * kStride, kCanary);
  for (int y = 0; y < kH; ++y) {
    const int phys_y = kH - 1 - y;
    std::memcpy(&from_neg[(phys_y + 1) * kStride], &from_pos[y * kStride],
                kStride);
    std::memcpy(&to_neg[(phys_y + 1) * kStride], &to_pos[y * kStride], kStride);
  }
  {
    MockPropertySet f_img, t_img, o_img;
    ConfigureClipAndImage(&host, inst.clips["SourceFrom"].get(), &f_img,
                          &from_neg[kH * kStride], kW, kH, -kStride,
                          kOfxBitDepthByte, range);
    ConfigureClipAndImage(&host, inst.clips["SourceTo"].get(), &t_img,
                          &to_neg[kH * kStride], kW, kH, -kStride,
                          kOfxBitDepthByte, range);
    ConfigureClipAndImage(&host, inst.clips["Output"].get(), &o_img,
                          &out_neg[kH * kStride], kW, kH, -kStride,
                          kOfxBitDepthByte, range);
    ASSERT_EQ(plugin->mainEntry(kOfxImageEffectActionRender, &inst,
                                ToPropHandle(&render_in), nullptr),
              kOfxStatOK);
  }

  // Verify zero corruption of canary rows and bit-exact match per logical row.
  for (int x = 0; x < kStride; ++x) {
    EXPECT_EQ(out_neg[x], kCanary);
    EXPECT_EQ(out_neg[(kH + 1) * kStride + x], kCanary);
  }
  for (int y = 0; y < kH; ++y) {
    const int phys_y = kH - 1 - y;
    EXPECT_EQ(std::memcmp(&out_neg[(phys_y + 1) * kStride],
                          &out_pos[y * kStride], kStride),
              0)
        << "Mismatch at logical row " << y;
  }

  ASSERT_EQ(
      plugin->mainEntry(kOfxActionDestroyInstance, &inst, nullptr, nullptr),
      kOfxStatOK);
  ASSERT_EQ(plugin->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr),
            kOfxStatOK);

  // Also test CutoutFX with 8-bit negative rowBytes and canary guard rows.
  const OfxPlugin* cutout =
      FindPluginById(get_plugin, get_count(), "com.papercraft.CutoutFX");
  ASSERT_NE(cutout, nullptr);
  cutout->setHost(&host.ofx_host);
  ASSERT_EQ(cutout->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr),
            kOfxStatOK);
  MockEffectObject c_desc;
  c_desc.host = &host;
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
  MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                        c_inst.param_set.params["introStyle"].get()),
                    0);
  MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                        c_inst.param_set.params["sourceMode"].get()),
                    1);

  std::vector<std::uint8_t> c_pos(kH * kStride, 0);
  std::vector<std::uint8_t> c_neg((kH + 2) * kStride, kCanary);
  {
    MockPropertySet o_img;
    ConfigureClipAndImage(&host, c_inst.clips["Output"].get(), &o_img,
                          c_pos.data(), kW, kH, kStride, kOfxBitDepthByte,
                          range);
    ASSERT_EQ(cutout->mainEntry(kOfxImageEffectActionRender, &c_inst,
                                ToPropHandle(&render_in), nullptr),
              kOfxStatOK);
  }
  {
    MockPropertySet o_img;
    ConfigureClipAndImage(&host, c_inst.clips["Output"].get(), &o_img,
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

OfxStatus RenderFrame(const ScopedPluginInstance& pi, OfxTime time) {
  MockPropertySet render_in;
  MockPropSetDouble(ToPropHandle(&render_in), kOfxPropTime, 0, time);
  const double rs[2] = {1.0, 1.0};
  MockPropSetDoubleN(ToPropHandle(&render_in), kOfxImageEffectPropRenderScale,
                     2, rs);
  return pi.plugin->mainEntry(kOfxImageEffectActionRender,
                              const_cast<MockEffectObject*>(&pi.inst),
                              ToPropHandle(&render_in), nullptr);
}

TEST(PaperCraftOfxTest,
     UpperTrackTransparentOverlaysPreserveAlphaInAllTransitions) {
  LoadedOfxBundle bundle(ResolvePluginBinaryPath());
  ASSERT_NE(bundle.raw(), nullptr);

  constexpr int kW = 64;
  constexpr int kH = 48;
  const double range[2] = {0.0, 24.0};
  std::vector<float> from_buf(kW * kH * 4, 0.0f);
  std::vector<float> to_buf(kW * kH * 4, 0.0f);
  for (int y = 14; y < 34; ++y) {
    for (int x = 20; x < 44; ++x) {
      const std::size_t idx = static_cast<std::size_t>(y * kW + x) * 4;
      from_buf[idx + 0] = 0.9f;
      from_buf[idx + 1] = 0.2f;
      from_buf[idx + 2] = 0.2f;
      from_buf[idx + 3] = 1.0f;
      to_buf[idx + 0] = 0.1f;
      to_buf[idx + 1] = 0.8f;
      to_buf[idx + 2] = 0.6f;
      to_buf[idx + 3] = 1.0f;
    }
  }

  for (const char* id :
       {"com.papercraft.UnfoldTransition", "com.papercraft.CrumpleTransition",
        "com.papercraft.RipWipeTransition"}) {
    MockHost host;
    host.ofx_host.host = ToPropHandle(&host.host_props);
    host.ofx_host.fetchSuite = &MockHost::FetchSuite;
    ScopedPluginInstance pi;
    InitPluginInstance(&bundle, id, kOfxImageEffectContextTransition, &host,
                       &pi);

    std::vector<float> out_buf(kW * kH * 4, 1.0f);
    MockPropertySet f_img, t_img, o_img;
    ConfigureClipAndImage(&host, pi.inst.clips["SourceFrom"].get(), &f_img,
                          from_buf.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);
    ConfigureClipAndImage(&host, pi.inst.clips["SourceTo"].get(), &t_img,
                          to_buf.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);
    ConfigureClipAndImage(&host, pi.inst.clips["Output"].get(), &o_img,
                          out_buf.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);

    MockParamSetValue(
        reinterpret_cast<OfxParamHandle>(
            pi.inst.param_set.params[kOfxImageEffectTransitionParamName].get()),
        0.5);
    ASSERT_EQ(RenderFrame(pi, 12.0), kOfxStatOK) << id;
    EXPECT_LT(out_buf[3], 0.01f) << id << " top-left corner leaked alpha";
    const std::size_t br_idx =
        static_cast<std::size_t>((kH - 1) * kW + (kW - 1)) * 4;
    EXPECT_LT(out_buf[br_idx + 3], 0.01f)
        << id << " bottom-right corner leaked alpha";
  }
}

TEST(PaperCraftOfxTest, OpaqueAndUnpremultipliedInputClipsRenderCleanly) {
  LoadedOfxBundle bundle(ResolvePluginBinaryPath());
  ASSERT_NE(bundle.raw(), nullptr);

  MockHost host;
  host.ofx_host.host = ToPropHandle(&host.host_props);
  host.ofx_host.fetchSuite = &MockHost::FetchSuite;

  // 1) kOfxImageOpaque camera source whose raw alpha slot is 0.0f everywhere.
  {
    ScopedPluginInstance pi;
    InitPluginInstance(&bundle, "com.papercraft.CutoutFX",
                       kOfxImageEffectContextFilter, &host, &pi);

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
    ConfigureClipAndImage(&host, pi.inst.clips["Source"].get(), &s_img,
                          src_buf.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);
    MockPropSetString(ToPropHandle(&s_img),
                      kOfxImageEffectPropPreMultiplication, 0, kOfxImageOpaque);
    ConfigureClipAndImage(&host, pi.inst.clips["Output"].get(), &o_img,
                          out_buf.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);
    MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                          pi.inst.param_set.params["enableChromaKey"].get()),
                      1);
    MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                          pi.inst.param_set.params["introStyle"].get()),
                      0);
    MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                          pi.inst.param_set.params["shadowOpacity"].get()),
                      0.0);

    ASSERT_EQ(RenderFrame(pi, 30.0), kOfxStatOK);
    const std::size_t center =
        static_cast<std::size_t>((kH / 2) * kW + kW / 2) * 4;
    EXPECT_GT(out_buf[center + 3], 0.9f);
    EXPECT_LT(out_buf[3], 0.02f);
  }

  // 2) kOfxImageUnPreMultiplied transparent PNG in UnfoldTransition must not
  // bleed bright RGB from alpha == 0 texels.
  {
    ScopedPluginInstance pi;
    InitPluginInstance(&bundle, "com.papercraft.UnfoldTransition",
                       kOfxImageEffectContextTransition, &host, &pi);

    constexpr int kW = 64;
    constexpr int kH = 48;
    const double range[2] = {0.0, 24.0};
    std::vector<float> from_buf(kW * kH * 4, 0.0f);
    std::vector<float> to_buf(kW * kH * 4, 0.0f);
    std::vector<float> out_buf(kW * kH * 4, 1.0f);
    for (int i = 0; i < kW * kH; ++i) {
      from_buf[i * 4 + 0] = 1.0f;
      from_buf[i * 4 + 2] = 1.0f;
      from_buf[i * 4 + 3] = 0.0f;
      to_buf[i * 4 + 0] = 1.0f;
      to_buf[i * 4 + 1] = 1.0f;
      to_buf[i * 4 + 3] = 0.0f;
    }
    MockPropertySet f_img, t_img, o_img;
    ConfigureClipAndImage(&host, pi.inst.clips["SourceFrom"].get(), &f_img,
                          from_buf.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);
    ConfigureClipAndImage(&host, pi.inst.clips["SourceTo"].get(), &t_img,
                          to_buf.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);
    ConfigureClipAndImage(&host, pi.inst.clips["Output"].get(), &o_img,
                          out_buf.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);
    MockPropSetString(ToPropHandle(&f_img),
                      kOfxImageEffectPropPreMultiplication, 0,
                      kOfxImageUnPreMultiplied);
    MockPropSetString(ToPropHandle(&t_img),
                      kOfxImageEffectPropPreMultiplication, 0,
                      kOfxImageUnPreMultiplied);
    MockParamSetValue(
        reinterpret_cast<OfxParamHandle>(
            pi.inst.param_set.params[kOfxImageEffectTransitionParamName].get()),
        0.5);

    ASSERT_EQ(RenderFrame(pi, 12.0), kOfxStatOK);
    for (float v : out_buf) {
      EXPECT_NEAR(v, 0.0f, 1e-4f);
    }
  }
}

TEST(PaperCraftOfxTest, MirrorVerticalAndShortClipOverlapBehaveSmoothly) {
  LoadedOfxBundle bundle(ResolvePluginBinaryPath());
  ASSERT_NE(bundle.raw(), nullptr);
  MockHost host;
  host.ofx_host.host = ToPropHandle(&host.host_props);
  host.ofx_host.fetchSuite = &MockHost::FetchSuite;

  // 1) Mirror Vertical toggles pixels in CrumpleTransition.
  {
    ScopedPluginInstance pi;
    InitPluginInstance(&bundle, "com.papercraft.CrumpleTransition",
                       kOfxImageEffectContextTransition, &host, &pi);

    constexpr int kW = 64;
    constexpr int kH = 48;
    const double range[2] = {0.0, 24.0};
    std::vector<float> from_buf(kW * kH * 4);
    std::vector<float> to_buf(kW * kH * 4, 0.5f);
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < kW; ++x) {
        const std::size_t idx = static_cast<std::size_t>(y * kW + x) * 4;
        from_buf[idx + 0] = static_cast<float>(x) / kW;
        from_buf[idx + 1] = static_cast<float>(y) / kH;
        from_buf[idx + 2] = 0.25f;
        from_buf[idx + 3] = 1.0f;
      }
    }
    std::vector<float> out_a(kW * kH * 4, 0.0f);
    std::vector<float> out_b(kW * kH * 4, 0.0f);
    MockPropertySet f_img, t_img, o_img;
    ConfigureClipAndImage(&host, pi.inst.clips["SourceFrom"].get(), &f_img,
                          from_buf.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);
    ConfigureClipAndImage(&host, pi.inst.clips["SourceTo"].get(), &t_img,
                          to_buf.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);
    ConfigureClipAndImage(&host, pi.inst.clips["Output"].get(), &o_img,
                          out_a.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);
    MockParamSetValue(
        reinterpret_cast<OfxParamHandle>(
            pi.inst.param_set.params[kOfxImageEffectTransitionParamName].get()),
        0.35);
    ASSERT_EQ(RenderFrame(pi, 8.0), kOfxStatOK);

    ConfigureClipAndImage(&host, pi.inst.clips["Output"].get(), &o_img,
                          out_b.data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                          range);
    MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                          pi.inst.param_set.params["mirrorV"].get()),
                      1);
    ASSERT_EQ(RenderFrame(pi, 8.0), kOfxStatOK);
    EXPECT_NE(out_a, out_b);
  }

  // 2) Short 20-frame CutoutFX clip with introFrames=14 & outroFrames=14 stays
  // continuous across the midpoint and honors mirrorTransV.
  {
    ScopedPluginInstance pi;
    InitPluginInstance(&bundle, "com.papercraft.CutoutFX",
                       kOfxImageEffectContextGenerator, &host, &pi);

    constexpr int kW = 64;
    constexpr int kH = 48;
    const double short_range[2] = {0.0, 20.0};
    MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                          pi.inst.param_set.params["enableStopMotion"].get()),
                      0);
    MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                          pi.inst.param_set.params["introFrames"].get()),
                      14);
    MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                          pi.inst.param_set.params["outroFrames"].get()),
                      14);

    auto sum_alpha_at = [&](double t, int mirror_v, std::vector<float>* buf) {
      MockPropertySet o_img;
      ConfigureClipAndImage(&host, pi.inst.clips["Output"].get(), &o_img,
                            buf->data(), kW, kH, kW * 16, kOfxBitDepthFloat,
                            short_range);
      MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                            pi.inst.param_set.params["mirrorTransV"].get()),
                        mirror_v);
      EXPECT_EQ(RenderFrame(pi, t), kOfxStatOK);
      double total_a = 0.0;
      for (int i = 0; i < kW * kH; ++i) {
        total_a += (*buf)[i * 4 + 3];
      }
      return total_a;
    };

    std::vector<float> buf9(kW * kH * 4, 0.0f);
    std::vector<float> buf10(kW * kH * 4, 0.0f);
    std::vector<float> buf11(kW * kH * 4, 0.0f);
    std::vector<float> buf14(kW * kH * 4, 0.0f);
    const double a9 = sum_alpha_at(9.0, 0, &buf9);
    const double a10 = sum_alpha_at(10.0, 0, &buf10);
    const double a11 = sum_alpha_at(11.0, 0, &buf11);
    const double a14 = sum_alpha_at(14.0, 0, &buf14);
    EXPECT_GT(a10, 50.0);
    // Midpoint transition between intro and outro must not collapse or jump by
    // >30% between frames 9->10->11->14.
    EXPECT_NEAR(a9, a10, a10 * 0.25);
    EXPECT_NEAR(a11, a10, a10 * 0.25);
    EXPECT_LT(a14, a10);

    std::vector<float> buf6_a(kW * kH * 4, 0.0f);
    std::vector<float> buf6_b(kW * kH * 4, 0.0f);
    sum_alpha_at(6.0, 0, &buf6_a);
    sum_alpha_at(6.0, 1, &buf6_b);
    EXPECT_NE(buf6_a, buf6_b);
  }
}

TEST(PaperCraftOfxTest, CroppedInputTileMapsViaRegionOfDefinition) {
  LoadedOfxBundle bundle(ResolvePluginBinaryPath());
  ASSERT_NE(bundle.raw(), nullptr);
  MockHost host;
  host.ofx_host.host = ToPropHandle(&host.host_props);
  host.ofx_host.fetchSuite = &MockHost::FetchSuite;
  ScopedPluginInstance pi;
  InitPluginInstance(&bundle, "com.papercraft.CutoutFX",
                     kOfxImageEffectContextFilter, &host, &pi);

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
  ConfigureClipAndImage(&host, pi.inst.clips["Source"].get(), &s_img,
                        tile_buf.data(), kTileW, kTileH, kTileW * 16,
                        kOfxBitDepthFloat, range);
  const int tile_bounds[4] = {kTileX1, kTileY1, kTileX1 + kTileW,
                              kTileY1 + kTileH};
  const int full_rod[4] = {0, 0, kCanvasW, kCanvasH};
  MockPropSetIntN(ToPropHandle(&s_img), kOfxImagePropBounds, 4, tile_bounds);
  MockPropSetIntN(ToPropHandle(&s_img), kOfxImagePropRegionOfDefinition, 4,
                  full_rod);
  ConfigureClipAndImage(&host, pi.inst.clips["Output"].get(), &o_img,
                        out_buf.data(), kCanvasW, kCanvasH, kCanvasW * 16,
                        kOfxBitDepthFloat, range);

  MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                        pi.inst.param_set.params["introStyle"].get()),
                    0);
  MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                        pi.inst.param_set.params["mediaScale"].get()),
                    1.0);
  MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                        pi.inst.param_set.params["enableBorder"].get()),
                    0);
  MockParamSetValue(reinterpret_cast<OfxParamHandle>(
                        pi.inst.param_set.params["shadowOpacity"].get()),
                    0.0);

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

}  // namespace
}  // namespace ofx_test
}  // namespace papercraft
