# Vendored PhotonLibOS

- Upstream: https://github.com/alibaba/PhotonLibOS
- Version: **master snapshot** at commit `fe63fabb093305953162a6961cab684fef282e1f`
  (2026-11-16), taken **ahead of a release** because it carries the new
  HTTP/2 layer (`net/http/streams.{h,cpp}`: H2Connection/H2Stream, HPACK,
  huffman) that mustang's gRPC stack is built on. Re-pin to a tagged
  release once upstream ships one containing it.
- License: Apache-2.0 (see `LICENSE`)

PhotonLibOS is not on the Bazel Central Registry and upstream has no Bazel
support in this source layout, so the source tree is vendored here and
built with our own `BUILD.bazel` (styleguide rule: vendor into
`third_party` and write the build rules ourselves).

## Scope

The tree is a copy of the pinned commit minus directories irrelevant to
the Bazel build: `test/`, `examples/`, `doc/`, `tools/`, `CMake/`,
`CMakeLists.txt`, `ecosystem/`, `.github/`.

`BUILD.bazel` compiles the upstream `photon_module(core ...)` set
(common, thread, fs, net) plus the modules we enable:

- `io/epoll.cpp`, `io/epoll-ng.cpp`, `io/iouring-wrapper.cpp` — event
  engines; io_uring is enabled (`PHOTON_URING=on`, liburing from BCR).
- `io/aio-wrapper.cpp` — required by `fs/localfs.cpp` (vendored libaio in
  `third_party/libaio`).
- `net/http/*` (incl. `streams.cpp`, `websocket.cpp`, `huffman/codec.cpp`)
  — HTTP/1.x plus the HTTP/2 layer.
- `net/curl.cpp`, `fs/httpfs/httpfs.cpp` — libcurl integration
  (`ENABLE_CURL`, curl from BCR).
- `net/security-context/tls-stream.cpp` — TLS (OpenSSL 4 via curl's BCR
  backend).

Not compiled (present in tree for future use): `rpc/`, `blk/`, `kcp`,
`vdma`, fs cache, fuse, extfs, photon's own `third_party/fstack_weak`.

## Local modifications

These differ from the pristine upstream commit; keep them when
re-vendoring until upstream picks them up:

- `net/security-context/tls-stream.cpp`: `TLSContextImpl(TLSVersion)` uses
  `TLS_method()` plus `SSL_CTX_set_{min,max}_proto_version` instead of the
  version-specific `TLSv1_1_method()`/`TLSv1_2_method()`, which were removed
  in OpenSSL 4.0 (the TLS stack curl pulls in from BCR).

## Updating

Re-vendor from the pinned commit (or a newer release), keep this README
and the `BUILD.bazel` source list in sync with upstream's
`photon_module(core ...)` set in `CMakeLists.txt`.
