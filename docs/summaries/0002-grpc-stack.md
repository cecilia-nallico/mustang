# gRPC on PhotonLibOS — implementation summary

`mustang/grpc/` now carries a working, interop-tested gRPC stack built
directly on PhotonLibOS sockets and coroutines. All layers are ours except
the sockets/event loop, HPACK huffman codec and frame-header conventions
reused from photon master.

## Layer map

| Layer | Files | Verified by |
|---|---|---|
| HPACK (RFC 7541, full) | hpack.{h,cc} | 16 unit tests incl. RFC vectors |
| gRPC wire utils (R1-R20) | wire.{h,cc} | 16 unit tests |
| HTTP/2 transport (demux, flow control, CONTINUATION, GOAWAY, PING) | connection.{h,cc} | 11 loopback tests incl. adversarial raw peers |
| gRPC calls/server/client | call.{h,cc}, server.{h,cc} | 13 end-to-end loopback tests |
| Compliance oracle | interop_test.cc | 6 tests vs real gRPC C++ 1.85 (BCR, dev-only) |

## Key decisions & lessons (all encoded as comments where they bite)

- Photon's own H2Connection is unusable for multiplexed gRPC (RST for
  unparked streams, no flow control, static-only HPACK): mustang owns the
  transport; photon provides sockets/event loop/huffman.
- Photon coroutine + socket lifecycle: a blocked photon read/accept cannot
  be unblocked by close() — interrupt the coroutine first, join, then
  close. FIN before close to avoid RST destroying peer's queued frames.
- Never block the photon vCPU OS thread (std::thread joins, blocking CQs):
  park in thread_usleep and poll, or use photon thread pools.
- Delivered stream data wins over connection errors racing in after it.
- One TLS stack per binary: boringssl everywhere (curl + grpc defaults);
  mixing OpenSSL+boringssl duplicates ERR_*/EVP_*.
- grpc-timeout is enforced by the SERVER too (it cancels on expiry); a
  client should map post-deadline transport failure to DEADLINE_EXCEEDED.
- gRPC percent-encoding keeps space unreserved (unlike URI encoding).

## Java compliance suite

`java/` + `//tools:java_interop_test`: the **official gRPC Java client**
(1.84, Maven artifacts via rules_jvm_external — building grpc-java from
BCR source drags in conflicting maven universes from its android deps)
runs 12 scenarios against a mustang server started from the new
`mustang.interop.TestService` proto (generated through our own plugin):
unary, 1MiB payloads, NOT_FOUND with unicode percent-encoded message,
deadline (enforced within ~50ms), server-stream + 4MiB flow-control
stream, client-stream aggregate, interleaved bidi, metadata roundtrip via
request headers + response trailers, UNIMPLEMENTED, cancel-in-flight →
CANCELLED, and 8-thread mixed concurrency. No Java codegen: byte[]
marshaller + hand-rolled proto3 wire format, mirroring the C++ generic
interop. Protobuf-first include order matters (abseil logging macros vs
photon alog — see photon README).

## Try it

```bash
bazel test //mustang/grpc:all          # 4 test targets, loopback + unit
bazel test //mustang/grpc:interop_test   # vs real gRPC C++ (needs network once for deps)
bazel test //tools:java_interop_test    # vs official gRPC Java client (JDK 17)
```

## Next (M5+)

TLS+ALPN, per-message gzip, metadata bins, perf baseline vs grpc C++,
soak with random cancels; then the protobuf/REAPI layer for the bazel
cache on top of this transport.
