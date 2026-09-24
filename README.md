# nx foundation

The bottom layer of the engine: a C++20 standard-library replacement built
around the `nx` namespace. It uses `mimalloc` for allocation, packs custom
containers and utilities, and favors explicit, allocation-aware, exception-light
designs. Every public type listed below carries Doxygen documentation in its
header.

Foundation builds as a static library (`nx_foundation`, alias `nx::foundation`).
Most of it is header-only; the platform backends (`threading`, `platform`,
`diagnostics`) are real translation units. Include paths are rooted at
`engine/`, so headers are pulled in by scope:

```cpp
#include "core/foundation/core/foundation.h"
#include "foundation/containers/small_vector.h"
#include "foundation/strings/utf8_string.h"
```

## Layout

| Folder | Scope |
| --- | --- |
| `core/` | Platform/compiler detection, allocators, and the primitives everything else builds on (`foundation.h`, `handle.h`, `hash.h`, `result.h`, `scope_guard.h`, `callable.h`, `enum.h`, `random.h`). |
| `containers/` | Custom container types. |
| `strings/` | UTF-8 string, string view, `std::format` integration, interned ids. |
| `fibers/` | Stackful context switching and fiber stacks, the base of the fiber job system. |
| `threading/` | Threads, synchronization primitives, and the fiber-backed thread pool. |
| `platform/` | OS-facing services: filesystem, virtual memory, process/memory/time queries. |
| `diagnostics/` | Logging, assertions, and profiler instrumentation. |
| `serialization/` | Data interchange helpers, including declarative command-line parsing. |
| `ecs/` | The entity-component-system registry. |

## Containers

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::blob` | `containers/blob.h` | An owning array whose size is fixed at construction. A `std::vector` that never resizes, for buffers of known length (file contents, decoded data, scratch). |
| `nx::fixed_vector` | `containers/fixed_vector.h` | A vector with compile-time capacity and no heap use. Lives on the stack; growing past `N` is an error. |
| `nx::small_vector` | `containers/small_vector.h` | A dynamic vector that keeps its first `N` elements inline and only spills to the heap when it grows past them. For collections that are usually small. |
| `nx::flat_map` | `containers/flat_map.h` | An ordered key/value map over sorted contiguous arrays. Cache-friendly binary-search lookups; best for read-heavy maps built once and queried often. |
| `nx::hash_map` | `containers/hash_map.h` | A SwissTable-style open-addressing unordered map with SIMD probing. The general-purpose key/value map. |
| `nx::slot_map` | `containers/slot_map.h` | A handle-keyed pool that keeps live elements densely packed for cache-friendly iteration. Handles survive moves; addresses do not. |
| `nx::stable_pool` | `containers/stable_pool.h` | A handle-keyed pool whose element addresses stay valid until erased. Use when other systems hold raw references into the pool. |
| `nx::sparse_set` | `containers/sparse_set.h` | A sparse set mapping handles (entity ids) to dense payloads with O(1) insert/erase/contains. The component store for an ECS; `T = void` makes a plain set. |
| `nx::mpmc::Queue` | `containers/concurrent_queue.h` | A bounded lock-free queue for many producers and many consumers. |
| `nx::mpsc::Queue` | `containers/concurrent_queue.h` | A bounded lock-free queue for many producers and a single consumer, with `consume_all`/`consume_batch` helpers. |

## Strings

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::string` | `strings/utf8_string.h` | An owning UTF-8 string with small-string optimization. Transcodes from char/char8_t/char16_t/char32_t/wchar_t and converts to `nx::string_view`. |
| `nx::string_view` | `strings/utf8_string_view.h` | A non-owning UTF-8 view (`std::string_view` alias). |
| `nx::id_string` | `strings/id_string.h` | An 8-byte interned string identified by a 64-bit FNV-1a hash: integer compare/order/hash, computable at compile time, with the original bytes recoverable via a process-wide intern pool. A baked asset key and constant-expression switch label (Unreal `FName` / bitsquid `idstring` in spirit). |
| `nx::format` / `nx::format_to` | `strings/format.h` | `std::format` returning `nx::string`, plus a bounded `format_to(char[N], ...)` for stack buffers. A `std::formatter` for `nx::string` is provided. |

## ECS registry

`ecs/registry.h` provides `nx::ecs::registry` (and `nx::ecs::registry64`, plus
the `nx::ecs::basic_registry<Entity, Alloc>` template): an allocator-aware ECS
over versioned entity handles with explicit component registration, O(1)
add/get/remove, transient views and cached queries, a built-in parent-child
hierarchy, and statistics. Built on `nx::sparse_set`. Components must be
registered before use; see the header for the full API, invalidation rules,
destroy policies, and thread-safety model.

```cpp
#include "foundation/ecs/registry.h"

struct Transform { float x, y, z; };
struct Velocity  { float dx, dy; };

nx::ecs::registry world;
world.register_component<Transform>({ .name = "Transform" });
world.register_component<Velocity>({ .name = "Velocity" });

auto e = world.create();
world.emplace<Transform>(e, 0.f, 0.f, 0.f);
world.emplace<Velocity>(e, 1.f, 0.f);

world.view<Transform, Velocity>().each(
    [](auto entity, Transform& t, Velocity& v) {
      t.x += v.dx;
      t.y += v.dy;
    });

auto child = world.create();
world.set_parent(child, e);

world.destroy(e); // detaches children by default; see destroy_policy
```

