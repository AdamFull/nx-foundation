#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

#include <jni.h>

namespace nx::android {

/// Borrows a JNIEnv for the calling thread and detaches only when this scope
/// performed the attachment. Any exception is consumed at the JNI boundary so
/// one failed platform call cannot poison later engine calls.
class JniScope {
public:
  explicit JniScope(JavaVM *const vm) noexcept : m_vm(vm) {
    if (m_vm == nullptr)
      return;
    if (m_vm->GetEnv(reinterpret_cast<void **>(&m_env), JNI_VERSION_1_6) ==
        JNI_EDETACHED)
      m_attached = m_vm->AttachCurrentThread(&m_env, nullptr) == JNI_OK;
  }

  ~JniScope() {
    if (m_env != nullptr && m_env->ExceptionCheck())
      m_env->ExceptionClear();
    if (m_attached)
      m_vm->DetachCurrentThread();
  }

  JniScope(const JniScope &) = delete;
  JniScope &operator=(const JniScope &) = delete;

  [[nodiscard]] JNIEnv *operator->() const noexcept { return m_env; }
  [[nodiscard]] JNIEnv *get() const noexcept { return m_env; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return m_env != nullptr;
  }

private:
  JavaVM *m_vm = nullptr;
  JNIEnv *m_env = nullptr;
  bool m_attached = false;
};

/// Looks up a class by its JNI-style slash-separated name (e.g.
/// "com/nx2d/runtime/NxHaptics"). Clears any pending exception and returns
/// nullptr on failure; the caller owns the returned local ref otherwise.
[[nodiscard]] jclass find_class(JNIEnv *env, const char *name);

/// GetMethodID/GetStaticMethodID wrapped the same null-safe way: resolves
/// the object's class (or takes one directly), clears any pending
/// exception, and always releases the local class ref it created itself.
[[nodiscard]] jmethodID instance_method(JNIEnv *env, jobject object,
                                        const char *name,
                                        const char *signature);
[[nodiscard]] jmethodID static_method(JNIEnv *env, jclass cls,
                                      const char *name,
                                      const char *signature);

/// JNI strings are "modified UTF-8" - close enough to real UTF-8 for the
/// ids/names/prices this engine actually round-trips across the JNI
/// boundary (no embedded nulls, no need for exact supplementary-plane
/// fidelity) that a byte-for-byte reinterpretation is fine in both
/// directions.
[[nodiscard]] nx::string to_nx_string(JNIEnv *env, jstring value);
[[nodiscard]] jstring to_jstring(JNIEnv *env, nx::string_view value);

/// Array conversions for the shapes a JNI callback typically carries -
/// every mobile storefront backend needing a hand-written Java shim
/// (Google Play, Huawei App Gallery, Samsung Galaxy Store, Amazon
/// Appstore - none of them ship a native SDK) marshals product ids,
/// titles, and purchase tokens across this exact boundary.
[[nodiscard]] nx::vector<nx::string> to_nx_string_vector(JNIEnv *env,
                                                         jobjectArray value);
[[nodiscard]] jobjectArray
to_jstring_array(JNIEnv *env, const nx::vector<nx::string> &values);
[[nodiscard]] nx::vector<i64> to_nx_i64_vector(JNIEnv *env, jlongArray value);

/// Raw byte payloads (e.g. Play Games Services' Snapshots cloud-save
/// contents) - `nx::string` is used purely as a byte buffer here, the same
/// convention `GameCenterCloudSaves::read()` already uses for
/// `NSData`-backed saved games.
[[nodiscard]] nx::string to_nx_bytes(JNIEnv *env, jbyteArray value);
[[nodiscard]] jbyteArray to_jbyte_array(JNIEnv *env, nx::string_view value);

}
