#include "core/foundation/vfs/vfs_internal.h"

#include "core/foundation/containers/slot_map.h"
#include "core/foundation/core/hash.h"
#include "core/foundation/containers/small_vector.h"
#include "core/foundation/diagnostics/log.h"
#include "core/foundation/diagnostics/profiler.h"

#include <algorithm>

namespace nx::vfs {

namespace {

const nx::log::Category log_vfs = nx::log::category("vfs");

bool match_mount(const nx::string_view prefix, const nx::string_view v,
                 nx::string &rel) {
  if (prefix == "/") {
    rel.assign(v.substr(1));
    return true;
  }
  if (v == prefix) {
    rel.clear();
    return true;
  }
  if (v.size() > prefix.size() && v.starts_with(prefix) &&
      v[prefix.size()] == '/') {
    rel.assign(v.substr(prefix.size() + 1));
    return true;
  }
  return false;
}

struct Mount {
  i32 priority = 0;
  u32 order = 0;
  nx::string prefix;
  Device *device = nullptr;
};

struct Candidate {
  Device *device;
  nx::string rel;
  i32 priority;
  u32 order;
  usize prefix_len;
};

using CandidateList = nx::small_vector<Candidate, 8>;

struct State {
  nx::rw_lock mounts_lock;
  nx::slot_map<Mount, MountId> mounts;
  u32 mount_order = 1;
  bool initialized = false;
};

State &state() noexcept {
  static auto *s = nx::allocate<State>();
  return *s;
}

bool candidate_better(const Candidate &a, const Candidate &b) noexcept {
  if (a.priority != b.priority)
    return a.priority > b.priority;
  if (a.prefix_len != b.prefix_len)
    return a.prefix_len > b.prefix_len;
  return a.order > b.order;
}

// Best first. Caller holds at least the shared mount lock; the Device pointers
// stay valid only while it is held.
void gather(const State &s, nx::string_view v, CandidateList &out) {
  nx::string rel;
  for (const Mount &m : s.mounts) {
    if (match_mount(m.prefix.view(), v, rel))
      out.push_back(Candidate{.device = m.device,
                              .rel = rel,
                              .priority = m.priority,
                              .order = m.order,
                              .prefix_len = m.prefix.size()});
  }
  std::sort(out.begin(), out.end(), candidate_better);
}

template <typename Op>
io_result<void> write_route(const path_view vpath, Op &&op) {
  const nx::string v = detail::canon_vpath(vpath);
  if (v.empty())
    return err(file_error{io_error::InvalidArgument, 0});

  State &s = state();
  nx::scoped_shared_lock lock(s.mounts_lock);
  CandidateList cands;
  gather(s, v.view(), cands);
  for (const Candidate &c : cands) {
    if (c.device->writable())
      return op(c.device, c.rel);
  }
  return err(file_error{io_error::PermissionDenied, 0});
}

}

// Lifetime

bool initialize() {
  state().initialized = true;
  return true;
}

bool initialized() noexcept { return state().initialized; }

nx::vector<MountInfo> mounts() {
  State &s = state();
  nx::scoped_shared_lock lock(s.mounts_lock);
  nx::vector<MountInfo> out;
  out.reserve(s.mounts.size());
  for (const Mount &mount : s.mounts) {
    out.push_back(MountInfo{.priority = mount.priority,
                            .prefix = mount.prefix,
                            .provider = nx::string(mount.device->name()),
                            .writable = mount.device->writable(),
                            .watchable = mount.device->watchable()});
  }
  std::sort(out.begin(), out.end(), [](const MountInfo &a, const MountInfo &b) {
    if (a.priority != b.priority)
      return a.priority > b.priority;
    return a.prefix < b.prefix;
  });
  return out;
}

void shutdown() {
  State &s = state();
  if (!s.initialized)
    return;
  unmount_all();
  s.initialized = false;
}

MountId mount(const path_view virtual_prefix, Device *device, i32 priority) {
  State &s = state();
  if (!s.initialized || !device) {
    if (device)
      nx::release(device); // ownership passed to us even on failure
    return {};
  }
  nx::string prefix = detail::canon_vpath(virtual_prefix);
  if (prefix.empty()) {
    nx::release(device);
    return {};
  }

  nx::scoped_lock lock(s.mounts_lock);
  const MountId id = s.mounts.insert(Mount{.priority = priority,
                                           .order = s.mount_order++,
                                           .prefix = std::move(prefix),
                                           .device = device});
  nx::logi(log_vfs, "vfs: mounted {} at '{}' (priority {})", device->name(),
           s.mounts[id].prefix.view(), priority);
  return id;
}

void unmount(const MountId id) {
  State &s = state();
  Device *victim = nullptr;
  {
    nx::scoped_lock lock(s.mounts_lock);
    if (const Mount *m = s.mounts.try_get(id)) {
      victim = m->device;
      s.mounts.erase(id);
    }
  }
  if (victim)
    nx::release(victim);
}

void unmount_all() {
  State &s = state();
  nx::small_vector<Device *, 8> victims;
  {
    nx::scoped_lock lock(s.mounts_lock);
    victims.reserve(s.mounts.size());
    for (const Mount &m : s.mounts)
      victims.push_back(m.device);
    s.mounts.clear();
  }
  for (Device *d : victims)
    nx::release(d);
}

io_result<nx::blob<u8>> read(path_view vpath) {
  NX_PROFILE_ZONE("vfs::read");
  const nx::string v = detail::canon_vpath(vpath);
  if (v.empty())
    return err(file_error{io_error::InvalidArgument, 0});

  State &s = state();
  nx::scoped_shared_lock lock(s.mounts_lock);
  CandidateList cands;
  gather(s, v.view(), cands);

  file_error last{io_error::NotFound, 0};
  for (const Candidate &c : cands) {
    auto r = c.device->read(c.rel);
    if (r)
      return r;
    if (r.error().kind != io_error::NotFound)
      return r;
    last = r.error();
  }
  return err(last);
}

io_result<nx::string> read_text(path_view vpath) {
  auto r = read(vpath);
  if (!r)
    return err(r.error());
  const nx::blob<u8> &b = r.value();
  if (b.empty())
    return nx::string();
  return nx::string(reinterpret_cast<const char *>(b.data()), b.size());
}

io_result<stream_ptr> open(path_view vpath) {
  NX_PROFILE_ZONE("vfs::open");
  const nx::string v = detail::canon_vpath(vpath);
  if (v.empty())
    return err(file_error{io_error::InvalidArgument, 0});

  State &s = state();
  nx::scoped_shared_lock lock(s.mounts_lock);
  CandidateList cands;
  gather(s, v.view(), cands);

  file_error last{io_error::NotFound, 0};
  for (const Candidate &c : cands) {
    auto r = c.device->open(c.rel);
    if (r)
      return r;
    if (r.error().kind != io_error::NotFound)
      return r;
    last = r.error();
  }
  return err(last);
}

FileInfo stat(const path_view vpath) {
  const nx::string v = detail::canon_vpath(vpath);
  if (v.empty())
    return {};

  State &s = state();
  nx::scoped_shared_lock lock(s.mounts_lock);
  CandidateList cands;
  gather(s, v.view(), cands);

  for (const Candidate &c : cands) {
    const FileInfo info = c.device->stat(c.rel);
    if (info.exists)
      return info;
  }
  return {};
}

bool exists(const path_view vpath) { return stat(vpath).exists; }

u64 file_generation(const path_view vpath, const FileInfo &info) noexcept {
  return nx::fnv1a64()
      .combine_all(nx::hash_fnv1a64(vpath.native()), info.mtime_ns, info.size,
                   info.exists ? 1u : 0u, info.is_directory ? 1u : 0u)
      .value();
}

u64 file_generation(const path_view vpath) noexcept {
  return file_generation(vpath, stat(vpath));
}

u64 files_generation(const std::span<const nx::string> vpaths) noexcept {
  nx::fnv1a64 hash;
  for (const nx::string &path : vpaths)
    hash.combine(file_generation(path.view()));
  return hash.value();
}

io_result<nx::vector<DirEntry>> list(path_view vdir) {
  NX_PROFILE_ZONE("vfs::list");
  const nx::string v = detail::canon_vpath(vdir);
  if (v.empty())
    return err(file_error{io_error::InvalidArgument, 0});

  State &s = state();
  nx::scoped_shared_lock lock(s.mounts_lock);
  CandidateList cands;
  gather(s, v.view(), cands);

  nx::vector<DirEntry> out;
  detail::ChildSet seen;

  bool any = false;
  file_error last{io_error::NotFound, 0};

  for (const Candidate &c : cands) {
    ListFn cb([&](path_view name, const FileInfo &info) {
      const nx::string_view nv = name.native();
      if (!seen.insert(nv))
        return;
      out.push_back(DirEntry{.name = nx::string(nv), .info = info});
    });
    auto r = c.device->list(c.rel, cb);
    if (r)
      any = true;
    else
      last = r.error();
  }
  if (!any)
    return err(last);
  return out;
}

io_result<void> write(const path_view vpath, const std::span<const u8> data) {
  NX_PROFILE_ZONE("vfs::write");
  return write_route(vpath, [&](Device *d, const nx::string &rel) {
    return d->write(rel, data);
  });
}

io_result<void> write_text(const path_view vpath, const nx::string_view text) {
  return write(vpath,
               std::span<const u8>(reinterpret_cast<const u8 *>(text.data()),
                                   text.size()));
}

io_result<void> remove(const path_view vpath) {
  return write_route(
      vpath, [](Device *d, const nx::string &rel) { return d->remove(rel); });
}

io_result<void> create_directories(const path_view vpath) {
  return write_route(vpath, [](Device *d, const nx::string &rel) {
    return d->create_directories(rel);
  });
}

}
