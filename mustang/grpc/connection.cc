#include "mustang/grpc/connection.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "absl/strings/str_cat.h"

#include "mustang/grpc/wire.h"

namespace mustang {
namespace grpc {
namespace {

constexpr size_t kFrameHeaderSize = 9;
constexpr char kPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr uint64_t kMaxWindow = 0x7FFFFFFF;  // RFC 9113 6.9.1
constexpr uint64_t kDefaultWindow = 65535;

// Frame types (RFC 9113 s6.2).
enum FrameType : uint8_t {
  kData = 0,
  kHeaders = 1,
  kPriority = 2,
  kRstStream = 3,
  kSettings = 4,
  kPushPromise = 5,
  kPing = 6,
  kGoaway = 7,
  kWindowUpdate = 8,
  kContinuation = 9,
};
constexpr uint8_t kFlagAck = 0x1;
constexpr uint8_t kFlagEndStream = 0x1;
constexpr uint8_t kFlagEndHeaders = 0x4;
constexpr uint8_t kFlagPadded = 0x8;

// SETTINGS ids.
enum SettingId : uint16_t {
  kSettingHeaderTableSize = 0x1,
  kSettingEnablePush = 0x2,
  kSettingMaxConcurrentStreams = 0x3,
  kSettingInitialWindowSize = 0x4,
  kSettingMaxFrameSize = 0x5,
  kSettingMaxHeaderListSize = 0x6,
};

absl::Status Unavailable(absl::string_view what) { return absl::UnavailableError(absl::StrCat("h2: ", what)); }

absl::Status RstToStatus(uint32_t rst_error_code) {
  switch (StatusCodeFromRstError(rst_error_code)) {
    case StatusCode::kUnavailable:
      return absl::UnavailableError(absl::StrCat("h2: reset by peer (", rst_error_code, ")"));
    case StatusCode::kCancelled:
      return absl::CancelledError(absl::StrCat("h2: cancelled by peer (", rst_error_code, ")"));
    case StatusCode::kResourceExhausted:
      return absl::ResourceExhaustedError("h2: peer says enhance your calm");
    case StatusCode::kPermissionDenied:
      return absl::PermissionDeniedError("h2: inadequate security");
    default:
      return absl::InternalError(absl::StrCat("h2: reset by peer (", rst_error_code, ")"));
  }
}

absl::Status ReadExact(photon::net::ISocketStream* socket, void* buf, size_t len) {
  char* p = static_cast<char*>(buf);
  size_t got = 0;
  while (got < len) {
    ssize_t n = socket->read(p + got, len - got);
    if (n <= 0) {
      return Unavailable("socket read failed");
    }
    got += static_cast<size_t>(n);
  }
  return absl::OkStatus();
}

absl::Status WriteAll(photon::net::ISocketStream* socket, const void* buf, size_t len) {
  const char* p = static_cast<const char*>(buf);
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = socket->write(p + sent, len - sent);
    if (n <= 0) {
      return Unavailable("socket write failed");
    }
    sent += static_cast<size_t>(n);
  }
  return absl::OkStatus();
}

void PutBe16(char* p, uint16_t v) {
  p[0] = static_cast<char>(v >> 8);
  p[1] = static_cast<char>(v);
}

void PutBe32(char* p, uint32_t v) {
  p[0] = static_cast<char>(v >> 24);
  p[1] = static_cast<char>(v >> 16);
  p[2] = static_cast<char>(v >> 8);
  p[3] = static_cast<char>(v);
}

std::string RstPayload(uint32_t error_code) {
  std::string payload(4, '\0');
  PutBe32(payload.data(), error_code);
  return payload;
}

uint32_t GetBe32(const char* p) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
         static_cast<uint32_t>(static_cast<unsigned char>(p[3]));
}

}  // namespace

struct Connection::StreamState {
  uint32_t id = 0;
  // Receive side.
  std::vector<HeaderField> headers;
  bool headers_ready = false;
  bool headers_end_stream = false;  // this block carried END_STREAM
  std::string recv_buf;
  std::string recv_out;  // chunk last handed out by StreamRecvData
  bool recv_end = false;
  bool trailers_ready = false;
  std::vector<HeaderField> trailers;
  // Send side.
  bool headers_sent = false;
  bool send_done = false;
  int64_t send_window = kDefaultWindow;
  // Flow control receive accounting.
  uint32_t recv_unannounced = 0;
  bool failed = false;
  absl::Status error;
};

// ---------------------------------------------------------------------------
// Stream handle -> Connection ops
// ---------------------------------------------------------------------------

