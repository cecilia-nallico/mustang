#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

#include "walleye/grpc/hpack.h"
#include "walleye/photon_headers.h"

namespace walleye {
namespace grpc {

// Values we advertise in SETTINGS. The peer's advertised values are tracked
// separately and constrain what we send.
struct ConnectionOptions {
  uint32_t max_frame_size = 16384;
  uint32_t initial_window_size = 65535;
  uint32_t max_header_list_size = 8192;
  uint32_t max_header_table_size = 4096;
  uint32_t max_concurrent_streams = 128;
};

class Connection;

// Handle to one HTTP/2 stream. All operations block the calling photon
// coroutine. The handle is valid until Done() or Connection destruction.
class Stream {
 public:
  Stream(Connection* conn, uint32_t id) : conn_(conn), id_(id) {}

  uint32_t id() const { return id_; }
  explicit operator bool() const { return conn_ != nullptr; }

  // --- send side ---
  absl::Status SendHeaders(std::vector<HeaderField> headers, bool end_stream);
  absl::Status SendData(absl::string_view data, bool end_stream);
  // HEADERS frame with END_STREAM (the Trailers part of a gRPC response).
  absl::Status SendTrailers(std::vector<HeaderField> trailers);
  absl::Status Reset(uint32_t error_code);

  // --- receive side ---
  // First HEADERS block; *end_of_stream is true for a trailers-only
  // exchange (no body at all).
  absl::StatusOr<std::vector<HeaderField>> RecvHeaders(bool* end_of_stream = nullptr);
  // Blocks until at least one byte is available (or end of stream). The
  // returned view stays valid until the next Stream call. *end_of_stream is
  // true when this chunk exhausts the receive side.
  absl::StatusOr<absl::string_view> RecvData(bool* end_of_stream);
  // Trailers block after RecvData reported end of stream.
  absl::StatusOr<std::vector<HeaderField>> RecvTrailers();

  // Releases the stream's connection-side state; no other call afterwards.
  void Done();

 private:
  friend class Connection;
  Connection* conn_;
  uint32_t id_;
};

// A demultiplexed HTTP/2 connection (h2c, prior knowledge) over a photon
// socket stream: one reader coroutine dispatches every frame; per-stream
// blocking operations; both-level flow control; CONTINUATION reassembly;
// SETTINGS bookkeeping; PING auto-reply; GOAWAY tracking.
class Connection {
 public:
  Connection(photon::net::ISocketStream* socket,
             bool is_client,
             const ConnectionOptions& options = ConnectionOptions(),
             bool ownership = true);
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  // h2c preface + SETTINGS exchange, then starts the reader coroutine.
  absl::Status Start();

  // Client side: new stream with the next odd id (waits while at the peer's
  // MAX_CONCURRENT_STREAMS). Server side: next incoming stream.
  absl::StatusOr<Stream*> OpenStream();
  absl::StatusOr<Stream*> AcceptStream();

  // Graceful: GOAWAY with the highest stream id we accepted, no error.
  // Existing streams keep running; the socket closes when the Connection is
  // destroyed (or the peer goes away).
  absl::Status Shutdown(absl::string_view debug_data = {});

  bool going_away() const;
  uint32_t last_accepted_stream_id() const;

  // Blocks until the peer's SETTINGS frame arrived (initial windows and
  // max frame size applied). Callers that must not overshoot a smaller
  // peer window before it is known can synchronize with this.
  absl::Status WaitForPeerSettings();

 private:
  struct StreamState;
  friend class Stream;

  // Stream ops (invoked by Stream).
  absl::Status StreamSendHeaders(uint32_t id, std::vector<HeaderField> headers, bool end_stream);
  absl::Status StreamSendData(uint32_t id, absl::string_view data, bool end_stream);
  absl::Status StreamSendTrailers(uint32_t id, std::vector<HeaderField> trailers);
  absl::Status StreamReset(uint32_t id, uint32_t error_code);
  absl::StatusOr<std::vector<HeaderField>> StreamRecvHeaders(uint32_t id, bool* end_of_stream);
  absl::StatusOr<absl::string_view> StreamRecvData(uint32_t id, bool* end_of_stream);
  absl::StatusOr<std::vector<HeaderField>> StreamRecvTrailers(uint32_t id);
  void StreamDone(uint32_t id);

  // Reader.
  void RunReader();
  void ProcessFrame(uint8_t type, uint8_t flags, uint32_t stream_id, std::string payload);
  void DeliverHeaderBlock(uint32_t stream_id, absl::string_view block, bool end_stream);
  void ProcessSettings(absl::string_view payload);
  void ProcessWindowUpdate(uint32_t stream_id, uint32_t increment);
  void ProcessData(uint8_t flags, uint32_t stream_id, std::string payload, bool end_stream);

  // Writing.
  absl::Status WriteFrame(uint8_t type, uint8_t flags, uint32_t stream_id, absl::string_view payload);
  absl::Status WriteHeaderBlock(uint32_t stream_id, absl::string_view block, bool end_stream);
  absl::Status SendSettings();

  // Stream bookkeeping (mu_ held by callers where noted).
  StreamState* FindStreamLocked(uint32_t id);
  void FailStreamLocked(StreamState* st, absl::Status status);
  void FailAllStreamsLocked(const absl::Status& status);
  void Abort(uint32_t goaway_error, absl::string_view debug_data);

  photon::net::ISocketStream* socket_;
  bool ownership_;
  const bool is_client_;
  const ConnectionOptions options_;

  // Guards everything below except socket writes.
  photon::mutex mu_;
  photon::condition_variable cv_;         // stream events + send windows
  photon::condition_variable accept_cv_;  // new streams + open slots
  photon::mutex write_mu_;                // serializes socket writes

  HpackDecoder hpack_;  // per-connection decode state
  std::map<uint32_t, std::unique_ptr<StreamState>> streams_;
  std::deque<uint32_t> accept_queue_;
  uint32_t next_stream_id_ = 1;  // ours: odd on client, even on server
  uint32_t peer_initial_window_ = 65535;
  uint32_t peer_max_frame_size_ = 16384;
  uint32_t peer_max_concurrent_ = 100;  // 0 means unlimited
  int64_t conn_send_window_ = 65535;
  uint32_t conn_recv_unannounced_ = 0;
  uint32_t open_streams_ = 0;
  bool going_away_ = false;
  uint32_t last_accepted_stream_id_ = 0;
  absl::Status conn_error_;
  photon::thread* reader_ = nullptr;
  photon::join_handle* reader_join_ = nullptr;
  bool started_ = false;
  bool aborted_ = false;
  bool peer_settings_received_ = false;
  // CONTINUATION reassembly: header block in progress.
  uint32_t cont_stream_id_ = 0;
  std::string header_block_;
  bool header_end_stream_ = false;
};

}  // namespace grpc
}  // namespace walleye
