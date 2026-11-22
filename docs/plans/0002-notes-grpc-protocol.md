# 0002 notes — gRPC wire protocol checklist (M0)

Distilled from grpc/grpc **v1.85.4** (reference checkout in /tmp/grpc-1.85.4,
not vendored). Files: `doc/PROTOCOL-HTTP2.md`, `doc/http-grpc-status-mapping.md`,
`doc/keepalive.md`, `doc/connection-backoff.md`,
`include/grpcpp/support/status_code_enum.h`, end2end corpus in `test/core/`.

Rules are numbered (R#) so M1 tests can reference them. MUST = conformance
test required; SHOULD = test when cheap.

## Requests

- R1 MUST: `:method POST`, `:scheme http|https`, `:path /Service-Name/Method`
  (path case-sensitive; Service-Name = proto package "." service).
- R2 MUST: `te: trailers` present (detect incompatible proxies).
- R3 MUST: `content-type: application/grpc[+proto|+json|custom]`; servers
  respond HTTP 415 when it does not start with "application/grpc".
- R4 MUST: `grpc-timeout: TimeoutValue TimeoutUnit`, value = ASCII digits,
  max 8 digits, no leading zeros implied; units H M S m u n. Omitted =
  infinite. Should be sent right after reserved headers.
- R5 Custom-Metadata: ASCII header names `[0-9a-z_.-]`, printable ASCII
  values; `-bin` suffix = base64 (RFC 4648 §4) — receivers MUST accept padded
  and unpadded, emit unpadded; duplicate names joined with "," are
  equivalent, split `-bin` values on "," before decoding. `grpc-*` names not
  in the spec are reserved. ASCII-Value leading/trailing whitespace may be
  stripped; invalid-but-HTTP-legal values must not error.
- R6 MUST: reserved (`:`) headers before all others; Call-Definition headers
  before Custom-Metadata.
- R7 Header size limit: default 8 KiB suggested, accounting like
  SETTINGS_MAX_HEADER_LIST_SIZE (sum over fields of len(name)+len(value)+32,
  binary lengths post-base64). Exceeding → server refuses (grpc uses
  RESOURCE_EXHAUSTED / RST).

## Messages (both directions)

- R8 MUST: Length-Prefixed-Message = 1B Compressed-Flag (0/1) + 4B unsigned
  BE length + payload. Delivered in DATA frames.
- R9 MUST: DATA frame boundaries have NO relation to message boundaries
  (parser must reassemble; sender may split anywhere).
- R10 MUST: EOS on requests = END_STREAM on last DATA; if nothing to send,
  send an empty DATA frame with END_STREAM.
- R11 MUST: Compressed-Flag=1 requires `grpc-encoding`; flag=0 required when
  grpc-encoding omitted. Compression contexts are per-message (no cross-
  message state). v1: we advertise/accept only identity (uncompressed).

## Responses

- R12 Response = Response-Headers *Messages Trailers, or Trailers-Only.
- R13 MUST: `:status 200` for gRPC-level responses (errors included);
  grpc-status lives in Trailers even when OK.
- R14 MUST: Trailers-Only (single HEADERS block w/ END_STREAM) allowed for
  immediate errors: HTTP-Status + Content-Type + Status [+Message].
- R15 MUST: `grpc-status` = decimal ASCII, no leading zeros.
- R16 MUST: `grpc-message` percent-encoded UTF-8; unreserved = %x20-24,
  %x26-7E (space + VCHAR except %); decoding invalid input must NOT error —
  keep raw / decode valid parts / replacement char.
- R17: `grpc-status-details-bin` = base64 google.rpc.Status proto; only when
  status != OK; must not contradict grpc-status (consumer verifies).
- R18 MUST (client): response without grpc-status → synthesize from HTTP
  status: 400→INTERNAL, 401→UNAUTHENTICATED, 403→PERMISSION_DENIED,
  404→UNIMPLEMENTED, 429→UNAVAILABLE, 502/503/504→UNAVAILABLE, 200→UNKNOWN
  (because grpc-status should have been there), else UNKNOWN. If
  grpc-status present it MUST be used, never the mapping.
- R19 MUST (client): 1xx informational responses → skip the header block,
  read the next one (RFC 7540 §8.1).

## Errors & connection events

- R20 MUST: RST_STREAM mapping → grpc: NO_ERROR→INTERNAL, PROTOCOL_ERROR→
  INTERNAL, INTERNAL_ERROR→INTERNAL, FLOW_CONTROL_ERROR→INTERNAL,
  SETTINGS_TIMEOUT→INTERNAL, FRAME_SIZE_ERROR→INTERNAL, COMPRESSION_ERROR→
  INTERNAL, CONNECT_ERROR→INTERNAL, REFUSED_STREAM→UNAVAILABLE (retryable),
  CANCEL(8)→CANCELLED (from server; from client = cancellation), ENHANCE_
  YOUR_CALM→RESOURCE_EXHAUSTED, INADEQUATE_SECURITY→PERMISSION_DENIED.
  STREAM_CLOSED: no open stream, log only.
- R21 MUST: detectable connection failure → client closes all calls
  UNAVAILABLE; server closes open calls CANCELLED.
- R22 MUST: GOAWAY: streams > last_stream_id → UNAVAILABLE (client may
  retry elsewhere); accepted streams continue; server sends GOAWAY before
  terminating (graceful drain).

## Keepalive (PING)

- R23 MUST: PING must be answered by precisely echoing the 8 opaque bytes.
- R24: server ping watchdog expiry → close its calls CANCELLED; client ping
  watchdog expiry → close calls UNAVAILABLE.
- R25: ping policy — server defaults: min interval 5min without data,
  max 2 ping strikes then GOAWAY(ENHANCE_YOUR_CALM, "too_many_pings");
  client defaults: keepalive off, timeout 20s, ≤2 pings without data, no
  pings without calls unless permitted. v1: implement the policy knobs
  around these defaults; strict interop test only for echo + watchdog.

## Client connection management

- R26: exponential backoff reconnect: INITIAL_BACKOFF x MULTIPLIER jittered,
  MAX_BACKOFF cap (grpc defaults 1s/1.6x/120s, ±20% jitter — see
  connection-backoff.md). Calls during connecting: fail fast vs wait
  (WAIT_FOR_READY) — v1 implements fail-fast only.

## Status codes (canonical, all 16)

OK=0 CANCELLED=1 UNKNOWN=2 INVALID_ARGUMENT=3 DEADLINE_EXCEEDED=4
NOT_FOUND=5 ALREADY_EXISTS=6 PERMISSION_DENIED=7 RESOURCE_EXHAUSTED=8
FAILED_PRECONDITION=9 ABORTED=10 OUT_OF_RANGE=11 UNIMPLEMENTED=12
INTERNAL=13 UNAVAILABLE=14 DATA_LOSS=15 UNAUTHENTICATED=16

## HTTP/2 base behavior gRPC leans on (verify on photon master h2)

- H1 client preface `PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n` + SETTINGS; server
  sends SETTINGS first; both ACK. h2c prior knowledge both directions.
- H2 SETTINGS: MAX_CONCURRENT_STREAMS, INITIAL_WINDOW_SIZE, MAX_FRAME_SIZE,
  MAX_HEADER_LIST_SIZE; peer must apply + ACK; unknown settings ignored.
- H3 flow control: connection + stream windows; WINDOW_UPDATE; DATA must
  not exceed window or MAX_FRAME_SIZE; sender blocks, never errors, when
  window is exhausted.
- H4 HEADERS + CONTINUATION for header blocks > MAX_FRAME_SIZE; END_HEADERS
  terminates; HPACK with dynamic table + huffman (must decode peer's
  huffman; may encode literal).
- H5 stream states RFC 9113 §5.1; reserved-header ordering (R6); trailers
  are just HEADERS with END_STREAM (R12/R14).
- H6 unknown frame types MUST be ignored; frame with unknown flags ignored.

## Open questions deferred (not v1-blocking)

- grpc-status-details-bin propagation into mustang Status (needs google.rpc
  proto dependency) — decide at M3.
- Server-side grpc-timeout enforcement granularity (per-message check vs
  deadline in handler context).
- Whether our client sends `grpc-message-type` (optional; grpc core doesn't).