absl::Status Stream::SendHeaders(std::vector<HeaderField> headers, bool end_stream) {
  return conn_->StreamSendHeaders(id_, std::move(headers), end_stream);
}

absl::Status Stream::SendData(absl::string_view data, bool end_stream) {
  return conn_->StreamSendData(id_, data, end_stream);
}

absl::Status Stream::SendTrailers(std::vector<HeaderField> trailers) {
  return conn_->StreamSendTrailers(id_, std::move(trailers));
}

absl::Status Stream::Reset(uint32_t error_code) { return conn_->StreamReset(id_, error_code); }

absl::StatusOr<std::vector<HeaderField>> Stream::RecvHeaders(bool* end_of_stream) {
  return conn_->StreamRecvHeaders(id_, end_of_stream);
}

absl::StatusOr<absl::string_view> Stream::RecvData(bool* end_of_stream) {
  return conn_->StreamRecvData(id_, end_of_stream);
}

absl::StatusOr<std::vector<HeaderField>> Stream::RecvTrailers() { return conn_->StreamRecvTrailers(id_); }

void Stream::Done() { conn_->StreamDone(id_); }

// ---------------------------------------------------------------------------
// Connection lifecycle
// ---------------------------------------------------------------------------

Connection::Connection(photon::net::ISocketStream* socket,
                       bool is_client,
                       const ConnectionOptions& options,
                       bool ownership)
    : socket_(socket),
      ownership_(ownership),
      is_client_(is_client),
      options_(options),
      // Our decoder must at least accept what we allow in SETTINGS.
      hpack_(options.max_header_table_size, options.max_header_list_size) {
  next_stream_id_ = is_client ? 1 : 2;
  conn_send_window_ = kDefaultWindow;
}

Connection::~Connection() {
  {
    SCOPED_LOCK(mu_);
    conn_error_ = conn_error_.ok() ? Unavailable("connection closed") : conn_error_;
    FailAllStreamsLocked(is_client_ ? absl::UnavailableError("h2: connection closed")
                                    : absl::CancelledError("h2: connection closed"));
    going_away_ = true;
    accept_cv_.notify_all();
  }
  // The reader coroutine owns socket reads; a photon read blocked in the
  // event loop cannot be unblocked by closing the fd from another
  // coroutine. Interrupt it first (ReadExact then fails), join it, and only
  // afterwards tear the socket down. FIN before close so unread inbound
  // data does not make the kernel send RST and destroy frames the peer
  // has not read yet.
  if (reader_ != nullptr) {
    photon::thread_interrupt(reader_);
  }
  if (reader_join_ != nullptr) {
    photon::thread_join(reader_join_);
  }
  if (socket_ != nullptr) {
    socket_->shutdown(ShutdownHow::Write);
    socket_->close();
  }
  if (ownership_ && socket_ != nullptr) {
    delete socket_;
  }
}

absl::Status Connection::Start() {
  if (started_) {
    return absl::InternalError("h2: connection already started");
  }
  if (is_client_) {
    absl::Status st = WriteAll(socket_, kPreface, sizeof(kPreface) - 1);
    if (!st.ok()) {
      return st;
    }
    st = SendSettings();
    if (!st.ok()) {
      return st;
    }
  } else {
    char preface[sizeof(kPreface) - 1];
    absl::Status st = ReadExact(socket_, preface, sizeof(preface));
    if (!st.ok()) {
      return st;
    }
    if (memcmp(preface, kPreface, sizeof(preface)) != 0) {
      Abort(0x1 /* PROTOCOL_ERROR */, "bad preface");
      return absl::InternalError("h2: invalid client preface");
    }
    st = SendSettings();
    if (!st.ok()) {
      return st;
    }
  }
  started_ = true;
  reader_ = photon::thread_create11([this]() { RunReader(); });
  reader_join_ = photon::thread_enable_join(reader_, true);
  return absl::OkStatus();
}

absl::Status Connection::Shutdown(absl::string_view debug_data) {
  char payload[8 + 64];
  PutBe32(payload, last_accepted_stream_id_);
  PutBe32(payload + 4, 0 /* NO_ERROR */);
  std::string block(payload, 8);
  block.append(debug_data.data(), debug_data.size());
  {
    SCOPED_LOCK(mu_);
    going_away_ = true;
    accept_cv_.notify_all();
  }
  return WriteFrame(kGoaway, 0, 0, block);
}

bool Connection::going_away() const { return going_away_; }

absl::Status Connection::WaitForPeerSettings() {
  SCOPED_LOCK(mu_);
  bool ok = cv_.wait(mu_, [&]() { return peer_settings_received_ || !conn_error_.ok(); });
  (void)ok;
  return peer_settings_received_ ? absl::OkStatus() : conn_error_;
}

