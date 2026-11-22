#include "mustang/grpc/wire.h"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

namespace mustang {
namespace grpc {
namespace {

constexpr int64_t kMaxTimeoutDigitsValue = 99999999;  // 8 ASCII digits

struct TimeoutUnit {
  char unit;
  absl::Duration duration;
};
// Largest to smallest; FormatTimeout picks the first exact fit.
constexpr TimeoutUnit kTimeoutUnits[] = {
    {'H', absl::Hours(1)},
    {'M', absl::Minutes(1)},
    {'S', absl::Seconds(1)},
    {'m', absl::Milliseconds(1)},
    {'u', absl::Microseconds(1)},
    {'n', absl::Nanoseconds(1)},
};

char HexDigit(int v) { return static_cast<char>(v < 10 ? '0' + v : 'A' + v - 10); }

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

absl::StatusOr<absl::Duration> ParseTimeout(absl::string_view value) {
  if (value.empty() || value.size() > 9) {  // 8 digits + 1 unit char
    return absl::InvalidArgumentError("malformed grpc-timeout");
  }
  absl::string_view digits = value.substr(0, value.size() - 1);
  char unit = value.back();
  if (digits.empty()) {
    return absl::InvalidArgumentError("grpc-timeout has no value");
  }
  int64_t n = 0;
  for (char c : digits) {
    if (c < '0' || c > '9') {
      return absl::InvalidArgumentError("grpc-timeout value is not numeric");
    }
    n = n * 10 + (c - '0');
  }
  if (n == 0) {
    return absl::InvalidArgumentError("grpc-timeout must be positive");
  }
  for (const TimeoutUnit& u : kTimeoutUnits) {
    if (u.unit == unit) {
      return u.duration * n;
    }
  }
  return absl::InvalidArgumentError(absl::StrCat("unknown grpc-timeout unit '", std::string(1, unit), "'"));
}

std::string FormatTimeout(absl::Duration timeout) {
  int64_t ns = absl::ToInt64Nanoseconds(timeout);
  if (ns <= 0) {
    return "1n";  // clamp: the wire format cannot express zero/negative
  }
  for (const TimeoutUnit& u : kTimeoutUnits) {
    int64_t value = ns / absl::ToInt64Nanoseconds(u.duration);
    int64_t remainder = ns % absl::ToInt64Nanoseconds(u.duration);
    if (value >= 1 && value <= kMaxTimeoutDigitsValue && remainder == 0) {
      return absl::StrCat(value, std::string(1, u.unit));
    }
  }
  // Sub-nanosecond remainder cannot happen with absl::Duration; durations
  // larger than the wire format can express clamp to the maximum (R4).
  return "99999999H";
}

std::string PercentEncode(absl::string_view message) {
  std::string out;
  out.reserve(message.size());
  for (char c : message) {
    unsigned char b = static_cast<unsigned char>(c);
    if (b >= 0x20 && b <= 0x24) {
      out.push_back(c);  // space through '$'
    } else if (b >= 0x26 && b <= 0x7E) {
      out.push_back(c);  // '&' through '~', except '%' (0x25)
    } else {
      out.push_back('%');
      out.push_back(HexDigit(b >> 4));
      out.push_back(HexDigit(b & 0xf));
    }
  }
  return out;
}

std::string PercentDecode(absl::string_view message) {
  std::string out;
  out.reserve(message.size());
  for (size_t i = 0; i < message.size(); i++) {
    if (message[i] == '%' && i + 2 < message.size()) {
      int hi = HexValue(message[i + 1]);
      int lo = HexValue(message[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    // Invalid or truncated escape: keep the raw byte (R16: never error).
    out.push_back(message[i]);
  }
  return out;
}

std::string EncodeFrame(absl::string_view payload, bool compressed) {
  std::string out;
  out.resize(5 + payload.size());
  out[0] = compressed ? '\x01' : '\x00';
  uint32_t length = static_cast<uint32_t>(payload.size());
  out[1] = static_cast<char>(length >> 24);
  out[2] = static_cast<char>(length >> 16);
  out[3] = static_cast<char>(length >> 8);
  out[4] = static_cast<char>(length);
  out.replace(5, payload.size(), payload.data(), payload.size());
  return out;
}

MessageDecoder::MessageDecoder(size_t max_message_size) : max_message_size_(max_message_size) {}

absl::StatusOr<std::optional<MessageFrame>> MessageDecoder::Next(absl::string_view* data) {
  if (!have_header_) {
    size_t need = 5 - header_.size();
    size_t take = std::min(need, data->size());
    header_.append(data->data(), take);
    data->remove_prefix(take);
    if (header_.size() < 5) {
      return std::nullopt;
    }
    compressed_ = header_[0] != '\x00';
    length_ = (static_cast<uint32_t>(static_cast<unsigned char>(header_[1])) << 24) |
              (static_cast<uint32_t>(static_cast<unsigned char>(header_[2])) << 16) |
              (static_cast<uint32_t>(static_cast<unsigned char>(header_[3])) << 8) |
              static_cast<uint32_t>(static_cast<unsigned char>(header_[4]));
    header_.clear();
    have_header_ = true;
    if (length_ > max_message_size_) {
      return absl::ResourceExhaustedError(
          absl::StrCat("message of ", length_, " bytes exceeds limit of ", max_message_size_));
    }
  }
  size_t need = length_ - payload_.size();
  size_t take = std::min(need, data->size());
  payload_.append(data->data(), take);
  data->remove_prefix(take);
  if (payload_.size() < length_) {
    return std::nullopt;
  }
  have_header_ = false;
  length_ = 0;
  MessageFrame frame{compressed_, std::move(payload_)};
  compressed_ = false;
  payload_.clear();
  return frame;
}

absl::StatusOr<PathParts> ParsePath(absl::string_view path) {
  if (path.empty() || path.front() != '/') {
    return absl::InvalidArgumentError("gRPC :path must start with '/'");
  }
  path.remove_prefix(1);
  size_t slash = path.find('/');
  if (slash == absl::string_view::npos || slash == 0 || slash + 1 == path.size()) {
    return absl::InvalidArgumentError("gRPC :path must be /Service/Method");
  }
  PathParts parts;
  parts.service = std::string(path.substr(0, slash));
  parts.method = std::string(path.substr(slash + 1));
  if (parts.method.find('/') != std::string::npos) {
    return absl::InvalidArgumentError("gRPC :path method must not contain '/'");
  }
  return parts;
}

StatusCode StatusCodeFromRstError(uint32_t rst_error_code) {
  switch (rst_error_code) {
    case 7:  // REFUSED_STREAM
      return StatusCode::kUnavailable;
    case 8:  // CANCEL
      return StatusCode::kCancelled;
    case 11:  // ENHANCE_YOUR_CALM
      return StatusCode::kResourceExhausted;
    case 12:  // INADEQUATE_SECURITY
      return StatusCode::kPermissionDenied;
    default:
      // NO_ERROR, PROTOCOL_ERROR, INTERNAL_ERROR, FLOW_CONTROL_ERROR,
      // SETTINGS_TIMEOUT, FRAME_SIZE_ERROR, COMPRESSION_ERROR, CONNECT_ERROR
      // and anything unknown.
      return StatusCode::kInternal;
  }
}

StatusCode StatusCodeFromHttp(int http_status) {
  switch (http_status) {
    case 400:
      return StatusCode::kInternal;
    case 401:
      return StatusCode::kUnauthenticated;
    case 403:
      return StatusCode::kPermissionDenied;
    case 404:
      return StatusCode::kUnimplemented;
    case 429:
    case 502:
    case 503:
    case 504:
      return StatusCode::kUnavailable;
    default:
      // Including 200: a truly-OK response carries grpc-status, so a bare
      // 200 without it is an unknown condition (R18).
      return StatusCode::kUnknown;
  }
}

}  // namespace grpc
}  // namespace mustang
