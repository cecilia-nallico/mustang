#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"

#include "mustang/grpc/connection.h"
#include "mustang/grpc/hpack.h"
#include "mustang/photon_headers.h"
#include "mustang/runtime.h"

namespace mustang {
namespace grpc {
namespace {

using photon::net::ISocketStream;

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

std::string FrameBytes(uint8_t type, uint8_t flags, uint32_t stream_id, absl::string_view payload) {
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

// Raw (hand-rolled) HTTP/2 peer for adversarial/inspection tests.
struct RawPeer {
  ISocketStream* sock = nullptr;

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
  // Server-side raw handshake: read preface + our SETTINGS, send SETTINGS.
  absl::Status ServerHandshake() {
    std::string preface;
    absl::Status st = ReadExact(&preface, sizeof(kPreface) - 1);
    if (!st.ok()) {
      return st;
    }
    absl::StatusOr<RawPeer::Frame> settings = ReadFrame();  // client SETTINGS
    if (!settings.ok()) {
      return settings.status();
    }
    return WriteFrame(kTypeSettings, 0, 0, "");
  }
};

class ConnectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    absl::StatusOr<Runtime> rt = Runtime::Create(RuntimeOptions{});
    ASSERT_TRUE(rt.ok());
    runtime_.emplace(std::move(rt).value());
  }
  void TearDown() override { runtime_.reset(); }

  std::optional<Runtime> runtime_;
};

// TCP loopback pair: photon server socket + dialed client socket.
struct Loopback {
  photon::net::ISocketServer* server = nullptr;
  photon::net::ISocketClient* client = nullptr;
  photon::net::EndPoint endpoint;