## Command-line parsing

`serialization/arguments.h` provides `nx::cli::Parser`, a typed, declarative
argument parser for engine executables and tools. It supports root arguments,
subcommands, long and short options, strict scalar conversion, repeatable
values, enumerated choices, `--` termination, generated help, and consistent
diagnostics. Bound values must outlive the parser.

```cpp
#include "core/foundation/serialization/arguments.h"

nx::string input;
bool verbose = false;

nx::cli::Parser arguments("tool", "Process an input file.");
arguments.root()
    .positional("input", input, "Input file.")
    .flag("verbose", verbose, "Enable diagnostics.", {.short_name = 'v'});

const nx::cli::ParseResult result = arguments.parse(argc, argv);
if (result.status == nx::cli::ParseStatus::Help)
  std::puts(arguments.help(result.command).c_str());
```

## Threading

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::thread` | `threading/thread.h` | A cross-platform OS thread with name, stack size, affinity and priority set at creation. Joins on destruction. Also `nx::this_thread` (id, yield, sleep, name, affinity, priority). |
| `nx::thread_parker` | `threading/thread.h` | Parks one thread until `unpark()` or a deadline kept to well under a millisecond on every platform (a Windows futex timeout rounds to the ~15.6 ms system tick). An `unpark()` before the park is not lost. |
| `nx::cpu` | `threading/thread.h` | CPU topology queries: logical/physical core counts, SMT layout, hybrid P/E efficiency classes, cache hierarchy, NUMA node count. |
| `nx::thread_pool` | `threading/thread_pool.h` | A priority-aware task pool on an `nx::fiber::Scheduler` of its own, with bounded background concurrency, `task_group` batches, cancellable futures and continuations, `when_all`/`when_any`, and opt-in statistics/timing. Waiting in a task suspends its fiber instead of occupying the worker, and its tasks reach two blocking-service threads (`config::blocking_threads`) through `nx::fiber::run_blocking`. With `config::adopt_main_thread` the constructing thread becomes its main thread: `run_on_main(fn)` runs `fn` there, and that thread runs such work in `pump_main(budget)` or while it waits. |
| `nx::cancellation_source` / `nx::cancellation_token` | `threading/cancellation.h` | Shared cooperative cancellation for queued and running work. Tokens are cheap to copy and retain their cancellation generation when a source is reset. `nx::cancellation_callback` runs a function when a token is cancelled, so a wait can be woken by cancellation instead of polling for it; its destructor unregisters it, waiting if another thread is running it. |
| `nx::mutex` et al. | `threading/sync.h` | Futex-based synchronization: `spin_lock`, `mutex`, `recursive_mutex`, `cond_var`, `rw_lock` (writer-preferring), `semaphore`, `event`, `barrier`, `once_flag`/`call_once`, and `scoped_lock`/`scoped_shared_lock` guards. 4-8 bytes of state each, kernel wait only under contention. |
| `nx::futex` | `threading/sync.h` | The raw 32-bit futex wait/wake primitive everything in `sync.h` is built on (WaitOnAddress / `SYS_futex`). |

## Fibers

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::fiber::Context` | `fibers/context.h` | A suspended or running execution context. `switch_to(from, to, data)` suspends one and resumes the other, passing a pointer both ways; `exit_to` leaves a fiber for good. Saves the ABI's callee-saved registers and FP control state; on Windows it also swaps the TIB stack bounds, so SEH, `__chkstk` and guard-page growth work on fiber stacks. Carries unwind info so debuggers walk fiber stacks, and ASan/TSan annotations. |
| `nx::fiber::StackPool` / `nx::fiber::Stack` | `fibers/stack.h` | Guard-paged fiber stacks that only consume memory as deep as they are used (Windows commits on demand; POSIX faults pages lazily), recycled most-recent-first with a bounded cache and a `trim()` that returns grown pages to the OS. |

| `nx::fiber::Scheduler` | `fibers/scheduler.h` | The fiber job system. Worker threads run jobs (`{entry, arg}`, three priorities, per-worker queues with stealing) on pooled fibers. `wait(counter)` suspends the calling fiber instead of its thread. The constructing thread is the main thread: it runs only main-affine work (`run_on_main`, `switch_to_main`), inside `pump_main(budget)` or while it waits. `run_blocking(fn)` moves the calling fiber to one of `Config::blocking_threads` service threads for a call that blocks in the OS or a driver, then back to a worker. Destruction finishes outstanding jobs. |
| `nx::fiber::Counter` | `fibers/scheduler.h` | Outstanding-work count that jobs decrement on completion and fibers wait on. Any thread may decrement it, including I/O completion threads the scheduler does not own. |

