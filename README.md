# nx foundation

The bottom layer of the engine: a C++20 standard-library replacement in the
`nx` namespace, allocating through `mimalloc`, with explicit,
allocation-aware, exception-light designs.

Foundation builds as a static library (`nx_foundation`, alias
`nx::foundation`). Containers, strings, math, ECS and most of SIMD are
header-only; `core`, `fibers` (with per-ABI assembly), `threading`,
`platform`, `vfs` (with one native async-I/O backend per OS), `diagnostics`,
`serialization`, `reflection` and `simd/gemm` are compiled. Sources sit beside
their headers under `include/core/foundation/`; `include/` is the include
root:

```cpp
#include "core/foundation/core/foundation.h"
#include "core/foundation/containers/small_vector.h"
```

## Using it in a project

Foundation is the nx2d engine's standard library and builds on its own. Its
dependencies are submodules under `third_party/`, so check it out
recursively, then link `nx::foundation`:

```text
git submodule add https://github.com/AdamFull/nx-foundation external/nx-foundation
git submodule update --init --recursive
```

```cmake
add_subdirectory(external/nx-foundation)
target_link_libraries(my_game PRIVATE nx::foundation)
```

It needs C++20 and CMake 3.21. Linking `nx::foundation` brings the include
root, the definitions below, `/utf-8` on any MSVC-ABI compiler, and on cl.exe
also `/Zc:preprocessor` and `/GT`. `nx::foundation_options` is the warning
and toolchain policy foundation itself is built with.

| Option | Default | Effect |
| --- | --- | --- |
| `NX_FOUNDATION_MIMALLOC` | `ON` | Allocate through mimalloc; off, the system allocator (`NX_USE_MIMALLOC=0`). |
| `NX_FOUNDATION_TRACY` | `ON` | Build Tracy's client, the profiler backend. |
| `NX_ENABLE_PROFILER` | `OFF` | Send the `NX_PROFILE_*` instrumentation to Tracy (`NX_PROFILE_ENABLED=1`); needs Tracy. |
| `NX_SHIPPING` | `OFF` | A sealed release build (`NX_BUILD_SHIPPING=1`): quieter logs, cooked assets only. |
| `NX_WARNINGS_AS_ERRORS` | `OFF` | `/WX` or `-Werror` in `nx::foundation_options`. |
| `NX_FOUNDATION_TESTS` | on when top level | The tests under `tests/` and nxtest. |

| Dependency | Used for |
| --- | --- |
| mimalloc | The allocator (optional). |
| Tracy | Profiler instrumentation (optional). |
| minizip-ng, zlib-ng, Zstandard | ZIP archives in the VFS; Zstandard also backs `nx::zstd`. Their targets (`nx::minizip`, `nx::zstd`, `zlib-ng::zlibstatic`) are there for the parent project too. |

Built as the top-level project, the tests run with CTest:

```text
cmake -S . -B build
cmake --build build
ctest --test-dir build -C Debug
```

nxtest (`tests/framework/nxtest.h`, target `nx::test`) is the test framework:
`TEST_CASE`, `SUBCASE`, `CHECK`, `REQUIRE` and the `CHECK_THROWS` forms;
`nxtest_discover_tests(target)` registers each case with CTest.

## Layout

| Folder (under `include/core/foundation/`) | Scope |
| --- | --- |
| `core/` | Detection macros (`config.h`, cheap to include), allocators and primitives: `foundation.h`, `handle.h`, `hash.h`, `result.h`, `scope_guard.h`, `callable.h`, `enum.h`, `random.h`, `sha256.h`, `time.h`, `fixed.h`, `fixed_step.h`, `noise.h`, `type_index.h`. |
| `containers/` | Container types. |
| `strings/` | UTF-8 string and view, formatting, base64, interned ids. |
| `fibers/` | Context switching, fiber stacks, the fiber scheduler and its primitives. |
| `threading/` | Threads, synchronization, the thread pool, workload telemetry. |
| `platform/` | Filesystem, virtual memory, OS queries, child processes, sockets, shared libraries, window handles, Windows long paths, Android JNI. |
| `vfs/` | Virtual filesystem, devices, async I/O, staged asset pipeline, pack manifests. |
| `diagnostics/` | Logging, crash reports, assertions, profiler instrumentation. |
| `serialization/` | JSON, asset documents, NVA, resource bundles, asset contracts, INI, Zstandard, command lines, Content-Length framing. |
| `reflection/` | Type descriptions, the type registry, layout digests. |
| `ecs/` | The entity-component-system registry. |
| `simd/` | C++26 `std::simd` for C++20, and kernels. |
| `math/` | Slang-named vectors, matrices and quaternions. |

## Core

`core/foundation.h` holds detection macros, fixed-width aliases (`i32`,
`u64`, `usize`, `f32`, ...), `min`, `max`, `clamp`, `align_up`,
`next_pow2`, `nx::cast` and `nx::safe_cast` (a `dynamic_cast` check in
`NX_DEBUG`), `str_copy`, the `mem_alloc` / `allocate` family, the
mimalloc-backed `nx::Allocator` and `nx::vector` (`std::vector` over it).

