#include "mustang/grpc/call.h"

#include <utility>

#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"

namespace mustang {
namespace grpc {
namespace {

// Finds a header value; gRPC metadata allows duplicates, so this returns the
// first occurrence only (used for protocol headers which cannot repeat).
absl::optional<absl::string_view> Find(absl::string_view name, const std::vector<HeaderField>& headers) {
  for (const HeaderField& field : headers) {
    if (field.name == name) {
      return field.value;
    }
  }
  return std::nullopt;
}

absl::Status DeadlineStatus() { return absl::DeadlineExceededError("grpc: deadline exceeded"); }

bool DeadlinePassed(const absl::optional<absl::Time>& deadline) {
  return deadline.has_value() && absl::Now() >= *deadline;
}

std::string StatusHeader(StatusCode code) { return absl::StrCat(static_cast<int>(code)); }

std::vector<HeaderField> StatusTrailerFields(StatusCode code,
                                             const std::string& message,
                                             std::vector<HeaderField> metadata) {
  std::vector<HeaderField> fields;
  fields.push_back(HeaderField{"grpc-status", StatusHeader(code)});
  if (!message.empty()) {
    fields.push_back(HeaderField{"grpc-message", PercentEncode(message)});
  }
  for (auto& field : metadata) {
    fields.push_back(std::move(field));
  }
  return fields;
}

}  // namespace

// ---------------------------------------------------------------------------
// ServerCall
// ---------------------------------------------------------------------------

ServerCall::ServerCall(Stream* stream,
                       std::vector<HeaderField> request_headers,
                       std::optional<absl::Time> deadline,
                       size_t max_message_size)
    : stream_(stream), request_headers_(std::move(request_headers)), deadline_(deadline), decoder_(max_message_size) {}

absl::StatusOr<std::optional<std::string>> ServerCall::ReadMessage() {
  while (true) {
    absl::string_view feed = pending_;
    while (!feed.empty()) {
      absl::StatusOr<std::optional<MessageFrame>> frame = decoder_.Next(&feed);
      if (!frame.ok()) {
        // Oversize -> RESOURCE_EXHAUSTED; malformed framing -> INTERNAL.
        if (!read_failed_.has_value()) {
          read_failed_ = frame.status().code() == absl::StatusCode::kResourceExhausted ? StatusCode::kResourceExhausted
                                                                                       : StatusCode::kInternal;
        }
        return frame.status();
      }
      if (frame->has_value()) {
        pending_.assign(feed.data(), feed.size());  // keep the remainder
        return std::move((*frame)->payload);
      }
    }
    bool had_partial = !pending_.empty();
    pending_.clear();
    if (recv_done_) {
      if (had_partial) {
        // Stream ended mid-message: only complete frames may be delivered.
        if (!read_failed_.has_value()) {
          read_failed_ = StatusCode::kInternal;
        }
        return absl::InternalError("grpc: truncated message at end of stream");
      }
      return std::nullopt;  // clean end of the request stream
    }
    absl::StatusOr<absl::string_view> chunk = stream_->RecvData(&recv_done_);
    if (!chunk.ok()) {
      return chunk.status();
    }
    pending_.assign(chunk->data(), chunk->size());
  }
}

absl::Status ServerCall::WriteMessage(absl::string_view payload) {
  if (finished_) {
    return absl::InternalError("grpc: call already finished");
  }
  absl::Status st = SendResponseHeaders();
  if (!st.ok()) {
    return st;
  }
  return stream_->SendData(EncodeFrame(payload, /*compressed=*/false), false);
}

absl::Status ServerCall::SendResponseHeaders() {
  if (headers_sent_) {
    return absl::OkStatus();
  }
  headers_sent_ = true;
  return stream_->SendHeaders({HeaderField{":status", "200"}, HeaderField{"content-type", "application/grpc"}}, false);
}

absl::Status ServerCall::Finish(StatusCode code, std::string message, std::vector<HeaderField> metadata) {
  if (finished_) {
    return absl::InternalError("grpc: call already finished");
  }
  finished_ = true;
  // A request stream that could not be decoded fails the RPC regardless of
  // what the handler wants to report (grpc-core behavior).
  if (read_failed_.has_value()) {
    code = *read_failed_;
    message = "malformed request stream";
  }
  std::vector<HeaderField> trailer_fields = StatusTrailerFields(code, message, std::move(metadata));
  if (!headers_sent_) {
    // Trailers-only response (R14): status travels in the single block.
    std::vector<HeaderField> only;
    only.push_back(HeaderField{":status", "200"});
    only.push_back(HeaderField{"content-type", "application/grpc"});
    for (auto& field : trailer_fields) {
      only.push_back(std::move(field));
    }
    return stream_->SendHeaders(std::move(only), /*end_stream=*/true);
  }
  return stream_->SendTrailers(std::move(trailer_fields));
}

void ServerCall::Cancel() {
  if (!finished_) {
    finished_ = true;
  }
  (void)stream_->Reset(0x8 /* CANCEL */);
}

// ---------------------------------------------------------------------------
// ClientCall
// ---------------------------------------------------------------------------

ClientCall::ClientCall(Stream* stream, absl::optional<absl::Time> deadline) : stream_(stream), deadline_(deadline) {}

absl::Status ClientCall::WriteMessage(absl::string_view payload) {
  if (send_closed_ || finished_) {
    return absl::InternalError("grpc: request stream already closed");
  }
  return stream_->SendData(EncodeFrame(payload, /*compressed=*/false), false);
}

absl::Status ClientCall::CloseSend() {
  // Only send_closed_ guards this: Finish() sets finished_ before calling
  // here, and it must still send the half-close exactly once.
  if (send_closed_) {
    return absl::OkStatus();
  }
  send_closed_ = true;
  // R10: a request with nothing left to send closes with an empty DATA
  // frame carrying END_STREAM.
  return stream_->SendData("", true);
}

absl::Status ClientCall::AwaitResponseHeaders() {
  if (headers_done_) {
    return absl::OkStatus();
  }
  bool end = false;
  absl::StatusOr<std::vector<HeaderField>> headers = stream_->RecvHeaders(&end);
  if (!headers.ok()) {
    return headers.status();
  }
  headers_done_ = true;
  response_headers_ = *std::move(headers);
  if (end) {
    // Trailers-only response (R14): the status lives in this block.
    trailers_only_ = true;
    response_end_ = true;
  }
  return absl::OkStatus();
}

absl::StatusOr<std::optional<std::string>> ClientCall::ReadMessage() {
  if (DeadlinePassed(deadline_)) {
    Cancel();
    return DeadlineStatus();
  }
  absl::Status st = AwaitResponseHeaders();
  if (!st.ok()) {
    return st;
  }
  if (response_end_) {
    return std::nullopt;  // trailers-only: no messages
  }
  while (true) {
    absl::string_view feed = pending_;
    while (!feed.empty()) {
      absl::StatusOr<std::optional<MessageFrame>> frame = decoder_.Next(&feed);
      if (!frame.ok()) {
        return frame.status();
      }
      if (frame->has_value()) {
        if (DeadlinePassed(deadline_)) {
          Cancel();
          return DeadlineStatus();
        }
        pending_.assign(feed.data(), feed.size());  // keep the remainder
        return std::move((*frame)->payload);
      }
    }
    bool had_partial = !pending_.empty();
    pending_.clear();
    if (response_end_) {
      if (had_partial) {
        return absl::InternalError("grpc: truncated message at end of stream");
      }
      return std::nullopt;
    }
    if (DeadlinePassed(deadline_)) {
      Cancel();
      return DeadlineStatus();
    }
    absl::StatusOr<absl::string_view> chunk = stream_->RecvData(&response_end_);
    if (!chunk.ok()) {
      return chunk.status();
    }
    pending_.assign(chunk->data(), chunk->size());
  }
}

absl::StatusOr<CallResult> ClientCall::Finish() {
  if (finished_) {
    return absl::InternalError("grpc: call already finished");
  }
  finished_ = true;
  absl::Status st = CloseSend();
  if (!st.ok()) {
    return st;
  }
  if (DeadlinePassed(deadline_)) {
    Cancel();
    return DeadlineStatus();
  }
  st = AwaitResponseHeaders();
  if (!st.ok()) {
    return st;
  }

  std::vector<HeaderField> trailers;
  if (trailers_only_) {
    trailers = std::move(response_headers_);
  } else {
    // Normal response: read the actual trailers block. response_end_ being
    // set only means no further messages; trailers always follow.
    absl::StatusOr<std::vector<HeaderField>> got = stream_->RecvTrailers();
    if (!got.ok()) {
      return got.status();
    }
    trailers = *std::move(got);
  }

  CallResult result;
  absl::optional<absl::string_view> status = Find("grpc-status", trailers);
  if (!status.has_value()) {
    // R18: synthesize from the HTTP status when grpc-status is absent
    // (trailers-only responses were moved out of response_headers_ above).
    absl::optional<absl::string_view> http = Find(":status", trailers);
    if (!http.has_value()) {
      http = Find(":status", response_headers_);
    }
    int code = 200;
    if (!http.has_value() || !absl::SimpleAtoi(*http, &code)) {
      code = 200;
    }
    result.code = StatusCodeFromHttp(code);
    result.message = "missing grpc-status in response";
    return result;
  }
  int code = 0;
  if (!absl::SimpleAtoi(*status, &code)) {
    code = static_cast<int>(StatusCode::kInternal);
  }
  result.code = static_cast<StatusCode>(code);
  if (absl::optional<absl::string_view> message = Find("grpc-message", trailers)) {
    result.message = PercentDecode(*message);  // R16
  }
  for (auto& field : trailers) {
    // Expose application metadata only: gRPC protocol headers and HTTP/2
    // pseudo-headers (trailers-only responses fold :status in, R14).
    if (!field.name.empty() && field.name[0] != ':' && field.name != "grpc-status" && field.name != "grpc-message" &&
        field.name != "content-type") {
      result.metadata.push_back(std::move(field));
    }
  }
  if (DeadlinePassed(deadline_)) {
    return DeadlineStatus();
  }
  return result;
}

void ClientCall::Cancel() {
  if (!finished_) {
    finished_ = true;
  }
  (void)stream_->Reset(0x8 /* CANCEL */);
}

}  // namespace grpc
}  // namespace mustang
