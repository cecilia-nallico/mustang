#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "gtest/gtest.h"

#include "walleye/grpc/call.h"
#include "walleye/grpc/connection.h"
#include "walleye/grpc/hpack.h"
#include "walleye/grpc/server.h"
#include "walleye/grpc/test_util.h"
#include "walleye/grpc/wire.h"
#include "walleye/photon_headers.h"
#include "walleye/runtime.h"

namespace walleye {
namespace grpc {
namespace {

HeaderField H(std::string name, std::string value) { return HeaderField{std::move(name), std::move(value)}; }

class GrpcTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto rt = Runtime::Create(RuntimeOptions{});
    ASSERT_TRUE(rt.ok());
    runtime_.emplace(std::move(rt).value());
  }
  void TearDown() override {
    // Everything that touches photon must be gone before the Runtime
    // tears the photon runtime down.
    server_.Stop();
    runtime_.reset();
  }

  std::optional<Runtime> runtime_;
  GrpcServer server_;
};

class ConnectedGrpcTest : public GrpcTest {
 protected:
  void TearDown() override {
    client_.reset();  // before the Runtime goes down (see base TearDown)
    GrpcTest::TearDown();
  }

  void ConnectClient() {
    client_ = std::make_unique<GrpcClient>(server_.endpoint());
    ASSERT_TRUE(client_->Connect().ok());
  }
  std::unique_ptr<GrpcClient> client_;
};

// --- unary & streaming shapes ---

TEST_F(ConnectedGrpcTest, UnaryEcho) {
  server_.Register("/test.Echo/Unary", [](ServerCall* call) {
    auto message = call->ReadMessage();
    ASSERT_TRUE(message.ok());
    ASSERT_TRUE(message->has_value());
    (void)call->WriteMessage("echo:" + **message);
    (void)call->Finish(StatusCode::kOk);
  });
  ASSERT_TRUE(server_.Start(0).ok());
  ConnectClient();

  auto call = client_->Call("/test.Echo/Unary");
  ASSERT_TRUE(call.ok());
  ASSERT_TRUE(call->WriteMessage("hello").ok());
  auto reply = call->ReadMessage();
  ASSERT_TRUE(reply.ok());
  ASSERT_TRUE(reply->has_value());
  EXPECT_EQ(**reply, "echo:hello");
  auto result = call->Finish();
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result->code, StatusCode::kOk) << "message=" << result->message;
}

TEST_F(ConnectedGrpcTest, ServerStreaming) {
  server_.Register("/test.Echo/ServerStream", [](ServerCall* call) {
    (void)call->ReadMessage();  // single request
    for (int i = 0; i < 3; i++) {
      (void)call->WriteMessage(absl::StrCat("m", i));
      photon::thread_usleep(2000);
    }
    (void)call->Finish(StatusCode::kOk);
  });
  ASSERT_TRUE(server_.Start(0).ok());
  ConnectClient();

  auto call = client_->Call("/test.Echo/ServerStream");
  ASSERT_TRUE(call.ok());
  ASSERT_TRUE(call->WriteMessage("go").ok());
  for (int i = 0; i < 3; i++) {
    auto message = call->ReadMessage();
    ASSERT_TRUE(message.ok());
    ASSERT_TRUE(message->has_value());
    EXPECT_EQ(**message, absl::StrCat("m", i));
  }
  auto end = call->ReadMessage();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());  // server half-closed
  auto result = call->Finish();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->code, StatusCode::kOk);
}

