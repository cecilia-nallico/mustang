# Plan 0001 — Bootstrap the Bazel build

Status: DONE (see docs/summaries/0001-bootstrap-build.md)

Goal: standing build + a hello-world vertical slice demonstrating the
styleguide stack (abseil flags, photon logging/runtime, gtest, fully static
binaries, ASAN config) before any real feature work.

## Steps

1. Bazel scaffolding
   - `.bazelversion` (9.3.3), `.bazelrc` (clang toolchain via repo_env,
     `--cxxopt=-std=c++20` globally, `--features=fully_static_link`,
     `--config=asan`), `MODULE.bazel` (bzlmod only, Bazel 9 has no WORKSPACE).
   - `tools/defaults.bzl`: `walleye_cc_{library,binary,test}` macros owning
     the strict flags (`-std=c++20 -Wall -Wextra -Werror`, linkstatic).

2. Third-party
   - BCR: abseil-cpp, googletest, rules_cc, rules_shell, platforms, curl,
     liburing.
   - PhotonLibOS not on BCR and CMake-only upstream -> vendor source tree at
     `third_party/photon` (v0.9.6, tag dd236e97) and write our own
     `BUILD.bazel` compiling the upstream core module set + epoll/epoll-ng/
     iouring engines + aio + signal + curl + tls-stream + easy_weak stubs.
   - libaio 0.3.113 vendored at `third_party/libaio` (hard dep of photon
     fs on Linux; system package avoided for hermeticity).
   - curl from BCR with `--@curl//:ssl_lib=openssl` so photon's TLS code
     (written against OpenSSL) and curl share one stack.

3. Hello-world slice under `walleye/`
   - `greet.{h,cc}`: trivial `absl::StatusOr<std::string>` library + gtest.
   - `runtime.{h,cc}`: RAII `Runtime` over `photon::init/fini` +
     `set_log_output_level` + `EventEngineFromName` mapping + gtest that
     runs a coroutine on the runtime.
   - `main.cc`: binary `//walleye:walleye` — parses absl flags
     (`--name/--log_level/--event_engine`), initializes photon runtime +
     logs, greets from a coroutine, joins, shuts down cleanly.
   - `tools/walleye_e2e_test.sh` (sh_test in `tools/BUILD.bazel`): runs the
     binary, asserts greeting, flag validation, exit codes.
   - `photon_headers.h`: umbrella include silencing clang diagnostics
     inside photon public headers only (they are not -Wextra clean and the
     styleguide only allows suppressing warnings for third-party code).

## Open items

- ASAN binaries crash on this workstation before main(): the machine-wide
  `/etc/ld.so.preload` of libAppProtection.so intercepts dlsym and
  segfaults inside ASan init. Trivial non-bazel asan binaries crash too.
  Needs an AppProtection exclusion or removal of the preload; the asan
  config itself is in place.
- photon `rpc/`, fs cache, fuse, extfs, kcp are vendored but not compiled;
  add to third_party/photon/BUILD.bazel when needed.
