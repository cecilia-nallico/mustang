// Interop oracle tests: mustang gRPC client/server against real gRPC C++
// 1.85 (generic ::grpc::ByteBuffer API, no protobuf codegen). gRPC runs its own
// OS threads; mustang runs on photon in the test main thread.

#include <grpcpp/generic/async_generic_service.h>
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "gtest/gtest.h"

#include "mustang/grpc/call.h"
#include "mustang/grpc/hpack.h"
#include "mustang/grpc/server.h"
#include "mustang/grpc/wire.h"
#include "mustang/runtime.h"

namespace mustang {
namespace grpc {
namespace {

std::string BufferToBytes(const ::grpc::ByteBuffer& buffer) {
  ::grpc::Slice slice;
  if (!buffer.DumpToSingleSlice(&slice).ok()) {
    return "";
  }
  return std::string(reinterpret_cast<const char*>(slice.begin()), slice.size());
}

::grpc::ByteBuffer BytesToBuffer(const std::string& bytes) {
  ::grpc::Slice slice(bytes.data(), bytes.size());
  return ::grpc::ByteBuffer(&slice, 1);
}

// A generic gRPC server: echoes the first request message prefixed with
// "echo:", or (error mode) finishes with a fixed status after reading.
class GrpcGenericServer {
 public:
  struct Options {
    std::string reply_prefix = "echo:";
    absl::Duration delay = absl::ZeroDuration();
    std::optional<::grpc::StatusCode> force_status;
    std::string force_message;
    int messages_before_ok = 1;  // >1: server-streaming of prefix+i
  };

  ~GrpcGenericServer() {
    if (server_ != nullptr) {
      // Deadline shutdown lets in-flight RPCs finish before the completion
      // queue is torn down (killing the CQ mid-op trips a grpc CHECK).
      server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
      cq_->Shutdown();  // without this, cq Next() never returns false
      loop_.join();
    }
  }

  int Start(const Options& options) {
    options_ = options;
    ::grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", ::grpc::InsecureServerCredentials(), &port_);
    builder.RegisterAsyncGenericService(&service_);
    cq_ = builder.AddCompletionQueue();
    server_ = builder.BuildAndStart();
    if (server_ == nullptr) {
      return -1;
    }
    // Prime the request pump.
    new CallData(&service_, cq_.get(), options_);
    loop_ = std::thread([this]() {
      void* tag = nullptr;
      bool ok = false;
      while (cq_->Next(&tag, &ok)) {
        static_cast<CallData*>(tag)->Proceed(ok);
      }
    });
    return port_;
  }

 private:
  struct CallData {
    ::grpc::GenericServerContext ctx;
    ::grpc::GenericServerAsyncReaderWriter stream;
    Options options;
    ::grpc::ByteBuffer request;
    int stage = 0;
    int written = 0;
    ::grpc::ServerCompletionQueue* cq;
    ::grpc::AsyncGenericService* service;

    CallData(::grpc::AsyncGenericService* svc, ::grpc::ServerCompletionQueue* completion_queue, const Options& opts)
        : stream(&ctx), options(opts), cq(completion_queue), service(svc) {
      service->RequestCall(&ctx, &stream, cq, cq, this);
    }

    void Proceed(bool ok) {
      if (!ok) {
        delete this;
        return;
      }
      switch (stage) {
        case 0: {
          stage = 1;
          stream.Read(&request, this);
          break;
        }
        case 1: {
          if (options.force_status.has_value()) {
            stage = 3;
            stream.Finish(::grpc::Status(*options.force_status, options.force_message), this);
            break;
          }
          if (options.delay > absl::ZeroDuration()) {
            // Slow mode: sleep, then drop the call without further ops. Any
            // op issued after the peer's grpc-timeout expired trips a grpc
            // CHECK; abandoning the call lets grpc cancel it cleanly and
            // the client observes the deadline.
            std::this_thread::sleep_for(std::chrono::microseconds(absl::ToInt64Microseconds(options.delay)));
            delete this;
            return;
          }
          stage = 2;
          stream.Write(BytesToBuffer(options.reply_prefix + BufferToBytes(request)), this);
          break;
        }
        case 2: {
          written++;
          if (written < options.messages_before_ok) {
            stream.Write(BytesToBuffer(absl::StrCat(options.reply_prefix, written)), this);
            break;
          }
          stage = 3;
          stream.Finish(::grpc::Status::OK, this);
          break;
        }
        default:
          delete this;
      }
    }
  };

  Options options_;
  ::grpc::AsyncGenericService service_;
  std::unique_ptr<::grpc::ServerCompletionQueue> cq_;

