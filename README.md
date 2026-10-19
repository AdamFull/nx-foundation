# nx foundation

The bottom layer of the engine: a C++20 standard-library replacement built
around the `nx` namespace. It uses `mimalloc` for allocation, packs custom
containers and utilities, and favors explicit, allocation-aware, exception-light
designs. Every public type listed below carries Doxygen documentation in its
header.

Foundation builds as a static library (`nx_foundation`, alias `nx::foundation`).
Most of it is header-only; the platform backends (`threading`, `platform`,
`diagnostics`) are real translation units. The library lives under
`include/core/foundation/`, and `include/` is its include root, so headers are
pulled in by scope:

```cpp
#include "core/foundation/core/foundation.h"
#include "core/foundation/containers/small_vector.h"
#include "core/foundation/strings/utf8_string.h"
```

## Using it in a project

Foundation is the nx2d engine's standard library and builds on
its own. Its dependencies are submodules under `third_party/`, so check it out
recursively, then add it to a CMake project and link `nx::foundation`:

```text
git submodule add https://github.com/AdamFull/nx-foundation external/nx-foundation
git submodule update --init --recursive
```

```cmake
add_subdirectory(external/nx-foundation)
target_link_libraries(my_game PRIVATE nx::foundation)
```

It needs C++20 and CMake 3.21. Linking `nx::foundation` brings what its headers
need (the include root, the definitions below, and on MSVC `/utf-8`,
`/Zc:preprocessor` and `/GT`). `nx::foundation_options` is the warning and
toolchain policy foundation itself is built with, for code that wants the same.

| Option | Default | Effect |
| --- | --- | --- |
| `NX_FOUNDATION_MIMALLOC` | `ON` | Allocate through mimalloc; off, the system allocator (`NX_USE_MIMALLOC=0`). |
| `NX_FOUNDATION_TRACY` | `ON` | Build Tracy's client, the profiler backend. |
| `NX_ENABLE_PROFILER` | `OFF` | Compile the `NX_PROFILE_*` instrumentation (`NX_PROFILE_ENABLED=1`); needs Tracy. |
| `NX_SHIPPING` | `OFF` | A sealed release build (`NX_BUILD_SHIPPING=1`): quieter logs, cooked assets only. |
| `NX_WARNINGS_AS_ERRORS` | `OFF` | `/WX` or `-Werror` in `nx::foundation_options`. |
| `NX_FOUNDATION_TESTS` | on when top level | The tests under `tests/` and nxtest (`nx::test`, `nxtest_discover_tests`). |

| Dependency | Used for |
| --- | --- |
| mimalloc | The allocator (optional). |
| Tracy | Profiler instrumentation (optional). |
| minizip-ng, zlib-ng, Zstandard | ZIP archives in the VFS (`ZipDevice`). Their targets (`nx::minizip`, `nx::zstd`, `zlib-ng::zlibstatic`) are there for the parent project too. |

Built as the top-level project, the tests run with CTest:

```text
cmake -S . -B build
cmake --build build
ctest --test-dir build -C Debug
```

## Layout

| Folder (under `include/core/foundation/`) | Scope |
| --- | --- |
| `core/` | Platform/compiler detection, allocators, and the primitives everything else builds on (`foundation.h`, `handle.h`, `hash.h`, `result.h`, `scope_guard.h`, `callable.h`, `enum.h`, `random.h`, `sha256.h`). |
| `containers/` | Custom container types. |
| `strings/` | UTF-8 string, string view, `std::format` integration, interned ids. |
| `fibers/` | Stackful context switching and fiber stacks, the base of the fiber job system. |
| `threading/` | Threads, synchronization primitives, and the fiber-backed thread pool. |
| `platform/` | OS-facing services: filesystem, virtual memory, process/memory/time queries. |
| `diagnostics/` | Logging, assertions, and profiler instrumentation. |
| `serialization/` | Data interchange helpers, including declarative command-line parsing. |
| `ecs/` | The entity-component-system registry. |
| `simd/` | Data-parallel types: C++26 `std::simd` for C++20. |

