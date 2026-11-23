#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"

#include "mustang/grpc/call.h"
#include "mustang/grpc/hpack.h"
#include "mustang/grpc/server.h"

namespace mustang {
namespace grpc {

// Options for generated stub calls.
struct CallOptions {
  std::vector<HeaderField> metadata;
  absl::optional<absl::Duration> timeout;
};

// gRPC status codes map 1:1 onto absl status codes (same semantics,
// different numeric values).
absl::StatusCode ToAbslCode(StatusCode code);
StatusCode ToGrpcCode(absl::StatusCode code);
absl::Status ToAbslStatus(const CallResult& result);

// Ends the call, logging (not propagating) transport-level failures: by
// the time final trailers are being written there is nothing left to do
// with an error. Framework-internal.
void FinishQuietly(ServerCall* call,
                   StatusCode code,
                   const std::string& message,
                   std::vector<HeaderField> metadata = {});

// --- typed helpers over the byte-oriented call API (M = protobuf message) ---

template <typename M>
absl::Status WriteMessage(ServerCall* call, const M& message) {
  std::string bytes;
  if (!message.SerializeToString(&bytes)) {
    return absl::InternalError("grpc: failed to serialize message");
  }
  return call->WriteMessage(bytes);
}

template <typename M>
absl::Status WriteMessage(ClientCall* call, const M& message) {
  std::string bytes;
  if (!message.SerializeToString(&bytes)) {
    return absl::InternalError("grpc: failed to serialize message");
  }
  return call->WriteMessage(bytes);
}

template <typename M>
absl::StatusOr<std::optional<M>> ReadMessage(ServerCall* call) {
  absl::StatusOr<std::optional<std::string>> raw = call->ReadMessage();
  if (!raw.ok()) {
    return raw.status();
  }
  if (!raw->has_value()) {
    return std::nullopt;
  }
  M message;
  if (!message.ParseFromString(**raw)) {
    return absl::InternalError("grpc: failed to parse message");
  }
  return message;
}

template <typename M>
absl::StatusOr<std::optional<M>> ReadMessage(ClientCall* call) {
  absl::StatusOr<std::optional<std::string>> raw = call->ReadMessage();
  if (!raw.ok()) {
    return raw.status();
  }
  if (!raw->has_value()) {
    return std::nullopt;
  }
  M message;
  if (!message.ParseFromString(**raw)) {
    return absl::InternalError("grpc: failed to parse message");
  }
  return message;
}

// Reads exactly one request message (dispatcher side).
template <typename M>
absl::StatusOr<M> ReadOne(ServerCall* call) {
  absl::StatusOr<std::optional<M>> message = ReadMessage<M>(call);
  if (!message.ok()) {
    return message.status();
  }
  if (!message->has_value()) {
    return absl::InvalidArgumentError("grpc: missing request message");
  }
  return **std::move(message);
}

// Context for unary and client-streaming handlers: request metadata,
// deadline, and response metadata the framework merges into the trailers
// it writes. Handlers return absl::StatusOr<Response>; the framework
// writes the response or the error status — handlers never touch
// WriteMessage themselves.
class UnaryContext {
 public:
  explicit UnaryContext(ServerCall* call) : call_(call) {}

  const std::vector<HeaderField>& request_metadata() const { return call_->request_metadata(); }
  absl::optional<absl::Time> deadline() const { return call_->deadline(); }
  // Metadata to attach to the final trailers (grpc-status still added by
  // the framework).
  void SetResponseMetadata(std::vector<HeaderField> metadata) { response_metadata_ = std::move(metadata); }

  // Framework-internal (generated dispatcher): trailers to merge in.
  std::vector<HeaderField> TakeResponseMetadata() { return std::move(response_metadata_); }

 private:
  ServerCall* call_;
  std::vector<HeaderField> response_metadata_;
};

// Shared machinery of the server-side stream views: call access plus the
// request context every streaming handler may need.
class ServerStreamBase {
 protected:
  explicit ServerStreamBase(ServerCall* call) : call_(call) {}

 public:
  // Aborts with RST_STREAM(CANCEL).
  void Cancel() { call_->Cancel(); }
  const std::vector<HeaderField>& request_metadata() const { return call_->request_metadata(); }
  absl::optional<absl::Time> deadline() const { return call_->deadline(); }

