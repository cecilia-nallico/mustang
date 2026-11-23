# Styleguide

The styleguide describes how we code.

## Language

The main programming language is C++.
You can write bash scripts under the `tools` directory only if needed.

## Build

Everything is built using bazel.
`bazel build ...` and `bazel test ...` must be kept green at all times.
All C++ libraries are built as static and every resulting binary is a fully statically linked binary.
When we need a third party dependency, check if it's available in the bazel central registry, if so that is the preferred
method. If not, use bazel rules to vendor it in and write a bazel build for it yourself.

Build rules and patches for third-party go under `third_party`.
Every `third_party/<dep>` directory carries a README.md recording the upstream version and commit,
the license, what is compiled and any local modifications.

First-party C++ targets are always declared through the `mustang_cc_*` macros in
`tools/defaults.bzl` (they own the strict warning flags and static linking); never raw `cc_*` rules.
`tools/scripts/format.sh` (clang-format + buildifier) must pass with `--check` before every commit.

We build using clang and target the standard C++20.
Our parts of the code must enable all warnings and always build warnings free.
Warnings can be explicitely disabled for third party libraries that we do not control.
Maitain an ASAN build that must be green at all times.

## File organization

Use the monorepo style. Files go under `mustang/foo.cc` , `mustang/bar/baz.cc`, etc.
Import style is fully qualified: `#include "mustang/bar/baz.h"`.
File suffixes are ".h" and ".cc" for headers.

## C++ style

We apply the [Google C++ style guide]( https://google.github.io/styleguide/cppguide.html) except
for that is explicitly overridden here.

### Imports

Use this style:

```c
// in a cc file, its own header always goes first.
// in a test file, the header under test is its own header.
#include "mustang/foo.h"

// system headers are in angle brackets. C and C++ are mixed together. 
#include <stddef.h>
#include <string>
#include <vector>

// headers of third party libraries, always in quotes not brackets.
#include "absl/strings/str_cat.h"
#include "photon/photon.h"

// headers of other mustang files.
#include "mustang/bar.h"
#include "mustang/baz.h"
```

Within each block, includes are sorted alphabetically (enforced by clang-format).

### Libraries

We use Abseil as a base library. Use it extensively in particular always favor the Abseil libraries
instead of the standard.

  - Strings: `absl::string_view` (never `std::`), `StrCat`, `StrSplit`, `StrJoin`.
  - Containers: `flat_hash_map`/`flat_hash_set` by default, `node_hash_map`/`node_hash_set` when
    pointer or reference stability is needed, `InlinedVector` for small vectors, `btree_map` for
    ordered maps.
  - Time: `absl::Time` and `absl::Duration`, never `time(3)` or `std::chrono`.
  - Errors: `absl::Status`, `absl::StatusOr`, `absl::ErrnoToStatus` for errno.
  - Flags for command line arguments.
  - Scope guards: `absl::Cleanup`, preferred over photon's `DEFER` in mustang code.
  - Wide integers: `absl::int128`.

We use PhotonLibOS as our general coroutine/event loop. Always use PhotonLibOS for:

  - Networking and interactions with the filesystem: it must be done in an async way.
  - Threading: assume from day 1 that we will have a thread per core design, and will have multiple vCPU. 
    Use a photon thread pool for blocking tasks.
  - Synchronization: never `std::mutex`, `absl::Mutex` or std/absl condition variables — they block
    the whole vCPU across a yield. Always use the photon primitives: `photon::mutex`,
    `photon::condvar`, `SCOPED_LOCK`.
  - Log: we use PhotonLibOS logs instead of Abseil.

Photon headers are included only through the umbrella `mustang/photon_headers.h` (extend the
umbrella when a new photon header is needed); direct `photon/...` includes are not allowed in
mustang code.

Photon logging conventions: backtick as variable placeholder (`LOG_WARN("count `", n)`), `VALUE(x)`
to log a variable's name and value, `LOG_ERROR_RETURN` when logging and returning an error.

### Exceptions

Never use exceptions. Use `absl::Status` and `absl::StatusOr` for all error handling. If we call an external
library that could throw an exception, wrap it in a narrow `try catch` that adapts it into a status.

We do not like the status macros (`ABSL_ASSIGN_OR_RETURN` etc). Never use them, always use explicit `if` checks instead.

### In-out parameters

Non-null short lived const parameters are const references.
If you need to keep a copy of the address: pass it as a const pointer instead.
Out parameters are non-const pointers.

Never take non-const reference parameters: an object the callee calls into or
mutates through (contexts, streams, clients, servers) is passed as a non-const
pointer. This makes mutation and ownership visible at call sites
(`server.Register(handler)` vs `Serve(&server)`).

### Mundane

Always check nullability explicitly: `if (foo != nullptr)`, `if (foo == nullptr)`.

Always use braces for `if` statements, even around one liner `if` statements.

### Namespace

All headers start with `#pragma once`. Not old style header guards.
All code is in namespace `mustang`. Use anonymous namespace in `.cc` files every time it's possible.

No blank line directly after a `namespace ... {` opening and none directly before a
`}  // namespace` closing:

```c
namespace mustang {
namespace {
constexpr int kFoo = 1;
}  // namespace

int Api();
}  // namespace mustang
```

## Comments

Do not be verbose, don't document everything and never document the past behaviors.
Add comments where they help the understanding of a strategic decision or complex implementation.
Keep comments up to date.

## Performance

Keep the code as performant as it can be: leverage return value optimization, never do extra copies, etc.
