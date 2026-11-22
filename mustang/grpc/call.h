#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"

#include "mustang/grpc/connection.h"
#include "mustang/grpc/hpack.h"
#include "mustang/grpc/wire.h"

namespace mustang {
namespace grpc {

// Final status of a finished call as observed by the client.
struct CallResult {
  StatusCode code = StatusCode::kOk;
  std::string message;
  std::vector<HeaderField> metadata;  // trailers without grpc-*
  bool ok() const { return code == StatusCode::kOk; }
};

// Server side of one RPC. The handler runs on its own photon coroutine and
// must call Finish() (or Cancel()) exactly once; the dispatcher enforces a
// fallback Finish(INTERNAL) if the handler returns without either.
class ServerCall {
 public:
  ServerCall(Stream* stream,
             std::vector<HeaderField> request_headers,
             std::optional<absl::Time> deadline,
             size_t max_message_size);

  const std::vector<HeaderField>& request_metadata() const { return request_headers_; }
  absl::optional<absl::Time> deadline() const { return deadline_; }

  // One request message; std::nullopt = end of the client stream.
  absl::StatusOr<std::optional<std::string>> ReadMessage();
  // One response message (identity encoding; R11).
  absl::Status WriteMessage(absl::string_view payload);
  // Ends the call with status/message (+ response metadata). First use after
  // no WriteMessage produces a trailers-only response (R14).
  absl::Status Finish(StatusCode code, std::string message = "", std::vector<HeaderField> metadata = {});
  // Aborts with RST_STREAM(CANCEL) (R20).
  void Cancel();

  bool finished() const { return finished_; }

 private:
  absl::Status SendResponseHeaders();

  Stream* stream_;
  std::vector<HeaderField> request_headers_;
  absl::optional<absl::Time> deadline_;
  MessageDecoder decoder_;
  std::string pending_;  // unconsumed bytes of the last DATA chunk
  std::optional<StatusCode> read_failed_;
  bool recv_done_ = false;
  bool headers_sent_ = false;
  bool finished_ = false;
};

// Client side of one RPC over a GrpcClient connection.
class ClientCall {
 public:
  ClientCall(Stream* stream, std::optional<absl::Time> deadline);

  // One request message. Sending zero messages total is legal.
  absl::Status WriteMessage(absl::string_view payload);
  // Half-closes the request side (empty DATA + END_STREAM, R10). Client
  // streaming calls must do this before reading the final reply; Finish()
  // does it automatically when not done yet.
  absl::Status CloseSend();
  // One response message; std::nullopt = server half-closed (headers
  // promised trailers follow).
  absl::StatusOr<std::optional<std::string>> ReadMessage();
  // Waits for trailers and returns the final status; synthesizes one from
  // the HTTP status when the peer did not send grpc-status (R18).
  absl::StatusOr<CallResult> Finish();
  void Cancel();

 private:
  absl::Status AwaitResponseHeaders();

  Stream* stream_;
  absl::optional<absl::Time> deadline_;
  MessageDecoder decoder_;
  std::string pending_;  // unconsumed bytes of the last DATA chunk
  bool send_closed_ = false;
  bool headers_done_ = false;
  // The first HEADERS block carried END_STREAM: status lives in it (R14).
  bool trailers_only_ = false;
  bool response_end_ = false;  // no further response messages
  bool finished_ = false;
  std::vector<HeaderField> response_headers_;
};

}  // namespace grpc
}  // namespace mustang
