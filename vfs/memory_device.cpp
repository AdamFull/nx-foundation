#include "core/foundation/vfs/vfs_internal.h"

namespace nx::vfs {

using detail::BlobStream;
using detail::canon_rel;
using detail::ChildSet;

void MemoryDevice::add(const path_view rel, nx::blob<u8> bytes) {
  nx::string key = canon_rel(rel);
  if (key.empty())
    return;
  nx::scoped_lock lock(m_lock);
  m_files[std::move(key)] = std::move(bytes);
}

io_result<nx::blob<u8>> MemoryDevice::read(const path_view rel) {
  const nx::string key = canon_rel(rel);
  nx::scoped_shared_lock lock(m_lock);
  const auto it = m_files.find(key);
  if (it == m_files.end())
    return err(file_error{io_error::NotFound, 0});
  return it->second; // deep copy
}

io_result<stream_ptr> MemoryDevice::open(const path_view rel) {
  const nx::string key = canon_rel(rel);
  nx::scoped_shared_lock lock(m_lock);
  const auto it = m_files.find(key);
  if (it == m_files.end())
    return err(file_error{io_error::NotFound, 0});
  auto *s = nx::allocate<BlobStream>(it->second); // owns a copy
  if (!s)
    return err(file_error{io_error::OpenFailed, 0});
  return stream_ptr(s);
}

FileInfo MemoryDevice::stat(const path_view rel) {
  const nx::string key = canon_rel(rel);
  FileInfo info;
  if (key.empty()) { // the root
    info.exists = true;
    info.is_directory = true;
    return info;
  }
  nx::scoped_shared_lock lock(m_lock);
  if (const auto it = m_files.find(key); it != m_files.end()) {
    info.exists = true;
    info.size = it->second.size();
    return info;
  }
  // A path with children behaves as a directory.
  nx::string prefix = key;
  prefix.push_back('/');
  for (const auto &[k, v] : m_files) {
    if (k.view().starts_with(prefix.view())) {
      info.exists = true;
      info.is_directory = true;
      break;
    }
  }
  return info;
}

io_result<void> MemoryDevice::list(const path_view rel_dir, const ListFn &cb) {
  const nx::string dir = canon_rel(rel_dir);
  const nx::string prefix = dir.empty() ? nx::string() : dir + nx::string("/");

  nx::scoped_shared_lock lock(m_lock);
  ChildSet seen;
  bool any = false;
  for (const auto &[k, v] : m_files) {
    const nx::string_view kv = k.view();
    if (!prefix.empty() && !kv.starts_with(prefix.view()))
      continue;
    const nx::string_view rest = kv.substr(prefix.size());
    if (rest.empty())
      continue;
    const usize slash = rest.find('/');
    const bool is_dir = slash != nx::string_view::npos;
    const nx::string_view child = is_dir ? rest.substr(0, slash) : rest;
    if (!seen.insert(child))
      continue;
    any = true;
    FileInfo info;
    info.exists = true;
    info.is_directory = is_dir;
    if (!is_dir)
      info.size = v.size();
    cb(child, info);
  }
  if (!any && !dir.empty())
    return err(file_error{io_error::NotFound, 0});
  return {};
}

io_result<void> MemoryDevice::write(const path_view rel,
                                    const std::span<const u8> data) {
  nx::string key = canon_rel(rel);
  if (key.empty())
    return err(file_error{io_error::InvalidArgument, 0});
  nx::blob<u8> bytes(data);
  nx::scoped_lock lock(m_lock);
  m_files[std::move(key)] = std::move(bytes);
  return {};
}

io_result<void> MemoryDevice::remove(const path_view rel) {
  const nx::string key = canon_rel(rel);
  nx::scoped_lock lock(m_lock);
  if (m_files.erase(key) == 0)
    return err(file_error{io_error::NotFound, 0});
  return {};
}

MemoryDevice *make_memory_device() { return nx::allocate<MemoryDevice>(); }

} // namespace nx::vfs
