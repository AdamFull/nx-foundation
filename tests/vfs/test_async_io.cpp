#include "framework/nxtest.h"

#include "core/foundation/threading/thread.h"
#include "core/foundation/threading/thread_pool.h"
#include "vfs_fixture.h"

#include "core/foundation/strings/format.h"
#include "core/foundation/vfs/async_io.h"

#include <atomic>
#include <chrono>
#include <cstring>

namespace {

// More blocking-service threads than any test asks to use, so each gets its
// requested concurrency.
nx::thread_pool &io_workers() {
  static nx::thread_pool pool(
      {.thread_count = 2, .name = "nx-test-io", .blocking_threads = 5});
  return pool;
}

} // namespace

TEST_CASE("async vfs: whole and range reads share one service") {
  vfs_fixture::scoped_vfs guard;
  nx::vfs::MemoryDevice *mem = nx::vfs::make_memory_device();
  mem->add("asset.bin", nx::blob<u8>({0, 1, 2, 3, 4, 5, 6, 7}));
  REQUIRE(nx::vfs::mount("/", mem).valid());

  nx::vfs::AsyncIoService io(io_workers(),
                             {.max_concurrent_requests = 2, .byte_limit = 16});
  auto whole = io.read("/asset.bin");
  auto range = io.read_range("/asset.bin", 2, 4);
  REQUIRE(whole.valid());
  REQUIRE(range.valid());
  auto whole_result = whole.get();
  auto range_result = range.get();
  REQUIRE(whole_result);
  REQUIRE(range_result);
  CHECK(whole_result->size() == 8u);
  REQUIRE(range_result->size() == 4u);
  CHECK((*range_result)[0] == 2u);
  CHECK((*range_result)[3] == 5u);

  const nx::vfs::AsyncIoStatistics stats = io.statistics();
  CHECK(stats.whole_reads == 1u);
  CHECK(stats.range_reads == 1u);
  CHECK(stats.bytes_read == 12u);
  CHECK(stats.workload.submitted == 2u);
  CHECK(stats.workload.completed == 2u);
  CHECK(stats.bytes_in_flight == 0u);
  CHECK(stats.peak_bytes_in_flight <= 16u);
}

TEST_CASE("async vfs: cancellation interrupts a byte-budget wait") {
  vfs_fixture::scoped_vfs guard;
  nx::vfs::AsyncIoService io(io_workers(),
                             {.max_concurrent_requests = 2, .byte_limit = 4});
  std::atomic<bool> first_reserved{false};
  std::atomic<bool> release_first{false};
  std::atomic<u64> retained_reservation{0};
  auto first =
      io.async({.priority = nx::task_priority::Normal, .reservation_bytes = 4},
               [&](nx::vfs::AsyncIoContext &context) {
                 context.reserve(2);
                 retained_reservation.store(io.statistics().bytes_in_flight,
                                            std::memory_order_release);
                 first_reserved.store(true, std::memory_order_release);
                 while (!release_first.load(std::memory_order_acquire)) {
                   context.throw_if_cancelled();
                   nx::this_thread::sleep_for(std::chrono::milliseconds(1));
                 }
               });
  REQUIRE(first.valid());
  while (!first_reserved.load(std::memory_order_acquire))
    nx::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(retained_reservation.load(std::memory_order_acquire) == 4u);

  nx::cancellation_source cancellation;
  auto waiting = io.async({.priority = nx::task_priority::Critical,
                           .cancellation = cancellation.token(),
                           .reservation_bytes = 4},
                          [](nx::vfs::AsyncIoContext &) {});
  REQUIRE(waiting.valid());
  // Cancelled once it waits for bytes, not before it starts.
  for (u32 attempt = 0;
       attempt != 1000u && io.statistics().workload.active < 2u; ++attempt)
    nx::this_thread::sleep_for(std::chrono::milliseconds(1));
  REQUIRE(io.statistics().workload.active == 2u);
  nx::this_thread::sleep_for(std::chrono::milliseconds(20));
  const auto cancelled_at = std::chrono::steady_clock::now();
  (void)cancellation.request_stop();
  // Frees the bytes later, so a waiter the cancellation failed to wake
  // fails the timing check below instead of hanging the test.
  nx::thread release_later([&] {
    nx::this_thread::sleep_for(std::chrono::seconds(2));
    release_first.store(true, std::memory_order_release);
  });
  CHECK_THROWS_AS(waiting.get(), nx::task_cancelled);
  CHECK(std::chrono::steady_clock::now() - cancelled_at <
        std::chrono::seconds(1));
  release_first.store(true, std::memory_order_release);
  release_later.join();
  first.get();

  const nx::vfs::AsyncIoStatistics stats = io.statistics();
  CHECK(stats.workload.completed == 2u);
  CHECK(stats.workload.cancelled == 1u);
  CHECK(stats.bytes_in_flight == 0u);
  CHECK(stats.peak_bytes_in_flight == 4u);
}

