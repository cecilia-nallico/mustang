# Plan 0002 — gRPC client & server on top of PhotonLibOS

Status: PROPOSED (awaiting review; kickoff = M0)

Goal (PROJECT.md): a full, compliant gRPC client and server implementation on
PhotonLibOS, as the transport foundation for the bazel cache and action
server. Methodology mandated by the project owner:

1. checkout the gRPC codebase and understand it,
2. write ALL the tests first,
3. iterate the implementation until every test passes.

## Grounded facts (research done 2026-11-22)

- PhotonLibOS **v0.9.6** (currently vendored) has **no HTTP/2** at all:
  `net/http` is HTTP/1.1 only.
- PhotonLibOS **master** (commit `fe63fab`, 2026-11-16, unreleased) has an
  early HTTP/2 layer in `net/http/streams.{h,cpp}` (~1k LOC):
  `H2Connection`/`H2Stream` over any `IStream`, frame codec
  (FrameHeader w/ all frame types, error codes, settings ids), server/client
  preface, PING, GOAWAY, RST, per-stream window updates, an HPACK codec
  (integer/string encoding incl. huffman via `net/http/huffman/`), and a
  unit test `net/http/test/test_h2.cpp`. It is NOT wired into photon's
  HTTP server/client (server.cpp/client.cpp never reference it).
- BCR availability: `protobuf` 37.0-rc3 (36.2 stable), `grpc` 1.85.4 — can
  serve as build-time reference oracles and codegen, test-only.
- Canonical gRPC wire spec lives in grpc/grpc: `doc/PROTOCOL-HTTP2.md`,
  `doc/naming.md`; status codes in `include/grpcpp/support/...`; behavioral
  corpus in `test/core/end2end`.

## Decisions (proposed)

1. **Build on photon master's h2 layer** — re-vendor `third_party/photon`
   at pinned master commit `fe63fab` (README records that it is a pre-release
   snapshot; revisit when upstream tags it). We do NOT backport into v0.9.6
   and we do NOT write our own framing from scratch. First task after
   re-vendor: **audit** the h2 layer (see M1 audit list); gaps are fixed by
   us (documented in third_party/photon/README.md, contributed upstream when
   clean). Fallback if the audit is damning: vendor nghttp2 under
   third_party and drive it from photon coroutines (its callbacks are
   non-blocking I/O agnostic). nghttp2 is NOT the default because photon
   already owns the coroutine/socket layer and a pure-C++ stack fits the
   thread-per-core design.
2. **gRPC stack owned by mustang** on top of H2Connection/H2Stream:
   we own h2c prior-knowledge connection setup, the accept loop (photon
   socket server), HPACK policy, flow-control policy, and all gRPC
   semantics. TLS+ALPN comes later (M5); v1 speaks h2c (plaintext) which is
   the norm inside a cluster.
3. **Test oracle = real gRPC**: `@grpc` (C++) + `@protobuf` from BCR are
   test-only deps. Interop tests run our client against a grpc C++ server
   and a grpc C++ client against our server. This is the compliance
   authority, plus h2spec for the h2 layer.
4. Protobuf is the message format (REAPI is proto); codegen via protoc
   plugin in `tools/`, generating into `mustang/gen/` (gitignored) — same
   pattern frontend/walleye used for buf.
5. API shape (C++20, coroutines are photon threads):
   `mustang::grpc::Server` / `mustang::grpc::Client`, method handlers
   registered as `photon::Delegate`-style callbacks over
   `ServerContext`/`ClientContext` with `absl::StatusOr` returns; unary +
   server/client/bidi streaming from day 1 (REAPI needs them).

## Scope

In: HTTP/2 (RFC 9113 core), gRPC over HTTP/2 (doc/PROTOCOL-HTTP2.md):
unary + all streaming forms, grpc-status/-message trailers, trailers-only,
grpc-timeout (deadline propagation), cancellation via RST_STREAM, status
codes, max message size limits, flow control on both directions, keepalive
(PING), GOAWAY graceful drain, concurrent streams, h2c prior knowledge,
content-type negotiation. Later (M5+): TLS+ALPN, gzip compression,
grpc-accept-encoding, custom metadata bins, per-message compression.
Out (explicit non-goals for v1): retries/hedging, LB/xDS/ORCA, grpc-web,
HTTP/1.1 cleartext upgrade, proxy env support, deadlines-in-WS.

## Architecture sketch

```
mustang/grpc/
  connection.{h,cc}     h2c connection: preface/settings/GOAWAY/keepalive
  stream.{h,cc}         gRPC stream over H2Stream: framing, trailers, status
  framing.{h,cc}        5-byte message prefix codec (encode/decode)
  status.{h,cc}         grpc StatusCode <-> wire, <-> HTTP status mapping
  server.{h,cc}         accept loop (photon ISocketServer), dispatch table
  client.{h,cc}         connection pool, channel semantics, calls
  context.{h,cc}        Server/ClientContext: metadata, deadline, cancel
  codec.h               message serializer interface (protobuf impl)
mustang/grpc/test/      all tests (below) + proto test corpus
tools/proto/            protoc plugin/wrapper
tools/scripts/h2spec.sh conformance harness (optional, external h2spec)
```

