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

We build using clang and target the standard C++20.
Our parts of the code must enable all warnings and always build warnings free.
Warnings can be explicitely disabled for third party libraries that we do not control.
Maitain an ASAN build that must be green at all times.

## File organization

Use the monorepo style. Files go under `mustang/foo.cc` , `mustang/bar/baz.cc`, etc.
Import style is fully qualified: `#include "mustang/bar/baz.cc"`.
File suffixes are ".h" and ".cc" for headers.

## C++ style

We apply the [Google C++ style guide]( https://google.github.io/styleguide/cppguide.html) except
for that is explicitly overridden here.

### Imports

Use this style:

```c
// in a cc file, its own header always goes first.
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

### Libraries

We use Abseil as a base library. Use it extensively in particular always favor the Abseil libraries
instead of the standard.

  - Time manipulation is Abseil time.
  - Containers when available: `node_hash_map`, `node_hash_set`, `InlinedVector` for small vectors.
  - String manipulation: `StrCat` and the likes.
  - Flags for command line arguments.

We use PhotonLibOS as our general coroutine/event loop. Always use PhotonLibOS for:

  - Networking and interactions with the filesystem: it must be done in an async way.
  - Threading: assume from day 1 that we will have a thread per core design, and will have multiple vCPU. 
    Use a photon thread pool for blocking tasks.
  - Log: we use PhotonLibOS logs instead of Abseil.

### Exceptions

Never use exceptions. Use `absl::Status` and `absl::StatusOr` for all error handling. If we call an external
library that could throw an exception, wrap it in a narrow `try catch` that adapts it into a status.

### In-out parameters

Non-null short lived const parameters are const references.
If you need to keep a copy of the address: pass it as a const pointer instead.
Out parameters are non-const pointers.

### Mundane

Always check nullability explicitly: `if (foo != nullptr)`, `if (foo == nullptr)`.

Always use braces for `if` statements, even around one liner `if` statements.

### Namespace

All headers start with `#pragma once`. Not old style header guards.
All code is in namespace `mustang`. Use anonymous namespace in `.cc` files every time it's possible.

## Comments

Do not be verbose, don't document everything and never document the past behaviors.
Add comments where they help the understanding of a strategic decision or complex implementation.
Keep comments up to date.

## Performance

Keep the code as performant as it can be: leverage return value optimization, never do extra copies, etc.