TEST_CASE("async vfs: a transferred reservation remains in the byte budget") {
  vfs_fixture::scoped_vfs guard;
  nx::vfs::AsyncIoService io(io_workers(),
                             {.max_concurrent_requests = 2, .byte_limit = 4});
  std::atomic<bool> second_started{false};

  auto first =
      io.async({.reservation_bytes = 4}, [](nx::vfs::AsyncIoContext &context) {
        return context.take_reservation();
      });
  REQUIRE(first.valid());
  nx::vfs::AsyncIoReservation retained = first.get();
  REQUIRE(retained.bytes() == 4u);
  CHECK(io.statistics().bytes_in_flight == 4u);

  auto second =
      io.async({.reservation_bytes = 4}, [&](nx::vfs::AsyncIoContext &) {
        second_started.store(true, std::memory_order_release);
      });
  REQUIRE(second.valid());
  for (u32 attempt = 0;
       attempt != 1000u && io.statistics().workload.active == 0u; ++attempt)
    nx::this_thread::sleep_for(std::chrono::milliseconds(1));
  REQUIRE(io.statistics().workload.active == 1u);
  CHECK_FALSE(second_started.load(std::memory_order_acquire));

  retained.reset();
  second.get();
  CHECK(second_started.load(std::memory_order_acquire));
  CHECK(io.statistics().bytes_in_flight == 0u);
}