uint32_t Connection::last_accepted_stream_id() const { return last_accepted_stream_id_; }

// ---------------------------------------------------------------------------
// Stream creation
// ---------------------------------------------------------------------------

absl::StatusOr<Stream*> Connection::OpenStream() {
  SCOPED_LOCK(mu_);
  bool ok = accept_cv_.wait(mu_, [&]() {
    return !conn_error_.ok() || going_away_ || (peer_max_concurrent_ != 0 && open_streams_ < peer_max_concurrent_) ||
           (peer_max_concurrent_ == 0);
  });
  (void)ok;
  if (!conn_error_.ok()) {
    return conn_error_;
  }
  if (going_away_) {
    return Unavailable("connection going away");
  }
  if (next_stream_id_ >= 0x7FFFFFFF) {
    return Unavailable("stream ids exhausted");
  }
  uint32_t id = next_stream_id_;
  next_stream_id_ += 2;
  auto st = std::make_unique<StreamState>();
  st->id = id;
  st->send_window = peer_initial_window_;
  streams_[id] = std::move(st);
  open_streams_++;
  return new Stream(this, id);
}

absl::StatusOr<Stream*> Connection::AcceptStream() {
  SCOPED_LOCK(mu_);
  bool ok = accept_cv_.wait(mu_, [&]() { return !accept_queue_.empty() || !conn_error_.ok() || going_away_; });
  (void)ok;
  if (!accept_queue_.empty()) {
    uint32_t id = accept_queue_.front();
    accept_queue_.pop_front();
    return new Stream(this, id);
  }
  return !conn_error_.ok() ? conn_error_ : Unavailable("connection going away");
}

// ---------------------------------------------------------------------------
// Stream ops
// ---------------------------------------------------------------------------

Connection::StreamState* Connection::FindStreamLocked(uint32_t id) {
  auto it = streams_.find(id);
  return it == streams_.end() ? nullptr : it->second.get();
}

void Connection::FailStreamLocked(StreamState* st, absl::Status status) {
  st->failed = true;
  st->error = std::move(status);
  cv_.notify_all();
}

void Connection::FailAllStreamsLocked(const absl::Status& status) {
  for (auto& [id, st] : streams_) {
    if (!st->failed) {
      FailStreamLocked(st.get(), status);
    }
  }
}

absl::Status Connection::StreamSendHeaders(uint32_t id, std::vector<HeaderField> headers, bool end_stream) {
  {
    SCOPED_LOCK(mu_);
    StreamState* st = FindStreamLocked(id);
    if (st == nullptr) {
      return absl::InternalError("h2: stream done");
    }
    if (st->failed) {
      return st->error;
    }
  }
  absl::StatusOr<std::string> block = HpackEncode(headers);
  if (!block.ok()) {
    return block.status();
  }
  absl::Status st = WriteHeaderBlock(id, *block, end_stream);
  if (!st.ok()) {
    return st;
  }
  SCOPED_LOCK(mu_);
  if (StreamState* s = FindStreamLocked(id); s != nullptr) {
    s->headers_sent = true;
    if (end_stream) {
      s->send_done = true;
    }
  }
  return absl::OkStatus();
}

absl::Status Connection::StreamSendData(uint32_t id, absl::string_view data, bool end_stream) {
  size_t offset = 0;
  if (data.empty() && end_stream) {
    // R10: close an empty request stream with an empty DATA frame.
    absl::Status st = WriteFrame(kData, kFlagEndStream, id, "");
    if (!st.ok()) {
      return st;
    }
    SCOPED_LOCK(mu_);
    if (StreamState* s = FindStreamLocked(id); s != nullptr) {
      s->send_done = true;
    }
    return absl::OkStatus();
  }
  while (offset < data.size()) {
    uint64_t chunk;
    {
      SCOPED_LOCK(mu_);
      StreamState* st = FindStreamLocked(id);
      if (st == nullptr) {
        return absl::InternalError("h2: stream done");
      }
      if (st->failed) {
        return st->error;
      }
      if (!conn_error_.ok()) {
        return conn_error_;
      }
      bool windowed = cv_.wait(mu_, [&]() {
        StreamState* s = FindStreamLocked(id);
        if (s == nullptr || s->failed) {
          return true;
        }
        if (!conn_error_.ok()) {
          return true;
        }
        return std::min(conn_send_window_, s->send_window) > 0;
      });
      (void)windowed;
      st = FindStreamLocked(id);
      if (st == nullptr) {
        return absl::InternalError("h2: stream done");
      }
      if (st->failed) {
        return st->error;
      }
      if (!conn_error_.ok()) {
        return conn_error_;
      }
      chunk = std::min<uint64_t>({static_cast<uint64_t>(conn_send_window_),
                                  static_cast<uint64_t>(st->send_window),
                                  static_cast<uint64_t>(peer_max_frame_size_),
                                  static_cast<uint64_t>(data.size() - offset)});
      conn_send_window_ -= static_cast<int64_t>(chunk);
      st->send_window -= static_cast<int64_t>(chunk);
    }
    uint8_t flags = (offset + chunk == data.size() && end_stream) ? kFlagEndStream : 0;
    absl::Status st2 = WriteFrame(kData, flags, id, data.substr(offset, static_cast<size_t>(chunk)));
    if (!st2.ok()) {
      return st2;
    }
    offset += chunk;
  }
  if (end_stream) {
    SCOPED_LOCK(mu_);
    if (StreamState* st = FindStreamLocked(id); st != nullptr) {
      st->send_done = true;
    }
  }
  return absl::OkStatus();
}

