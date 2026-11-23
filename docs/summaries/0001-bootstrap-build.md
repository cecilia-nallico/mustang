# Bootstrap build — decisions

The repository now has a working Bazel (bzlmod) build with a hello-world
vertical slice proving the whole stack. `bazel build //...` and
`bazel test //...` are green.

## Layout

- `walleye/` — all first-party code (`//walleye:walleye` binary, `greet`,
  `runtime` libraries, gtest + sh tests).
- `tools/defaults.bzl` — `walleye_cc_*` macros; the single place that owns
  `-std=c++20 -Wall -Wextra -Werror` and `linkstatic`.
- `third_party/photon` — PhotonLibOS v0.9.6 source tree + our BUILD
  (upstream has no Bazel support and is not on the BCR).
- `third_party/libaio` — libaio 0.3.113 + our BUILD (photon's fs needs it).

## Stack decisions

- **Bazel 9.3.3, bzlmod only.** `.bazelversion` pins it; MODULE.bazel has
  no WORKSPACE counterpart (removed in Bazel 9).
- **System clang (21.1.8)** selected via `--repo_env=CC/CXX=clang` through
  the auto-configured toolchain.
- **One C++ standard for everything**: `build --cxxopt=-std=c++20` in
  .bazelrc. This is load-bearing, not cosmetic: with the toolchain default
  (c++17) abseil libraries and C++20 consumers mangle
  `absl::SourceLocation` differently (`std::source_location` vs the
  fallback struct) and the link fails with confusing undefined references.
  Target copts still win, so photon compiles at `gnu++17` (its sweet spot).
- **PhotonLibOS**: core module set (common/thread/fs/net) + epoll, epoll-ng,
  io_uring (`PHOTON_URING=on`, liburing from BCR), libaio aio engine,
  signalfd engine, easy_weak stubs, and `net/http` + `net/curl.cpp` +
  `fs/httpfs` with `ENABLE_CURL` (curl 8.23.0 from BCR), plus
  `tls-stream.cpp`. Not compiled: rpc, fs cache, fuse, extfs, kcp — flip
  them on in `third_party/photon/BUILD.bazel` when needed.
- **curl TLS backend = OpenSSL** (`--@curl//:ssl_lib=openssl`), not
  boringssl: photon's TLS code is written against OpenSSL and uses reason
  macros boringssl lacks. One TLS stack for curl and photon.
- **Local photon patch** (documented in third_party/photon/README.md):
  `tls-stream.cpp` uses `TLS_method()` + min/max proto version instead of
  `TLSv1_1_method()/TLSv1_2_method()`, which OpenSSL 4.0 removed.
- **Photon headers are not -Wextra clean**: walleye code includes them via
  `walleye/photon_headers.h`, which pushes/pops clang diagnostics so
  suppression applies inside photon headers only (styleguide allows
  suppressing warnings for third-party, not for us).
- **Fully static binaries** via `build --features=fully_static_link`
  (verified: `file` reports statically linked; the usual glibc NSS
  getaddrinfo/dlopen link warnings are expected and benign).

## Testing

- `//walleye:greet_test`, `//walleye:runtime_test` (gtest; runtime test
  runs a coroutine with a timer sleep and joins it),
  `//tools:walleye_e2e_test` (sh_test executing the real binary, flags
  and exit codes).
- ASAN: `--config=asan` exists (asan + frame pointers, full-static
  disabled under asan because the static ASan runtime requires a dynamic
  section). NOTE: every ASan binary currently crashes **on this
  workstation** before main — `/etc/ld.so.preload` loads
  libAppProtection.so (endpoint security) whose dlsym hook segfaults
  during ASan init; a plain clang hello-world with -fsanitize=address
  crashes the same way. Fix the machine (exclusion or remove the preload)
  and `bazel test --config=asan` should be meaningful.

## Commands

```bash
bazel build //...            # everything (fully static binaries)
bazel test  //...            # unit + e2e tests
bazel test --config=asan //...   # once the AppProtection preload issue is fixed
bazel run   //walleye:walleye -- --name=you --log_level=0 --event_engine=iouring
tools/scripts/format.sh      # clang-format walleye/**, buildifier all BUILD files
tools/scripts/format.sh --check   # verify only, non-zero exit if unformatted
```