- `nx::scoped_no_alloc` makes `mem_alloc`, `realloc` and `free` assert on
  its thread (`NX_ALLOC_GUARD`, on in `NX_DEBUG`), for a thread with a
  deadline such as an audio callback. A fiber may not suspend inside one, so
  its thread-local depth cannot follow it to another thread.
- `nx::set_heap_observer(fn)` is told every allocation's size on the
  allocating thread; `fn` must not allocate. Absent in Shipping.
- `core/handle.h`: the versioned `basic_handle` (`handle32`, `handle64`,
  `typed_handle32`) the handle-based containers key on.
- `core/hash.h`: FNV-1a (`hash_fnv1a32`/`64`, incremental `fnv1a32`/`64`),
  the mixers `hash_u32`/`hash_u64`, `hash_ptr` and `hash_comb`.
- `type_index<T>()` (`core/type_index.h`) is a process-local `u32` assigned
  on first use, different between runs: never a persistent identifier.

## Containers

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::vector` | `core/foundation.h` | The general dynamic sequence. |
| `nx::blob` | `containers/blob.h` | An owning array sized at construction, for buffers of known length. |
| `nx::owned_blob_slice` | `containers/owned_blob_slice.h` | A read-only slice that owns its `blob`, so a decoder sees one payload of a chunked container without a copy. |
| `nx::fixed_vector` | `containers/fixed_vector.h` | Compile-time capacity, no heap. Like C++26 `inplace_vector`, growing past `N` throws `std::bad_alloc` in every build; `try_push_back`/`try_emplace_back` return null, `full()` says when. |
| `nx::small_vector` | `containers/small_vector.h` | Its first `N` elements inline, the rest on the heap. |
| `nx::flat_map` | `containers/flat_map.h` | Ordered, over sorted arrays: binary-search lookups, linear inserts; for maps built once and read. Like `std::flat_map`, no `get_allocator`. |
| `nx::hash_map` / `nx::hash_set` | `containers/hash_map.h`, `hash_set.h` | One SwissTable-style table with SIMD probing (`raw_hash_table.h`): the general map, and the set (const iterators). `nx::string_set` looks up by `string_view`. |
| `nx::map` / `nx::set` | `containers/map.h` | Ordered, on a B-tree of 512-byte nodes, with bounds: for keys with no hash, key-order iteration, or entries that come and go. Insert and erase invalidate every iterator and reference, and keys and values must move without throwing. |
| `nx::slot_map` | `containers/slot_map.h` | A handle-keyed pool kept dense. Handles survive moves; addresses do not. |
| `nx::stable_pool` | `containers/stable_pool.h` | A handle-keyed pool whose addresses stay valid until erased. |
| `nx::sparse_set` | `containers/sparse_set.h` | Handles to dense payloads, O(1) insert/erase/contains: the ECS component store; `T = void` is a plain set. |
| `nx::mpmc::queue` / `nx::mpsc::queue` | `containers/concurrent_queue.h` | Bounded lock-free queues; the MPSC one has `consume_all`/`consume_batch`. |
| `nx::spsc::ring` | `containers/ring_buffer.h` | A bounded lock-free single-producer ring of trivially copyable data, in bulk. |
| `nx::sort_by_key` / `nx::sort_by_key_into` | `containers/radix_sort.h` | A stable 32-bit-key radix sort that gathers each element once. `coherent_sort_by_key_into` starts from the previous call's order, for data re-sorted every frame, with the same result. `radix_context` / `radix_sort_pairs`, with caller-owned scratch, run serially or across a `thread_pool`. |
| `nx::compact_context` | `containers/stream_compact.h` | Keeps what a predicate accepts, in order, serially or across a `thread_pool`; `transform_compact` produces and filters in one pass. |
| `nx::bit_array` | `containers/bit_array.h` | `N` flags with `count`, `all`/`any`/`none`, `find_first`/`find_next`. Not thread-safe. |
| `nx::string_map` | `containers/string_map.h` | A hash map keyed by `nx::string`, looked up by `string_view`. |
| `nx::sync_slot_map` | `containers/sync_slot_map.h` | A `slot_map` two threads share. A `try_get` pointer survives other entries changing; do not erase one in use. |

The maps share a member set. Under a transparent hash and equality, or
comparison, `find`, `contains`, `erase`, `try_emplace`, `insert_or_assign`,
`[]` and `at` take any key a `Key` is made from, building the `Key` only to
insert. Each has hinted `insert`, `try_emplace` and `insert_or_assign`, a
free `swap` and `nx::erase_if`; the ordered ones compare with `<=>`.
`small_vector` and `fixed_vector` compare across capacities.

## Strings

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::string` | `strings/utf8_string.h` | Owning UTF-8 with small-string optimization; transcodes from every character type; the `std::string` editing members, `resize_and_overwrite` included. `+` with a temporary reuses its buffer; `std::hash<nx::string>` is transparent; a lone character converts only explicitly. |
| `nx::string_view` | `strings/utf8_string_view.h` | `std::string_view`. |
| `nx::id_string` | `strings/id_string.h` | An 8-byte interned string keyed by a 64-bit FNV-1a hash: integer compare and hash, computable at compile time, bytes recoverable from a process-wide pool. Asset keys and `switch` labels (Unreal `FName` / bitsquid `idstring` in spirit). |
| `nx::base64_encode` | `strings/base64.h` | RFC 4648 with padding; `base64url_encode` is URL-safe without padding (PKCE). |
| `nx::format` | `strings/utf8_string.h` | `std::format` returning `nx::string`. |
| `nx::format_to` / `nx::vformat_to` | `strings/format.h` | `format_to(char[N], ...)` never allocates and checks its format at compile time. `vformat_to` takes a run-time format; logs and asserts use it, since checking would reject a logged run-time string. |

