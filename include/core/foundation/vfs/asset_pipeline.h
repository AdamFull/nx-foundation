#pragma once

#include "core/foundation/containers/slot_map.h"
#include "core/foundation/containers/string_map.h"
#include "core/foundation/core/callable.h"
#include "core/foundation/core/scope_guard.h"
#include "core/foundation/fibers/sync.h"
#include "core/foundation/threading/thread_pool.h"
#include "core/foundation/threading/workload.h"
#include "core/foundation/vfs/async_io.h"

#include <atomic>
#include <optional>

namespace nx::vfs {

enum class AssetPipelineStage : u8 {
  None,
  Queued,
  Acquiring,
  WaitingToPrepare,
  Preparing,
  ReadyToPublish,
  Published,
  Failed,
  Cancelled,
  Stale,
};

enum class AssetFenceStatus : u8 {
  Pending,
  Ready,
  Failed,
  Cancelled,
  Stale,
};

namespace detail {
struct AssetFenceState {
  std::atomic<u32> status{static_cast<u32>(AssetFenceStatus::Pending)};
};
} // namespace detail

/// Copyable completion token for an exact asset generation. Dependencies use
/// fences rather than pipeline-local handles, so assets of different types can
/// form one load graph without coupling their payload types or registries.
class AssetFence {
public:
  AssetFence() = default;

  [[nodiscard]] bool valid() const noexcept { return m_state != nullptr; }
  [[nodiscard]] AssetFenceStatus status() const noexcept {
    return m_state != nullptr
               ? static_cast<AssetFenceStatus>(
                     m_state->status.load(std::memory_order_acquire))
               : AssetFenceStatus::Failed;
  }
  [[nodiscard]] bool ready() const noexcept {
    return status() == AssetFenceStatus::Ready;
  }
  [[nodiscard]] bool terminal() const noexcept {
    return status() != AssetFenceStatus::Pending;
  }

private:
  template <typename, typename> friend class StagedAssetPipeline;
  friend class AssetFenceSource;

  explicit AssetFence(nx::shared_ptr<detail::AssetFenceState> state) noexcept
      : m_state(std::move(state)) {}

  nx::shared_ptr<detail::AssetFenceState> m_state;
};

/// Owner-side completion source for dependency work which spans more than one
/// typed pipeline. The produced fence is immutable to consumers and may be
/// completed exactly once. This is useful for registry publication plus
/// follow-up dependencies, where no single worker operation represents the
/// complete readiness contract.
class AssetFenceSource {
public:
  AssetFenceSource() : m_state(nx::make_shared<detail::AssetFenceState>()) {}

  AssetFenceSource(const AssetFenceSource &) = delete;
  AssetFenceSource &operator=(const AssetFenceSource &) = delete;
  AssetFenceSource(AssetFenceSource &&) noexcept = default;
  AssetFenceSource &operator=(AssetFenceSource &&) noexcept = default;

  [[nodiscard]] bool valid() const noexcept { return m_state != nullptr; }
  [[nodiscard]] AssetFenceStatus status() const noexcept {
    return m_state != nullptr
               ? static_cast<AssetFenceStatus>(
                     m_state->status.load(std::memory_order_acquire))
               : AssetFenceStatus::Failed;
  }
  [[nodiscard]] bool ready() const noexcept {
    return status() == AssetFenceStatus::Ready;
  }
  [[nodiscard]] bool terminal() const noexcept {
    return status() != AssetFenceStatus::Pending;
  }
  [[nodiscard]] AssetFence fence() const noexcept {
    return AssetFence(m_state);
  }