## Containers

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::blob` | `containers/blob.h` | An owning array whose size is fixed at construction. A `std::vector` that never resizes, for buffers of known length (file contents, decoded data, scratch). |
| `nx::fixed_vector` | `containers/fixed_vector.h` | A vector with compile-time capacity and no heap use. Lives on the stack; growing past `N` is an error. |
| `nx::small_vector` | `containers/small_vector.h` | A dynamic vector that keeps its first `N` elements inline and only spills to the heap when it grows past them. For collections that are usually small. |
| `nx::flat_map` | `containers/flat_map.h` | An ordered key/value map over sorted contiguous arrays. Cache-friendly binary-search lookups; best for read-heavy maps built once and queried often. |
| `nx::hash_map` | `containers/hash_map.h` | A SwissTable-style open-addressing unordered map with SIMD probing. The general-purpose key/value map. |
| `nx::hash_set` | `containers/hash_set.h` | The unordered set on the same Swiss table as `hash_map` (`containers/raw_hash_table.h`). Iterators are always const. `nx::string_set` (`containers/string_set.h`) looks strings up by `string_view`. |
| `nx::map` / `nx::set` | `containers/map.h` | Ordered map and set on a B-tree of 512-byte nodes (`containers/btree.h`), with lower/upper bounds and heterogeneous lookup under a transparent comparison. For keys with an order but no hash, or iteration in key order. Elements move between nodes, so an insert or erase invalidates every iterator and reference, and keys and values must move without throwing. As fast as `std::map` on lookups, twice as fast where entries come and go; `flat_map` only when it is built once and read, since each insert into it is linear. |
| `nx::slot_map` | `containers/slot_map.h` | A handle-keyed pool that keeps live elements densely packed for cache-friendly iteration. Handles survive moves; addresses do not. |
| `nx::stable_pool` | `containers/stable_pool.h` | A handle-keyed pool whose element addresses stay valid until erased. Use when other systems hold raw references into the pool. |
| `nx::sparse_set` | `containers/sparse_set.h` | A sparse set mapping handles (entity ids) to dense payloads with O(1) insert/erase/contains. The component store for an ECS; `T = void` makes a plain set. |
| `nx::mpmc::Queue` | `containers/concurrent_queue.h` | A bounded lock-free queue for many producers and many consumers. |
| `nx::mpsc::Queue` | `containers/concurrent_queue.h` | A bounded lock-free queue for many producers and a single consumer, with `consume_all`/`consume_batch` helpers. |
| `nx::sort_by_key` / `nx::sort_by_key_into` | `containers/radix_sort.h` | A stable radix sort on a 32-bit key: it sorts (key, index) pairs, then gathers each element once. `coherent_sort_by_key_into` is for data re-sorted every frame with little change - it starts from the previous call's order and falls back to the radix when too much moved, with the same result either way. |
| `nx::sort_by_key` / `nx::sort_by_key_into` | `containers/radix_sort.h` | A stable radix sort on a 32-bit key: it sorts (key, index) pairs, then gathers each element once. `coherent_sort_by_key_into` is for data re-sorted every frame with little change - it starts from the previous call's order and falls back to the radix when too much moved, with the same result either way. |

## Strings

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::string` | `strings/utf8_string.h` | An owning UTF-8 string with small-string optimization. Transcodes from char/char8_t/char16_t/char32_t/wchar_t and converts to `nx::string_view`. |
| `nx::string_view` | `strings/utf8_string_view.h` | A non-owning UTF-8 view (`std::string_view` alias). |
| `nx::id_string` | `strings/id_string.h` | An 8-byte interned string identified by a 64-bit FNV-1a hash: integer compare/order/hash, computable at compile time, with the original bytes recoverable via a process-wide intern pool. A baked asset key and constant-expression switch label (Unreal `FName` / bitsquid `idstring` in spirit). |
| `nx::base64_encode` | `strings/base64.h` | Standard RFC 4648 base64 with padding; `base64url_encode` is the URL-safe alphabet without padding, as URLs and PKCE want it. |
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
#include "core/foundation/ecs/registry.h"

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

