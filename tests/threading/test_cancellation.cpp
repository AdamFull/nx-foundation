#include "framework/nxtest.h"

#include "core/foundation/threading/cancellation.h"
#include "core/foundation/threading/thread.h"

#include <atomic>
#include <chrono>

using nx::cancellation_callback;

TEST_CASE("cancellation callback: runs once when the token is cancelled") {
  nx::cancellation_source source;
  u32 calls = 0;
  const cancellation_callback callback(
      source.token(), cancellation_callback::Fn([&calls] { ++calls; }));
  CHECK(calls == 0u);
  CHECK(source.request_stop());
  CHECK(calls == 1u);
  CHECK_FALSE(source.request_stop());
  CHECK(calls == 1u);
}

TEST_CASE("cancellation callback: runs at once on a token already cancelled") {
  nx::cancellation_source source;
  (void)source.request_stop();
  u32 calls = 0;
  const cancellation_callback callback(
      source.token(), cancellation_callback::Fn([&calls] { ++calls; }));
  CHECK(calls == 1u);
}

TEST_CASE("cancellation callback: one destroyed first never runs") {
  nx::cancellation_source source;
  u32 first = 0;
  u32 second = 0;
  u32 third = 0;
  const cancellation_callback kept_first(
      source.token(), cancellation_callback::Fn([&first] { ++first; }));
  {
    const cancellation_callback gone(
        source.token(), cancellation_callback::Fn([&second] { ++second; }));
  }
  const cancellation_callback kept_third(
      source.token(), cancellation_callback::Fn([&third] { ++third; }));
  (void)source.request_stop();
  CHECK(first == 1u);
  CHECK(second == 0u);
  CHECK(third == 1u);
}

TEST_CASE("cancellation callback: a token that cannot stop never runs it") {
  u32 calls = 0;
  const cancellation_callback callback(
      nx::cancellation_token{},
      cancellation_callback::Fn([&calls] { ++calls; }));
  CHECK(calls == 0u);
}

TEST_CASE("cancellation callback: destruction waits for a run on another "
          "thread to finish") {
  for (u32 round = 0; round < 20; ++round) {
    nx::cancellation_source source;
    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};
    bool finished_before_destruction = false;
    nx::thread stopper;
    {
      const cancellation_callback callback(
          source.token(), cancellation_callback::Fn([&] {
            entered.store(true);
            nx::this_thread::sleep_for(std::chrono::milliseconds(20));
            finished.store(true);
          }));
      stopper = nx::thread([&] { (void)source.request_stop(); });
      while (!entered.load())
        nx::this_thread::yield();
    }
    finished_before_destruction = finished.load();
    stopper.join();
    CHECK(finished_before_destruction);
  }
}
