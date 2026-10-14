#include "framework/nxtest.h"

#include "core/foundation/threading/thread_pool.h"
#include "core/foundation/vfs/asset_pipeline.h"

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

namespace {

struct Source {
  nx::blob<u8> bytes;
};

struct Product {
  nx::string text;
};

using Pipeline = nx::vfs::StagedAssetPipeline<Source, Product>;

struct MountedFiles {
  MountedFiles() {
    REQUIRE(nx::vfs::initialize());
    files = nx::vfs::make_memory_device();
    REQUIRE(files != nullptr);
    mount = nx::vfs::mount("/", files);
    REQUIRE(mount.valid());
  }
  ~MountedFiles() { nx::vfs::shutdown(); }

  void add(const nx::string_view path, const nx::string_view value) {
    nx::blob<u8> bytes(value.size());
    std::memcpy(bytes.data(), value.data(), value.size());
    files->add(path, std::move(bytes));
  }

  nx::vfs::MemoryDevice *files = nullptr;
  nx::vfs::MountId mount{};
};

[[nodiscard]] Pipeline::AcquireFn
reader(std::atomic<u32> *const calls = nullptr) {
  return Pipeline::AcquireFn(
      [calls](nx::vfs::AsyncIoContext &context, const nx::string_view path,
              const nx::cancellation_token &cancellation) {
        cancellation.throw_if_requested();
        if (calls != nullptr)
          calls->fetch_add(1, std::memory_order_relaxed);
        auto bytes = context.read(path);
        if (!bytes)
          throw std::runtime_error("read failed");
        return Source{std::move(bytes.value())};
      });
}

[[nodiscard]] Pipeline::PrepareFn
decoder(std::atomic<u32> *const calls = nullptr) {
  return Pipeline::PrepareFn(
      [calls](Source &&source, nx::string_view,
              const nx::cancellation_token &cancellation) {
        cancellation.throw_if_requested();
        if (calls != nullptr)
          calls->fetch_add(1, std::memory_order_relaxed);
        return Product{
            nx::string(reinterpret_cast<const char *>(source.bytes.data()),
                       source.bytes.size())};
      });
}

[[nodiscard]] Pipeline::PreparedBytesFn product_bytes() {
  return Pipeline::PreparedBytesFn([](const Product &product) {
    return nx::cast<u64>(product.text.size());
  });
}

} // namespace

TEST_CASE("asset fence source resolves an aggregate generation once") {
  nx::vfs::AssetFenceSource source;
  const nx::vfs::AssetFence fence = source.fence();
  REQUIRE(fence.valid());
  CHECK(fence.status() == nx::vfs::AssetFenceStatus::Pending);
  CHECK_FALSE(fence.terminal());

  CHECK(source.resolve(nx::vfs::AssetFenceStatus::Ready));
  CHECK(fence.ready());
  CHECK(fence.terminal());
  CHECK_FALSE(source.resolve(nx::vfs::AssetFenceStatus::Failed));
  CHECK(fence.ready());
}