## SIMD

`simd/simd.h` is C++26's `<simd>` (`std::simd`, as in the current working
draft) for C++20, in `nx::simd`: `basic_vec`/`vec` and `basic_mask`/`mask`
with every operator, conversion and constructor (broadcast, converting,
generator, range, complex parts); the flags, `alignment`, `rebind` and
`resize` traits; checked and partial loads and stores, gathers and scatters;
`reduce` and the mask reductions; `select`, `min`, `max`, `minmax`, `clamp`;
static and dynamic `permute`, `compress`, `expand`, `chunk`, `cat`, `iota`;
the `<cmath>`, `<bit>` and `<complex>` functions. Names and semantics follow
the standard, so code moves to `std::simd` by changing the namespace.

```cpp
#include "core/foundation/simd/simd.h"

namespace sd = nx::simd;

void scale(std::span<float> values, float factor) {
  using V = sd::vec<float>;
  size_t i = 0;
  for (; i + V::size() <= values.size(); i += V::size())
    sd::unchecked_store(sd::unchecked_load<V>(values.subspan(i)) * factor,
                        values.subspan(i));
  sd::partial_store(sd::partial_load<V>(values.subspan(i)) * factor,
                    values.subspan(i));
}
```

| Backend | Used for |
| --- | --- |
| GCC and Clang vector extensions | Every target those compilers build: SSE to AVX-512 on x86, NEON and SVE on AArch64. Mask movemasks use SSE2/AVX2 or NEON directly. |
| NEON and SVE intrinsics | AArch64 under GCC and Clang, over the vector extensions: across-vector reductions, rounding, bit counting and reversal with NEON; with SVE, predicated partial and masked loads and stores, compress, same-width gather and scatter, and SVE2 `bit_compress`/`bit_expand`. |
| SSE2, SSE4.1 and AVX2 intrinsics | MSVC on x64. `/arch:AVX` enables the SSE4.1 paths; `/arch:AVX2` makes 32-byte registers native and adds FMA. |
| Arrays of registers | Widths that fill several registers (`vec<float, 12>` is three SSE registers). |
| Plain arrays | Widths that fill no register exactly, other compilers (MSVC on ARM64 among them), and every type under `NX_SIMD_FORCE_FIXED`, which the tests use as the reference. |

- `vec<T>` has the native width of the compile flags: 4 floats with SSE or
  NEON, 8 with AVX2, and `__ARM_FEATURE_SVE_BITS / 32` with a fixed SVE
  vector length (`-msve-vector-bits=256`). Without a fixed length SVE works
  on 16-byte vectors. Translation units built with different flags see different
  types, so keep `vec<T>` out of interfaces shared between them.
- Implicit broadcasts follow the standard: only value-preserving ones, so
  `v * 2.f` compiles and `v * 2` does not for `vec<float>`.
  `std::integral_constant<int, 2>{}` stands in for C++26's `std::cw<2>`.
- Vectors, masks and the algorithms work in constant expressions; the
  `<cmath>` and `<complex>` functions do not, as C++20's `<cmath>` is not
  `constexpr`.
- Exact operations (arithmetic, comparisons, `sqrt`, rounding, `fma`, `fmin`,
  `fmax`, classification) run on whole registers. Transcendental and special
  functions apply the scalar `<cmath>` function per element, so results equal
  the scalar ones bit for bit. The special functions exist where the
  standard library has them (`__cpp_lib_math_special_functions`; not libc++).
- `vec<std::complex<T>>` keeps real and imaginary parts in separate
  registers; multiplication is the plain formula, division is
  `std::complex`'s.
- Widths go up to 64 lanes. `flag_aligned` and `flag_overaligned<N>` are
  checked in debug builds only: unaligned loads cost the same on every
  supported CPU.
- The tests build once per backend: `nx_simd_fixed_tests` on plain arrays,
  `nx_simd_avx2_tests` when the machine runs AVX2, `nx_simd_sve2_tests` when
  it runs SVE2 or the build targets another AArch64 machine,
  `nx_simd_sve2_bitperm_tests` when it runs SVE2 BitPerm, and
  `nx_simd_tests` alone when cross-compiling, to be run on the target.
