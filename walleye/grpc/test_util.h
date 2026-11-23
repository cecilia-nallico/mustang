#pragma once

// Shared helpers for gRPC transport tests: a hand-rolled raw HTTP/2 peer
// for adversarial/inspection cases.

#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

#include "walleye/photon_headers.h"

namespace walleye {
namespace grpc {
namespace testing {

constexpr uint8_t kTypeData = 0;
constexpr uint8_t kTypeHeaders = 1;
constexpr uint8_t kTypeSettings = 4;
constexpr uint8_t kTypePing = 6;
constexpr uint8_t kTypeGoaway = 7;
constexpr uint8_t kTypeWindowUpdate = 8;
constexpr uint8_t kTypeContinuation = 9;
constexpr uint8_t kFlagEndStream = 0x1;
constexpr uint8_t kFlagEndHeaders = 0x4;
constexpr uint8_t kFlagAck = 0x1;
constexpr char kPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

inline std::string FrameBytes(uint8_t type, uint8_t flags, uint32_t stream_id, absl::string_view payload) {
  std::string out(9 + payload.size(), '\0');
  out[0] = static_cast<char>(payload.size() >> 16);
  out[1] = static_cast<char>(payload.size() >> 8);
  out[2] = static_cast<char>(payload.size());
  out[3] = static_cast<char>(type);
  out[4] = static_cast<char>(flags);
  uint32_t sid = stream_id;
  out[5] = static_cast<char>(sid >> 24);
  out[6] = static_cast<char>(sid >> 16);
  out[7] = static_cast<char>(sid >> 8);
  out[8] = static_cast<char>(sid);
  out.replace(9, payload.size(), payload.data(), payload.size());
  return out;
}

// Raw HTTP/2 peer speaking directly over a photon socket stream.
struct RawPeer {
  photon::net::ISocketStream* sock = nullptr;

  absl::Status Write(absl::string_view bytes) {
    ssize_t n = sock->write(bytes.data(), bytes.size());
    return n == static_cast<ssize_t>(bytes.size()) ? absl::OkStatus() : absl::InternalError("raw write failed");
  }
  absl::Status WriteFrame(uint8_t type, uint8_t flags, uint32_t sid, absl::string_view payload) {
    return Write(FrameBytes(type, flags, sid, payload));
  }
  absl::Status ReadExact(std::string* out, size_t n) {
    out->resize(n);
    size_t got = 0;
    while (got < n) {
      ssize_t r = sock->read(out->data() + got, n - got);
      if (r <= 0) {
        return absl::InternalError("raw read failed");
      }
      got += static_cast<size_t>(r);
    }
    return absl::OkStatus();
  }
  struct Frame {
    uint8_t type = 0;
    uint8_t flags = 0;
    uint32_t stream_id = 0;
    std::string payload;
  };
  absl::StatusOr<Frame> ReadFrame() {
    std::string head;
    absl::Status st = ReadExact(&head, 9);
    if (!st.ok()) {
      return st;
    }
    Frame f;
    uint32_t len = (static_cast<uint32_t>(static_cast<unsigned char>(head[0])) << 16) |
                   (static_cast<uint32_t>(static_cast<unsigned char>(head[1])) << 8) |
                   static_cast<uint32_t>(static_cast<unsigned char>(head[2]));
    f.type = static_cast<uint8_t>(head[3]);
    f.flags = static_cast<uint8_t>(head[4]);
    f.stream_id = (static_cast<uint32_t>(static_cast<unsigned char>(head[5])) << 24) |
                  (static_cast<uint32_t>(static_cast<unsigned char>(head[6])) << 16) |
                  (static_cast<uint32_t>(static_cast<unsigned char>(head[7])) << 8) |
                  static_cast<uint32_t>(static_cast<unsigned char>(head[8]));
    if (len > 0) {
      st = ReadExact(&f.payload, len);
      if (!st.ok()) {
        return st;
      }
    }
    return f;
  }
  // Server-side raw handshake: read preface + client SETTINGS, reply with
  // an empty SETTINGS.
  absl::Status ServerHandshake() {
    std::string preface;
    absl::Status st = ReadExact(&preface, sizeof(kPreface) - 1);
    if (!st.ok()) {
      return st;
    }
    absl::StatusOr<Frame> settings = ReadFrame();
    if (!settings.ok()) {
      return settings.status();
    }
    return WriteFrame(kTypeSettings, 0, 0, "");
  }
};

}  // namespace testing
}  // namespace grpc
}  // namespace walleye