## Threading

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::thread` | `threading/thread.h` | An OS thread with name, stack size, affinity and priority; joins on destruction. `nx::this_thread`: id, kernel `os_id`, yield, sleep, name, affinity, priority. |
| `nx::thread_parker` | `threading/thread.h` | Parks one thread until `unpark()` or a deadline kept well under a millisecond (a Windows futex timeout rounds to the ~15.6 ms tick). An early `unpark()` is not lost. |
| `nx::cpu` | `threading/thread.h` | Core counts, SMT, hybrid P/E classes, caches, NUMA nodes. |
| `nx::serial_queue` | `threading/serial_queue.h` | Tasks that must not overlap or reorder run on a `thread_pool` one at a time, in order, with no thread of its own; one that throws is logged. Bounded (`post` refuses past capacity unless told otherwise); `blocking` runs tasks through `run_blocking`; `flush` suspends a fiber, not its worker. |
| `nx::completion_queue` | `threading/completion_queue.h` | Results published on their owner's thread in request order: the owner `reserve`s a place, any thread completes or abandons it, `publish(max_count, fn, max_time)` hands over the finished front. `reserve` refuses at `capacity`. |
| `nx::cancellation_source` / `_token` | `threading/cancellation.h` | Cooperative cancellation; tokens keep their generation across a source reset. `cancellation_callback` wakes a wait on cancellation; its destructor waits out a running call. |
| `nx::workload_tracker` | `threading/workload.h` | Lock-free outcome counts, queue and execution times and peaks for a named class of work (`measure_workload`). Async I/O and the asset pipeline report through it. |
| `nx::mutex` et al. | `threading/sync.h` | Futex-based `spin_lock`, `mutex`, `recursive_mutex`, `cond_var`, `rw_lock` (writer-preferring), `semaphore`, `event`, `barrier`, `call_once`, `scoped_lock`/`scoped_shared_lock`; 4-8 bytes, kernel waits only under contention. Waits other than `spin_lock`'s and `barrier`'s have `_for`/`_until` forms; a timeout of zero or less tries once. A timed `rw_lock` writer waits as a reader would: holding readers back and then giving up would leave them parked. |
| `nx::unique_lock` / `nx::shared_lock` | `threading/sync.h` | Movable ownership (`defer_lock`, `try_to_lock`, `adopt_lock`, timeouts) of any lockable. Both `cond_var`s wait under any lock held. |
| `nx::futex` | `threading/sync.h` | The 32-bit wait/wake under `sync.h`: WaitOnAddress, `SYS_futex`, `__ulock` on Apple. |

`nx::thread_pool` (`threading/thread_pool.h`) runs tasks on an
`nx::fiber::scheduler` of its own.

- Priorities are `task_priority {Background, Normal, Critical}`; every 8th
  and 32nd pick looks at a lower lane first, so no lane starves. Background
  concurrency is bounded (`max_background_concurrency`,
  `set_background_limit`).
- `task_group`, `parallel_for`, `parallel_region` (a team meeting at a
  `fiber::barrier`, so it may start inside a task; both rethrow the first
  exception once the rest finish), cancellable futures with continuations,
  `when_all`/`when_any`, opt-in statistics. `completion_source<T>` completes
  a `future<T>`.
- A waiting task suspends its fiber, freeing the worker. `run_blocking`
  reaches `config::blocking_threads` (default 2) service threads.
- With `config::adopt_main_thread` the constructing thread is the main
  thread: `run_on_main(fn)` runs there in `pump_main(budget)` or while it
  waits. `worker_os_ids` names the workers for OS services.

## Fibers

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::fiber::context` | `fibers/context.h` | `switch_to(from, to, data)` and `exit_to`. Saves callee-saved registers and FP control state; on Windows swaps the TIB stack bounds, so SEH, `__chkstk` and guard pages work. Unwind info for debuggers, ASan/TSan annotations. |
| `nx::fiber::stack_pool` / `stack` | `fibers/stack.h` | Guard-paged stacks committed only as deep as used, recycled most-recent-first; `trim()` returns grown pages. |
| `nx::fiber::scheduler` | `fibers/scheduler.h` | Workers run jobs (`{entry, arg}`, `fiber::priority {High, Normal, Low}`, per-worker queues with stealing) on pooled fibers; a full queue makes the submitter run the job. The constructing thread is the main thread, running only `run_on_main` work, in `pump_main` or while it waits, on its own stack. `run_blocking(fn)` moves the fiber to a blocking-service thread and back. `fiber_local(slot)` follows the fiber across threads (`User`; `Pool` for `thread_pool`). Destruction finishes outstanding jobs. |
| `nx::fiber::counter` | `fibers/scheduler.h` | Outstanding work that fibers `wait` on; any thread may decrement it. |
| `nx::fiber::mutex` et al. | `fibers/sync.h` | `mutex`, `rw_lock` (writer-preferring), `event`, `semaphore`, `cond_var`, `barrier` (`arrive_and_drop`), `sleep_for`: they suspend the fiber, with `_for`/`_until` on the scheduler's timers, and block a thread no scheduler owns. They may be held across a suspension. |