  absl::Status Start() {
    server = photon::net::new_tcp_socket_server();
    client = photon::net::new_tcp_socket_client();
    if (server == nullptr || client == nullptr) {
      return absl::InternalError("socket creation failed");
    }
    if (server->bind_v4any(0) != 0 || server->listen() != 0) {
      return absl::InternalError("listen failed");
    }
    endpoint = server->getsockname();
    return absl::OkStatus();
  }
  ISocketStream* Dial() { return client->connect(endpoint); }
  ISocketStream* Accept() { return server->accept(); }
};

HeaderField H(std::string name, std::string value) { return HeaderField{std::move(name), std::move(value)}; }

// --- happy-path loopback over two of our Connections ---

TEST_F(ConnectionTest, HandshakeEchoHeadersDataTrailers) {
  Loopback lb;
  ASSERT_TRUE(lb.Start().ok());

  absl::Status server_status = absl::OkStatus();
  std::vector<HeaderField> got_headers;
  std::string got_body;
  std::unique_ptr<Connection> server_conn;
  photon::thread* server_thread = photon::thread_create11([&]() {
    ISocketStream* sock = lb.Accept();
    if (sock == nullptr) {
      server_status = absl::InternalError("accept failed");
      return;
    }
    server_conn = std::make_unique<Connection>(sock, /*is_client=*/false, ConnectionOptions{}, /*ownership=*/true);
    Connection& conn = *server_conn;
    absl::Status st = conn.Start();
    if (!st.ok()) {
      server_status = st;
      return;
    }
    absl::StatusOr<Stream*> accepted = conn.AcceptStream();
    if (!accepted.ok()) {
      server_status = accepted.status();
      return;
    }
    Stream* stream = *accepted;
    absl::StatusOr<std::vector<HeaderField>> headers = stream->RecvHeaders();
    if (!headers.ok()) {
      server_status = headers.status();
      return;
    }
    got_headers = *headers;
    bool end = false;
    while (!end) {
      absl::StatusOr<absl::string_view> chunk = stream->RecvData(&end);
      if (!chunk.ok()) {
        server_status = chunk.status();
        return;
      }
      got_body.append(chunk->data(), chunk->size());
    }
    // gRPC requests end on DATA END_STREAM (no request trailers exist).
    st = stream->SendHeaders({H(":status", "200")}, false);
    st.Update(stream->SendData("pong", false));
    st.Update(stream->SendTrailers({H("grpc-status", "0")}));
    stream->Done();
    if (!st.ok()) {
      server_status = st;
    }
  });
  photon::thread_enable_join(server_thread, true);

  Connection client(lb.Dial(), /*is_client=*/true, ConnectionOptions{}, /*ownership=*/true);
  ASSERT_TRUE(client.Start().ok());
  absl::StatusOr<Stream*> opened = client.OpenStream();
  ASSERT_TRUE(opened.ok());
  Stream* stream = *opened;
  ASSERT_TRUE(stream->SendHeaders({H(":method", "POST"), H(":path", "/pkg.Svc/Do")}, false).ok());
  ASSERT_TRUE(stream->SendData("ping", true).ok());

  bool end = false;
  absl::StatusOr<std::vector<HeaderField>> resp_headers = stream->RecvHeaders(&end);
  ASSERT_TRUE(resp_headers.ok()) << resp_headers.status().message();
  ASSERT_FALSE(end);
  ASSERT_EQ(resp_headers->size(), 1u);
  EXPECT_EQ((*resp_headers)[0].name, ":status");
  std::string body;
  while (!end) {
    absl::StatusOr<absl::string_view> chunk = stream->RecvData(&end);
    ASSERT_TRUE(chunk.ok());
    body.append(chunk->data(), chunk->size());
  }
  EXPECT_EQ(body, "pong");
  absl::StatusOr<std::vector<HeaderField>> trailers = stream->RecvTrailers();
  ASSERT_TRUE(trailers.ok());
  ASSERT_EQ(trailers->size(), 1u);
  EXPECT_EQ((*trailers)[0].name, "grpc-status");
  stream->Done();

  photon::thread_join(photon::thread_enable_join(server_thread, true));
  ASSERT_TRUE(server_status.ok()) << server_status.message();
  ASSERT_EQ(got_headers.size(), 2u);
  EXPECT_EQ(got_headers[1].name, ":path");
  EXPECT_EQ(got_headers[1].value, "/pkg.Svc/Do");
  EXPECT_EQ(got_body, "ping");
}

TEST_F(ConnectionTest, LargeBidirectionalEcho) {
  Loopback lb;
  ASSERT_TRUE(lb.Start().ok());

  const std::string payload(1024 * 1024, 'x');  // 1 MiB, >> initial windows
  std::string echoed;
  absl::Status server_status = absl::OkStatus();
  std::unique_ptr<Connection> server_conn;
  photon::thread* server_thread = photon::thread_create11([&]() {
    ISocketStream* sock = lb.Accept();
    server_conn = std::make_unique<Connection>(sock, false, ConnectionOptions{}, true);
    Connection& conn = *server_conn;
    absl::Status st = conn.Start();
    if (!st.ok()) {
      server_status = st;
      return;
    }
    absl::StatusOr<Stream*> accepted = conn.AcceptStream();
    if (!accepted.ok()) {
      server_status = accepted.status();
      return;
    }
    Stream* stream = *accepted;
    st = stream->SendHeaders({H(":status", "200")}, false);
    bool end = false;
    while (!end) {
      absl::StatusOr<absl::string_view> chunk = stream->RecvData(&end);
      if (!chunk.ok()) {
        server_status = chunk.status();
        return;
      }
      st.Update(stream->SendData(*chunk, false));
    }
    st.Update(stream->SendTrailers({H("grpc-status", "0")}));
    stream->Done();
    if (!st.ok()) {
      server_status = st;
    }
  });
  photon::thread_enable_join(server_thread, true);

  Connection client(lb.Dial(), true, ConnectionOptions{}, true);
  ASSERT_TRUE(client.Start().ok());
  absl::StatusOr<Stream*> opened = client.OpenStream();
  ASSERT_TRUE(opened.ok());
  Stream* stream = *opened;
  ASSERT_TRUE(stream->SendHeaders({H(":method", "POST"), H(":path", "/Echo")}, false).ok());
  ASSERT_TRUE(stream->SendData(payload, true).ok());

  bool end = false;
  while (!end) {
    absl::StatusOr<absl::string_view> chunk = stream->RecvData(&end);
    ASSERT_TRUE(chunk.ok());
    echoed.append(chunk->data(), chunk->size());
  }
  stream->Done();
  photon::thread_join(photon::thread_enable_join(server_thread, true));
  ASSERT_TRUE(server_status.ok()) << server_status.message();
  EXPECT_EQ(echoed.size(), payload.size());
  EXPECT_EQ(memcmp(echoed.data(), payload.data(), payload.size()), 0);
}

TEST_F(ConnectionTest, ConcurrentStreamsAreDemultiplexed) {
  Loopback lb;
  ASSERT_TRUE(lb.Start().ok());
  constexpr int kStreams = 8;
  constexpr int kPayload = 64 * 1024;  // above default stream window

  absl::Status server_status = absl::OkStatus();
  std::unique_ptr<Connection> server_conn;
  photon::thread* server_thread = photon::thread_create11([&]() {
    ISocketStream* sock = lb.Accept();
    server_conn = std::make_unique<Connection>(sock, false, ConnectionOptions{}, true);
    Connection& conn = *server_conn;
    absl::Status st = conn.Start();
    if (!st.ok()) {
      server_status = st;
      return;
    }
    for (int i = 0; i < kStreams; i++) {
      absl::StatusOr<Stream*> accepted = conn.AcceptStream();
      if (!accepted.ok()) {
        server_status = accepted.status();
        return;
      }
      Stream* stream = *accepted;
      st.Update(stream->SendHeaders({H(":status", "200")}, false));
      // Echo the stream's body prefixed with its stream id.
      bool end = false;
      while (!end) {
        absl::StatusOr<absl::string_view> chunk = stream->RecvData(&end);
        if (!chunk.ok()) {
          server_status = chunk.status();
          return;
        }
        st.Update(stream->SendData(*chunk, false));
      }
      st.Update(stream->SendTrailers({H("grpc-status", "0")}));
      stream->Done();
    }
    if (!st.ok()) {
      server_status = st;
    }
  });
  photon::thread_enable_join(server_thread, true);

  Connection client(lb.Dial(), true, ConnectionOptions{}, true);
  ASSERT_TRUE(client.Start().ok());
  std::vector<Stream*> streams;
  for (int i = 0; i < kStreams; i++) {
    absl::StatusOr<Stream*> opened = client.OpenStream();
    ASSERT_TRUE(opened.ok());
    streams.push_back(*opened);
    ASSERT_TRUE((*opened)->SendHeaders({H(":method", "POST"), H(":path", "/Echo")}, false).ok());
    ASSERT_TRUE((*opened)->SendData(std::string(kPayload, 'a' + i), true).ok());
  }
  for (int i = 0; i < kStreams; i++) {
    bool end = false;
    std::string body;
    while (!end) {
      absl::StatusOr<absl::string_view> chunk = streams[i]->RecvData(&end);
      ASSERT_TRUE(chunk.ok());
      body.append(chunk->data(), chunk->size());
    }
    EXPECT_EQ(body.size(), static_cast<size_t>(kPayload));
    EXPECT_EQ(body[0], static_cast<char>('a' + i));
    streams[i]->Done();
  }
  photon::thread_join(photon::thread_enable_join(server_thread, true));
  ASSERT_TRUE(server_status.ok()) << server_status.message();
}

TEST_F(ConnectionTest, ResetCancelsPeerReceive) {
  Loopback lb;
  ASSERT_TRUE(lb.Start().ok());

  absl::Status server_status = absl::OkStatus();
  absl::Status recv_result = absl::OkStatus();
  std::unique_ptr<Connection> server_conn;
  photon::thread* server_thread = photon::thread_create11([&]() {
    ISocketStream* sock = lb.Accept();
    server_conn = std::make_unique<Connection>(sock, false, ConnectionOptions{}, true);
    Connection& conn = *server_conn;
    if (!conn.Start().ok()) {
      server_status = absl::InternalError("server start failed");
      return;
    }
    absl::StatusOr<Stream*> accepted = conn.AcceptStream();
    if (!accepted.ok()) {
      server_status = accepted.status();
      return;
    }
    Stream* stream = *accepted;
    if (!stream->RecvHeaders().ok()) {
      server_status = absl::InternalError("recv headers failed");
      return;
    }
    bool end = false;
    // "part" is buffered before the RST arrives: consume it, then the next
    // receive observes the cancellation.
    absl::StatusOr<absl::string_view> chunk = stream->RecvData(&end);
    if (!chunk.ok()) {
      recv_result = chunk.status();
    } else {
      recv_result = stream->RecvData(&end).status();
    }
    stream->Done();
  });
  photon::thread_enable_join(server_thread, true);

  Connection client(lb.Dial(), true, ConnectionOptions{}, true);
  ASSERT_TRUE(client.Start().ok());
  absl::StatusOr<Stream*> opened = client.OpenStream();
  ASSERT_TRUE(opened.ok());
  Stream* stream = *opened;
  ASSERT_TRUE(stream->SendHeaders({H(":method", "POST"), H(":path", "/Slow")}, false).ok());
  ASSERT_TRUE(stream->SendData("part", false).ok());
  ASSERT_TRUE(stream->Reset(0x8 /* CANCEL */).ok());
  stream->Done();

  photon::thread_join(photon::thread_enable_join(server_thread, true));
  ASSERT_TRUE(server_status.ok()) << server_status.message();
  ASSERT_EQ(recv_result.code(), absl::StatusCode::kCancelled);
}

TEST_F(ConnectionTest, TrailersOnlyResponse) {
  Loopback lb;
  ASSERT_TRUE(lb.Start().ok());

  absl::Status server_status = absl::OkStatus();
  std::unique_ptr<Connection> server_conn;
  photon::thread* server_thread = photon::thread_create11([&]() {
    ISocketStream* sock = lb.Accept();
    server_conn = std::make_unique<Connection>(sock, false, ConnectionOptions{}, true);
    Connection& conn = *server_conn;
    absl::Status st = conn.Start();
    if (!st.ok()) {
      server_status = st;
      return;
    }
    absl::StatusOr<Stream*> accepted = conn.AcceptStream();
    if (!accepted.ok()) {
      server_status = accepted.status();
      return;
    }
    Stream* stream = *accepted;
    if (!stream->RecvHeaders().ok()) {
      server_status = absl::InternalError("recv headers failed");
      return;
    }
    bool end = false;
    (void)stream->RecvData(&end);  // drain request
    // Trailers-only: single HEADERS block with END_STREAM.
    st = stream->SendHeaders({H(":status", "200"), H("grpc-status", "12")}, true);
    stream->Done();
    if (!st.ok()) {
      server_status = st;
    }
  });
  photon::thread_enable_join(server_thread, true);

  Connection client(lb.Dial(), true, ConnectionOptions{}, true);
  ASSERT_TRUE(client.Start().ok());
  absl::StatusOr<Stream*> opened = client.OpenStream();
  ASSERT_TRUE(opened.ok());
  Stream* stream = *opened;
  ASSERT_TRUE(stream->SendHeaders({H(":method", "POST"), H(":path", "/X")}, false).ok());
  ASSERT_TRUE(stream->SendData("", true).ok());
  bool end = false;
  absl::StatusOr<std::vector<HeaderField>> headers = stream->RecvHeaders(&end);
  ASSERT_TRUE(headers.ok());
  ASSERT_TRUE(end);
  ASSERT_EQ(headers->size(), 2u);
  stream->Done();

  photon::thread_join(photon::thread_enable_join(server_thread, true));
  ASSERT_TRUE(server_status.ok()) << server_status.message();
}

// --- adversarial/inspection cases against a raw peer ---

TEST_F(ConnectionTest, GoawayAfterShutdownBlocksNewStreams) {
  Loopback lb;
  ASSERT_TRUE(lb.Start().ok());
  absl::Status server_status = absl::OkStatus();
  photon::thread* server_thread = photon::thread_create11([&]() {
    ISocketStream* sock = lb.Accept();
    Connection conn(sock, false, ConnectionOptions{}, true);
    if (!conn.Start().ok()) {
      server_status = absl::InternalError("server start failed");
      return;
    }
    // No stream accepted: just tell the client we are going away.
    absl::Status st = conn.Shutdown("bye");
    if (!st.ok()) {
      server_status = st;
    }
  });
  photon::thread_enable_join(server_thread, true);

  Connection client(lb.Dial(), true, ConnectionOptions{}, true);
  ASSERT_TRUE(client.Start().ok());
  while (!client.going_away()) {
    photon::thread_usleep(1000);
  }
  absl::StatusOr<Stream*> opened = client.OpenStream();
  ASSERT_FALSE(opened.ok());
  EXPECT_EQ(opened.status().code(), absl::StatusCode::kUnavailable);
  photon::thread_join(photon::thread_enable_join(server_thread, true));
  ASSERT_TRUE(server_status.ok()) << server_status.message();
}

TEST_F(ConnectionTest, PingIsEchoed) {
  Loopback lb;
  ASSERT_TRUE(lb.Start().ok());
  absl::Status peer_status = absl::OkStatus();
  photon::thread* peer_thread = photon::thread_create11([&]() {
    RawPeer peer{lb.Accept()};
    absl::Status st = peer.ServerHandshake();
    if (!st.ok()) {
      peer_status = st;
      return;
    }
    st = peer.WriteFrame(kTypePing, 0, 0, "12345678");
    if (!st.ok()) {
      peer_status = st;
      return;
    }
    // Skip the client's SETTINGS; expect a PING ACK with the same bytes.
    while (true) {
      absl::StatusOr<RawPeer::Frame> frame = peer.ReadFrame();
      if (!frame.ok()) {
        peer_status = frame.status();
        return;
      }
      if (frame->type == kTypePing && (frame->flags & kFlagAck) != 0) {
        if (frame->payload != std::string("12345678")) {
          peer_status = absl::InternalError("ping payload mismatch");
        }
        return;
      }
    }
  });
  photon::thread_enable_join(peer_thread, true);

  Connection client(lb.Dial(), true, ConnectionOptions{}, true);
  ASSERT_TRUE(client.Start().ok());
  photon::thread_join(photon::thread_enable_join(peer_thread, true));
  ASSERT_TRUE(peer_status.ok()) << peer_status.message();
}

TEST_F(ConnectionTest, ContinuationFramesAreReassembled) {
  Loopback lb;
  ASSERT_TRUE(lb.Start().ok());
  absl::Status peer_status = absl::OkStatus();
  photon::thread* peer_thread = photon::thread_create11([&]() {
    RawPeer peer{lb.Accept()};
    absl::Status st = peer.ServerHandshake();
    if (!st.ok()) {
      peer_status = st;
      return;
    }
    absl::StatusOr<std::string> block = HpackEncode({H(":status", "200"), H("x-big", std::string(200, 'z'))});
    if (!block.ok()) {
      peer_status = block.status();
      return;
    }
    // Split anywhere: first byte in HEADERS, rest in CONTINUATION.
    st = peer.WriteFrame(kTypeHeaders, 0, 1, block->substr(0, 1));
    st.Update(peer.WriteFrame(kTypeContinuation, kFlagEndHeaders, 1, block->substr(1)));
    if (!st.ok()) {
      peer_status = st;
    }
  });
  photon::thread_enable_join(peer_thread, true);

  Connection client(lb.Dial(), true, ConnectionOptions{}, true);
  ASSERT_TRUE(client.Start().ok());
  absl::StatusOr<Stream*> opened = client.OpenStream();
  ASSERT_TRUE(opened.ok());
  Stream* stream = *opened;
  ASSERT_TRUE(stream->SendHeaders({H(":method", "POST"), H(":path", "/C")}, false).ok());
  ASSERT_TRUE(stream->SendData("", true).ok());
  absl::StatusOr<std::vector<HeaderField>> headers = stream->RecvHeaders();
  ASSERT_TRUE(headers.ok());
  ASSERT_EQ(headers->size(), 2u);
  EXPECT_EQ((*headers)[1].name, "x-big");
  EXPECT_EQ((*headers)[1].value.size(), 200u);
  stream->Done();
  photon::thread_join(photon::thread_enable_join(peer_thread, true));
  ASSERT_TRUE(peer_status.ok()) << peer_status.message();
}

TEST_F(ConnectionTest, OversizeFrameGetsGoawayFrameSizeError) {
  Loopback lb;
  ASSERT_TRUE(lb.Start().ok());
  absl::Status peer_status = absl::OkStatus();
  photon::thread* peer_thread = photon::thread_create11([&]() {
    RawPeer peer{lb.Accept()};
    absl::Status st = peer.ServerHandshake();
    if (!st.ok()) {
      peer_status = st;
      return;
    }
    // Announce a DATA frame of 20 KiB, above the 16 KiB default max.
    std::string header(9, '\0');
    header[0] = static_cast<char>(20000 >> 16);
    header[1] = static_cast<char>(20000 >> 8);
    header[3] = static_cast<char>(kTypeData);
    st = peer.Write(header);
    if (!st.ok()) {
      peer_status = st;
      return;
    }
    while (true) {
      absl::StatusOr<RawPeer::Frame> frame = peer.ReadFrame();
      if (!frame.ok()) {
        peer_status = frame.status();
        return;
      }
      if (frame->type == kTypeGoaway) {
        uint32_t code = (static_cast<uint32_t>(static_cast<unsigned char>(frame->payload[4])) << 24) |
                        (static_cast<uint32_t>(static_cast<unsigned char>(frame->payload[5])) << 16) |
                        (static_cast<uint32_t>(static_cast<unsigned char>(frame->payload[6])) << 8) |
                        static_cast<uint32_t>(static_cast<unsigned char>(frame->payload[7]));
        if (code != 0x6 /* FRAME_SIZE_ERROR */) {
          peer_status = absl::InternalError(absl::StrCat("wrong goaway code ", code));
        }
        return;
      }
    }
  });
  photon::thread_enable_join(peer_thread, true);

  Connection client(lb.Dial(), true, ConnectionOptions{}, true);
  ASSERT_TRUE(client.Start().ok());
  photon::thread_join(photon::thread_enable_join(peer_thread, true));
  ASSERT_TRUE(peer_status.ok()) << peer_status.message();
}

TEST_F(ConnectionTest, PaddedDataIsStripped) {
  Loopback lb;
  ASSERT_TRUE(lb.Start().ok());
  absl::Status peer_status = absl::OkStatus();
  photon::thread* peer_thread = photon::thread_create11([&]() {
    RawPeer peer{lb.Accept()};
    absl::Status st = peer.ServerHandshake();
    if (!st.ok()) {
      peer_status = st;
      return;
    }
    absl::StatusOr<std::string> block = HpackEncode({H(":status", "200")});
    if (!block.ok()) {
      peer_status = block.status();
      return;
    }
    st = peer.WriteFrame(kTypeHeaders, kFlagEndHeaders, 1, *block);
    // DATA with 4 bytes of padding: pad-length byte + payload + 3 zeros.
    std::string payload(1, '\x03');
    payload += "data";
    payload += std::string(3, '\0');
    st.Update(peer.WriteFrame(kTypeData, kFlagEndStream | 0x8 /* PADDED */, 1, payload));
    if (!st.ok()) {
      peer_status = st;
    }
  });
  photon::thread_enable_join(peer_thread, true);

  Connection client(lb.Dial(), true, ConnectionOptions{}, true);
  ASSERT_TRUE(client.Start().ok());
  absl::StatusOr<Stream*> opened = client.OpenStream();
  ASSERT_TRUE(opened.ok());
  Stream* stream = *opened;
  ASSERT_TRUE(stream->SendHeaders({H(":method", "POST"), H(":path", "/P")}, false).ok());
  ASSERT_TRUE(stream->SendData("", true).ok());
  ASSERT_TRUE(stream->RecvHeaders().ok());
  bool end = false;
  absl::StatusOr<absl::string_view> chunk = stream->RecvData(&end);
  ASSERT_TRUE(chunk.ok());
  EXPECT_EQ(*chunk, "data");
  EXPECT_TRUE(end);
  stream->Done();
  photon::thread_join(photon::thread_enable_join(peer_thread, true));
  ASSERT_TRUE(peer_status.ok()) << peer_status.message();
}

TEST_F(ConnectionTest, SendRespectsPeerWindowsAndFrameSize) {
  Loopback lb;
  ASSERT_TRUE(lb.Start().ok());
  absl::Status peer_status = absl::OkStatus();
  photon::thread* peer_thread = photon::thread_create11([&]() {
    RawPeer peer{lb.Accept()};
    // Handshake manually: read preface, read client SETTINGS, then reply
    // with tiny windows so the client must wait for WINDOW_UPDATEs.
    std::string preface;
    absl::Status st = peer.ReadExact(&preface, sizeof(kPreface) - 1);
    if (!st.ok()) {
      peer_status = st;
      return;
    }
    absl::StatusOr<RawPeer::Frame> client_settings = peer.ReadFrame();
    if (!client_settings.ok()) {
      peer_status = client_settings.status();
      return;
    }
    // SETTINGS: initial window 100 for stream 1.
    std::string settings(6, '\0');
    settings[1] = '\x04';
    settings[5] = '\x64';  // 100
    st = peer.WriteFrame(kTypeSettings, 0, 0, settings);
    if (!st.ok()) {
      peer_status = st;
      return;
    }

    size_t received = 0;
    uint32_t frames = 0;
    bool expect_end = false;
    while (!expect_end || received < 4096) {
      absl::StatusOr<RawPeer::Frame> frame = peer.ReadFrame();
      if (!frame.ok()) {
        peer_status = frame.status();
        return;
      }
      if (frame->type == kTypeData) {
        if (frame->payload.size() > 100 + 0) {
          // Frames can carry at most what the windows allowed.
          peer_status = absl::InternalError("frame above granted window");
          return;
        }
        received += frame->payload.size();
        frames++;
        expect_end = (frame->flags & kFlagEndStream) != 0;
        // Grant another 100 bytes on stream 1 and the connection.
        std::string update(4, '\0');
        update[3] = '\x64';
        st = peer.WriteFrame(kTypeWindowUpdate, 0, 0, update);
        st.Update(peer.WriteFrame(kTypeWindowUpdate, 0, 1, update));
        if (!st.ok()) {
          peer_status = st;
          return;
        }
      }
    }
    if (received != 4096 || frames < 40) {
      peer_status =
          absl::InternalError(absl::StrCat("flow control broken: ", received, " bytes in ", frames, " frames"));
    }
  });
  photon::thread_enable_join(peer_thread, true);

  Connection client(lb.Dial(), true, ConnectionOptions{}, true);
  ASSERT_TRUE(client.Start().ok());
  ASSERT_TRUE(client.WaitForPeerSettings().ok());
  absl::StatusOr<Stream*> opened = client.OpenStream();
  ASSERT_TRUE(opened.ok());
  Stream* stream = *opened;
  ASSERT_TRUE(stream->SendHeaders({H(":method", "POST"), H(":path", "/W")}, false).ok());
  // 4 KiB with a 100-byte initial window and 100-byte grants: many rounds.
  ASSERT_TRUE(stream->SendData(std::string(4096, 'w'), true).ok());
  stream->Done();
  photon::thread_join(photon::thread_enable_join(peer_thread, true));
  ASSERT_TRUE(peer_status.ok()) << peer_status.message();
}

}  // namespace
}  // namespace grpc
}  // namespace mustang
