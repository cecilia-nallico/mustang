# Vendored libaio

- Upstream: https://github.com/axboe/libaio
- Version: **0.3.113**
- License: LGPL-2.1-or-later (see `COPYING`)

Photon's `io/aio-wrapper.cpp` links against libaio and photon's CMake treats
it as a hard dependency on Linux. Vendored so the build stays hermetic (no
system `libaio-dev` requirement) and fully static. Only `src/` sources and
headers are kept; `struct_offsets.c` (only used for symbol versioning in
upstream's shared-library build) is dropped.