 public:
  int Port() const { return port_; }
  std::unique_ptr<::grpc::Server> server_;
  std::thread loop_;
  int port_ = 0;
};

// Generic gRPC client used against mustang's server. Runs on its own
// std::thread because blocking on a CompletionQueue would stall the photon
// vCPU the mustang server runs on.
class GrpcGenericClient {
 public:
  GrpcGenericClient(const std::string& target) {
    channel_ = ::grpc::CreateChannel(target, ::grpc::InsecureChannelCredentials());
  }

  struct Result {
    bool ok = false;
    ::grpc::StatusCode code = ::grpc::StatusCode::OK;
    std::string message;
    std::vector<std::string> replies;
  };

  // Sends `requests` (in order), reads replies until the server closes,
  // then finishes. Runs on its own std::thread (blocking a CompletionQueue
  // would stall the photon vCPU the mustang server runs on).
  Result Call(const std::string& method, const std::vector<std::string>& requests) {
    Result result;
    std::atomic<bool> finished = false;
    std::thread runner([this, &method, &requests, &result, &finished]() {
      ::grpc::ClientContext ctx;
      ::grpc::CompletionQueue cq;
      ::grpc::GenericStub stub(channel_);
      auto call = stub.PrepareCall(&ctx, method, &cq);
      struct Tag {
        void* self;
        int step;
      };
      Tag started{nullptr, 0};
      call->StartCall(&started);
      void* tag = nullptr;
      bool ok = false;
      if (!cq.Next(&tag, &ok) || !ok) {
        result.message = "start failed";
        return;
      }
      for (const std::string& request : requests) {
        Tag write_tag{nullptr, 1};
        call->Write(BytesToBuffer(request), &write_tag);
        if (!cq.Next(&tag, &ok) || !ok) {
          result.message = "write failed";
          return;
        }
      }
      Tag done_tag{nullptr, 2};
      call->WritesDone(&done_tag);
      if (!cq.Next(&tag, &ok) || !ok) {
        result.message = "writes done failed";
        return;
      }
      while (true) {
        ::grpc::ByteBuffer reply;
        Tag read_tag{nullptr, 3};
        call->Read(&reply, &read_tag);
        if (!cq.Next(&tag, &ok)) {
          result.message = "read cq failed";
          return;
        }
        if (!ok) {
          break;  // server done sending
        }
        result.replies.push_back(BufferToBytes(reply));
      }
      ::grpc::Status status;
      Tag finish_tag{nullptr, 4};
      call->Finish(&status, &finish_tag);
      if (!cq.Next(&tag, &ok) || !ok) {
        result.message = "finish failed";
        return;
      }
      result.ok = status.ok();
      result.code = status.error_code();
      result.message = status.error_message();
      finished.store(true);
    });
    // The main thread is the photon vCPU: never block it on the runner.
    // Park in photon sleep so the server coroutines keep being scheduled,
    // and poll completion.
    while (!finished.load()) {
      photon::thread_usleep(1000);
    }
    runner.join();
    return result;
  }

 private:
  std::shared_ptr<::grpc::Channel> channel_;
};

class InteropTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto rt = Runtime::Create(RuntimeOptions{});
    ASSERT_TRUE(rt.ok());
    runtime_.emplace(std::move(rt).value());
  }
  void TearDown() override {
    server_.Stop();
    runtime_.reset();
  }

  std::optional<Runtime> runtime_;
  GrpcServer server_;
};

// --- mustang client -> gRPC server ---

TEST_F(InteropTest, MustangClientCallsGrpcServer) {
  GrpcGenericServer grpc_server;
  GrpcGenericServer::Options options;
  options.reply_prefix = "echo:";
  ASSERT_GT(grpc_server.Start(options), 0);

  GrpcClient client(photon::net::EndPoint(photon::net::IPAddr("127.0.0.1"), grpc_server.Port()));
  ASSERT_TRUE(client.Connect().ok());
  auto call = client.Call("/test.Interop/Unary");
  ASSERT_TRUE(call.ok());
  ASSERT_TRUE(call->WriteMessage("hello grpc").ok());
  auto reply = call->ReadMessage();
  ASSERT_TRUE(reply.ok());
  ASSERT_TRUE(reply->has_value());
  EXPECT_EQ(**reply, "echo:hello grpc");
  auto result = call->Finish();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->code, StatusCode::kOk);
}