TEST_CASE("asset pipeline: duplicate generation shares acquire and prepare") {
  MountedFiles mounted;
  mounted.add("/same", "prepared");
  nx::vfs::AsyncIoService io(io_workers(), {.max_concurrent_requests = 2});
  nx::thread_pool workers({.thread_count = 2});
  Pipeline pipeline(io, workers);
  std::atomic<u32> reads{0};
  std::atomic<u32> decodes{0};

  const nx::vfs::AssetLoadHandle first = pipeline.request(
      {.key = "/same", .generation = 7}, reader(&reads), decoder(&decodes));
  const nx::vfs::AssetLoadHandle second = pipeline.request(
      {.key = "/same", .generation = 7}, reader(&reads), decoder(&decodes));
  const nx::vfs::AssetLoadHandle third =
      pipeline.retain("/same", 7, nx::task_priority::Critical);
  REQUIRE(first.valid());
  CHECK(second == first);
  CHECK(third == first);
  CHECK(pipeline.wait(first) == nx::vfs::AssetPipelineStage::ReadyToPublish);

  nx::string published;
  REQUIRE(pipeline.publish(
      first, [&](Product &&product, nx::string_view key, const u64 generation) {
        CHECK(key == "/same");
        CHECK(generation == 7u);
        published = std::move(product.text);
        return true;
      }));
  CHECK(published == "prepared");
  CHECK(reads.load(std::memory_order_relaxed) == 1u);
  CHECK(decodes.load(std::memory_order_relaxed) == 1u);
  CHECK(pipeline.fence(first).ready());
  CHECK(pipeline.statistics().coalesced_requests == 2u);
  CHECK(pipeline.release(first));
  CHECK(pipeline.release(second));
  CHECK(pipeline.release(third));
  CHECK(pipeline.statistics().assets == 0u);
}

TEST_CASE("asset pipeline: a published generation keeps its completed fence") {
  MountedFiles mounted;
  mounted.add("/versioned", "value");
  nx::vfs::AsyncIoService io(io_workers(), {.max_concurrent_requests = 1});
  nx::thread_pool workers({.thread_count = 1});
  Pipeline pipeline(io, workers);

  const nx::vfs::AssetLoadHandle first = pipeline.request(
      {.key = "/versioned", .generation = 1}, reader(), decoder());
  const nx::vfs::AssetFence first_fence = pipeline.fence(first);
  REQUIRE(pipeline.wait(first) == nx::vfs::AssetPipelineStage::ReadyToPublish);
  REQUIRE(pipeline.publish(
      first, [](Product &&, nx::string_view, u64) { return true; }));
  REQUIRE(first_fence.ready());

  const nx::vfs::AssetLoadHandle second = pipeline.request(
      {.key = "/versioned", .generation = 2}, reader(), decoder());
  REQUIRE(second.valid());
  CHECK(first_fence.ready());
  CHECK(pipeline.stage(first) == nx::vfs::AssetPipelineStage::Published);
  CHECK(pipeline.release(first));
  CHECK(pipeline.release(second));
  pipeline.shutdown();
}

TEST_CASE(
    "asset pipeline: required dependency gates acquisition until publish") {
  MountedFiles mounted;
  mounted.add("/core", "core");
  mounted.add("/dependent", "dependent");
  nx::vfs::AsyncIoService io(io_workers(), {.max_concurrent_requests = 2});
  nx::thread_pool workers({.thread_count = 2});
  Pipeline pipeline(io, workers);
  std::atomic<u32> dependent_reads{0};

  const nx::vfs::AssetLoadHandle core =
      pipeline.request({.key = "/core", .generation = 1}, reader(), decoder());
  const nx::vfs::AssetDependency dependency{.fence = pipeline.fence(core)};
  const nx::vfs::AssetLoadHandle dependent = pipeline.request(
      {.key = "/dependent", .generation = 1, .dependencies = {&dependency, 1}},
      reader(&dependent_reads), decoder());
  REQUIRE(core.valid());
  REQUIRE(dependent.valid());

  CHECK(pipeline.wait(core) == nx::vfs::AssetPipelineStage::ReadyToPublish);
  pipeline.pump();
  CHECK(pipeline.stage(dependent) == nx::vfs::AssetPipelineStage::Queued);
  CHECK(dependent_reads.load(std::memory_order_relaxed) == 0u);
  REQUIRE(pipeline.publish(
      core, [](Product &&, nx::string_view, u64) { return true; }));
  CHECK(pipeline.wait(dependent) ==
        nx::vfs::AssetPipelineStage::ReadyToPublish);
  CHECK(dependent_reads.load(std::memory_order_relaxed) == 1u);
  REQUIRE(pipeline.publish(
      dependent, [](Product &&, nx::string_view, u64) { return true; }));
  CHECK(pipeline.release(core));
  CHECK(pipeline.release(dependent));
}

