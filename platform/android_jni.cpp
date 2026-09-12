#include "core/foundation/platform/android_jni.h"

namespace nx::android {

jclass find_class(JNIEnv *const env, const char *const name) {
  if (env == nullptr || name == nullptr)
    return nullptr;
  const jclass cls = env->FindClass(name);
  if (cls == nullptr)
    env->ExceptionClear();
  return cls;
}

jmethodID instance_method(JNIEnv *const env, const jobject object,
                          const char *const name, const char *const signature) {
  if (env == nullptr || object == nullptr)
    return nullptr;
  const jclass cls = env->GetObjectClass(object);
  if (cls == nullptr)
    return nullptr;
  const jmethodID id = env->GetMethodID(cls, name, signature);
  if (id == nullptr)
    env->ExceptionClear();
  env->DeleteLocalRef(cls);
  return id;
}

jmethodID static_method(JNIEnv *const env, const jclass cls,
                        const char *const name, const char *const signature) {
  if (env == nullptr || cls == nullptr)
    return nullptr;
  const jmethodID id = env->GetStaticMethodID(cls, name, signature);
  if (id == nullptr)
    env->ExceptionClear();
  return id;
}

nx::string to_nx_string(JNIEnv *const env, const jstring value) {
  if (env == nullptr || value == nullptr)
    return {};
  const jsize length = env->GetStringUTFLength(value);
  const char *const chars = env->GetStringUTFChars(value, nullptr);
  if (chars == nullptr)
    return {};
  nx::string out(chars, static_cast<usize>(length));
  env->ReleaseStringUTFChars(value, chars);
  return out;
}

jstring to_jstring(JNIEnv *const env, const nx::string_view value) {
  if (env == nullptr)
    return nullptr;
  const nx::string owned(value);
  return env->NewStringUTF(owned.c_str());
}

nx::vector<nx::string> to_nx_string_vector(JNIEnv *const env,
                                           const jobjectArray value) {
  nx::vector<nx::string> out;
  if (env == nullptr || value == nullptr)
    return out;
  const jsize count = env->GetArrayLength(value);
  out.reserve(static_cast<usize>(count));
  for (jsize i = 0; i < count; ++i) {
    const jstring element = static_cast<jstring>(env->GetObjectArrayElement(value, i));
    out.push_back(to_nx_string(env, element));
    if (element != nullptr)
      env->DeleteLocalRef(element);
  }
  return out;
}

jobjectArray to_jstring_array(JNIEnv *const env,
                              const nx::vector<nx::string> &values) {
  if (env == nullptr)
    return nullptr;
  const jclass string_class = find_class(env, "java/lang/String");
  if (string_class == nullptr)
    return nullptr;
  const jobjectArray array = env->NewObjectArray(
      static_cast<jsize>(values.size()), string_class, nullptr);
  env->DeleteLocalRef(string_class);
  if (array == nullptr)
    return nullptr;
  for (usize i = 0; i < values.size(); ++i) {
    const jstring element = to_jstring(env, values[i].view());
    env->SetObjectArrayElement(array, static_cast<jsize>(i), element);
    if (element != nullptr)
      env->DeleteLocalRef(element);
  }
  return array;
}

nx::vector<i64> to_nx_i64_vector(JNIEnv *const env, const jlongArray value) {
  nx::vector<i64> out;
  if (env == nullptr || value == nullptr)
    return out;
  const jsize count = env->GetArrayLength(value);
  out.resize(static_cast<usize>(count));
  static_assert(sizeof(jlong) == sizeof(i64));
  env->GetLongArrayRegion(value, 0, count, reinterpret_cast<jlong *>(out.data()));
  return out;
}

}