TEST_F(InteropTest, MustangClientSeesGrpcErrorStatus) {
  GrpcGenericServer grpc_server;
  ASSERT_GT(grpc_server.Start({.force_status = ::grpc::StatusCode::NOT_FOUND, .force_message = "café missing"}), 0);

  GrpcClient client(photon::net::EndPoint(photon::net::IPAddr("127.0.0.1"), grpc_server.Port()));
  ASSERT_TRUE(client.Connect().ok());
  auto call = client.Call("/test.Interop/Missing");
  ASSERT_TRUE(call.ok());
  ASSERT_TRUE(call->WriteMessage("x").ok());
  auto result = call->Finish();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->code, StatusCode::kNotFound);
  EXPECT_EQ(result->message, "café missing");  // percent-encoding interop (R16)
}

TEST_F(InteropTest, MustangClientDeadlineAgainstSlowGrpcServer) {
  GrpcGenericServer grpc_server;
  GrpcGenericServer::Options options;
  options.reply_prefix = "late:";
  options.delay = absl::Milliseconds(300);
  ASSERT_GT(grpc_server.Start(options), 0);

  GrpcClient client(photon::net::EndPoint(photon::net::IPAddr("127.0.0.1"), grpc_server.Port()));
  ASSERT_TRUE(client.Connect().ok());
  auto call = client.Call("/test.Interop/Slow", {}, absl::Milliseconds(50));
  ASSERT_TRUE(call.ok());
  ASSERT_TRUE(call->WriteMessage("x").ok());
  // The gRPC server enforces grpc-timeout itself (R4 is a wire header, not
  // just a client-side concept): it closes the call with DEADLINE_EXCEEDED
  // trailers, so ReadMessage observes end-of-messages and Finish reports
  // the server's status. Our client-side deadline check is the backstop.
  auto reply = call->ReadMessage();
  if (reply.ok()) {
    EXPECT_FALSE(reply->has_value());
  }
  auto result = call->Finish();
  if (result.ok()) {
    EXPECT_EQ(result->code, StatusCode::kDeadlineExceeded);
  } else {
    // Local deadline backstop fired while waiting for trailers.
    EXPECT_EQ(result.status().code(), absl::StatusCode::kDeadlineExceeded);
  }
}

// --- gRPC client -> mustang server ---

TEST_F(InteropTest, GrpcClientCallsMustangServer) {
  server_.Register("/test.Interop/Unary", [](ServerCall* call) {
    auto message = call->ReadMessage();
    ASSERT_TRUE(message.ok());
    ASSERT_TRUE(message->has_value());
    (void)call->WriteMessage("echo:" + **message);
    (void)call->Finish(StatusCode::kOk);
  });
  ASSERT_TRUE(server_.Start(0).ok());
  GrpcGenericClient grpc_client(absl::StrCat("127.0.0.1:", server_.endpoint().port));
  GrpcGenericClient::Result result = grpc_client.Call("/test.Interop/Unary", {"hello mustang"});
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.message, "");
  ASSERT_EQ(result.replies.size(), 1u);
  EXPECT_EQ(result.replies[0], "echo:hello mustang");
}

TEST_F(InteropTest, GrpcClientSeesMustangErrorStatus) {
  server_.Register("/test.Interop/Fail", [](ServerCall* call) {
    (void)call->ReadMessage();
    (void)call->Finish(StatusCode::kInvalidArgument, "bad caf\xC3\xA9 input");
  });
  ASSERT_TRUE(server_.Start(0).ok());
  GrpcGenericClient grpc_client(absl::StrCat("127.0.0.1:", server_.endpoint().port));
  GrpcGenericClient::Result result = grpc_client.Call("/test.Interop/Fail", {"x"});
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.code, ::grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(result.message, "bad café input");
}

TEST_F(InteropTest, GrpcClientReadsMustangServerStreaming) {
  server_.Register("/test.Interop/Stream", [](ServerCall* call) {
    (void)call->ReadMessage();
    for (int i = 0; i < 3; i++) {
      (void)call->WriteMessage(absl::StrCat("m", i));
    }
    (void)call->Finish(StatusCode::kOk);
  });
  ASSERT_TRUE(server_.Start(0).ok());
  GrpcGenericClient grpc_client(absl::StrCat("127.0.0.1:", server_.endpoint().port));
  GrpcGenericClient::Result result = grpc_client.Call("/test.Interop/Stream", {"go"});
  EXPECT_TRUE(result.ok);
  ASSERT_EQ(result.replies.size(), 3u);
  EXPECT_EQ(result.replies[0], "m0");
  EXPECT_EQ(result.replies[2], "m2");
}

}  // namespace
}  // namespace grpc
}  // namespace mustang