TEST_CASE("asset pipeline: newer generation makes old publication stale") {
  MountedFiles mounted;
  mounted.add("/asset", "latest");
  nx::vfs::AsyncIoService io(io_workers(), {.max_concurrent_requests = 1});
  nx::thread_pool workers({.thread_count = 1});
  Pipeline pipeline(io, workers);

  const nx::vfs::AssetLoadHandle old =
      pipeline.request({.key = "/asset", .generation = 3}, reader(), decoder());
  const nx::vfs::AssetFence old_fence = pipeline.fence(old);
  const nx::vfs::AssetLoadHandle latest =
      pipeline.request({.key = "/asset", .generation = 4}, reader(), decoder());
  REQUIRE(old.valid());
  REQUIRE(latest.valid());
  CHECK(old != latest);
  CHECK(old_fence.status() == nx::vfs::AssetFenceStatus::Stale);
  CHECK(pipeline.stage(old) == nx::vfs::AssetPipelineStage::Stale);
  CHECK_FALSE(
      pipeline.request({.key = "/asset", .generation = 2}, reader(), decoder())
          .valid());

  CHECK(pipeline.wait(latest) == nx::vfs::AssetPipelineStage::ReadyToPublish);
  REQUIRE(pipeline.publish(
      latest, [](Product &&, nx::string_view, u64) { return true; }));
  CHECK(pipeline.statistics().superseded_generations == 1u);
  CHECK(pipeline.statistics().stale_requests == 1u);
  CHECK(pipeline.release(old));
  CHECK(pipeline.release(latest));
  pipeline.pump();
  CHECK(pipeline.statistics().assets == 0u);
}

TEST_CASE("asset pipeline: rejected publication fails its dependents") {
  MountedFiles mounted;
  mounted.add("/bad", "bad");
  mounted.add("/blocked", "blocked");
  nx::vfs::AsyncIoService io(io_workers(), {.max_concurrent_requests = 1});
  nx::thread_pool workers({.thread_count = 1});
  Pipeline pipeline(io, workers);

  const nx::vfs::AssetLoadHandle bad =
      pipeline.request({.key = "/bad", .generation = 1}, reader(), decoder());
  const nx::vfs::AssetDependency dependency{.fence = pipeline.fence(bad)};
  const nx::vfs::AssetLoadHandle blocked = pipeline.request(
      {.key = "/blocked", .generation = 1, .dependencies = {&dependency, 1}},
      reader(), decoder());
  REQUIRE(pipeline.wait(bad) == nx::vfs::AssetPipelineStage::ReadyToPublish);
  CHECK_FALSE(pipeline.publish(
      bad, [](Product &&, nx::string_view, u64) { return false; }));
  pipeline.pump();
  CHECK(pipeline.stage(bad) == nx::vfs::AssetPipelineStage::Failed);
  CHECK(pipeline.stage(blocked) == nx::vfs::AssetPipelineStage::Failed);
  CHECK(pipeline.statistics().publication_failures == 1u);
  CHECK(pipeline.release(bad));
  CHECK(pipeline.release(blocked));
}

TEST_CASE("asset pipeline: final release cancels and retires work") {
  MountedFiles mounted;
  mounted.add("/cancel", "unused");
  nx::vfs::AsyncIoService io(io_workers(), {.max_concurrent_requests = 1});
  nx::thread_pool workers({.thread_count = 1});
  Pipeline pipeline(io, workers);

  const nx::vfs::AssetLoadHandle request = pipeline.request(
      {.key = "/cancel", .generation = 1}, reader(), decoder());
  const nx::vfs::AssetFence fence = pipeline.fence(request);
  REQUIRE(request.valid());
  CHECK(pipeline.release(request));
  pipeline.shutdown();
  CHECK(fence.status() == nx::vfs::AssetFenceStatus::Cancelled);
  CHECK(pipeline.statistics().assets == 0u);
}