A fiber can resume on another OS thread. Code that waits must not keep a
`thread_local` address across the call, or hold an OS lock (`nx::mutex`,
`rw_lock`, `spin_lock`) across it. `may_suspend()` is false under one, in
`scoped_no_alloc` or in `fiber::no_suspend_scope` (other thread-affine
resources), and debug builds assert a suspension then. cl.exe's `/GT` stops
the optimizer reusing a thread-local address across a call; Clang has no such
switch, so the scheduler reads its thread-locals through non-inlined calls.

The fiber primitives wake a waiter after unlocking. `rw_lock`, auto-reset
`event` and `semaphore` hand over what they release; `mutex` does not, so a
running fiber may take it first. A writer timing out on `rw_lock` admits the
readers queued behind it.

Exceptions unwind within a fiber's stack, and `thread_pool` carries a task's
to its waiter; a raw `fiber::context` entry does not. Never wait inside a
`catch` block: the exception being handled is per thread (a
`no_suspend_scope` there makes debug builds assert it). An unhandled fault
on a fiber reaches the process filter, so `nx::crash` reports it. A fiber
suspended on a wait shows in no debugger thread list; `for_each_waiting`
lists them and what each waits on, and on Windows x64
`context::read_suspended` gives registers to walk one's stack; a hang report
does both.

