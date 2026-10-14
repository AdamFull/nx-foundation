
#include "framework/nxtest.h"

#include "core/foundation/threading/thread_pool.h"

#include <algorithm>
#include <vector>

namespace {

void append_indices(const usize begin, const usize end,
                    nx::vector<usize> &bin) {
  for (usize i = begin; i < end; ++i)
    bin.push_back(i);
}

[[nodiscard]] bool is_iota(const nx::vector<usize> &v, const usize n) {
  if (v.size() != n)
    return false;
  for (usize i = 0; i < n; ++i)
    if (v[i] != i)
      return false;
  return true;
}

}

TEST_CASE("parallel_collect: without a pool it runs on the calling thread") {
  nx::vector<nx::vector<usize>> bins;
  nx::vector<usize> out;
  nx::parallel_collect(nullptr, 100u, 1u, bins, out, append_indices);

  CHECK(is_iota(out, 100));
  CHECK(bins.empty());
}

TEST_CASE("parallel_collect: below the threshold it stays serial") {
  nx::thread_pool pool;
  nx::vector<nx::vector<usize>> bins;
  nx::vector<usize> out;
  nx::parallel_collect(&pool, 10u, 1000u, bins, out, append_indices);

  CHECK(is_iota(out, 10));
  CHECK(bins.empty());
}

TEST_CASE("parallel_collect: an empty range does nothing") {
  nx::thread_pool pool;
  nx::vector<nx::vector<usize>> bins;
  nx::vector<usize> out;
  nx::parallel_collect(&pool, 0u, 1u, bins, out, append_indices);
  CHECK(out.empty());
}

TEST_CASE("parallel_collect: parallel covers every index exactly once") {
  constexpr usize COUNT = 10'000;
  nx::thread_pool pool;
  nx::vector<nx::vector<usize>> bins;
  nx::vector<usize> out;
  nx::parallel_collect(&pool, COUNT, 1u, bins, out, append_indices);

  CHECK(is_iota(out, COUNT));
}

TEST_CASE("parallel_collect: the two paths agree") {
  constexpr usize COUNT = 8'192;
  nx::thread_pool pool;

  nx::vector<nx::vector<usize>> bins;
  nx::vector<usize> parallel, serial;
  nx::parallel_collect(&pool, COUNT, 1u, bins, parallel, append_indices);
  nx::parallel_collect(nullptr, COUNT, 1u, bins, serial, append_indices);

  REQUIRE(parallel.size() == serial.size());
  bool same = true;
  for (usize i = 0; i < parallel.size(); ++i)
    if (parallel[i] != serial[i])
      same = false;
  CHECK(same);
}

TEST_CASE("parallel_collect: it appends, so two passes accumulate") {
  nx::thread_pool pool;
  nx::vector<nx::vector<usize>> bins;
  nx::vector<usize> out;

  nx::parallel_collect(&pool, 4'096u, 1u, bins, out, append_indices);
  const usize after_first = out.size();
  nx::parallel_collect(&pool, 4'096u, 1u, bins, out, append_indices);

  CHECK(after_first == 4'096u);
  CHECK(out.size() == 8'192u);
}

TEST_CASE("parallel_collect: the bins are reused rather than reallocated") {
  nx::thread_pool pool;
  nx::vector<nx::vector<usize>> bins;
  nx::vector<usize> out;

  nx::parallel_collect(&pool, 10'000u, 1u, bins, out, append_indices);
  const usize teams = bins.size();
  REQUIRE(teams > 0u);

  out.clear();
  nx::parallel_collect(&pool, 10'000u, 1u, bins, out, append_indices);
  CHECK(bins.size() == teams);
  CHECK(is_iota(out, 10'000));
}