TEST_F(ConnectedGrpcTest, ClientStreamingAggregates) {
  server_.Register("/test.Echo/ClientStream", [](ServerCall* call) {
    std::string joined;
    while (true) {
      auto message = call->ReadMessage();
      ASSERT_TRUE(message.ok());
      if (!message->has_value()) {
        break;
      }
      joined += **message;
    }
    (void)call->WriteMessage(joined);
    (void)call->Finish(StatusCode::kOk);
  });
  ASSERT_TRUE(server_.Start(0).ok());
  ConnectClient();

  auto call = client_->Call("/test.Echo/ClientStream");
  ASSERT_TRUE(call.ok());
  for (const char* part : {"a", "b", "c"}) {
    ASSERT_TRUE(call->WriteMessage(part).ok());
  }
  ASSERT_TRUE(call->CloseSend().ok());  // half-close before reading the reply
  auto reply = call->ReadMessage();
  ASSERT_TRUE(reply.ok());
  ASSERT_TRUE(reply->has_value());
  EXPECT_EQ(**reply, "abc");
  auto result = call->Finish();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->code, StatusCode::kOk);
}

TEST_F(ConnectedGrpcTest, BidiEchoInterleaved) {
  server_.Register("/test.Echo/Bidi", [](ServerCall* call) {
    while (true) {
      auto message = call->ReadMessage();
      ASSERT_TRUE(message.ok());
      if (!message->has_value()) {
        break;
      }
      ASSERT_TRUE(call->WriteMessage(">" + **message).ok());
    }
    (void)call->Finish(StatusCode::kOk);
  });
  ASSERT_TRUE(server_.Start(0).ok());
  ConnectClient();

  auto call = client_->Call("/test.Echo/Bidi");
  ASSERT_TRUE(call.ok());
  for (int i = 0; i < 5; i++) {
    ASSERT_TRUE(call->WriteMessage(absl::StrCat("x", i)).ok());
    auto reply = call->ReadMessage();
    ASSERT_TRUE(reply.ok());
    ASSERT_TRUE(reply->has_value());
    EXPECT_EQ(**reply, absl::StrCat(">x", i));
  }
  auto result = call->Finish();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->code, StatusCode::kOk);
}

// --- status & metadata ---

TEST_F(ConnectedGrpcTest, ErrorStatusRoundtripsWithPercentEncoding) {
  server_.Register("/test.Echo/Fail",
                   [](ServerCall* call) { (void)call->Finish(StatusCode::kNotFound, "café % 100\n"); });
  ASSERT_TRUE(server_.Start(0).ok());
  ConnectClient();

  auto call = client_->Call("/test.Echo/Fail");
  ASSERT_TRUE(call.ok());
  auto result = call->Finish();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->code, StatusCode::kNotFound);
  EXPECT_EQ(result->message, "café % 100\n");  // R16 percent-encode/decode
}

TEST_F(ConnectedGrpcTest, UnknownMethodIsUnimplemented) {
  ASSERT_TRUE(server_.Start(0).ok());
  ConnectClient();

  auto call = client_->Call("/test.Nope/Missing");
  ASSERT_TRUE(call.ok());
  auto result = call->Finish();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->code, StatusCode::kUnimplemented);
}

TEST_F(ConnectedGrpcTest, MetadataOrderAndDuplicatesSurvive) {
  std::vector<HeaderField> seen;
  std::string seen_path;
  server_.Register("/test.Echo/Meta", [&](ServerCall* call) {
    seen = call->request_metadata();
    for (const HeaderField& field : call->request_metadata()) {
      if (field.name == ":path") {
        seen_path = field.value;
      }
    }
    (void)call->Finish(StatusCode::kOk, "", {H("resp-bin", "AAAA"), H("resp", "v2")});
  });
  ASSERT_TRUE(server_.Start(0).ok());
  ConnectClient();

  auto call = client_->Call("/test.Echo/Meta", {H("k", "v1"), H("k", "v2"), H("trace", "abc")});
  ASSERT_TRUE(call.ok());
  auto result = call->Finish();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->code, StatusCode::kOk);
  EXPECT_EQ(seen_path, "/test.Echo/Meta");
  // R5: order and duplicates preserved.
  ASSERT_GE(seen.size(), 6u);
  int k_count = 0;
  bool in_order = true;
  std::string first_k;
  for (const HeaderField& field : seen) {
    if (field.name == "k") {
      if (k_count == 0) {
        first_k = field.value;
      } else {
        in_order = in_order && field.value == "v2";
      }
      k_count++;
    }
  }
  EXPECT_EQ(k_count, 2);
  EXPECT_TRUE(in_order);
  EXPECT_EQ(first_k, "v1");
  // Response metadata comes back through the trailers.
  ASSERT_EQ(result->metadata.size(), 2u);
  EXPECT_EQ(result->metadata[0].name, "resp-bin");
  EXPECT_EQ(result->metadata[1].value, "v2");
}