absl::Status Connection::StreamSendTrailers(uint32_t id, std::vector<HeaderField> trailers) {
  {
    SCOPED_LOCK(mu_);
    StreamState* st = FindStreamLocked(id);
    if (st == nullptr) {
      return absl::InternalError("h2: stream done");
    }
    if (st->failed || !st->headers_sent || st->send_done) {
      return st->failed ? st->error : absl::InternalError("h2: invalid send state for trailers");
    }
  }
  absl::StatusOr<std::string> block = HpackEncode(trailers);
  if (!block.ok()) {
    return block.status();
  }
  absl::Status st = WriteHeaderBlock(id, *block, /*end_stream=*/true);
  if (!st.ok()) {
    return st;
  }
  SCOPED_LOCK(mu_);
  if (StreamState* s = FindStreamLocked(id); s != nullptr) {
    s->send_done = true;
  }
  return absl::OkStatus();
}

absl::Status Connection::StreamReset(uint32_t id, uint32_t error_code) {
  {
    SCOPED_LOCK(mu_);
    StreamState* st = FindStreamLocked(id);
    if (st == nullptr) {
      return absl::InternalError("h2: stream done");
    }
    // A reset closes both directions immediately.
    st->send_done = true;
    st->recv_end = true;
    if (!st->failed) {
      st->failed = true;
      st->error = absl::CancelledError("h2: reset locally");
      cv_.notify_all();
    }
  }
  return WriteFrame(kRstStream, 0, id, RstPayload(error_code));
}

absl::StatusOr<std::vector<HeaderField>> Connection::StreamRecvHeaders(uint32_t id, bool* end_of_stream) {
  SCOPED_LOCK(mu_);
  StreamState* st = FindStreamLocked(id);
  if (st == nullptr) {
    return absl::InternalError("h2: stream done");
  }
  bool ok = cv_.wait(mu_, [&]() { return st->headers_ready || st->failed || !conn_error_.ok(); });
  (void)ok;
  st = FindStreamLocked(id);
  if (st == nullptr) {
    return absl::Status(absl::StatusCode::kInternal, "h2: stream done");
  }
  // Delivered data wins over a connection error that raced in after the
  // block arrived (peers may close right after their last frame).
  if (st->headers_ready) {
    if (end_of_stream != nullptr) {
      *end_of_stream = st->headers_end_stream;
    }
    return st->headers;
  }
  if (st->failed) {
    return st->error;
  }
  return conn_error_;
}

absl::StatusOr<absl::string_view> Connection::StreamRecvData(uint32_t id, bool* end_of_stream) {
  SCOPED_LOCK(mu_);
  StreamState* st = FindStreamLocked(id);
  if (st == nullptr) {
    return absl::InternalError("h2: stream done");
  }
  bool ok = cv_.wait(mu_, [&]() { return !st->recv_buf.empty() || st->recv_end || st->failed || !conn_error_.ok(); });
  (void)ok;
  st = FindStreamLocked(id);
  if (st == nullptr) {
    return absl::Status(absl::StatusCode::kInternal, "h2: stream done");
  }
  if (!st->recv_buf.empty()) {
    st->recv_out.swap(st->recv_buf);
    st->recv_buf.clear();
    if (end_of_stream != nullptr) {
      *end_of_stream = st->recv_end;  // buffer now drained
    }
    return absl::string_view(st->recv_out);
  }
  if (st->recv_end) {
    // End of stream with an empty buffer.
    if (end_of_stream != nullptr) {
      *end_of_stream = true;
    }
    return absl::string_view();
  }
  if (st->failed) {
    return st->error;
  }
  return conn_error_;
  st->recv_out.swap(st->recv_buf);
  st->recv_buf.clear();
  if (end_of_stream != nullptr) {
    *end_of_stream = st->recv_end;  // buffer now drained
  }
  return absl::string_view(st->recv_out);
}