TEST_CASE("asset pipeline: retained acquisition bytes apply backpressure") {
  MountedFiles mounted;
  mounted.add("/first", "1234");
  mounted.add("/second", "5678");
  nx::vfs::AsyncIoService io(io_workers(),
                             {.max_concurrent_requests = 2, .byte_limit = 8});
  nx::thread_pool workers({.thread_count = 1});
  Pipeline pipeline(io, workers);
  pipeline.set_limits(1, 0);
  pipeline.set_memory_limits(4, ~u64{0});

  const nx::vfs::AssetLoadHandle first =
      pipeline.request({.key = "/first", .generation = 1}, reader(), decoder());
  const nx::vfs::AssetLoadHandle second = pipeline.request(
      {.key = "/second", .generation = 1}, reader(), decoder());
  REQUIRE(first.valid());
  REQUIRE(second.valid());
  CHECK(pipeline.wait(first) == nx::vfs::AssetPipelineStage::WaitingToPrepare);
  pipeline.pump();
  CHECK(pipeline.stage(second) == nx::vfs::AssetPipelineStage::Queued);
  const nx::vfs::AssetPipelineStatistics blocked = pipeline.statistics();
  CHECK(blocked.acquired_bytes == 4u);
  CHECK(blocked.peak_acquired_bytes == 4u);
  CHECK(blocked.acquire_budget_stalls == 1u);

  pipeline.set_memory_limits(8, ~u64{0});
  pipeline.set_limits(2, 1);
  CHECK(pipeline.wait(first) == nx::vfs::AssetPipelineStage::ReadyToPublish);
  CHECK(pipeline.release(first));
  CHECK(pipeline.release(second));
  pipeline.shutdown();
}

TEST_CASE("asset pipeline: prepared bytes pause further preparation") {
  MountedFiles mounted;
  mounted.add("/one", "12345678");
  mounted.add("/two", "abcdefgh");
  nx::vfs::AsyncIoService io(io_workers(), {.max_concurrent_requests = 2});
  nx::thread_pool workers({.thread_count = 1});
  Pipeline pipeline(io, workers);
  pipeline.set_limits(2, 1);
  pipeline.set_memory_limits(~u64{0}, 8);

  const nx::vfs::AssetLoadHandle one = pipeline.request(
      {.key = "/one", .generation = 1}, reader(), decoder(), product_bytes());
  const nx::vfs::AssetLoadHandle two = pipeline.request(
      {.key = "/two", .generation = 1}, reader(), decoder(), product_bytes());
  REQUIRE(pipeline.wait(one) == nx::vfs::AssetPipelineStage::ReadyToPublish);
  // Acquisition completion races the caller under a loaded parallel test
  // run. wait() only drains worker stages; it cannot pass the prepared-byte
  // gate, so this remains a deterministic assertion of backpressure.
  CHECK(pipeline.wait(two) == nx::vfs::AssetPipelineStage::WaitingToPrepare);
  CHECK(pipeline.statistics().prepared_bytes == 8u);
  CHECK(pipeline.statistics().prepare_budget_stalls == 1u);

  REQUIRE(pipeline.publish(
      one, [](Product &&, nx::string_view, u64) { return true; }));
  REQUIRE(pipeline.wait(two) == nx::vfs::AssetPipelineStage::ReadyToPublish);
  CHECK(pipeline.statistics().peak_prepared_bytes == 8u);
  CHECK(pipeline.release(one));
  CHECK(pipeline.release(two));
  pipeline.shutdown();
}