TEST_F(ConnectedGrpcTest, OversizeMessageIsResourceExhausted) {
  GrpcServerOptions options;
  options.max_message_size = 100;
  GrpcServer small_server(options);
  small_server.Register("/test.Echo/Small", [](ServerCall* call) {
    (void)call->ReadMessage();
    (void)call->Finish(StatusCode::kOk);
  });
  ASSERT_TRUE(small_server.Start(0).ok());
  GrpcClient client(small_server.endpoint());
  ASSERT_TRUE(client.Connect().ok());

  auto call = client.Call("/test.Echo/Small");
  ASSERT_TRUE(call.ok());
  ASSERT_TRUE(call->WriteMessage(std::string(200, 'x')).ok());
  auto result = call->Finish();
  ASSERT_TRUE(result.ok());
  // The oversized request never reaches the handler: the server replies
  // RESOURCE_EXHAUSTED via the dispatcher fallback path.
  EXPECT_EQ(result->code, StatusCode::kResourceExhausted);
}

// --- deadlines & cancellation (R4, R20) ---

TEST_F(ConnectedGrpcTest, DeadlineExceededWhenServerTooSlow) {
  server_.Register("/test.Echo/Slow", [](ServerCall* call) {
    photon::thread_usleep(300 * 1000);
    (void)call->WriteMessage("late");
    (void)call->Finish(StatusCode::kOk);
  });
  ASSERT_TRUE(server_.Start(0).ok());
  ConnectClient();

  auto call = client_->Call("/test.Echo/Slow", {}, absl::Milliseconds(50));
  ASSERT_TRUE(call.ok());
  auto reply = call->ReadMessage();
  ASSERT_FALSE(reply.ok());
  EXPECT_EQ(reply.status().code(), absl::StatusCode::kDeadlineExceeded);
}

TEST_F(ConnectedGrpcTest, ServerObservesClientCancel) {
  absl::Status handler_error = absl::OkStatus();
  server_.Register("/test.Echo/Stuck", [&](ServerCall* call) {
    photon::thread_usleep(100 * 1000);
    auto message = call->ReadMessage();  // client reset in the meantime
    handler_error = message.status();
    (void)call->Finish(StatusCode::kOk);
  });
  ASSERT_TRUE(server_.Start(0).ok());
  ConnectClient();

  auto call = client_->Call("/test.Echo/Stuck");
  ASSERT_TRUE(call.ok());
  call->Cancel();
  // The handler sees the cancellation through the transport (R20 CANCEL).
  absl::Time limit = absl::Now() + absl::Seconds(5);
  while (handler_error.ok() && absl::Now() < limit) {
    photon::thread_usleep(5000);
  }
  EXPECT_EQ(handler_error.code(), absl::StatusCode::kCancelled);
}

// --- wire-adjacent behaviors via a raw peer ---