## Platform

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::fs::path_view` | `platform/filesystem.h` | Lexical UTF-8 path queries, each a `path_view`, compared byte-wise; `join` and `/` return `nx::string`, as a view cannot own joined bytes. |
| `nx::fs` files | `platform/filesystem.h` | Whole-file read/write, directories, `executable_dir()`, `application_data_dir(organization, application)`, seekable `stream`s (`open_file`, `open_memory`; `open_file_views` gives cursors readable from any thread while the file is replaced; `status()` tells a failed read from the end). Failures are `io_result`s with an `io_error` (`NotFound`, `NoSpace`, `Busy`, `ReadOnly`, `NotSupported`, ...) and the native code. |
| `nx::fs::staged_file` | `platform/filesystem.h` | Atomic replacement by rename on `commit()` (`file_write_atomic` in one call); destroyed first, it removes its temporary. `durability::cached` (default): readers see old or new, the OS writes later. `durability::flushed` puts the file, and on POSIX the directory entry, on storage (`F_FULLFSYNC` on Apple), for data a power cut must not lose. `remove_stale_staged_files` clears a crashed writer's temporaries. |
| `nx::fs::file_lock` | `platform/filesystem.h` | `try_acquire(path)`: an exclusive lock between processes and within one (`LockFileEx`, `flock`), or `io_error::Busy`. Advisory. The file is never removed: removing one another process is about to lock lets two hold it. |
| Long Windows paths | `platform/windows_path.h` | From 248 characters (where `CreateDirectoryW` stops taking the plain form) a path goes to Win32 as `\\?\C:\...` or `\\?\UNC\...`, up to 32767. `nx::fs` and the IOCP reader use it, so deep trees with staging suffixes work. |
| `nx::vmem` | `platform/os.h` | Reserve/commit/protect and page-size queries. |
| `nx::os` | `platform/os.h` | Resident and peak memory (`mem_get_stats`), `ticks_ns`/`ticks_ms`, seconds since 1970 on the user's clock (`local_time`), `system_ram_mb`, `region` (two ISO 3166 letters: the Windows geo setting, Android locale property, Apple locale, `LC_ALL`/`LC_MESSAGES`/`LANG`; `region_of_locale`), and `random_bytes` from the OS's secure generator (BCrypt, `arc4random_buf`, `/dev/urandom`). |
| `nx::os::shared_library` | `platform/shared_library.h` | `open(path)` (failing with the loader's text), `symbol`, `close`; `decorate(stem)` makes the platform file name, `supported()`. |
| `nx::process` | `platform/process.h` | Runs a child (Windows, Linux, macOS) with no input, stdout and stderr captured together up to a limit (the rest is dropped), a chosen environment, and a timeout that kills it and its children (job object; process group). It blocks: call it from `run_blocking`. `quote_argument` for Windows command lines. |
| `nx::net` | `platform/socket.h` | Transport only: framing, limits and authentication are the caller's. `Endpoint` is plain V4/V6 data (`parse` takes numeric text, `[::1]:80`). `resolve(host, port)` blocks, returning up to four in a `small_vector` or a `ResolveError`. `Connection` reads and writes without blocking (0 nothing waiting, -1 peer gone); `connect(Endpoint)` blocks for the connect; `connect(port)` and `Listener::listen(port)` use loopback. `UdpSocket::send_to` is all-or-nothing; `recv_from` truncates an oversized datagram on every platform. |
| `nx::NativeWindowHandle` | `platform/native_window.h` | `{kind, window, display}`, `NativeWindowKind` `Win32`, `Xlib`, `Wayland`, `Cocoa`, `Android`, `UIKit`: the window a graphics backend presents to. |
| `nx::android::JniScope` | `platform/android_jni.h` | Android only. Borrows the thread's `JNIEnv`, attaching if needed and detaching only if it did; clears a pending Java exception. Asserts it is not on a fiber, as Java fails on fiber stacks; `run_java(pool, vm, fn)` hops to the pool's main thread from one. Also null-safe class and method lookup, string and array conversions. |

## VFS

`vfs/vfs.h` (`nx::vfs`) mounts devices under canonical absolute virtual
paths.

- `initialize()`/`shutdown()` bracket it. `mount(prefix, device, priority)`
  returns a `MountId` and owns the device even if it fails. The higher
  priority wins, then the longer prefix, then the later mount. `mounts()`
  lists `MountInfo` without host paths.
- `read`, `read_text`, `open`, `stat`, `exists`, `list` take the best mount
  with the file; `write`, `write_text`, `remove`, `create_directories` the
  best writable one, else `ReadOnly`.
- Device I/O runs outside the mount lock, so a slow (remote) device holds up
  nothing else; `unmount` waits for its device's calls.
- A `Device` implements `read`, `open`, `stat`, `list`, and may add the
  writes, `writable()`, `watchable()` (`mtime_ns` may be polled) and
  `native_read_path()` (lets async I/O use the kernel). Built in:
  `make_host_device(root, HostDeviceAccess)` (writes atomic and flushed),
  `MemoryDevice`, and `make_zip_device` over a path, a stream factory or an
  archive in RAM.
- `file_generation(path)` / `files_generation(paths)`: one hot-reload
  generation from path and metadata for every subsystem.
- `resolve_beside(file, reference)`: a referenced path made absolute; a
  relative one stays in the file's folder (no empty, `.` or `..` parts).
- `in_user_data(path)`: whether a canonical path lies in `USER_DATA_ROOT`
  (`/data`) or `LEGACY_DATA_ROOT` (`/legacy-data`), the user's folders on
  every platform. Their files are the user's to change, so code loaders
  refuse them; a path that cannot be made canonical counts as inside.

### ZIP archives

`ZipWriter` creates or appends archives (`Store`, `Deflate`, `Zstd`; one
writer). `read_zip_directory` lists the central directory. A ZIP device
takes optional owner policies; foundation only hashes and holds no key.

- `ZipVerifier` decides once, from the whole directory and the entry it
  names, whether the archive mounts, then gives the SHA-256 of each block of
  an entry. No byte that does not match is handed out; a stream checks one
  block at a time, so a seek reads one block, and a mismatch fails the read
  as a bad CRC does.
- `ZipCipher` names the entries whose stored (compressed) bytes are
  enciphered and applies their keystream below minizip, before
  decompression, CRC or verifier. A keystream is its own inverse and any
  range deciphers alone, so reads stay seekable. `encipher_zip` applies it
  once for the writing tool. Overlapping or misplaced entries refuse the
  mount.

### Pack manifests

`parse_pack_manifest` (`vfs/pack_manifest.h`) validates a version-1
manifest of at most 256 packs: ASCII basenames in the manifest's folder,
unique priorities among enabled packs, the lowest `i32` reserved for the
bootstrap source. `mount_pack_manifest(path, opener)` opens every enabled
pack through the caller's opener and mounts the set at `/`, or on any
failure leaves none mounted (`PackManifestErrorCode`).

### Asynchronous I/O

`AsyncIoService(thread_pool&, AsyncIoConfig)` (`vfs/async_io.h`) runs whole
and range reads, writes, removals, listings and custom operations
(`async(request, fn(AsyncIoContext&))`) as futures with a `task_priority`,
cancellation and a byte budget.

- Host files use the kernel backend (`AsyncIoBackend`: IOCP, io_uring,
  dispatch I/O on Apple); archive, memory and custom devices run on the
  pool's blocking-service threads. Android builds only the blocking
  fallback: its seccomp policies may kill a process that probes io_uring.
- `max_concurrent_requests` (default 1) stays below the blocking-service
  thread count, leaving one free for other blocking calls.
- `byte_limit` (64 MiB, 0 for none) bounds payload in flight; an oversized
  request runs alone. `take_reservation` keeps a read's bytes charged until
  its consumer is done.
- One path's writes and removals run in order. Cancellation stops one not
  yet started; shutdown finishes those asked before it.
- `statistics()`: backend, bytes in flight, native against fallback reads.

### Staged asset pipeline

`StagedAssetPipeline<Acquired, Prepared>` (`vfs/asset_pipeline.h`) acquires
on async I/O, prepares on the CPU pool, and publishes only when the owner
calls `publish(handle)` on its thread; `pump()` advances it. It owns
in-flight generations until they drain, so cancellation never leaves a task
on destroyed state, and a failed publisher leaves the previous resource
alone.

- Requests are (key, generation): the same generation coalesces, an older
  one is stale, a newer one supersedes. The last `release` cancels.
- Dependencies are `AssetFence`s, also from an owner-resolved
  `AssetFenceSource`, so pipelines form one load graph; an optional one
  delays without failing.
- `set_limits` (concurrent stages), `set_memory_limits` (bytes between
  stages; an oversized asset enters an empty stage), `set_priority_aging`.

## Serialization

### JSON

`serialization/json.h`: a `value` tree that keeps integer and real literals
apart, so integers round-trip exactly. `parse(text, parse_options)` fails
with a code, byte offset and 1-based line and column (computed only then).
`parse_options`: `max_depth` 256; `allow_trailing_commas`, `allow_comments`,
`reject_duplicate_keys`, `validate_utf8` off. `write_options`: `indent`,
`escape_non_ascii`, `escape_forward_slash`, `real_precision` (0 shortest).
`serializer<T>` customizes; integers that do not fit exactly are rejected.
`json_binary.h` stores a tree in NVA (depth 256), `json_util.h` has `*_or`
getters and enum spellings, `number_parse.h` parses floats without the
locale.

### Asset documents

`serialization/json_document.h`:

- `parse_asset_document` accepts `//` and `/* */` comments and trailing
  commas, then removes every member named `_*` or `$comment`, recursively:
  editor notes that never reach a cooked payload. Each format rejects its
  other unknown fields itself.