  /// Resolves the pending generation. Pending is not a terminal result and is
  /// therefore rejected. A second resolver cannot overwrite the first result.
  [[nodiscard]] bool resolve(const AssetFenceStatus status) noexcept {
    if (m_state == nullptr || status == AssetFenceStatus::Pending)
      return false;
    u32 expected = static_cast<u32>(AssetFenceStatus::Pending);
    return m_state->status.compare_exchange_strong(
        expected, static_cast<u32>(status), std::memory_order_acq_rel,
        std::memory_order_acquire);
  }

private:
  nx::shared_ptr<detail::AssetFenceState> m_state;
};

struct AssetDependency {
  AssetFence fence;
  /// Optional dependencies delay acquisition while pending, but do not fail
  /// the dependent asset if they finish unsuccessfully.
  bool optional = false;
};

struct AssetLoadHandleTag;
using AssetLoadHandle = nx::typed_handle32<AssetLoadHandleTag>;

struct AssetPipelineRequest {
  nx::string_view key;
  /// Must increase when the observable source or its load policy changes.
  /// Requests for the same key and generation are coalesced.
  u64 generation = 1;
  nx::task_priority priority = nx::task_priority::Normal;
  std::span<const AssetDependency> dependencies;
};

struct AssetPipelineStatistics {
  usize assets = 0;
  usize interests = 0;
  usize queued = 0;
  usize acquiring = 0;
  usize waiting_to_prepare = 0;
  usize preparing = 0;
  usize ready_to_publish = 0;
  usize published = 0;
  usize failed = 0;
  usize cancelled = 0;
  usize stale = 0;
  u64 coalesced_requests = 0;
  u64 superseded_generations = 0;
  u64 stale_requests = 0;
  u64 publication_failures = 0;
  u64 acquired_bytes = 0;
  u64 peak_acquired_bytes = 0;
  u64 prepared_bytes = 0;
  u64 peak_prepared_bytes = 0;
  u64 acquire_budget_stalls = 0;
  u64 prepare_budget_stalls = 0;
  u64 priority_promotions = 0;
  nx::workload_statistics acquire_workload;
  nx::workload_statistics prepare_workload;
};

/// Reusable acquire -> prepare -> publish pipeline.
///
/// Acquisition always runs on the bounded VFS service. Preparation runs on the
/// CPU pool. Publication is deliberately caller-driven through publish(),
/// keeping renderer, audio-device and registry mutation on their owner thread.
/// The pipeline owns in-flight generations until they drain, so cancellation
/// never leaves a task referring to destroyed subsystem state.
template <typename Acquired, typename Prepared> class StagedAssetPipeline {
public:
  using AcquireFn = nx::unique_function<Acquired(
      AsyncIoContext &, nx::string_view, const nx::cancellation_token &), 48>;
  using PrepareFn = nx::unique_function<Prepared(
      Acquired &&, nx::string_view, const nx::cancellation_token &), 48>;
  using PreparedBytesFn = nx::unique_function<u64(const Prepared &), 32>;

  StagedAssetPipeline() = default;
  StagedAssetPipeline(AsyncIoService &io, nx::thread_pool &workers) noexcept
      : m_io(&io), m_workers(&workers) {}
  ~StagedAssetPipeline() { shutdown(); }

  StagedAssetPipeline(const StagedAssetPipeline &) = delete;
  StagedAssetPipeline &operator=(const StagedAssetPipeline &) = delete;

  /// @p progress, if given, is set whenever a stage ends, for an owner that
  /// waits between pumps; it must outlive the pipeline's jobs.
  void bind(AsyncIoService &io, nx::thread_pool &workers,
            fiber::event *progress = nullptr) noexcept {
    NX_ASSERT(m_entries.empty() && "cannot rebind an active asset pipeline");
    m_io = &io;
    m_workers = &workers;
    m_progress = progress;
  }

  /// Limits may change between pump calls. Zero pauses new work; max usize is
  /// unbounded at this layer, while the thread pools retain their own limits.
  void set_limits(const usize acquiring, const usize preparing) noexcept {
    m_acquire_limit = acquiring;
    m_prepare_limit = preparing;
  }

  /// Bounds payloads retained between stages. Acquisition may overshoot by at
  /// most the currently executing VFS requests, and preparation by at most the
  /// currently executing CPU jobs. An individually oversized asset is still
  /// admitted when its stage has no retained payload, avoiding permanent
  /// starvation. max u64 disables a limit; zero pauses that stage.
  void set_memory_limits(const u64 acquired_bytes,
                         const u64 prepared_bytes) noexcept {
    m_acquired_limit = acquired_bytes;
    m_prepared_limit = prepared_bytes;
  }

  /// Number of pump calls before queued work is promoted by one priority lane.
  /// Zero disables coordinator-level aging (worker queues still age normally).
  void set_priority_aging(const u64 pumps_per_lane) noexcept {
    m_aging_interval = pumps_per_lane;
  }

  /// Adds interest to the current matching generation without rebuilding the
  /// asset-specific stage callables. This is the allocation-free fast path for
  /// registries that already coalesce by key.
  [[nodiscard]] AssetLoadHandle
  retain(const nx::string_view key, const u64 generation,
         const nx::task_priority priority = nx::task_priority::Normal) noexcept {
    const auto found = m_current.find(key);
    if (found == m_current.end())
      return {};
    Entry *const current = m_entries.try_get(found->second);
    if (current == nullptr || current->generation != generation)
      return {};
    ++current->interests;
    if (priority > current->priority && !terminal(current->stage) &&
        current->stage != AssetPipelineStage::ReadyToPublish)
      current->priority = priority;
    ++m_coalesced_requests;
    return found->second;
  }

  [[nodiscard]] AssetLoadHandle request(AssetPipelineRequest request,
                                        AcquireFn acquire,
                                        PrepareFn prepare,
                                        PreparedBytesFn prepared_bytes = {}) {
    if (m_io == nullptr || m_workers == nullptr || request.key.empty() ||
        request.generation == 0 || !acquire || !prepare)
      return {};

    if (const auto found = m_current.find(request.key);
        found != m_current.end()) {
      Entry *const current = m_entries.try_get(found->second);
      if (current == nullptr) {
        m_current.erase(found);
      } else if (current->generation == request.generation) {
        return retain(request.key, request.generation, request.priority);
      } else if (request.generation < current->generation) {
        ++m_stale_requests;
        return {};
      } else {
        supersede(*current);
        m_current.erase(found);
      }
    }

    nx::shared_ptr<Operation> operation = nx::make_shared<Operation>(
        nx::string(request.key), std::move(acquire), std::move(prepare),
        std::move(prepared_bytes));
    if (operation == nullptr)
      return {};
    nx::shared_ptr<detail::AssetFenceState> fence =
        nx::make_shared<detail::AssetFenceState>();
    if (fence == nullptr)
      return {};

    Entry entry;
    entry.operation = std::move(operation);
    entry.fence = std::move(fence);
    entry.dependencies.assign(request.dependencies.begin(),
                              request.dependencies.end());
    entry.generation = request.generation;
    entry.priority = request.priority;
    entry.stage = AssetPipelineStage::Queued;
    entry.interests = 1;
    entry.queued_at = m_pump_serial;
    const AssetLoadHandle handle = m_entries.insert(std::move(entry));
    m_current.insert_or_assign(nx::string(request.key), handle);
    pump();
    return handle;
  }

  [[nodiscard]] bool prioritize(const AssetLoadHandle handle,
                                const nx::task_priority priority) noexcept {
    Entry *const entry = m_entries.try_get(handle);
    if (entry == nullptr)
      return false;
    if (!terminal(entry->stage) &&
        entry->stage != AssetPipelineStage::ReadyToPublish &&
        priority > entry->priority)
      entry->priority = priority;
    return true;
  }

  [[nodiscard]] AssetPipelineStage
  stage(const AssetLoadHandle handle) const noexcept {
    const Entry *const entry = m_entries.try_get(handle);
    return entry != nullptr ? entry->stage : AssetPipelineStage::None;
  }

  [[nodiscard]] u64 generation(const AssetLoadHandle handle) const noexcept {
    const Entry *const entry = m_entries.try_get(handle);
    return entry != nullptr ? entry->generation : 0;
  }

  [[nodiscard]] usize acquiring() const noexcept { return m_acquiring; }
  [[nodiscard]] usize preparing() const noexcept { return m_preparing; }

  [[nodiscard]] AssetFence fence(const AssetLoadHandle handle) const noexcept {
    const Entry *const entry = m_entries.try_get(handle);
    return entry != nullptr ? AssetFence(entry->fence) : AssetFence{};
  }

  /// Advances completed worker stages and starts the highest-priority work
  /// allowed by the current limits and dependency fences.
  void pump() {
    ++m_pump_serial;
    finish_acquisitions();
    finish_preparations();
    start_acquisitions();
    start_preparations();
    retire_unowned();
  }

  /// Waits for worker stages only. Publication still belongs to the caller.
  /// Same-pipeline dependencies are pumped as part of the wait; cross-pipeline
  /// dependencies must be progressed by their owning thread.
  [[nodiscard]] AssetPipelineStage wait(const AssetLoadHandle handle) {
    for (;;) {
      pump();
      Entry *const wanted = m_entries.try_get(handle);
      if (wanted == nullptr || !worker_pending(wanted->stage))
        return wanted != nullptr ? wanted->stage : AssetPipelineStage::None;

      bool waited = false;
      for (Entry &entry : m_entries) {
        if (entry.acquiring.valid()) {
          entry.acquiring.wait();
          waited = true;
          break;
        }
        if (entry.preparing.valid()) {
          entry.preparing.wait();
          waited = true;
          break;
        }
      }
      if (!waited)
        return wanted->stage;
    }
  }

  /// Runs publication synchronously on the calling thread. A failed publisher
  /// leaves the previously published resource entirely under caller control.
  template <typename PublishFn>
  [[nodiscard]] bool publish(const AssetLoadHandle handle,
                             PublishFn &&publisher) {
    pump();
    Entry *entry = m_entries.try_get(handle);
    if (entry == nullptr ||
        entry->stage != AssetPipelineStage::ReadyToPublish ||
        !entry->prepared.has_value())
      return false;

    const auto current = m_current.find(entry->operation->key);
    if (current == m_current.end() || current->second != handle) {
      supersede(*entry);
      return false;
    }

    Prepared prepared = std::move(entry->prepared.value());
    try {
      const bool accepted = static_cast<bool>(std::invoke(
          publisher, std::move(prepared), entry->operation->key.view(),
          entry->generation));
      if (accepted) {
        set_terminal(*entry, AssetPipelineStage::Published,
                     AssetFenceStatus::Ready);
        return true;
      }
      ++m_publication_failures;
      set_terminal(*entry, AssetPipelineStage::Failed,
                   AssetFenceStatus::Failed);
      return false;
    } catch (...) {
      ++m_publication_failures;
      set_terminal(*entry, AssetPipelineStage::Failed,
                   AssetFenceStatus::Failed);
      throw;
    }
  }

  [[nodiscard]] bool cancel(const AssetLoadHandle handle) noexcept {
    Entry *const entry = m_entries.try_get(handle);
    if (entry == nullptr || terminal(entry->stage))
      return false;
    (void)entry->operation->cancellation.request_stop();
    set_terminal(*entry, AssetPipelineStage::Cancelled,
                 AssetFenceStatus::Cancelled);
    erase_current(handle, *entry);
    return true;
  }

  /// Releases one coalesced interest. The final release cancels unfinished
  /// work, but its futures remain owned until pump() or shutdown() drains them.
  [[nodiscard]] bool release(const AssetLoadHandle handle) noexcept {
    Entry *const entry = m_entries.try_get(handle);
    if (entry == nullptr || entry->interests == 0)
      return false;
    --entry->interests;
    if (entry->interests != 0)
      return true;
    if (!terminal(entry->stage)) {
      (void)entry->operation->cancellation.request_stop();
      set_terminal(*entry, AssetPipelineStage::Cancelled,
                   AssetFenceStatus::Cancelled);
    }
    erase_current(handle, *entry);
    if (!entry->acquiring.valid() && !entry->preparing.valid())
      (void)m_entries.erase(handle);
    return true;
  }

  /// The service the pipeline reads through, for an owner's own I/O beside
  /// its loads; null until bound.
  [[nodiscard]] AsyncIoService *io() const noexcept { return m_io; }

  [[nodiscard]] AssetPipelineStatistics statistics() const noexcept {
    AssetPipelineStatistics out{
        .assets = m_entries.size(),
        .coalesced_requests = m_coalesced_requests,
        .superseded_generations = m_superseded_generations,
        .stale_requests = m_stale_requests,
        .publication_failures = m_publication_failures,
        .acquired_bytes = m_acquired_bytes,
        .peak_acquired_bytes = m_peak_acquired_bytes,
        .prepared_bytes = m_prepared_bytes,
        .peak_prepared_bytes = m_peak_prepared_bytes,
        .acquire_budget_stalls = m_acquire_budget_stalls,
        .prepare_budget_stalls = m_prepare_budget_stalls,
        .priority_promotions = m_priority_promotions,
        .acquire_workload = m_acquire_workload.statistics(),
        .prepare_workload = m_prepare_workload.statistics(),
    };
    for (const Entry &entry : m_entries) {
      out.interests += entry.interests;
      switch (entry.stage) {
      case AssetPipelineStage::Queued:
        ++out.queued;
        break;
      case AssetPipelineStage::Acquiring:
        ++out.acquiring;
        break;
      case AssetPipelineStage::WaitingToPrepare:
        ++out.waiting_to_prepare;
        break;
      case AssetPipelineStage::Preparing:
        ++out.preparing;
        break;
      case AssetPipelineStage::ReadyToPublish:
        ++out.ready_to_publish;
        break;
      case AssetPipelineStage::Published:
        ++out.published;
        break;
      case AssetPipelineStage::Failed:
        ++out.failed;
        break;
      case AssetPipelineStage::Cancelled:
        ++out.cancelled;
        break;
      case AssetPipelineStage::Stale:
        ++out.stale;
        break;
      case AssetPipelineStage::None:
        break;
      }
    }
    return out;
  }

  void shutdown() noexcept {
    for (Entry &entry : m_entries)
      (void)entry.operation->cancellation.request_stop();
    for (Entry &entry : m_entries) {
      drain(entry.acquiring);
      drain(entry.preparing);
      if (!terminal(entry.stage))
        set_terminal(entry, AssetPipelineStage::Cancelled,
                     AssetFenceStatus::Cancelled);
    }
    m_acquiring = 0;
    m_preparing = 0;
    m_acquired_bytes = 0;
    m_prepared_bytes = 0;
    m_acquire_budget_stalled = false;
    m_prepare_budget_stalled = false;
    m_current.clear();
    m_entries.clear();
  }

private:
  struct Operation {
    Operation(nx::string key_value, AcquireFn acquire_value,
              PrepareFn prepare_value, PreparedBytesFn prepared_bytes_value)
        : key(std::move(key_value)), acquire(std::move(acquire_value)),
          prepare(std::move(prepare_value)),
          prepared_bytes(std::move(prepared_bytes_value)) {}

    nx::string key;
    AcquireFn acquire;
    PrepareFn prepare;
    PreparedBytesFn prepared_bytes;
    nx::cancellation_source cancellation;
  };

  struct Entry {
    nx::shared_ptr<Operation> operation;
    nx::shared_ptr<detail::AssetFenceState> fence;
    nx::vector<AssetDependency> dependencies;
    struct AcquiredStage {
      Acquired value;
      AsyncIoReservation reservation;
    };
    nx::future<AcquiredStage> acquiring;
    std::optional<AcquiredStage> acquired;
    nx::future<Prepared> preparing;
    std::optional<Prepared> prepared;
    u64 generation = 0;
    nx::task_priority priority = nx::task_priority::Normal;
    AssetPipelineStage stage = AssetPipelineStage::None;
    usize interests = 0;
    u64 acquired_bytes = 0;
    u64 prepared_bytes = 0;
    u64 queued_at = 0;
    bool promoted = false;
  };

  enum class DependencyState : u8 { Pending, Ready, Failed };

  [[nodiscard]] static bool terminal(const AssetPipelineStage stage) noexcept {
    return stage == AssetPipelineStage::Published ||
           stage == AssetPipelineStage::Failed ||
           stage == AssetPipelineStage::Cancelled ||
           stage == AssetPipelineStage::Stale;
  }

  [[nodiscard]] static bool
  worker_pending(const AssetPipelineStage stage) noexcept {
    return stage == AssetPipelineStage::Queued ||
           stage == AssetPipelineStage::Acquiring ||
           stage == AssetPipelineStage::WaitingToPrepare ||
           stage == AssetPipelineStage::Preparing;
  }

  [[nodiscard]] static DependencyState
  dependencies(const Entry &entry) noexcept {
    bool pending = false;
    for (const AssetDependency &dependency : entry.dependencies) {
      const AssetFenceStatus status = dependency.fence.status();
      if (status == AssetFenceStatus::Pending) {
        pending = true;
      } else if (status != AssetFenceStatus::Ready && !dependency.optional) {
        return DependencyState::Failed;
      }
    }
    return pending ? DependencyState::Pending : DependencyState::Ready;
  }

  [[nodiscard]] bool acquire_room() const noexcept {
    return m_acquiring < m_acquire_limit &&
           (m_acquired_bytes < m_acquired_limit ||
            (m_acquired_bytes == 0 && m_acquired_limit != 0));
  }

  [[nodiscard]] bool prepare_room() const noexcept {
    return m_preparing < m_prepare_limit &&
           (m_prepared_bytes < m_prepared_limit ||
            (m_prepared_bytes == 0 && m_prepared_limit != 0));
  }

  [[nodiscard]] nx::task_priority aged_priority(const Entry &entry) const
      noexcept {
    if (m_aging_interval == 0)
      return entry.priority;
    const u64 age = m_pump_serial - entry.queued_at;
    const u64 maximum = nx::cast<u64>(
        task_priority_index(task_priority::Critical) -
        task_priority_index(entry.priority));
    const usize promotions =
        nx::cast<usize>(nx::min(age / m_aging_interval, maximum));
    return static_cast<task_priority>(task_priority_index(entry.priority) +
                                      promotions);
  }

  [[nodiscard]] bool older(const Entry &candidate,
                           const Entry &selected) const noexcept {
    const task_priority candidate_priority = aged_priority(candidate);
    const task_priority selected_priority = aged_priority(selected);
    return candidate_priority > selected_priority ||
           (candidate_priority == selected_priority &&
            candidate.queued_at < selected.queued_at);
  }

  static void update_peak(u64 &peak, const u64 value) noexcept {
    if (value > peak)
      peak = value;
  }

  static void subtract_saturated(u64 &value, const u64 amount) noexcept {
    value = amount < value ? value - amount : 0;
  }

  static void add_saturated(u64 &value, const u64 amount) noexcept {
    value = amount > ~u64{0} - value ? ~u64{0} : value + amount;
  }

  void release_acquired_memory(Entry &entry) noexcept {
    subtract_saturated(m_acquired_bytes, entry.acquired_bytes);
    entry.acquired_bytes = 0;
  }

  void release_prepared_memory(Entry &entry) noexcept {
    subtract_saturated(m_prepared_bytes, entry.prepared_bytes);
    entry.prepared_bytes = 0;
  }

  void start_acquisitions() {
    if (m_acquired_bytes >= m_acquired_limit) {
      bool queued = false;
      for (const Entry &entry : m_entries)
        queued |= entry.stage == AssetPipelineStage::Queued &&
                  dependencies(entry) == DependencyState::Ready;
      if (queued && !m_acquire_budget_stalled) {
        ++m_acquire_budget_stalls;
        m_acquire_budget_stalled = true;
      }
    } else {
      m_acquire_budget_stalled = false;
    }
    while (acquire_room()) {
      AssetLoadHandle selected;
      for (usize i = 0; i < m_entries.size(); ++i) {
        const AssetLoadHandle handle = m_entries.handle_at(i);
        Entry &entry = m_entries[handle];
        if (entry.stage != AssetPipelineStage::Queued)
          continue;
        const DependencyState state = dependencies(entry);
        if (state == DependencyState::Failed) {
          set_terminal(entry, AssetPipelineStage::Failed,
                       AssetFenceStatus::Failed);
          continue;
        }
        if (state != DependencyState::Ready)
          continue;
        if (!selected.valid() || older(entry, m_entries[selected]))
          selected = handle;
      }
      if (!selected.valid())
        return;

      Entry &entry = m_entries[selected];
      const nx::task_priority execution_priority = aged_priority(entry);
      if (!entry.promoted && execution_priority > entry.priority) {
        entry.promoted = true;
        ++m_priority_promotions;
      }
      const nx::shared_ptr<Operation> operation = entry.operation;
      const nx::cancellation_token cancellation =
          operation->cancellation.token();
      const nx::workload_ticket ticket = m_acquire_workload.submit();
      nx::future<typename Entry::AcquiredStage> future = m_io->async(
          {.priority = execution_priority, .cancellation = cancellation},
          [this, operation, cancellation,
           ticket](AsyncIoContext &context) mutable {
            const auto signal = nx::scoped_exit([this] { notify_progress(); });
            return nx::measure_workload(
                m_acquire_workload, ticket, cancellation,
                [&] {
                  Acquired value = operation->acquire(
                      context, operation->key.view(), cancellation);
                  return typename Entry::AcquiredStage{
                      std::move(value), context.take_reservation()};
                });
          });
      if (!future.valid()) {
        m_acquire_workload.reject(ticket);
        set_terminal(entry, AssetPipelineStage::Failed,
                     AssetFenceStatus::Failed);
        continue;
      }
      entry.acquiring = std::move(future);
      entry.stage = AssetPipelineStage::Acquiring;
      ++m_acquiring;
    }
  }

  void finish_acquisitions() {
    for (Entry &entry : m_entries) {
      if (!entry.acquiring.valid() || !entry.acquiring.ready())
        continue;
      const bool active = entry.stage == AssetPipelineStage::Acquiring;
      try {
        typename Entry::AcquiredStage acquired = entry.acquiring.get();
        if (active) {
          entry.acquired_bytes = acquired.reservation.bytes();
          add_saturated(m_acquired_bytes, entry.acquired_bytes);
          update_peak(m_peak_acquired_bytes, m_acquired_bytes);
          entry.acquired.emplace(std::move(acquired));
          entry.stage = AssetPipelineStage::WaitingToPrepare;
        }
      } catch (const nx::task_cancelled &) {
        if (active)
          set_terminal(entry, AssetPipelineStage::Cancelled,
                       AssetFenceStatus::Cancelled);
      } catch (...) {
        if (active)
          set_terminal(entry, AssetPipelineStage::Failed,
                       AssetFenceStatus::Failed);
      }
      NX_ASSERT(m_acquiring != 0);
      --m_acquiring;
    }
  }

  void start_preparations() {
    if (m_prepared_bytes >= m_prepared_limit) {
      bool waiting = false;
      for (const Entry &entry : m_entries)
        waiting |= entry.stage == AssetPipelineStage::WaitingToPrepare;
      if (waiting && !m_prepare_budget_stalled) {
        ++m_prepare_budget_stalls;
        m_prepare_budget_stalled = true;
      }
    } else {
      m_prepare_budget_stalled = false;
    }
    while (prepare_room()) {
      AssetLoadHandle selected;
      for (usize i = 0; i < m_entries.size(); ++i) {
        const AssetLoadHandle handle = m_entries.handle_at(i);
        const Entry &entry = m_entries[handle];
        if (entry.stage != AssetPipelineStage::WaitingToPrepare ||
            !entry.acquired.has_value())
          continue;
        if (!selected.valid() || older(entry, m_entries[selected]))
          selected = handle;
      }
      if (!selected.valid())
        return;

      Entry &entry = m_entries[selected];
      const nx::task_priority execution_priority = aged_priority(entry);
      if (!entry.promoted && execution_priority > entry.priority) {
        entry.promoted = true;
        ++m_priority_promotions;
      }
      nx::shared_ptr<typename Entry::AcquiredStage> source =
          nx::make_shared<typename Entry::AcquiredStage>(
              std::move(entry.acquired.value()));
      if (source == nullptr) {
        set_terminal(entry, AssetPipelineStage::Failed,
                     AssetFenceStatus::Failed);
        continue;
      }
      entry.acquired.reset();
      const nx::shared_ptr<Operation> operation = entry.operation;
      const nx::cancellation_token cancellation =
          operation->cancellation.token();
      const nx::workload_ticket ticket = m_prepare_workload.submit();
      nx::future<Prepared> future = m_workers->async(
          execution_priority, cancellation,
          [this, operation, source, cancellation, ticket]() mutable {
            const auto signal = nx::scoped_exit([this] { notify_progress(); });
            return nx::measure_workload(
                m_prepare_workload, ticket, cancellation,
                [&] {
                  return operation->prepare(std::move(source->value),
                                            operation->key.view(),
                                            cancellation);
                });
          });
      if (!future.valid()) {
        m_prepare_workload.reject(ticket);
        entry.acquired.emplace(std::move(*source));
        return;
      }
      entry.preparing = std::move(future);
      entry.stage = AssetPipelineStage::Preparing;
      ++m_preparing;
    }
  }

  void finish_preparations() {
    for (Entry &entry : m_entries) {
      if (!entry.preparing.valid() || !entry.preparing.ready())
        continue;
      const bool active = entry.stage == AssetPipelineStage::Preparing;
      try {
        Prepared prepared = entry.preparing.get();
        release_acquired_memory(entry);
        if (active) {
          entry.prepared_bytes = entry.operation->prepared_bytes
                                     ? entry.operation->prepared_bytes(prepared)
                                     : 0;
          add_saturated(m_prepared_bytes, entry.prepared_bytes);
          update_peak(m_peak_prepared_bytes, m_prepared_bytes);
          entry.prepared.emplace(std::move(prepared));
          entry.stage = AssetPipelineStage::ReadyToPublish;
        }
      } catch (const nx::task_cancelled &) {
        release_acquired_memory(entry);
        if (active)
          set_terminal(entry, AssetPipelineStage::Cancelled,
                       AssetFenceStatus::Cancelled);
      } catch (...) {
        release_acquired_memory(entry);
        if (active)
          set_terminal(entry, AssetPipelineStage::Failed,
                       AssetFenceStatus::Failed);
      }
      NX_ASSERT(m_preparing != 0);
      --m_preparing;
    }
  }

  void notify_progress() const noexcept {
    if (m_progress != nullptr)
      m_progress->set();
  }

  void supersede(Entry &entry) noexcept {
    if (entry.stage == AssetPipelineStage::Stale)
      return;
    // A fence describes one exact generation and completion is immutable.
    // Successfully published or already-failed generations retain their
    // terminal result while a newer generation becomes current.
    if (!terminal(entry.stage)) {
      (void)entry.operation->cancellation.request_stop();
      set_terminal(entry, AssetPipelineStage::Stale, AssetFenceStatus::Stale);
    }
    ++m_superseded_generations;
  }

  void set_terminal(Entry &entry, const AssetPipelineStage stage,
                    const AssetFenceStatus fence) noexcept {
    entry.stage = stage;
    if (entry.acquired.has_value())
      release_acquired_memory(entry);
    if (entry.prepared.has_value())
      release_prepared_memory(entry);
    entry.acquired.reset();
    entry.prepared.reset();
    if (entry.fence != nullptr) {
      entry.fence->status.store(static_cast<u32>(fence),
                                std::memory_order_release);
    }
  }

  void erase_current(const AssetLoadHandle handle, const Entry &entry) {
    const auto found = m_current.find(entry.operation->key);
    if (found != m_current.end() && found->second == handle)
      m_current.erase(found);
  }

  void retire_unowned() {
    for (usize i = 0; i < m_entries.size();) {
      const AssetLoadHandle handle = m_entries.handle_at(i);
      Entry &entry = m_entries[handle];
      if (entry.interests == 0 && terminal(entry.stage) &&
          !entry.acquiring.valid() && !entry.preparing.valid()) {
        erase_current(handle, entry);
        (void)m_entries.erase(handle);
        continue;
      }
      ++i;
    }
  }

  template <typename T> static void drain(nx::future<T> &future) noexcept {
    if (!future.valid())
      return;
    try {
      (void)future.get();
    } catch (...) {
    }
  }

  AsyncIoService *m_io = nullptr;
  nx::thread_pool *m_workers = nullptr;
  fiber::event *m_progress = nullptr;
  nx::slot_map<Entry, AssetLoadHandle> m_entries;
  nx::string_map<AssetLoadHandle> m_current;
  nx::workload_tracker m_acquire_workload{"asset::acquire"};
  nx::workload_tracker m_prepare_workload{"asset::prepare"};
  usize m_acquiring = 0;
  usize m_preparing = 0;
  usize m_acquire_limit = ~usize{0};
  usize m_prepare_limit = ~usize{0};
  u64 m_acquired_bytes = 0;
  u64 m_peak_acquired_bytes = 0;
  u64 m_prepared_bytes = 0;
  u64 m_peak_prepared_bytes = 0;
  u64 m_acquired_limit = ~u64{0};
  u64 m_prepared_limit = ~u64{0};
  u64 m_pump_serial = 0;
  u64 m_aging_interval = 120;
  u64 m_coalesced_requests = 0;
  u64 m_superseded_generations = 0;
  u64 m_stale_requests = 0;
  u64 m_publication_failures = 0;
  u64 m_acquire_budget_stalls = 0;
  u64 m_prepare_budget_stalls = 0;
  u64 m_priority_promotions = 0;
  bool m_acquire_budget_stalled = false;
  bool m_prepare_budget_stalled = false;
};

} // namespace nx::vfs
