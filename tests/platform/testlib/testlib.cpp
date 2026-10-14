
#if defined(_WIN32)
#define NX_TESTLIB_EXPORT __declspec(dllexport)
#else
#define NX_TESTLIB_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

NX_TESTLIB_EXPORT int nx_testlib_answer(void) { return 42; }

NX_TESTLIB_EXPORT int nx_testlib_add(const int a, const int b) {
  return a + b;
}

}