TEST_F(GrpcTest, NonGrpcContentTypeGetsHttp415) {
  absl::Status peer_status = absl::OkStatus();
  int observed_status = 0;
  photon::thread* peer_thread = photon::thread_create11([&]() {
    // Connect a raw socket to the server, speak gRPC-ish headers with the
    // wrong content-type.
    auto* dialer = photon::net::new_tcp_socket_client();
    auto* sock = dialer->connect(server_.endpoint());
    if (sock == nullptr) {
      peer_status = absl::InternalError("raw connect failed");
      return;
    }
    testing::RawPeer peer{sock};
    absl::Status st = peer.Write(testing::kPreface);
    std::string block =
        *HpackEncode({H(":method", "POST"), H(":path", "/x/Y"), H("content-type", "text/plain"), H("te", "trailers")});
    st.Update(peer.WriteFrame(testing::kTypeHeaders, testing::kFlagEndHeaders | testing::kFlagEndStream, 1, block));
    if (!st.ok()) {
      peer_status = st;
      return;
    }
    while (true) {
      auto frame = peer.ReadFrame();
      if (!frame.ok()) {
        peer_status = frame.status();
        return;
      }
      if (frame->type == testing::kTypeHeaders) {
        auto fields = std::vector<HeaderField>();
        HpackDecoder decoder;
        if (!decoder.Decode(frame->payload, &fields).ok()) {
          peer_status = absl::InternalError("hpack decode failed");
          return;
        }
        for (auto& f : fields) {
          if (f.name == ":status") {
            observed_status = atoi(f.value.c_str());
            return;
          }
        }
      }
    }
  });
  photon::thread_enable_join(peer_thread, true);

  ASSERT_TRUE(server_.Start(0).ok());
  photon::thread_join(photon::thread_enable_join(peer_thread, true));
  ASSERT_TRUE(peer_status.ok()) << peer_status.message();
  EXPECT_EQ(observed_status, 415);  // R3
}

TEST_F(GrpcTest, MissingGrpcStatusSynthesizedFromHttp) {
  // Raw server replying with a bare 503 and no grpc-status: the client must
  // synthesize UNAVAILABLE (R18).
  auto* listener = photon::net::new_tcp_socket_server();
  ASSERT_EQ(listener->bind_v4localhost(0), 0);
  ASSERT_EQ(listener->listen(), 0);
  auto endpoint = listener->getsockname();
  photon::thread* peer_thread = photon::thread_create11([&]() {
    auto* sock = listener->accept();
    ASSERT_NE(sock, nullptr);
    testing::RawPeer peer{sock};
    absl::Status st = peer.ServerHandshake();
    if (!st.ok()) {
      return;
    }
    // Read the client's request HEADERS (ignore contents).
    auto frame = peer.ReadFrame();
    if (!frame.ok()) {
      return;
    }
    std::string block = *HpackEncode({H(":status", "503")});
    (void)peer.WriteFrame(testing::kTypeHeaders, testing::kFlagEndHeaders | testing::kFlagEndStream, 1, block);
  });
  photon::thread_enable_join(peer_thread, true);

  GrpcClient client(endpoint);
  ASSERT_TRUE(client.Connect().ok());
  auto call = client.Call("/svc/M");
  ASSERT_TRUE(call.ok());
  auto result = call->Finish();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->code, StatusCode::kUnavailable);
  photon::thread_join(photon::thread_enable_join(peer_thread, true));
  listener->terminate();
  delete listener;
}

TEST_F(ConnectedGrpcTest, LargeMessagesFlowBothWays) {
  const std::string big(2 * 1024 * 1024, 'z');  // 2 MiB each way, R-H3
  server_.Register("/test.Echo/Big", [&big](ServerCall* call) {
    auto message = call->ReadMessage();
    ASSERT_TRUE(message.ok());
    ASSERT_TRUE(message->has_value());
    EXPECT_EQ((*message)->size(), big.size());
    (void)call->WriteMessage(**message);
    (void)call->Finish(StatusCode::kOk);
  });
  ASSERT_TRUE(server_.Start(0).ok());
  ConnectClient();

  auto call = client_->Call("/test.Echo/Big");
  ASSERT_TRUE(call.ok());
  ASSERT_TRUE(call->WriteMessage(big).ok());
  auto reply = call->ReadMessage();
  ASSERT_TRUE(reply.ok()) << reply.status().message();
  ASSERT_TRUE(reply->has_value());
  EXPECT_EQ((*reply)->size(), big.size());
  auto result = call->Finish();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->code, StatusCode::kOk);
}

}  // namespace
}  // namespace grpc
}  // namespace walleye