- `normalize_asset_document` rewrites one as strict JSON;
  `encode_document`/`decode_document` hold a tree in NVA, the cooked form, at
  `document_cooked_path(source)` (the source name plus `.nxb`).
- `json::load_document(io, source)` and `ini::load(io, source)`
  (`async_documents.h`) read the cooked file as an I/O job; the authored one
  only when the cooked one is absent and not in Shipping
  (`asset_policy.h`). A malformed cook always fails. `load_document_file`
  and `ini::load(source)` are deprecated blocking barriers over the same
  path.

### NVA containers

`serialization/nva.h`: a 16-byte header (`"NVA1"` as a little-endian u32,
u32 format fourcc, u16 version, u16 flags, u32 chunk count), 12-byte entries
(u32 id, offset, size), payloads 16-byte aligned. `ChunkWriter` builds one;
`View::open(bytes, format)` checks magic, format and that the table and
every chunk fit, else `nullopt`; the consumer checks the version. `Reader`
returns zeros and clears `ok()` past the end, and proves a count has payload
before it drives an allocation.

### Resource bundles

`resource_bundle::encode` (`serialization/resource_bundle.h`) makes a
deterministic NVA of named entries, sorted and gapless, refusing duplicates
and empty payloads. `Limits`: 4096 entries, 4096-byte names, 1 MiB metadata,
512 MiB data. `valid_name` takes portable relative names only (no root,
drive, device name, control byte or dot segment), so an entry cannot escape
its descriptor's folder. Decoding never allocates from an unvalidated count.

### Asset contracts

`asset_contract::Registry` (`serialization/asset_contract.h`): families of
`Format`s (source suffix, published `.nxb` suffix, NVA fourcc),
directory-wide formats and authoring-only suffixes such as `.meta`. A failed
`add` changes nothing; after `freeze()`, `classify_source` and
`find_published` match the longest suffix. Which formats exist is the
caller's.

### Command-line parsing

`nx::cli::Parser` (`serialization/arguments.h`): root arguments,
subcommands, long and short options, strict conversion, repeatable values,
choices, `--`, generated help and diagnostics. Bound values must outlive it.

```cpp
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

### INI files

`serialization/ini.h` parses into an `nx::ini::Document` and writes one
back deterministically, without comments. `nx::ini::assign(text, section,
key, value)` sets one key in a file a person edits and keeps every other
line, comments included: the key is rewritten in place, a missing key goes
after its section's last assignment, a missing section at the end, in the
text's line endings, after any BOM. Text that does not parse is refused. A
key cannot start with `;` or `#`.

### Compression