 protected:
  ServerCall* call_;
};

// Server-streaming handler view: the request arrived separately, so this
// only writes responses and closes.
template <typename R>
class ServerWriter : public ServerStreamBase {
 public:
  explicit ServerWriter(ServerCall* call) : ServerStreamBase(call) {}

  absl::Status Write(const R& message) { return WriteMessage(call_, message); }
  // Ends the RPC. The absl::Status overload maps codes 1:1.
  void Close(absl::Status status) { FinishQuietly(call_, ToGrpcCode(status.code()), std::string(status.message())); }
  void Close(StatusCode code, std::string message = "") { FinishQuietly(call_, code, std::move(message)); }
};

// Client-streaming handler view: read-only; the handler returns the
// single response (or an error) and the framework writes it.
template <typename W>
class ServerReader : public ServerStreamBase {
 public:
  explicit ServerReader(ServerCall* call) : ServerStreamBase(call) {}

  // One request message; std::nullopt = client half-closed.
  absl::StatusOr<std::optional<W>> Read() { return ReadMessage<W>(call_); }
};

// Bidi handler view: both directions.
template <typename W, typename R>
class ServerStream : public ServerStreamBase {
 public:
  explicit ServerStream(ServerCall* call) : ServerStreamBase(call) {}

  absl::StatusOr<std::optional<W>> Read() { return ReadMessage<W>(call_); }
  absl::Status Write(const R& message) { return WriteMessage(call_, message); }
  void Close(absl::Status status) { FinishQuietly(call_, ToGrpcCode(status.code()), std::string(status.message())); }
  void Close(StatusCode code, std::string message = "") { FinishQuietly(call_, code, std::move(message)); }
};

// A typed, move-only view of a client call. Do not copy: exactly one
// TypedCall owns the underlying ClientCall.
template <typename W, typename R>
class TypedCall {
 public:
  absl::Status Write(const W& message) { return WriteMessage(&call_, message); }
  absl::Status CloseSend() { return call_.CloseSend(); }
  absl::StatusOr<std::optional<R>> Read() { return ReadMessage<R>(&call_); }
  absl::StatusOr<CallResult> Finish() { return call_.Finish(); }
  void Cancel() { call_.Cancel(); }

 private:
  template <typename W2, typename R2>
  friend absl::StatusOr<TypedCall<W2, R2>> OpenStream(GrpcClient*, absl::string_view, const CallOptions&, const W2*);

  explicit TypedCall(ClientCall call) : call_(std::move(call)) {}

  ClientCall call_;
};

// Unary convenience used by generated stubs.
template <typename W, typename R>
absl::StatusOr<R> UnaryCall(GrpcClient* client, absl::string_view path, const W& request, const CallOptions& options) {
  absl::StatusOr<ClientCall> call = client->Call(std::string(path), options.metadata, options.timeout);
  if (!call.ok()) {
    return call.status();
  }
  absl::Status st = WriteMessage(&*call, request);
  if (!st.ok()) {
    return st;
  }
  st = call->CloseSend();
  if (!st.ok()) {
    return st;
  }
  absl::StatusOr<std::optional<R>> reply = ReadMessage<R>(&*call);
  if (!reply.ok()) {
    return reply.status();
  }
  absl::StatusOr<CallResult> result = call->Finish();
  if (!result.ok()) {
    return result.status();
  }
  if (!result->ok()) {
    return ToAbslStatus(*result);
  }
  if (!reply->has_value()) {
    return absl::InternalError("grpc: unary response missing message");
  }
  return **std::move(reply);
}

// Opens a streaming call; `initial` (if set) writes the first request
// message (server-streaming shape) before returning.
template <typename W, typename R>
absl::StatusOr<TypedCall<W, R>> OpenStream(GrpcClient* client,
                                           absl::string_view path,
                                           const CallOptions& options,
                                           const W* initial) {
  absl::StatusOr<ClientCall> call = client->Call(std::string(path), options.metadata, options.timeout);
  if (!call.ok()) {
    return call.status();
  }
  if (initial != nullptr) {
    absl::Status st = WriteMessage(&*call, *initial);
    if (!st.ok()) {
      return st;
    }
  }
  return TypedCall<W, R>(std::move(*call));
}

}  // namespace grpc
}  // namespace mustang