- `nx.natvis` shows vector lanes and masks in the Visual Studio debugger.
- Under MSVC, vectors narrower than a register (`vec<u8, 4>`) are plain
  arrays, so a widening conversion from them runs element by element.

### Interleaved data

`simd/interleave.h` (an extension) loads K-element tuples, such as stereo
frames, RGBA pixels or xyz positions, as one vector per component, and
stores them back:

```cpp
#include "core/foundation/simd/interleave.h"

using V = sd::vec<float>;
const auto [left, right] = sd::unchecked_load_interleaved<V, 2>(frames, 2 * V::size());
sd::unchecked_store_interleaved(std::array{right, left}, swapped, 2 * V::size());
```

- Lane i of part k is `first[i * K + k]`. The unchecked forms touch exactly
  `K * V::size()` elements; `partial_load_interleaved` and
  `partial_store_interleaved` take the first n, loading zeros past them and
  storing nothing past them.
- Two components are one shuffle per pair of registers: `uzp`/`zip` on NEON
  and SSE/AVX unpacks and shuffles on x86 (through the vector extensions
  under GCC and Clang, intrinsics under MSVC). Four are two rounds of two.
  Other counts, and plain-array vectors, go lane by lane.
- Elements are not converted: the range holds the vector's element type.

### Pipelines

`simd/pipeline.h` (an extension; `std::simd` has no counterpart) builds a
loop once from element-wise stages and runs it over ranges:

```cpp
#include "core/foundation/simd/pipeline.h"

const auto to_unit = sd::pipeline([](auto p) {
  return sd::rebind_t<float, decltype(p)>(p) * (1.f / 255.f);
});
to_unit(pixels, sd::out(unit)); // u8 in, float out

const auto [lo, hi] = sd::pipeline([](auto v) { return std::tuple(v, v); },
                                   sd::fold(sd::minimum, sd::maximum))(values);

const size_t visible = sd::pipeline(sd::keep_if([](auto r) { return r > 0.f; }))(
    radii, sd::out(kept));
```

- A stage takes the previous values as `vec`s, the inputs first, and returns
  a `vec` or a `std::tuple` of them; generic lambdas let the pipeline choose
  the width. `keep_if(pred)` drops lanes. `fold(reducers...)` ends a pipeline
  with one reducer per value: `sum`, `product`, `minimum`, `maximum`, the
  `std::plus<>`-style operations, or `reducer(op, identity)`. A pipeline
  without a fold takes `out(ranges...)` last and returns how many elements it
  wrote. A pipeline is itself a stage, so parts are built once and reused.