TEST_CASE("asset pipeline: queued work ages across priority lanes") {
  MountedFiles mounted;
  mounted.add("/old", "old");
  mounted.add("/new", "new");
  nx::vfs::AsyncIoService io(io_workers(), {.max_concurrent_requests = 1});
  nx::thread_pool workers({.thread_count = 1});
  Pipeline pipeline(io, workers);
  pipeline.set_limits(0, 1);
  pipeline.set_priority_aging(2);

  const nx::vfs::AssetLoadHandle old =
      pipeline.request({.key = "/old",
                        .generation = 1,
                        .priority = nx::task_priority::Background},
                       reader(), decoder());
  pipeline.pump();
  pipeline.pump();
  pipeline.pump();
  const nx::vfs::AssetLoadHandle fresh = pipeline.request(
      {.key = "/new", .generation = 1, .priority = nx::task_priority::Critical},
      reader(), decoder());
  pipeline.set_limits(1, 1);
  pipeline.pump();

  CHECK(pipeline.stage(old) == nx::vfs::AssetPipelineStage::Acquiring);
  CHECK(pipeline.stage(fresh) == nx::vfs::AssetPipelineStage::Queued);
  CHECK(pipeline.statistics().priority_promotions == 1u);
  CHECK(pipeline.release(old));
  CHECK(pipeline.release(fresh));
  pipeline.shutdown();
}

TEST_CASE("asset pipeline: retained priority carries into preparation") {
  MountedFiles mounted;
  mounted.add("/first", "first");
  mounted.add("/urgent", "urgent");
  nx::vfs::AsyncIoService io(io_workers(), {.max_concurrent_requests = 2});
  nx::thread_pool workers({.thread_count = 1});
  Pipeline pipeline(io, workers);
  pipeline.set_limits(2, 0);

  const nx::vfs::AssetLoadHandle first =
      pipeline.request({.key = "/first",
                        .generation = 1,
                        .priority = nx::task_priority::Background},
                       reader(), decoder());
  const nx::vfs::AssetLoadHandle urgent =
      pipeline.request({.key = "/urgent",
                        .generation = 1,
                        .priority = nx::task_priority::Critical},
                       reader(), decoder());
  REQUIRE(pipeline.wait(first) ==
          nx::vfs::AssetPipelineStage::WaitingToPrepare);
  REQUIRE(pipeline.wait(urgent) ==
          nx::vfs::AssetPipelineStage::WaitingToPrepare);
  CHECK(pipeline.retain("/first", 1, nx::task_priority::Critical) == first);

  pipeline.set_limits(2, 1);
  pipeline.pump();
  CHECK(pipeline.stage(first) == nx::vfs::AssetPipelineStage::Preparing);
  CHECK(pipeline.stage(urgent) ==
        nx::vfs::AssetPipelineStage::WaitingToPrepare);
  CHECK(pipeline.release(first));
  CHECK(pipeline.release(first));
  CHECK(pipeline.release(urgent));
  pipeline.shutdown();
}

TEST_CASE("asset pipeline: each stage that ends wakes an owner waiting on the "
          "progress signal") {
  MountedFiles mounted;
  mounted.add("/signalled", "prepared");
  nx::vfs::AsyncIoService io(io_workers(), {.max_concurrent_requests = 1});
  nx::thread_pool workers({.thread_count = 2});
  nx::fiber::event progress;
  Pipeline pipeline;
  pipeline.bind(io, workers, &progress);

  const nx::vfs::AssetLoadHandle handle = pipeline.request(
      {.key = "/signalled", .generation = 1}, reader(), decoder());
  REQUIRE(handle.valid());
  u32 woken = 0;
  for (u32 round = 0;
       round < 8 &&
       pipeline.stage(handle) != nx::vfs::AssetPipelineStage::ReadyToPublish;
       ++round) {
    if (progress.wait_for(std::chrono::seconds(2)))
      ++woken;
    pipeline.pump();
  }
  CHECK(pipeline.stage(handle) == nx::vfs::AssetPipelineStage::ReadyToPublish);
  CHECK(woken >= 2u);
  CHECK(pipeline.release(handle));
  pipeline.shutdown();
}