absl::StatusOr<std::vector<HeaderField>> Connection::StreamRecvTrailers(uint32_t id) {
  SCOPED_LOCK(mu_);
  StreamState* st = FindStreamLocked(id);
  if (st == nullptr) {
    return absl::InternalError("h2: stream done");
  }
  bool ok = cv_.wait(mu_, [&]() { return st->trailers_ready || st->recv_end || st->failed || !conn_error_.ok(); });
  (void)ok;
  st = FindStreamLocked(id);
  if (st == nullptr) {
    return absl::Status(absl::StatusCode::kInternal, "h2: stream done");
  }
  if (st->trailers_ready) {
    return st->trailers;
  }
  if (st->trailers_ready) {
    return st->trailers;
  }
  if (!st->failed && conn_error_.ok()) {
    // recv_end without another HEADERS block: requests legally end on DATA
    // END_STREAM, so callers that ask for trailers after that get an error.
    return absl::InternalError("h2: stream ended without trailers");
  }
  return st->failed ? st->error : conn_error_;
}

void Connection::StreamDone(uint32_t id) {
  SCOPED_LOCK(mu_);
  auto it = streams_.find(id);
  if (it == streams_.end()) {
    return;
  }
  // Reclaim when the peer cannot legally send us anything more (reset or
  // both directions closed); otherwise the state lives until the connection
  // ends so late frames for this stream can be accounted for.
  if (it->second->failed || (it->second->send_done && it->second->recv_end)) {
    open_streams_--;
    streams_.erase(it);
  }
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

void Connection::RunReader() {
  char header[kFrameHeaderSize];
  while (true) {
    absl::Status st = ReadExact(socket_, header, kFrameHeaderSize);
    if (!st.ok()) {
      break;
    }
    uint32_t length = (static_cast<uint32_t>(static_cast<unsigned char>(header[0])) << 16) |
                      (static_cast<uint32_t>(static_cast<unsigned char>(header[1])) << 8) |
                      static_cast<uint32_t>(static_cast<unsigned char>(header[2]));
    uint8_t type = static_cast<uint8_t>(header[3]);
    uint8_t flags = static_cast<uint8_t>(header[4]);
    uint32_t stream_id = GetBe32(header + 5) & 0x7FFFFFFF;
    if (length > options_.max_frame_size) {
      Abort(0x6 /* FRAME_SIZE_ERROR */, "frame above our SETTINGS_MAX_FRAME_SIZE");
      break;
    }
    std::string payload;
    if (length > 0) {
      payload.resize(length);
      st = ReadExact(socket_, payload.data(), length);
      if (!st.ok()) {
        break;
      }
    }
    ProcessFrame(type, flags, stream_id, std::move(payload));
    {
      SCOPED_LOCK(mu_);
      if (!conn_error_.ok()) {
        break;
      }
    }
  }
  {
    SCOPED_LOCK(mu_);
    peer_settings_received_ = true;
    cv_.notify_all();
    accept_cv_.notify_all();
    if (conn_error_.ok()) {
      conn_error_ = Unavailable("connection closed");
      FailAllStreamsLocked(is_client_ ? absl::UnavailableError("h2: connection closed")
                                      : absl::CancelledError("h2: connection closed"));
      cv_.notify_all();
      accept_cv_.notify_all();
    }
  }
}

void Connection::ProcessFrame(uint8_t type, uint8_t flags, uint32_t stream_id, std::string payload) {
  // In the middle of a header block: only CONTINUATION for that stream.
  if (cont_stream_id_ != 0) {
    if (type != kContinuation || stream_id != cont_stream_id_) {
      Abort(0x1 /* PROTOCOL_ERROR */, "expected CONTINUATION");
      return;
    }
    header_block_.append(payload);
    if (flags & kFlagEndHeaders) {
      uint32_t id = cont_stream_id_;
      cont_stream_id_ = 0;
      std::string block;
      block.swap(header_block_);
      header_block_.clear();
      DeliverHeaderBlock(id, block, header_end_stream_);
    }
    return;
  }

  switch (type) {
    case kData: {
      if (stream_id == 0) {
        Abort(0x1, "DATA on stream 0");
        return;
      }
      ProcessData(flags, stream_id, std::move(payload), flags & kFlagEndStream);
      return;
    }
    case kHeaders:
    case kContinuation: {
      if (stream_id == 0) {
        Abort(0x1, "HEADERS on stream 0");
        return;
      }
      if (type == kContinuation) {
        Abort(0x1, "unexpected CONTINUATION");
        return;
      }
      // Strip padding / priority per RFC 9113 8.1... gRPC peers do not use
      // them, but PADDED must be honored to keep parsing correct.
      absl::string_view block = payload;
      if (flags & kFlagPadded) {
        // RFC 9113 8.1: [pad-length][payload][padding], pad-length first.
        if (block.empty()) {
          Abort(0x1, "padded HEADERS without padding");
          return;
        }
        uint8_t pad = static_cast<uint8_t>(block.front());
        block.remove_prefix(1);
        if (pad > block.size()) {
          Abort(0x1, "padding larger than frame");
          return;
        }
        block.remove_suffix(pad);
      }
      if (flags & 0x20 /* PRIORITY */) {
        if (block.size() < 5) {
          Abort(0x1, "HEADERS priority field too short");
          return;
        }
        block.remove_prefix(5);
      }
      if (flags & kFlagEndHeaders) {
        DeliverHeaderBlock(stream_id, block, flags & kFlagEndStream);
      } else {
        cont_stream_id_ = stream_id;
        header_block_.assign(block.data(), block.size());
        header_end_stream_ = flags & kFlagEndStream;
      }
      return;
    }
    case kPriority:
      return;  // deprecated, ignore
    case kRstStream: {
      if (stream_id == 0 || payload.size() != 4) {
        Abort(0x1, "bad RST_STREAM");
        return;
      }
      uint32_t code = GetBe32(payload.data());
      SCOPED_LOCK(mu_);
      if (StreamState* st = FindStreamLocked(stream_id); st != nullptr && !st->failed) {
        FailStreamLocked(st, RstToStatus(code));
      }
      return;
    }
    case kSettings: {
      if (stream_id != 0) {
        Abort(0x1, "SETTINGS on nonzero stream");
        return;
      }
      if (flags & kFlagAck) {
        return;
      }
      if (payload.size() % 6 != 0) {
        Abort(0x1, "SETTINGS length not multiple of 6");
        return;
      }
      ProcessSettings(payload);
      return;
    }
    case kPushPromise: {
      Abort(0x1, "PUSH_PROMISE not allowed");
      return;
    }
    case kPing: {
      if (stream_id != 0 || payload.size() != 8) {
        Abort(0x1, "bad PING");
        return;
      }
      if ((flags & kFlagAck) == 0) {
        (void)WriteFrame(kPing, kFlagAck, 0, payload);  // R23: echo precisely
      }
      return;
    }
    case kGoaway: {
      if (stream_id != 0 || payload.size() < 8) {
        Abort(0x1, "bad GOAWAY");
        return;
      }
      // uint32_t last = GetBe32(payload.data()); code = GetBe32(+4)
      SCOPED_LOCK(mu_);
      going_away_ = true;  // R22: peer stops accepting new streams
      accept_cv_.notify_all();
      return;
    }
    case kWindowUpdate: {
      if (payload.size() != 4) {
        Abort(0x1, "bad WINDOW_UPDATE");
        return;
      }
      ProcessWindowUpdate(stream_id, GetBe32(payload.data()) & 0x7FFFFFFF);
      return;
    }
    default:
      return;  // H6: unknown frame types are ignored
  }
}

void Connection::DeliverHeaderBlock(uint32_t stream_id, absl::string_view block, bool end_stream) {
  std::vector<HeaderField> fields;
  absl::Status st = hpack_.Decode(block, &fields);
  if (!st.ok()) {
    Abort(0x9 /* COMPRESSION_ERROR */, "HPACK decode failed");
    return;
  }
  SCOPED_LOCK(mu_);
  StreamState* existing = FindStreamLocked(stream_id);
  const bool is_new_stream = existing == nullptr;
  if (is_new_stream) {
    if (!is_client_ && (stream_id & 1) && stream_id > last_accepted_stream_id_) {
      // New incoming server stream.
      if (open_streams_ >= options_.max_concurrent_streams) {
        // Peer above our limit: refuse so it can retry elsewhere
        // (R20 REFUSED_STREAM -> UNAVAILABLE).
        (void)WriteFrame(kRstStream, 0, stream_id, RstPayload(0x7 /* REFUSED_STREAM */));
        return;
      }
      auto state = std::make_unique<StreamState>();
      state->id = stream_id;
      state->headers = std::move(fields);
      state->headers_ready = true;
      state->headers_end_stream = end_stream;
      state->recv_end = end_stream;
      state->send_window = peer_initial_window_;
      streams_[stream_id] = std::move(state);
      open_streams_++;
      last_accepted_stream_id_ = stream_id;
      accept_queue_.push_back(stream_id);
      accept_cv_.notify_all();
      return;
    }
    // Unknown/stale stream id on our side.
    return;
  }
  if (existing->failed) {
    return;
  }
  if (!existing->headers_ready) {
    existing->headers = std::move(fields);
    existing->headers_ready = true;
    existing->headers_end_stream = end_stream;
  } else {
    existing->trailers = std::move(fields);
    existing->trailers_ready = true;
  }
  if (end_stream) {
    existing->recv_end = true;
  }
  cv_.notify_all();
}

void Connection::ProcessSettings(absl::string_view payload) {
  const char* abort_reason = nullptr;
  uint32_t abort_code = 0x1;
  {
    SCOPED_LOCK(mu_);
    peer_settings_received_ = true;
    cv_.notify_all();
    for (size_t i = 0; i + 6 <= payload.size(); i += 6) {
      uint16_t id = (static_cast<uint16_t>(static_cast<unsigned char>(payload[i])) << 8) |
                    static_cast<uint16_t>(static_cast<unsigned char>(payload[i + 1]));
      uint32_t value = GetBe32(payload.data() + i + 2);
      switch (id) {
        case kSettingInitialWindowSize:
          if (value > kMaxWindow) {
            abort_reason = "initial window above 2^31-1";
            abort_code = 0x3 /* FLOW_CONTROL_ERROR */;
          } else {
            // Delta applies to all open streams (RFC 9113 6.9.2).
            for (auto& [sid, st] : streams_) {
              st->send_window += static_cast<int64_t>(value) - static_cast<int64_t>(peer_initial_window_);
            }
            peer_initial_window_ = value;
          }
          break;
        case kSettingMaxFrameSize:
          if (value < 16384 || value > 16777215) {
            abort_reason = "bad SETTINGS_MAX_FRAME_SIZE";
          } else {
            peer_max_frame_size_ = value;
          }
          break;
        case kSettingMaxConcurrentStreams:
          peer_max_concurrent_ = value;
          break;
        default:
          break;  // header table/list sizes are enforced on decode already
      }
      if (abort_reason != nullptr) {
        break;
      }
    }
    cv_.notify_all();
    accept_cv_.notify_all();
  }
  if (abort_reason != nullptr) {
    Abort(abort_code, abort_reason);
    return;
  }
  (void)WriteFrame(kSettings, kFlagAck, 0, "");
}

void Connection::ProcessWindowUpdate(uint32_t stream_id, uint32_t increment) {
  if (increment == 0) {
    Abort(0x1, "WINDOW_UPDATE increment 0");
    return;
  }
  uint32_t flow_error_stream = 0;
  {
    SCOPED_LOCK(mu_);
    if (stream_id == 0) {
      conn_send_window_ += static_cast<int64_t>(increment);
      if (conn_send_window_ > static_cast<int64_t>(kMaxWindow)) {
        flow_error_stream = 0xFFFFFFFF;  // connection-level
      }
    } else if (StreamState* st = FindStreamLocked(stream_id); st != nullptr) {
      st->send_window += static_cast<int64_t>(increment);
      if (st->send_window > static_cast<int64_t>(kMaxWindow)) {
        flow_error_stream = stream_id;
      }
    }
    cv_.notify_all();
  }
  if (flow_error_stream == 0xFFFFFFFF) {
    Abort(0x3 /* FLOW_CONTROL_ERROR */, "connection window overflow");
  } else if (flow_error_stream != 0) {
    (void)WriteFrame(kRstStream, 0, stream_id, RstPayload(0x3 /* FLOW_CONTROL_ERROR */));
    SCOPED_LOCK(mu_);
    if (StreamState* st = FindStreamLocked(stream_id); st != nullptr) {
      FailStreamLocked(st, absl::InternalError("h2: flow control error"));
    }
  }
}

void Connection::ProcessData(uint8_t flags, uint32_t stream_id, std::string payload, bool end_stream) {
  absl::string_view data = payload;
  if (flags & kFlagPadded) {
    // RFC 9113 6.1: [pad-length][payload][padding], pad-length first.
    if (data.empty()) {
      Abort(0x1, "padded DATA without padding");
      return;
    }
    uint8_t pad = static_cast<uint8_t>(data.front());
    data.remove_prefix(1);
    if (pad > data.size()) {
      Abort(0x1, "DATA padding larger than frame");
      return;
    }
    data.remove_suffix(pad);
  }
  size_t bytes = data.size();
  uint32_t stream_update = 0;
  uint32_t conn_update = 0;
  {
    SCOPED_LOCK(mu_);
    if (StreamState* st = FindStreamLocked(stream_id); st != nullptr && !st->failed) {
      st->recv_buf.append(data.data(), data.size());
      if (end_stream) {
        st->recv_end = true;
      }
      st->recv_unannounced += static_cast<uint32_t>(bytes);
      if (st->recv_unannounced >= options_.initial_window_size / 2) {
        stream_update = st->recv_unannounced;
        st->recv_unannounced = 0;
      }
      cv_.notify_all();
    }
    // Connection-level debt accrues even for stale streams and must be
    // repaid or the peer stalls forever (RFC 9113 6.9).
    conn_recv_unannounced_ += static_cast<uint32_t>(bytes);
    if (conn_recv_unannounced_ >= options_.initial_window_size / 2) {
      conn_update = conn_recv_unannounced_;
      conn_recv_unannounced_ = 0;
    }
  }
  char inc[4];
  if (stream_update != 0) {
    PutBe32(inc, stream_update);
    (void)WriteFrame(kWindowUpdate, 0, stream_id, absl::string_view(inc, 4));
  }
  if (conn_update != 0) {
    PutBe32(inc, conn_update);
    (void)WriteFrame(kWindowUpdate, 0, 0, absl::string_view(inc, 4));
  }
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

absl::Status Connection::WriteFrame(uint8_t type, uint8_t flags, uint32_t stream_id, absl::string_view payload) {
  char header[kFrameHeaderSize];
  header[0] = static_cast<char>(payload.size() >> 16);
  header[1] = static_cast<char>(payload.size() >> 8);
  header[2] = static_cast<char>(payload.size());
  header[3] = static_cast<char>(type);
  header[4] = static_cast<char>(flags);
  PutBe32(header + 5, stream_id);
  SCOPED_LOCK(write_mu_);
  absl::Status st = WriteAll(socket_, header, kFrameHeaderSize);
  if (!st.ok()) {
    return st;
  }
  if (!payload.empty()) {
    return WriteAll(socket_, payload.data(), payload.size());
  }
  return absl::OkStatus();
}

absl::Status Connection::WriteHeaderBlock(uint32_t stream_id, absl::string_view block, bool end_stream) {
  size_t max_chunk = peer_max_frame_size_;
  size_t offset = 0;
  do {
    size_t len = std::min(max_chunk, block.size() - offset);
    bool last = offset + len == block.size();
    uint8_t type = offset == 0 ? kHeaders : kContinuation;
    uint8_t flags = 0;
    if (offset == 0 && end_stream) {
      flags |= kFlagEndStream;
    }
    if (last) {
      flags |= kFlagEndHeaders;
    }
    absl::Status st = WriteFrame(type, flags, stream_id, block.substr(offset, len));
    if (!st.ok()) {
      return st;
    }
    offset += len;
  } while (offset < block.size());
  return absl::OkStatus();
}

absl::Status Connection::SendSettings() {
  char payload[6 * 5];
  size_t n = 0;
  auto put = [&](uint16_t id, uint32_t value) {
    PutBe16(payload + n * 6, id);
    PutBe32(payload + n * 6 + 2, value);
    n++;
  };
  put(kSettingEnablePush, 0);
  put(kSettingMaxConcurrentStreams, options_.max_concurrent_streams);
  put(kSettingInitialWindowSize, options_.initial_window_size);
  put(kSettingMaxFrameSize, options_.max_frame_size);
  put(kSettingMaxHeaderListSize, options_.max_header_list_size);
  return WriteFrame(kSettings, 0, 0, absl::string_view(payload, n * 6));
}

void Connection::Abort(uint32_t goaway_error, absl::string_view debug_data) {
  {
    SCOPED_LOCK(mu_);
    if (aborted_) {
      return;
    }
    aborted_ = true;
    conn_error_ = absl::InternalError(absl::StrCat("h2: connection error: ", debug_data));
    FailAllStreamsLocked(is_client_ ? absl::UnavailableError("h2: connection error")
                                    : absl::CancelledError("h2: connection error"));
    cv_.notify_all();
    accept_cv_.notify_all();
  }
  char payload[8 + 128];
  PutBe32(payload, last_accepted_stream_id_);
  PutBe32(payload + 4, goaway_error);
  size_t len = 8 + std::min<size_t>(debug_data.size(), sizeof(payload) - 8);
  memcpy(payload + 8, debug_data.data(), len - 8);
  (void)WriteFrame(kGoaway, 0, 0, absl::string_view(payload, len));
  socket_->close();
}

}  // namespace grpc
}  // namespace mustang