| `nx::fiber::mutex` et al. | `fibers/sync.h` | `mutex`, `rw_lock` (writer-preferring), `event`, `semaphore`, `cond_var` and `sleep_for` that suspend the waiting fiber instead of its thread, with timed waits on the scheduler's timers. Waking hands ownership straight to the waiter. On a thread no scheduler owns, the same objects block the thread. Unlike `nx::mutex`, they may be held across a suspension. |

A fiber can resume on a different OS thread than the one it suspended on. Code
that switches or waits must not keep a `thread_local` address across the call,
and must not hold an OS-level lock (`nx::mutex`, `nx::rw_lock`) across a wait.
Debug builds assert the lock rule, and `nx::fiber::no_suspend_scope` extends
the same check to other thread-affine resources. MSVC builds add `/GT`, so the
optimizer does not reuse a thread-local address across a call. Clang has no
such switch, which is why the scheduler reads its own thread-locals through
non-inlined calls.

## Platform

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::fs::path_view` | `platform/filesystem.h` | A non-owning view over a UTF-8 path with lexical queries (filename, stem, extension, parent, join). Also: whole-file read/write, atomic replacement through `staged_file`/`file_write_atomic`, directory helpers, `executable_dir()`, platform-standard per-user `application_data_dir()`, and a seekable `stream` abstraction (`open_file`/`open_memory` returning an RAII `stream_ptr`). |
| `nx::vmem` | `platform/os.h` | Virtual-memory reservation/commit/protect and page-size/granularity queries. |
| `nx::os` | `platform/os.h` | Process/system queries: resident and peak memory (`mem_get_stats`), a monotonic clock (`ticks_ns`/`ticks_ms`), and total physical RAM (`system_ram_mb`). |

## Diagnostics

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::log` | `diagnostics/log.h` | Asynchronous multi-sink logger: `nx::logv/logd/logi/logw/loge/logc` helpers, runtime-interned string categories (`log::category("name")`, predefined `app`/`gpu`/`render`/`system`), per-category and global level filtering, and console (colored) / debugger / rotating-file sinks. Records format on the caller. Once `log::attach_drain_pool(pool)` is called (the engine attaches its pool), they queue and a low-priority job on that pool writes them. Otherwise, and whenever the queue is full, the caller writes them itself. `log::flush()` writes whatever is queued on the calling thread, and a fatal report calls it before copying the log tail. `log::init()`/`shutdown()` bracket its lifetime. |
| `nx::crash` | `diagnostics/crash.h` | Cross-platform fatal-report installation, bounded runtime notes and log tail, native minidumps where supported, and explicit non-signal reports. A `Fatal` report may include bounded binary attachments; they are atomically written as sanitized sidecars beside the text report and pruned with their owner. |
| `NX_ASSERT` | `diagnostics/diagnostic.h` | Contract checks that log, flush, and break in debug builds and compile out in release. Also the `nx::dbg_*` helpers (break, is-present, wait-for-attach). |
| `NX_PROFILE_*` | `diagnostics/profiler.h` | Zone/frame/thread-name/plot instrumentation macros, forwarding to the Tracy client; `NX_PROFILE_ZONE_TEXT` names a zone at run time. No-ops unless the profile configuration is built (`NX_ENABLE_PROFILER` / `NX_PROFILE_ENABLED`). |

## Utilities and components

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::function` | `core/callable.h` | A type-erased, heap-backed callable wrapper, like `std::function`. |
| `nx::inplace_function` | `core/callable.h` | A type-erased callable stored in a fixed inline buffer; never allocates. |
| `nx::enums::map` | `core/enum.h` | A fixed array indexed directly by enum value, sized from the enum's `Count`. Zero-overhead enum-to-value lookup. |
| `nx::enums::flags` | `core/enum.h` | A type-safe bitset over a scoped enum's enumerators, with bit operators. |
| `nx::result` / `nx::err` | `core/result.h` | An error-or-value type for exception-free error handling, with `and_then`/`transform`/`or_else`/`transform_error` combinators. |
| `nx::scope_guard` | `core/scope_guard.h` | Run an action on scope exit unless dismissed. Also `scoped_exit()` and the Go-style `defer { ... };` macro. |
| `nx::scoped_value` | `core/scope_guard.h` | Temporarily replace a value and restore it on scope exit, including correctly nested scopes. |
| `nx::Guid` | `core/random.h` | A 128-bit unique identifier (UUID): parse, format, compare, hash. Generate via `nx::Rng::guid()`. |
| `nx::Rng` | `core/random.h` | A seedable pseudo-random generator. |

## Core

`core/foundation.h` underpins everything: platform/compiler/architecture
detection macros, fixed-width type aliases (`i32`, `u64`, `usize`, `f32`, ...),
small math helpers (`min`, `max`, `clamp`, `align_up`, `next_pow2`), the bounded
`str_copy` string helper, the `mem_alloc` / `allocate` family, and the default
`nx::Allocator` (mimalloc-backed). `core/handle.h` defines the versioned
`basic_handle` (and `handle32` / `handle64`) used as keys by the handle-based
containers. `core/hash.h` provides the hashing utilities (`nx::hash`,
FNV/wyhash-style helpers) consumed by `hash_map` and `id_string`.