`serialization/zstd.h` (`nx::zstd`): `compress`/`decompress` whole buffers;
`compressor`/`decompressor` keep a context between calls and take frames in
pieces (`begin`, `feed`, `finish`). Frames carry a checksum, so damage reads
as `Corrupt`. Decompression takes a limit: a frame recording a larger size
is refused before allocating, one that does not once it passes. Concatenated
frames read back joined.

### Content-Length framing

`content_length_frame(buffered, max_body, max_header = 8 KiB)`
(`serialization/content_length.h`) finds one LSP-style message in the bytes
so far: `Incomplete`, `Ready` (body offset, size, consumed) or `Invalid`.

## Reflection

`reflection/` (`nx::reflect`) describes types without macros.

- `type_of<T>()` gives a `type_info`: name, size, alignment, `type_kind`
  (`opaque`, `boolean`, `integer`, `unsigned_integer`, `floating`, `string`,
  `id`, `floats`, `enumeration`, `aggregate`, `sequence`) and an aggregate's
  `fields` (name, offset, type, attributes). Plain aggregates of up to 64
  fields are discovered by the compiler. An opaque type is left to its owner.
- `NX_ATTRIBUTES(Type, (field, {...}), ...)` sets `field_attributes`:
  `flags` (`transient`, `read_only`, `hidden`), `key` (the document name, so
  renaming the field keeps the key), `min`, `max`, and `label` under
  `NX_REFLECT_EDITOR` (default 1, one value per build). An unknown field
  fails to compile. `NX_REFLECT_ENUM(Enum, (enumerator, "text"), ...)`
  spells an enum.
- `type_registry::instance()` finds types by id or name.
- `layout_digest` folds type names, sizes, field names, offsets and nested
  types into a native-layout `u64`; `describe_layout` prints it.
- `reflect_json.h`: `write_fields`/`read_fields` (`elide_defaults` or
  `every_field`), `write_value`/`read_value` (omitted input stays), and
  `schema_of(type)`, a form description for tools.
- `reflect_binary.h`: `write_binary`/`read_binary` and
  `binary_schema_digest`, independent of native offsets and container sizes,
  so a host cooker and a target runtime agree.

## Diagnostics

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::log` | `diagnostics/log.h` | `nx::logv/logt/logd/logi/logw/loge/logc`, interned categories (`log::category("name")`; `app`, `gpu`, `render`, `rhi`, `system`), per-category and global levels, console, debugger, rotating-file and Android sinks. Shipping compiles `logv`, `logt` and `logd` to nothing (arguments still evaluated). Records format on the caller; after `log::attach_drain_pool(pool)` a low-priority job writes them, otherwise or when the queue is full the caller does. `log::flush()` writes the queue on the caller; a fatal report flushes first. Outside `log::init()`/`shutdown()` records go to stderr and to sinks added then. |
| `nx::crash` | `diagnostics/crash.h` | Fatal reports with bounded notes and log tail, minidumps where supported, explicit reports, and bounded binary attachments written atomically as sanitized sidecars beside the report and pruned with it. `start_hang_watch` ends a process whose `beat()` stops, reporting every thread and waiting fiber it can walk; `add_hang_warning` hears at half the timeout. |
| `NX_ASSERT` | `core/foundation.h` | The contract check: logs, flushes, breaks and aborts in `NX_DEBUG`, compiled out (still type-checked) otherwise. An expression, so a constant condition draws no warning. A formatted message needs `diagnostics/diagnostic.h`, which also has the `nx::dbg_*` helpers without `<windows.h>`. |
| `NX_PROFILE_*` | `diagnostics/profiler.h` | Zone, frame, thread-name and plot macros (`NX_PROFILE_ZONE_TEXT` names at run time). With `NX_ENABLE_PROFILER` they go to Tracy, in Shipping nowhere, otherwise to the `nx::profile::hooks` set with `set_hooks`, if any. Foundation keeps no trace of its own. |

## ECS registry

`ecs/registry.h`: `nx::ecs::registry` (`registry64`, and the
`basic_registry<Entity, Alloc>` template) is an allocator-aware ECS over
versioned handles on `nx::sparse_set`, with O(1) add/get/remove, views and
cached queries, a parent-child hierarchy and statistics. Components are
registered first; an unregistered one throws `std::logic_error`. Views walk
in storage order, which callers may rely on. An `each` callback may remove or
destroy its entity. `each_in(first, last, fn)` walks positions of the same
storage (`size_hint()` of them), so disjoint ranges may run on several
threads; its callback may not remove or destroy. The header has the
invalidation, destroy and thread-safety rules.

```cpp
struct Transform { float x, y, z; };
struct Velocity  { float dx, dy; };

nx::ecs::registry world;
world.register_component<Transform>({ .name = "Transform" });
world.register_component<Velocity>({ .name = "Velocity" });

auto e = world.create();
world.emplace<Transform>(e, 0.f, 0.f, 0.f);
world.emplace<Velocity>(e, 1.f, 0.f);

world.view<Transform, Velocity>().each(
    [](auto entity, Transform& t, Velocity& v) { t.x += v.dx; });

