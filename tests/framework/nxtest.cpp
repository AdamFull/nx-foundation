#include "nxtest.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace nxtest {
namespace {

std::vector<test_case> &registry() {
  static std::vector<test_case> *r = new std::vector<test_case>();
  return *r;
}

struct run_state {
  const test_case *current = nullptr;
  int failures = 0;
  long long assertions = 0;
  std::vector<const char *> subcases;
  std::string skip_reason;
};

run_state &state() {
  static run_state s;
  return s;
}

std::string subcase_path() {
  const run_state &s = state();
  if (s.subcases.empty())
    return {};
  std::string out;
  for (const char *name : s.subcases) {
    out += out.empty() ? "" : " / ";
    out += name;
  }
  return " [" + out + "]";
}

}

void register_test(const test_case &tc) { registry().push_back(tc); }

void report_success() { ++state().assertions; }

void report_failure(const char *file, int line, const char *macro,
                    const char *expr, const std::string &decomposed) {
  run_state &s = state();
  ++s.assertions;
  ++s.failures;
  std::fprintf(stderr, "%s(%d): FAILED%s\n", file, line,
               subcase_path().c_str());
  std::fprintf(stderr, "  %s( %s )\n", macro, expr);
  if (!decomposed.empty())
    std::fprintf(stderr, "  with expansion: %s\n", decomposed.c_str());
  std::fflush(stderr);
}

void report_skip(const char *file, const int line, const std::string &reason) {
  run_state &s = state();
  s.skip_reason = reason.empty() ? "no reason given" : reason;
  s.skip_reason += " (";
  s.skip_reason += file;
  s.skip_reason += ":";
  s.skip_reason += std::to_string(line);
  s.skip_reason += ")";
  throw skipped{};
}

void push_subcase(const char *name) { state().subcases.push_back(name); }
void pop_subcase() {
  run_state &s = state();
  if (!s.subcases.empty())
    s.subcases.pop_back();
}

namespace {

enum class outcome { passed, failed, skipped_case };

outcome run_one(const test_case &tc, bool verbose) {
  run_state &s = state();
  s.current = &tc;
  s.failures = 0;
  s.subcases.clear();
  s.skip_reason.clear();

  const long long before = s.assertions;

  try {
    tc.fn();
  } catch (const require_failed &) {
  } catch (const skipped &) {
  } catch (const std::exception &e) {
    ++s.failures;
    std::fprintf(stderr, "%s(%d): FAILED%s\n", tc.file, tc.line,
                 subcase_path().c_str());
    std::fprintf(stderr, "  uncaught std::exception: %s\n", e.what());
  } catch (...) {
    ++s.failures;
    std::fprintf(stderr, "%s(%d): FAILED%s\n", tc.file, tc.line,
                 subcase_path().c_str());
    std::fprintf(stderr, "  uncaught exception of unknown type\n");
  }

  if (s.failures != 0) {
    std::printf("[FAIL] %s (%lld assertions)\n", tc.name,
                s.assertions - before);
    std::fflush(stdout);
    return outcome::failed;
  }

  if (!s.skip_reason.empty()) {
    std::printf("[SKIP] %s - %s\n", tc.name, s.skip_reason.c_str());
    std::fflush(stdout);
    return outcome::skipped_case;
  }

  if (verbose)
    std::printf("[PASS] %s (%lld assertions)\n", tc.name,
                s.assertions - before);
  std::fflush(stdout);
  return outcome::passed;
}

void print_usage(const char *argv0) {
  std::printf(
      "usage: %s [options] [name-filter]\n"
      "  --list-test-cases   print every test case name, one per line\n"
      "  --test-case=<name>  run only the case with this exact name\n"
      "  --verbose, -v       print a line per case even when it passes\n"
      "  --help, -h          show this message\n",
      argv0);
}

}

int run(int argc, char **argv) {
  std::vector<test_case> &tests = registry();

  std::sort(tests.begin(), tests.end(),
            [](const test_case &a, const test_case &b) {
              return std::strcmp(a.name, b.name) < 0;
            });

  for (std::size_t i = 1; i < tests.size(); ++i) {
    if (std::strcmp(tests[i - 1].name, tests[i].name) == 0) {
      std::fprintf(stderr, "nxtest: duplicate test case name \"%s\"\n",
                   tests[i].name);
      return 2;
    }
  }

  const char *filter = nullptr;
  bool list_only = false;
  bool verbose = false;

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (std::strcmp(arg, "--list-test-cases") == 0) {
      list_only = true;
    } else if (std::strncmp(arg, "--test-case=", 12) == 0) {
      filter = arg + 12;
    } else if (std::strcmp(arg, "--verbose") == 0 ||
               std::strcmp(arg, "-v") == 0) {
      verbose = true;
    } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
      print_usage(argv[0]);
      return 0;
    } else if (arg[0] == '-') {
      std::fprintf(stderr, "nxtest: unknown option \"%s\"\n", arg);
      print_usage(argv[0]);
      return 2;
    } else {
      filter = arg;
    }
  }

  if (list_only) {
    for (const test_case &tc : tests)
      std::printf("%s\n", tc.name);
    return 0;
  }

  int passed = 0, failed = 0, skipped_count = 0, ran = 0;
  for (const test_case &tc : tests) {
    if (filter && std::strcmp(filter, tc.name) != 0)
      continue;
    ++ran;
    switch (run_one(tc, verbose)) {
    case outcome::passed:
      ++passed;
      break;
    case outcome::failed:
      ++failed;
      break;
    case outcome::skipped_case:
      ++skipped_count;
      break;
    }
  }

  if (filter && ran == 0) {
    std::fprintf(stderr, "nxtest: no test case named \"%s\"\n", filter);
    return 2;
  }

  std::printf("\n%d/%d test cases passed, %lld assertions\n", passed, ran,
              state().assertions);
  if (skipped_count != 0)
    std::printf("%d test case(s) SKIPPED\n", skipped_count);
  if (failed != 0)
    std::printf("%d test case(s) FAILED\n", failed);
  std::fflush(stdout);

  if (failed != 0)
    return 1;
  if (skipped_count == ran)
    return SKIP_EXIT_CODE;
  return 0;
}

}

int main(int argc, char **argv) { return nxtest::run(argc, argv); }