TEST_CASE("async vfs: critical work overtakes queued background work") {
  vfs_fixture::scoped_vfs guard;
  nx::vfs::AsyncIoService io(io_workers(),
                             {.max_concurrent_requests = 1, .byte_limit = 4});
  std::atomic<bool> gate_started{false};
  std::atomic<bool> release_gate{false};
  std::atomic<u32> sequence{0};
  std::atomic<u32> background_order{0};
  std::atomic<u32> critical_order{0};

  auto gate = io.async({}, [&](nx::vfs::AsyncIoContext &context) {
    gate_started.store(true, std::memory_order_release);
    while (!release_gate.load(std::memory_order_acquire)) {
      context.throw_if_cancelled();
      nx::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  REQUIRE(gate.valid());
  while (!gate_started.load(std::memory_order_acquire))
    nx::this_thread::sleep_for(std::chrono::milliseconds(1));

  auto background =
      io.async({.priority = nx::task_priority::Background},
               [&](nx::vfs::AsyncIoContext &) {
                 background_order.store(sequence.fetch_add(1),
                                        std::memory_order_release);
               });
  auto critical = io.async({.priority = nx::task_priority::Critical},
                           [&](nx::vfs::AsyncIoContext &) {
                             critical_order.store(sequence.fetch_add(1),
                                                  std::memory_order_release);
                           });
  REQUIRE(background.valid());
  REQUIRE(critical.valid());
  release_gate.store(true, std::memory_order_release);
  gate.get();
  critical.get();
  background.get();

  CHECK(critical_order.load(std::memory_order_acquire) == 0u);
  CHECK(background_order.load(std::memory_order_acquire) == 1u);
}

TEST_CASE("async vfs: host files use the native kernel backend") {
  vfs_fixture::scoped_vfs guard;
  vfs_fixture::temp_dir dir("async_native");
  nx::vector<u8> source(1024u * 1024u);
  for (usize i = 0; i < source.size(); ++i)
    source[i] = nx::cast<u8>((i * 31u) & 0xffu);
  REQUIRE(nx::fs::file_write(dir.file("asset.bin").view(), source));
  REQUIRE(nx::vfs::mount("/", nx::vfs::make_host_device(dir.view())).valid());

  nx::vfs::AsyncIoService io(io_workers(), {.max_concurrent_requests = 4,
                                            .byte_limit = 2u * 1024u * 1024u});
  auto whole = io.read("/asset.bin");
  auto range = io.read_range("/asset.bin", 37, 8193);
  REQUIRE(whole.valid());
  REQUIRE(range.valid());
  auto whole_result = whole.get();
  auto range_result = range.get();
  auto acquired = io.async({}, [](nx::vfs::AsyncIoContext &context) {
    return context.read("/asset.bin");
  });
  REQUIRE(acquired.valid());
  auto acquired_result = acquired.get();
  REQUIRE(whole_result);
  REQUIRE(range_result);
  REQUIRE(acquired_result);
  REQUIRE(whole_result->size() == source.size());
  CHECK(std::memcmp(whole_result->data(), source.data(), source.size()) == 0);
  REQUIRE(range_result->size() == 8193u);
  CHECK(std::memcmp(range_result->data(), source.data() + 37, 8193) == 0);
  REQUIRE(acquired_result->size() == source.size());
  CHECK(std::memcmp(acquired_result->data(), source.data(), source.size()) ==
        0);

  const nx::vfs::AsyncIoStatistics stats = io.statistics();
#if defined(NX_PLATFORM_WINDOWS)
  CHECK(stats.backend == nx::vfs::AsyncIoBackend::Iocp);
#elif defined(NX_PLATFORM_ANDROID)
  // Android application sandboxes may kill the process with SIGSYS instead of
  // returning an error from io_uring_setup. Never probe that Linux backend.
  CHECK(stats.backend == nx::vfs::AsyncIoBackend::Blocking);
#endif
  if (stats.backend == nx::vfs::AsyncIoBackend::Blocking) {
    CHECK(stats.fallback_reads == 3u);
  } else {
    CHECK(stats.native_reads == 3u);
    CHECK(stats.fallback_reads == 0u);
  }
  CHECK(stats.bytes_read == source.size() * 2u + 8193u);
}

TEST_CASE("async vfs: a host file past Windows' MAX_PATH reads natively") {
  vfs_fixture::scoped_vfs guard;
  vfs_fixture::temp_dir dir("async_long");
  nx::string deep = dir.str();
  for (u32 i = 0; i < 6; ++i)
    deep = nx::fs::path::join(
        deep.view(), nx::format("{}{}", i, nx::string(59, 'd')).view());
  REQUIRE(nx::fs::create_directories(deep.view()).has_value());
  const nx::vector<u8> source{1, 2, 3, 4, 5};
  REQUIRE(nx::fs::file_write(nx::fs::path::join(deep.view(), "a.bin").view(),
                             source));
  REQUIRE(nx::vfs::mount("/", nx::vfs::make_host_device(dir.view())).valid());

  nx::vfs::AsyncIoService io(io_workers(), {.max_concurrent_requests = 1});
  nx::string path;
  for (u32 i = 0; i < 6; ++i)
    path += nx::format("/{}{}", i, nx::string(59, 'd'));
  path += "/a.bin";
  auto read = io.read(path.view());
  REQUIRE(read.valid());
  auto result = read.get();
  REQUIRE(result);
  REQUIRE(result->size() == source.size());
  CHECK(std::memcmp(result->data(), source.data(), source.size()) == 0);
#if defined(NX_PLATFORM_WINDOWS)
  CHECK(io.statistics().backend == nx::vfs::AsyncIoBackend::Iocp);
  CHECK(io.statistics().native_reads == 1u);
#endif
}

TEST_CASE("async vfs: provider shadowing is preserved by native routing") {
  vfs_fixture::scoped_vfs guard;
  vfs_fixture::temp_dir dir("async_shadow");
  REQUIRE(nx::fs::file_write(dir.file("same.bin").view(),
                             vfs_fixture::bytes_of("host")));
  REQUIRE(
      nx::vfs::mount("/", nx::vfs::make_host_device(dir.view()), 0).valid());
  nx::vfs::MemoryDevice *memory = nx::vfs::make_memory_device();
  memory->add("same.bin", vfs_fixture::blob_of("memory"));
  REQUIRE(nx::vfs::mount("/", memory, 10).valid());

  nx::vfs::AsyncIoService io(io_workers());
  auto read = io.read("/same.bin");
  REQUIRE(read.valid());
  auto result = read.get();
  REQUIRE(result);
  REQUIRE(result->size() == 6u);
  CHECK(std::memcmp(result->data(), "memory", 6) == 0);
  CHECK(io.statistics().fallback_reads == 1u);
  CHECK(io.statistics().native_reads == 0u);
}

namespace {

bool wait_for_flag(const std::atomic<bool> &flag) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!flag.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    nx::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  return true;
}

} // namespace

TEST_CASE("async vfs: a blocking request leaves the pool's worker free") {
  nx::thread_pool pool({.thread_count = 1, .blocking_threads = 2});
  nx::vfs::AsyncIoService io(pool, {.max_concurrent_requests = 1});
  std::atomic<bool> blocking{false};
  std::atomic<bool> released{false};
  std::atomic<bool> on_worker{true};
  auto request = io.async({}, [&](nx::vfs::AsyncIoContext &) {
    on_worker.store(pool.is_worker_thread());
    blocking.store(true, std::memory_order_release);
    return wait_for_flag(released);
  });
  REQUIRE(request.valid());
  REQUIRE(wait_for_flag(blocking));

  pool.submit([&] { released.store(true, std::memory_order_release); });
  CHECK(request.get());
  CHECK_FALSE(on_worker.load());
  io.shutdown();
}

TEST_CASE("async vfs: requests leave one blocking-service thread for others") {
  nx::thread_pool pool({.thread_count = 2, .blocking_threads = 2});
  nx::vfs::AsyncIoService io(pool, {.max_concurrent_requests = 4});
  CHECK(io.concurrency_limit() == 1u);

  std::atomic<bool> blocking{false};
  std::atomic<bool> released{false};
  std::atomic<bool> second_started{false};
  auto first = io.async({}, [&](nx::vfs::AsyncIoContext &) {
    blocking.store(true, std::memory_order_release);
    return wait_for_flag(released);
  });
  auto second = io.async({}, [&](nx::vfs::AsyncIoContext &) {
    second_started.store(true, std::memory_order_release);
  });
  REQUIRE(wait_for_flag(blocking));

  // A present would take this path; it must not wait behind the reads.
  nx::task_group other;
  pool.submit(other, [&] {
    nx::fiber::run_blocking(
        [&] { released.store(true, std::memory_order_release); });
  });
  other.wait();
  CHECK(first.get());
  second.get();
  CHECK(second_started.load());
  io.shutdown();
}

TEST_CASE("async vfs: shutdown cancels queued requests and waits for them") {
  nx::thread_pool pool({.thread_count = 2, .blocking_threads = 2});
  nx::vfs::AsyncIoService io(pool, {.max_concurrent_requests = 1});
  std::atomic<bool> blocking{false};
  std::atomic<u32> ran{0};
  auto gate = io.async({}, [&](nx::vfs::AsyncIoContext &context) {
    blocking.store(true, std::memory_order_release);
    for (;;) {
      context.throw_if_cancelled();
      nx::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  nx::vector<nx::future<void>> queued;
  for (u32 i = 0; i < 8; ++i)
    queued.push_back(
        io.async({}, [&](nx::vfs::AsyncIoContext &) { ran.fetch_add(1); }));
  REQUIRE(wait_for_flag(blocking));

  io.shutdown();
  CHECK_FALSE(io.valid());
  CHECK(io.statistics().workload.active == 0u);
  CHECK(ran.load() == 0u);
  CHECK_THROWS_AS(gate.get(), nx::task_cancelled);
  for (nx::future<void> &one : queued)
    CHECK_THROWS_AS(one.get(), nx::task_cancelled);
  CHECK_FALSE(io.async({}, [](nx::vfs::AsyncIoContext &) {}).valid());
}

TEST_CASE("async vfs: a request waiting on a native read lets the next one "
          "run, then waits for its slot back") {
  vfs_fixture::scoped_vfs guard;
  vfs_fixture::temp_dir dir("async_native_wait");
  const nx::vector<u8> source(64u * 1024u * 1024u, 7u);
  REQUIRE(nx::fs::file_write(dir.file("big.bin").view(), source));
  REQUIRE(nx::vfs::mount("/", nx::vfs::make_host_device(dir.view())).valid());

  nx::thread_pool pool({.thread_count = 2, .blocking_threads = 2});
  nx::vfs::AsyncIoService io(pool,
                             {.max_concurrent_requests = 1, .byte_limit = 0});
  if (io.statistics().backend == nx::vfs::AsyncIoBackend::Blocking)
    SKIP("no native backend on this platform");

  using clock = std::chrono::steady_clock;
  std::atomic<i64> read_done{0};
  std::atomic<i64> other_started{0};
  std::atomic<bool> other_running{false};
  std::atomic<bool> overlapped{false};
  const auto now = [] { return clock::now().time_since_epoch().count(); };

  auto reader = io.async({}, [&](nx::vfs::AsyncIoContext &context) {
    auto bytes = context.read("/big.bin");
    overlapped.store(other_running.load());
    read_done.store(now());
    return bytes ? bytes->size() : usize{0};
  });
  // Holds its slot until the reader's read has completed, and a little
  // longer, so a reader that went on without its slot back would see it.
  auto other = io.async({}, [&](nx::vfs::AsyncIoContext &) {
    other_started.store(now());
    other_running.store(true);
    const auto deadline = clock::now() + std::chrono::seconds(5);
    while (io.statistics().bytes_read < source.size() &&
           clock::now() < deadline)
      nx::this_thread::sleep_for(std::chrono::microseconds(200));
    nx::this_thread::sleep_for(std::chrono::milliseconds(20));
    other_running.store(false);
  });
  REQUIRE(reader.valid());
  REQUIRE(other.valid());
  CHECK(reader.get() == source.size());
  other.get();

  CHECK(other_started.load() < read_done.load());
  CHECK_FALSE(overlapped.load());
  io.shutdown();
}

// With one slot: the reader holds bytes and gives its slot up for the kernel
// read; the second request takes it and waits for bytes. The reader then needs
// a slot back, so a budget wait that kept its slot would deadlock the two.
TEST_CASE("async vfs: a request waiting for bytes does not keep the slot the "
          "holder of those bytes needs") {
  vfs_fixture::scoped_vfs guard;
  vfs_fixture::temp_dir dir("async_budget_slot");
  const nx::vector<u8> source(40u * 1024u * 1024u, 3u);
  REQUIRE(nx::fs::file_write(dir.file("big.bin").view(), source));
  REQUIRE(nx::vfs::mount("/", nx::vfs::make_host_device(dir.view())).valid());

  nx::thread_pool pool({.thread_count = 2, .blocking_threads = 2});
  nx::vfs::AsyncIoService io(
      pool, {.max_concurrent_requests = 1, .byte_limit = 64u * 1024u * 1024u});
  if (io.statistics().backend == nx::vfs::AsyncIoBackend::Blocking)
    SKIP("no native backend on this platform");

  auto reader = io.async({}, [](nx::vfs::AsyncIoContext &context) {
    auto bytes = context.read("/big.bin");
    return bytes ? bytes->size() : usize{0};
  });
  auto waiter = io.async({.reservation_bytes = 40u * 1024u * 1024u},
                         [](nx::vfs::AsyncIoContext &) { return true; });
  REQUIRE(reader.valid());
  REQUIRE(waiter.valid());

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while ((!reader.ready() || !waiter.ready()) &&
         std::chrono::steady_clock::now() < deadline)
    nx::this_thread::sleep_for(std::chrono::milliseconds(1));
  const bool finished = reader.ready() && waiter.ready();
  CHECK(finished);
  // A deadlock is broken by shutdown, which cancels the waiter.
  io.shutdown();
  if (finished) {
    CHECK(reader.get() == source.size());
    CHECK(waiter.get());
  }
}

namespace {

thread_local bool t_requesting = false;

/// Has nothing, and counts the stats made on the requesting thread.
class RemoteLikeDevice final : public nx::vfs::Device {
public:
  explicit RemoteLikeDevice(std::atomic<u32> &on_caller)
      : m_on_caller(on_caller) {}

  nx::vfs::io_result<nx::blob<u8>> read(nx::vfs::path_view) override {
    return nx::err(nx::vfs::file_error{nx::vfs::io_error::NotFound, 0});
  }
  nx::vfs::io_result<nx::vfs::stream_ptr> open(nx::vfs::path_view) override {
    return nx::err(nx::vfs::file_error{nx::vfs::io_error::NotFound, 0});
  }
  nx::vfs::FileInfo stat(nx::vfs::path_view) override {
    if (t_requesting)
      m_on_caller.fetch_add(1);
    return {};
  }
  nx::vfs::io_result<void> list(nx::vfs::path_view,
                                const nx::vfs::ListFn &) override {
    return nx::err(nx::vfs::file_error{nx::vfs::io_error::NotFound, 0});
  }
  [[nodiscard]] const char *name() const noexcept override {
    return "remote-like";
  }

private:
  std::atomic<u32> &m_on_caller;
};

} // namespace

TEST_CASE("async vfs: a device that may block is not asked on the caller's "
          "thread") {
  vfs_fixture::scoped_vfs guard;
  vfs_fixture::temp_dir dir("async_remote_first");
  REQUIRE(nx::fs::file_write(dir.file("asset.bin").view(),
                             vfs_fixture::bytes_of("host")));
  REQUIRE(
      nx::vfs::mount("/", nx::vfs::make_host_device(dir.view()), 0).valid());
  std::atomic<u32> on_caller{0};
  REQUIRE(nx::vfs::mount("/", nx::allocate<RemoteLikeDevice>(on_caller), 10)
              .valid());

  nx::vfs::AsyncIoService io(io_workers());
  t_requesting = true;
  auto read = io.read("/asset.bin");
  t_requesting = false;
  CHECK(on_caller.load() == 0u);
  REQUIRE(read.valid());
  auto result = read.get();
  REQUIRE(result);
  CHECK(std::memcmp(result->data(), "host", 4) == 0);
}