auto child = world.create();
world.set_parent(child, e);
world.destroy(e); // detaches children by default; see destroy_policy
```

## Utilities and components

| Type | Header | Use it for |
| --- | --- | --- |
| `nx::unique_ptr` | `core/foundation.h` | Sole ownership on the nx heap, with no raw-pointer constructor: `make_unique` (honours `alignof(T)`, reaches a befriending private constructor), `adopt_unique` for a pointer from `release()` or `nx::allocate`. Converts to a base with a virtual destructor. A custom deleter takes raw pointers. |
| `nx::shared_ptr` | `core/foundation.h` | Shared ownership, one pointer wide, count in front of the object. No weak half; `nx::life_token` / `life_watch` say whether it lives. |
| `nx::function` / `nx::unique_function` | `core/callable.h` | Heap-backed and move-only callables (`std::function`, `std::move_only_function`); `unique_function` keeps small targets inline and is the shape for queued work. |
| `nx::inplace_function` | `core/callable.h` | A fixed inline buffer (16 bytes by default); never allocates. |
| `nx::function_ref` | `core/callable.h` | A non-owning callable parameter. A lambda made in the call is gone after it, so an object that keeps one takes an lvalue. |
| `nx::enums::map` / `flags` | `core/enum.h` | An array indexed by enum (sized by `Count`); a type-safe bitset over a scoped enum. |
| `nx::result` / `nx::err` | `core/result.h` | `[[nodiscard]]` error-or-value shaped like `std::expected`: `==` against results, values and `err`s, `and_then`/`transform`/`or_else`/`transform_error`, `error_or`, `swap`, and `emplace` with nothrow construction only. |
| `nx::scope_guard` / `nx::scoped_value` | `core/scope_guard.h` | An action on scope exit unless dismissed (`scoped_exit()`, `NX_DEFER { ... };`); a value replaced and restored, nesting correctly. |
| `nx::guid` | `core/random.h` | A 128-bit UUID: parse, format, compare, hash; `nx::rng::guid()` makes one. |
| `nx::rng` | `core/random.h` | Pcg, Xoshiro256 or Splitmix64, the same stream for a seed on every platform. A default `rng` is Pcg seeded with 0 and repeats every run. Secrets come from `nx::os::random_bytes`. |
| `nx::sha256` | `core/sha256.h` | SHA-256 for identities and checks such as PKCE, not passwords; SHA-NI or ARMv8 SHA2 where the CPU has them (`cpuid`, `getauxval`, always on Apple, `IsProcessorFeaturePresent`). |
| `nx::duration` / `nx::timestamp` / `nx::stopwatch` | `core/time.h` | Nanosecond spans, monotonic points, a stopwatch, `scoped_micros`. |
| `nx::fixed` | `core/fixed.h` | Q16.16 whose overflow wraps as defined behaviour, so rollback peers compute the same bits. |
| `nx::noise` | `core/noise.h` | Value, simplex and cellular noise in 2D and 3D in integers only, so a seed gives the same world on every CPU; Q16.16 results, `fbm`. No simplex gradient lies square to an edge, so no field is zero along a line. |
| `nx::fixed_step` | `core/fixed_step.h` | A fixed-rate clock: steps per frame, capped by measured step cost, and an interpolation `alpha()`. |

The owning callables convert from any callable with their signature (void
discards a result); a null function pointer or `nullptr` makes them empty
(`valid()`, `== nullptr`, `reset()`). Types are snake_case; the CamelCase
spellings that remain (`Guid`, `Rng`, `mpmc::Queue`, the `fiber::` types,
`defer`, ...) are deprecated aliases.

## SIMD

`simd/simd.h` is C++26's `std::simd` for C++20 in `nx::simd`, with the
standard's names and semantics, over vector extensions, NEON, SVE, SSE and
AVX2, plus interleaved loads, fused element-wise pipelines, linear algebra
with a deterministic `gemm`, and neural-network kernels. `vec<T>`'s width
follows the compile flags: keep it out of interfaces between translation
units built differently. Reference:
[`simd/README.md`](include/core/foundation/simd/README.md).

## Math

`math/math.h` is vector math under Slang's names in `nx::math` (`float3`,
`float4x4`, `quat`, `mul`, ...): glm's storage, Slang's meaning, code that
reads differently in the two refusing to compile, and the same results on
every compiler and backend. Reference and differences from glm:
[`math/README.md`](include/core/foundation/math/README.md).

## Known limitations

- Apple's futex is the private `__ulock_wait`/`__ulock_wake`, as libc++'s
  own; private API is an App Store review risk.
- A new fiber starts from the default FP control state (MXCSR `0x1F80`,
  zero FPCR), not its creator's.
- JNI strings are modified UTF-8 converted byte for byte: embedded nulls and
  supplementary characters do not round-trip exactly.
- The POSIX crash handler's alternate signal stack is set on the installing
  thread only, so a stack overflow elsewhere has no stack to report on.