One photon thread per connection (server) and per in-flight call window
(client); blocking gRPC handlers run on a photon thread pool — never the
vCPU thread (styleguide: photon thread pool for blocking tasks).

## Milestones

### M0 — understand gRPC (owner-mandated first step)
- Shallow-clone grpc/grpc at a pinned release tag matching the BCR version
  (1.85.4) into /tmp (reference material only, NOT vendored).
- Read: doc/PROTOCOL-HTTP2.md, HTTP/2 mapping tables, keepalive spec,
  status code semantics; skim test/core/end2end for behavioral cases.
- Write findings into docs/plans/0002-notes-grpc-protocol.md (wire rules
  checklist our tests derive from).
- Deliverable: the checklist above, reviewed, then tests are written.

### M1 — all tests written, all red (TDD baseline)
Complete test corpus (gtest unless noted), stubs return UNIMPLEMENTED:

h2 layer (some may already exist upstream as test_h2.cpp — port them):
  frame codec roundtrip incl. 24-bit lengths/flags/stream-id;
  preface handshake both sides; SETTINGS exchange + ACK + ack timeout;
  SETTINGS_INITIAL_WINDOW_SIZE / MAX_FRAME_SIZE effects;
  HEADERS + CONTINUATION splitting (header block > max frame size);
  HPACK: static idx, literal ±indexing, dynamic table insert/evict/size
  updates, huffman enc/dec, never-indexed literals, malformed HPACK error;
  DATA + padding; flow control: window exhaustion blocks send, WINDOW_UPDATE
  resumes, connection AND stream windows, oversize frame error;
  trailers (HEADERS after DATA with END_STREAM);
  RST_STREAM; GOAWAY last_stream_id semantics + drain;
  PING + PING-ACK; stream state machine (RFC 9113 5.1) invalid transitions;
  unknown frame type ignored; connection error vs stream error handling.
h2 conformance: tools/scripts/h2spec.sh wrapping h2spec (Go tool) against a
  tiny mustang h2 test server; record pass/fail report in docs/plans/.
gRPC wire (loopback over our own stack):
  unary roundtrip; message framing: 1B compressed-flag + 4BE length,
  multi-message streams; grpc-status trailer mapping for every code we
  support; grpc-message percent-encoding/decoding;
  trailers-only error responses; unknown method -> UNIMPLEMENTED;
  bad content-type -> HTTP 415; malformed frame -> protocol error;
  message > limit -> RESOURCE_EXHAUSTED;
  grpc-timeout honored + propagated, expiry -> DEADLINE_EXCEEDED;
  cancel -> RST_STREAM -> CANCELLED observed on the other side;
  server-streaming order + completion; client-streaming aggregation;
  bidi echo; multi-MB payloads (flow control both directions);
  100+ concurrent streams on one connection, mixed traffic;
  keepalive PING under idle; GOAWAY drain finishes in-flight calls.
Interop (gtest + BCR grpc C++ as oracle):
  our client -> grpc server: unary, all streamings, deadline, cancel,
  large messages, custom metadata, error codes;
  grpc client -> our server: same matrix;
  both directions with trailers-only responses.
Deliverable: every test exists, compiles, fails for the right reason.

### M2 — h2 layer green
Re-vendor photon @ fe63fab; port/extend tests; audit + fix:
dynamic table correctness, CONTINUATION handling, connection-level flow
control, WINDOW_UPDATE accounting, GOAWAY/PING timers, error paths,
iovec scatter/gather, zero-copy recv where photon allows. h2spec clean
(generic h2 parts).

### M3 — gRPC wire green (loopback)
framing, status, trailers, deadline, cancel, limits — everything in the
"gRPC wire" block above, our client against our server.

### M4 — interop green
Full matrix vs grpc C++ 1.85.4; soak: long-running streaming with random
cancels and disconnects; valgrind/ASAN clean (pending the workstation
AppProtection fix) under the interop suite.

### M5 — hardening & extras
TLS+ALPN (photon TLS client/server, offer h2 via ALPN, require prior
knowledge otherwise); gzip per-message compression (grpc-encoding);
metadata bins; perf baseline vs grpc C++ (rps + latency on loopback +
localhost), documented in docs/summaries.

## Risks / open questions

- Photon h2 is unreleased and unaudited; M1 audit may force the nghttp2
  fallback (decision framework: dynamic table + CONTINUATION + connection
  flow control are the make-or-break items).
- grpc C++ from BCR pulls its own abseil/cares/zlib pins — bzlmod must
  unify to one abseil (we already pin 20260817.0; verify no conflict).
- fully_static_link + grpc test binaries: grpc links fine static, verify
  once M1 lands.
- h2spec is a Go binary run ad hoc (not hermetic); keep it optional/scripted.
- Client channel semantics (subchannels, ready backoff) are simplified v1:
  one connection per endpoint, reconnect with backoff — documented.
