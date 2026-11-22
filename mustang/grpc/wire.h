#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"

namespace mustang {
namespace grpc {

// gRPC status codes (protocol rule: canonical 16, values are wire-stable).
enum class StatusCode : int {
  kOk = 0,
  kCancelled = 1,
  kUnknown = 2,
  kInvalidArgument = 3,
  kDeadlineExceeded = 4,
  kNotFound = 5,
  kAlreadyExists = 6,
  kPermissionDenied = 7,
  kResourceExhausted = 8,
  kFailedPrecondition = 9,
  kAborted = 10,
  kOutOfRange = 11,
  kUnimplemented = 12,
  kInternal = 13,
  kUnavailable = 14,
  kDataLoss = 15,
  kUnauthenticated = 16,
};

// grpc-timeout header value <-> absl::Duration (R4: at most 8 ASCII digits,
// one unit of H M S m u n; omitted timeout means infinite; zero/negative or
// 9+ digits are protocol errors).
absl::StatusOr<absl::Duration> ParseTimeout(absl::string_view value);
// Picks the largest unit that represents the duration exactly within 8
// digits; sub-nanosecond remainders round up; zero clamps to "1n".
std::string FormatTimeout(absl::Duration timeout);

// grpc-message percent-encoding (R16). Encoding escapes everything outside
// %x20-%24 and %x26-%7E as %XX (uppercase hex). Decoding must never fail:
// invalid %-escapes are passed through unchanged.
std::string PercentEncode(absl::string_view message);
std::string PercentDecode(absl::string_view message);

// One Length-Prefixed-Message (R8): 1B compressed-flag + 4B big-endian
// length + payload.
struct MessageFrame {
  bool compressed = false;
  std::string payload;
};

std::string EncodeFrame(absl::string_view payload, bool compressed = false);

// Reassembles messages out of arbitrary DATA chunk boundaries (R9: frame
// boundaries have no relation to message boundaries). One decoder per gRPC
// stream direction.
class MessageDecoder {
 public:
  explicit MessageDecoder(size_t max_message_size = size_t{4} << 20);

  MessageDecoder(const MessageDecoder&) = delete;
  MessageDecoder& operator=(const MessageDecoder&) = delete;

  // Consumes from *data (advancing it). Returns a complete frame, std::nullopt
  // when more data is needed, or an error on protocol violation. A payload
  // larger than max_message_size is RESOURCE_EXHAUSTED.
  absl::StatusOr<std::optional<MessageFrame>> Next(absl::string_view* data);

 private:
  const size_t max_message_size_;
  std::string header_;   // partial 5-byte prefix across chunks
  std::string payload_;  // partial payload across chunks
  bool have_header_ = false;
  bool compressed_ = false;
  uint32_t length_ = 0;
};

// ":path" -> {service, method} (R1: /Service-Name/Method, both non-empty).
struct PathParts {
  std::string service;
  std::string method;
};
absl::StatusOr<PathParts> ParsePath(absl::string_view path);

// RST_STREAM error code -> gRPC status (R20).
StatusCode StatusCodeFromRstError(uint32_t rst_error_code);

// HTTP status -> gRPC status, used only when a response lacks grpc-status
// (R18). 200 maps to UNKNOWN because grpc-status should have been present.
StatusCode StatusCodeFromHttp(int http_status);

}  // namespace grpc
}  // namespace mustang
