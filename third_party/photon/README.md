# Vendored PhotonLibOS

- Upstream: https://github.com/alibaba/PhotonLibOS
- Version: **v0.9.6** (tag), commit `dd236e97866fd3d4cbff64b7ada7b69ef9464695`
- License: Apache-2.0 (see `LICENSE`)

PhotonLibOS is not on the Bazel Central Registry and upstream dropped its
Bazel support when it moved to the current source layout, so the source tree
is vendored here and built with our own `BUILD.bazel` (styleguide rule:
vendor into `third_party` and write the build rules ourselves).

## Scope

The tree is a copy of the upstream tag minus directories irrelevant to the
Bazel build: `test/`, `examples/`, `doc/`, `tools/`, `CMake/`,
`CMakeLists.txt`, `ecosystem/`, `.github/`.

`BUILD.bazel` compiles the upstream "core" module set (common, thread, fs,
net, io) plus the modules we enable:

- `io/epoll.cpp`, `io/epoll-ng.cpp`, `io/iouring-wrapper.cpp` — event
  engines; io_uring is enabled (`PHOTON_URING=on`, liburing from BCR).
- `io/aio-wrapper.cpp` — required by `fs/localfs.cpp` (vendored libaio in
  `third_party/libaio`).
- `net/curl.cpp`, `fs/httpfs/httpfs.cpp` — libcurl integration
  (`ENABLE_CURL`, curl from BCR).

Not compiled (present in tree for future use): `rpc/`, photon's own
`third_party/fstack_weak`. TLS (`net/security-context/tls-stream.cpp`) and
the fs cache are not compiled either; add them here if needed.

## Local modifications

These differ from the pristine v0.9.6 tag; keep them when re-vendoring
until upstream picks them up:

- `net/security-context/tls-stream.cpp`: `TLSContextImpl(TLSVersion)` uses
  `TLS_method()` plus `SSL_CTX_set_{min,max}_proto_version` instead of the
  version-specific `TLSv1_1_method()`/`TLSv1_2_method()`, which were removed
  in OpenSSL 4.0 (the TLS stack curl pulls in from BCR).

## Updating

Re-vendor from a tagged release, keep this README and the `BUILD.bazel`
source list in sync with upstream's `photon_module(core ...)` set in
`CMakeLists.txt`.
