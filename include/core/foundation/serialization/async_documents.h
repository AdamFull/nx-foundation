#pragma once

#include "core/foundation/serialization/ini.h"
#include "core/foundation/serialization/json.h"
#include "core/foundation/vfs/async_io.h"

#include <optional>

// The loads of JSON and INI assets as I/O jobs: they read on @p io's workers
// and never block the caller. load_document_file and ini::load(source) take
// the same path through blocking reads.

namespace nx::json {

/// The document at VFS path @p source, as load_document_file finds it; empty
/// when it is missing or does not decode.
[[nodiscard]] future<std::optional<value>>
load_document(vfs::AsyncIoService &io, nx::string_view source,
              vfs::AsyncIoRequest request = {});

} // namespace nx::json

namespace nx::ini {

/// The INI asset at VFS path @p source, as load(source) finds it.
[[nodiscard]] future<nx::result<Document, LoadError>>
load(vfs::AsyncIoService &io, nx::string_view source,
     vfs::AsyncIoRequest request = {});

} // namespace nx::ini
