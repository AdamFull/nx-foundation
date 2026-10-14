# Adapter for dependencies that insist on find_package(zstd) after
# third_party/CMakeLists.txt has added the vendored zstd build. Importing the
# generated zstd build-tree package here is invalid because its targets export
# does not exist until generation.
if(TARGET zstd::libzstd_static)
  set(zstd_FOUND TRUE)
else()
  set(zstd_FOUND FALSE)
endif()