- An input is a range, `interleaved<K>(range)` for K values per element, or
  `index` (the element's index as `int`; `index_as<T>` for another type),
  which takes no data and needs a sized input beside it. `out` takes ranges
  and `interleaved<K>(range)` the same way, K values each:

  ```cpp
  // Gain ramp over stereo frames, in place.
  sd::pipeline([=](auto l, auto r, auto f) {
    const auto g = start + step * f;
    return std::tuple(l * g, r * g);
  })(sd::interleaved<2>(frames), sd::index_as<float>,
     sd::out(sd::interleaved<2>(frames)));
  ```
- The stages run as one loop: nothing is stored between stages, and several
  outputs or folds take one pass over the data.
- The widest element type in the flow fills one register, so `u8` in and
  `float` out run `vec<float>::size()` lanes. `lanes<N>` overrides that and
  `unroll<N>` sets the chunks in flight: 2 by default, and 8, 4 or 2
  accumulators for folds of one, two or more values.
- The last partial chunk is padded with its own first element, and the
  index with its own first index: stages only see values from the data,
  never a padding zero to divide by.
- Element types convert at the edges by the `std::simd` rules; a narrowing
  output takes `out(flag_convert, ranges...)`.
- Inputs have equal sizes. An output holds at least that many elements and is
  one of the inputs or disjoint from all of them.
- A fold's order depends on the lane count and unroll: it is the same on
  every run of a build, but differs in the last bits from a scalar loop and
  between targets.
- Pipelines run in constant expressions.
- With Clang a pipeline is as fast as the hand-written loop, and its folds
  beat a one-accumulator loop. MSVC keeps fold accumulators in memory, so a
  fold of several values runs at about half Clang's speed there.
- `sd::pipeline` is a function: as a class, `sd::pipeline(f)(in, sd::out(o));`
  would declare a variable `f`.

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

## INI files

`serialization/ini.h` parses INI text into an `nx::ini::Document` and writes
one back deterministically, without comments. To change a value in a file a
person edits, `nx::ini::assign(text, section, key, value)` returns the text
with that key set and every other line, comments included, as it was: the
key's lines are rewritten in place, a missing key goes after its section's
last assignment, a missing section at the end, in the text's own line
endings. Text that does not parse is refused rather than rewritten.

## Threading

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::thread` | `threading/thread.h` | A cross-platform OS thread with name, stack size, affinity and priority set at creation. Joins on destruction. Also `nx::this_thread` (id, the kernel's `os_id`, yield, sleep, name, affinity, priority). |
| `nx::thread_parker` | `threading/thread.h` | Parks one thread until `unpark()` or a deadline kept to well under a millisecond on every platform (a Windows futex timeout rounds to the ~15.6 ms system tick). An `unpark()` before the park is not lost. |
| `nx::cpu` | `threading/thread.h` | CPU topology queries: logical/physical core counts, SMT layout, hybrid P/E efficiency classes, cache hierarchy, NUMA node count. |
| `nx::thread_pool` | `threading/thread_pool.h` | A priority-aware task pool on an `nx::fiber::Scheduler` of its own, with bounded background concurrency, `task_group` batches, `parallel_for`, `parallel_region` (a team that meets at an `nx::fiber::barrier`, so it may start inside a task), cancellable futures and continuations, `when_all`/`when_any`, and opt-in statistics/timing. Waiting in a task suspends its fiber instead of occupying the worker, and its tasks reach two blocking-service threads (`config::blocking_threads`) through `nx::fiber::run_blocking`. With `config::adopt_main_thread` the constructing thread becomes its main thread: `run_on_main(fn)` runs `fn` there, and that thread runs such work in `pump_main(budget)` or while it waits. `worker_os_ids` names the threads its tasks run on, for OS services that take thread ids. |
| `nx::serial_queue` | `threading/serial_queue.h` | Tasks that must not overlap or reorder, such as one database's reads and writes, run on a `thread_pool` one at a time in the order posted, with no thread of its own: one pool task drains the queue while anything is in it. Bounded (`post` refuses past its capacity unless told otherwise); `blocking` runs tasks through `nx::fiber::run_blocking` for work that blocks in the OS; `flush` suspends a fiber rather than its worker. |
| `nx::cancellation_source` / `nx::cancellation_token` | `threading/cancellation.h` | Shared cooperative cancellation for queued and running work. Tokens are cheap to copy and retain their cancellation generation when a source is reset. `nx::cancellation_callback` runs a function when a token is cancelled, so a wait can be woken by cancellation instead of polling for it; its destructor unregisters it, waiting if another thread is running it. |
| `nx::mutex` et al. | `threading/sync.h` | Futex-based synchronization: `spin_lock`, `mutex`, `recursive_mutex`, `cond_var`, `rw_lock` (writer-preferring), `semaphore`, `event`, `barrier`, `once_flag`/`call_once`, and `scoped_lock`/`scoped_shared_lock` guards. 4-8 bytes of state each, kernel wait only under contention. |
| `nx::futex` | `threading/sync.h` | The raw 32-bit futex wait/wake primitive everything in `sync.h` is built on (WaitOnAddress / `SYS_futex`). |

## Fibers

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::fiber::Context` | `fibers/context.h` | A suspended or running execution context. `switch_to(from, to, data)` suspends one and resumes the other, passing a pointer both ways; `exit_to` leaves a fiber for good. Saves the ABI's callee-saved registers and FP control state; on Windows it also swaps the TIB stack bounds, so SEH, `__chkstk` and guard-page growth work on fiber stacks. Carries unwind info so debuggers walk fiber stacks, and ASan/TSan annotations. |
| `nx::fiber::StackPool` / `nx::fiber::Stack` | `fibers/stack.h` | Guard-paged fiber stacks that only consume memory as deep as they are used (Windows commits on demand; POSIX faults pages lazily), recycled most-recent-first with a bounded cache and a `trim()` that returns grown pages to the OS. |

| `nx::fiber::Scheduler` | `fibers/scheduler.h` | The fiber job system. Worker threads run jobs (`{entry, arg}`, three priorities, per-worker queues with stealing) on pooled fibers. `wait(counter)` suspends the calling fiber instead of its thread. The constructing thread is the main thread: it runs only main-affine work (`run_on_main`), inside `pump_main(budget)` or while it waits, and always on its own stack, never a fiber's. `run_blocking(fn)` moves the calling fiber to one of `Config::blocking_threads` service threads for a call that blocks in the OS or a driver, then back to a worker. Destruction finishes outstanding jobs. |
| `nx::fiber::Counter` | `fibers/scheduler.h` | Outstanding-work count that jobs decrement on completion and fibers wait on. Any thread may decrement it, including I/O completion threads the scheduler does not own. |

| `nx::fiber::mutex` et al. | `fibers/sync.h` | `mutex`, `rw_lock` (writer-preferring), `event`, `semaphore`, `cond_var`, `barrier` and `sleep_for` that suspend the waiting fiber instead of its thread, with timed waits on the scheduler's timers. Waking hands ownership straight to the waiter. On a thread no scheduler owns, the same objects block the thread. Unlike `nx::mutex`, they may be held across a suspension. |

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
| `nx::fs::path_view` | `platform/filesystem.h` | A non-owning view over a UTF-8 path with lexical queries (filename, stem, extension, parent, join). Also: whole-file read/write, atomic replacement through `staged_file`/`file_write_atomic`, directory helpers, `executable_dir()`, platform-standard per-user `application_data_dir()`, and a seekable `stream` abstraction (`open_file`/`open_memory` returning an RAII `stream_ptr`; `open_file_views` opens a file once for streams with cursors of their own, readable from any thread, while the file may be deleted or replaced on disk). |
| `nx::vfs::resolve_beside` | `vfs/vfs.h` | The path a file names, made absolute: a relative one is in that file's folder and may not leave it (no empty, `.` or `..` parts). Assets use it for the files they name. |
| `nx::vmem` | `platform/os.h` | Virtual-memory reservation/commit/protect and page-size/granularity queries. |
| `nx::os` | `platform/os.h` | Process/system queries: resident and peak memory (`mem_get_stats`), a monotonic clock (`ticks_ns`/`ticks_ms`), total physical RAM (`system_ram_mb`), the user's country or region as two ISO 3166 letters (`region`: the Windows geo setting, the Android locale property, the Apple current locale, `LC_ALL`/`LC_MESSAGES`/`LANG` elsewhere; `region_of_locale` parses a locale name), and bytes from the operating system's cryptographically secure generator (`random_bytes`: BCrypt on Windows, `arc4random_buf` on Apple, `/dev/urandom` elsewhere). |
| `nx::process` | `platform/process.h` | Runs a child process on Windows, Linux and macOS with no input, standard output and error captured together up to a limit (the rest is read and dropped), the environment given or the parent's, and a timeout. A timeout kills the child and what it started, through a job object on Windows and the child's own process group elsewhere. It blocks: call it from `nx::fiber::run_blocking`. `quote_argument` spells an argument for a Windows command line. |

## Diagnostics

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::log` | `diagnostics/log.h` | Asynchronous multi-sink logger: `nx::logv/logd/logi/logw/loge/logc` helpers, runtime-interned string categories (`log::category("name")`, predefined `app`/`gpu`/`render`/`system`), per-category and global level filtering, and console (colored) / debugger / rotating-file sinks. Records format on the caller. Once `log::attach_drain_pool(pool)` is called (the engine attaches its pool), they queue and a low-priority job on that pool writes them. Otherwise, and whenever the queue is full, the caller writes them itself. `log::flush()` writes whatever is queued on the calling thread, and a fatal report calls it before copying the log tail. `log::init()`/`shutdown()` bracket its lifetime; outside them records go to stderr, and a sink added then, such as the engine's in-game console, observes them beside it. |
| `nx::crash` | `diagnostics/crash.h` | Cross-platform fatal-report installation, bounded runtime notes and log tail, native minidumps where supported, and explicit non-signal reports. A `Fatal` report may include bounded binary attachments; they are atomically written as sanitized sidecars beside the text report and pruned with their owner. |
| `NX_ASSERT` | `diagnostics/diagnostic.h` | Contract checks that log, flush, and break in debug builds and compile out in release. Also the `nx::dbg_*` helpers (break, is-present, wait-for-attach). |
| `NX_PROFILE_*` | `diagnostics/profiler.h` | Zone/frame/thread-name/plot instrumentation macros, forwarding to the Tracy client; `NX_PROFILE_ZONE_TEXT` names a zone at run time. No-ops unless the profile configuration is built (`NX_ENABLE_PROFILER` / `NX_PROFILE_ENABLED`). |

## Utilities and components

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::unique_ptr` / `nx::make_unique` | `core/foundation.h` | Sole ownership of an object on the nx heap. There is no constructor from a raw pointer: `make_unique` creates (honouring `alignof(T)`, and reaching a private constructor that befriends it), and `adopt_unique` takes back a pointer from `release()` or `nx::allocate`. Converts to a base with a virtual destructor, so it owns interfaces too. With a custom deleter it accepts raw pointers like `std::unique_ptr`. |
| `nx::shared_ptr` / `nx::make_shared` | `core/foundation.h` | Shared ownership, one pointer wide: the count sits in front of the object. No weak half; `nx::life_token` / `life_watch` answer "is it still alive". |
| `nx::function` | `core/callable.h` | A type-erased, heap-backed callable wrapper, like `std::function`. |
| `nx::inplace_function` | `core/callable.h` | A type-erased callable stored in a fixed inline buffer; never allocates. |
| `nx::enums::map` | `core/enum.h` | A fixed array indexed directly by enum value, sized from the enum's `Count`. Zero-overhead enum-to-value lookup. |
| `nx::enums::flags` | `core/enum.h` | A type-safe bitset over a scoped enum's enumerators, with bit operators. |
| `nx::result` / `nx::err` | `core/result.h` | An error-or-value type for exception-free error handling, with `and_then`/`transform`/`or_else`/`transform_error` combinators. |
| `nx::scope_guard` | `core/scope_guard.h` | Run an action on scope exit unless dismissed. Also `scoped_exit()` and the Go-style `defer { ... };` macro. |
| `nx::scoped_value` | `core/scope_guard.h` | Temporarily replace a value and restore it on scope exit, including correctly nested scopes. |
| `nx::Guid` | `core/random.h` | A 128-bit unique identifier (UUID): parse, format, compare, hash. Generate via `nx::Rng::guid()`. |
| `nx::Rng` | `core/random.h` | A seedable pseudo-random generator; not for secrets, which come from `nx::os::random_bytes`. |
| `nx::sha256` | `core/sha256.h` | The SHA-256 digest (FIPS 180-4) of a byte span, for identities and checks such as PKCE; not a password hash. |

## Core

`core/foundation.h` underpins everything: platform/compiler/architecture
detection macros, fixed-width type aliases (`i32`, `u64`, `usize`, `f32`, ...),
small math helpers (`min`, `max`, `clamp`, `align_up`, `next_pow2`), the bounded
`str_copy` string helper, the `mem_alloc` / `allocate` family, and the default
`nx::Allocator` (mimalloc-backed). `core/handle.h` defines the versioned
`basic_handle` (and `handle32` / `handle64`) used as keys by the handle-based
containers. `core/hash.h` provides the hashing utilities (`nx::hash`,
FNV/wyhash-style helpers) consumed by `hash_map` and `id_string`.
