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
absl::Status ToAbslStatus(const CallResult& result);

// --- typed helpers over the byte-oriented call API (M = protobuf message) ---

template <typename M>
absl::Status WriteMessage(ServerCall& call, const M& message) {
  std::string bytes;
  if (!message.SerializeToString(&bytes)) {
    return absl::InternalError("grpc: failed to serialize message");
  }
  return call.WriteMessage(bytes);
}

template <typename M>
absl::Status WriteMessage(ClientCall& call, const M& message) {
  std::string bytes;
  if (!message.SerializeToString(&bytes)) {
    return absl::InternalError("grpc: failed to serialize message");
  }
  return call.WriteMessage(bytes);
}

template <typename M>
absl::StatusOr<std::optional<M>> ReadMessage(ServerCall& call) {
  absl::StatusOr<std::optional<std::string>> raw = call.ReadMessage();
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
absl::StatusOr<std::optional<M>> ReadMessage(ClientCall& call) {
  absl::StatusOr<std::optional<std::string>> raw = call.ReadMessage();
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
absl::StatusOr<M> ReadOne(ServerCall& call) {
  absl::StatusOr<std::optional<M>> message = ReadMessage<M>(call);
  if (!message.ok()) {
    return message.status();
  }
  if (!message->has_value()) {
    return absl::InvalidArgumentError("grpc: missing request message");
  }
  return **std::move(message);
}

// A typed, move-only view of a client call. Do not copy: exactly one
// TypedCall owns the underlying ClientCall.
template <typename W, typename R>
class TypedCall {
 public:
  absl::Status Write(const W& message) { return WriteMessage(call_, message); }
  absl::Status CloseSend() { return call_.CloseSend(); }
  absl::StatusOr<std::optional<R>> Read() { return ReadMessage<R>(call_); }
  absl::StatusOr<CallResult> Finish() { return call_.Finish(); }
  void Cancel() { call_.Cancel(); }

 private:
  template <typename W2, typename R2>
  friend absl::StatusOr<TypedCall<W2, R2>> OpenStream(GrpcClient&, absl::string_view, const CallOptions&, const W2*);

  explicit TypedCall(ClientCall call) : call_(std::move(call)) {}

  ClientCall call_;
};

// Unary convenience used by generated stubs.
template <typename W, typename R>
absl::StatusOr<R> UnaryCall(GrpcClient& client, absl::string_view path, const W& request, const CallOptions& options) {
  absl::StatusOr<ClientCall> call = client.Call(std::string(path), options.metadata, options.timeout);
  if (!call.ok()) {
    return call.status();
  }
  absl::Status st = WriteMessage(*call, request);
  if (!st.ok()) {
    return st;
  }
  st = call->CloseSend();
  if (!st.ok()) {
    return st;
  }
  absl::StatusOr<std::optional<R>> reply = ReadMessage<R>(*call);
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
absl::StatusOr<TypedCall<W, R>> OpenStream(GrpcClient& client,
                                           absl::string_view path,
                                           const CallOptions& options,
                                           const W* initial) {
  absl::StatusOr<ClientCall> call = client.Call(std::string(path), options.metadata, options.timeout);
  if (!call.ok()) {
    return call.status();
  }
  if (initial != nullptr) {
    absl::Status st = WriteMessage(*call, *initial);
    if (!st.ok()) {
      return st;
    }
  }
  return TypedCall<W, R>(std::move(*call));
}

}  // namespace grpc
}  // namespace mustang
