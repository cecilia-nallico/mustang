// End-to-end tests through the protoc-generated typed stubs.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "mustang/grpc/test/echo.grpc.pb.h"  // subject header; protobuf first (macro order)

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "gtest/gtest.h"

#include "mustang/grpc/call.h"
#include "mustang/grpc/server.h"
#include "mustang/grpc/typed.h"
#include "mustang/runtime.h"

namespace mustang {
namespace test {
namespace {

class EchoImpl : public EchoHandler {
 public:
  absl::StatusOr<Resp> Unary(grpc::UnaryContext* /*ctx*/, const Req& request) override {
    Resp response;
    response.set_text("echo:" + request.text());
    return response;
  }

  void ServerStream(grpc::ServerWriter<Resp>* writer, const Req& request) override {
    for (int i = 0; i < request.n(); i++) {
      Resp response;
      response.set_text(absl::StrCat(request.text(), i));
      if (!writer->Write(response).ok()) {
        return;
      }
    }
    writer->Close(grpc::StatusCode::kOk);
  }

  absl::StatusOr<Resp> ClientStream(grpc::ServerReader<Req>* reader) override {
    int total = 0;
    while (true) {
      auto request = reader->Read();
      if (!request.ok()) {
        return absl::Status(grpc::ToAbslCode(grpc::StatusCode::kInternal), "read failed");
      }
      if (!request->has_value()) {
        break;
      }
      total += (*request)->n();
    }
    Resp response;
    response.set_text(absl::StrCat("total:", total));
    return response;
  }

  void Chat(grpc::ServerStream<Req, Resp>* stream) override {
    while (true) {
      auto request = stream->Read();
      if (!request.ok()) {
        stream->Close(grpc::StatusCode::kInternal, "read failed");
        return;
      }
      if (!request->has_value()) {
        break;
      }
      Resp response;
      response.set_text(">" + (*request)->text());
      if (!stream->Write(response).ok()) {
        return;
      }
    }
    stream->Close(grpc::StatusCode::kOk);
  }
};

class TypedGrpcTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto rt = Runtime::Create(RuntimeOptions{});
    ASSERT_TRUE(rt.ok());
    runtime_.emplace(std::move(rt).value());
    RegisterEchoService(&server_, &impl_);
    ASSERT_TRUE(server_.Start(0).ok());
    client_ = std::make_unique<grpc::GrpcClient>(server_.endpoint());
    ASSERT_TRUE(client_->Connect().ok());
    stub_ = std::make_unique<EchoStub>(client_.get());
  }
  void TearDown() override {
    // Photon objects must die before the runtime.
    stub_.reset();
    client_.reset();
    server_.Stop();
    runtime_.reset();
  }

  std::optional<Runtime> runtime_;
  EchoImpl impl_;
  grpc::GrpcServer server_;
  std::unique_ptr<grpc::GrpcClient> client_;
  std::unique_ptr<EchoStub> stub_;
};

TEST_F(TypedGrpcTest, UnaryRoundtrip) {
  Req request;
  request.set_text("typed");
  absl::StatusOr<Resp> reply = stub_->Unary(request);
  ASSERT_TRUE(reply.ok()) << reply.status().message();
  EXPECT_EQ(reply->text(), "echo:typed");
}

TEST_F(TypedGrpcTest, UnaryErrorMapsToAbslStatus) {
  // Unknown method -> UNIMPLEMENTED (via the raw call API).
  absl::StatusOr<grpc::ClientCall> raw = client_->Call("/mustang.test.Echo/Nope");
  ASSERT_TRUE(raw.ok());
  auto result = raw->Finish();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->code, grpc::StatusCode::kUnimplemented);
}

TEST_F(TypedGrpcTest, ServerStreaming) {
  Req request;
  request.set_text("m");
  request.set_n(3);
  auto stream = stub_->ServerStream(request);
  ASSERT_TRUE(stream.ok()) << stream.status().message();
  for (int i = 0; i < 3; i++) {
    auto reply = stream->Read();
    ASSERT_TRUE(reply.ok());
    ASSERT_TRUE(reply->has_value());
    EXPECT_EQ((*reply)->text(), absl::StrCat("m", i));
  }
  auto end = stream->Read();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());
  auto result = stream->Finish();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->code, grpc::StatusCode::kOk);
}

TEST_F(TypedGrpcTest, ClientStreamingAggregates) {
  auto stream = stub_->ClientStream();
  ASSERT_TRUE(stream.ok()) << stream.status().message();
  for (int i = 1; i <= 4; i++) {
    Req request;
    request.set_n(i);
    ASSERT_TRUE(stream->Write(request).ok());
  }
  ASSERT_TRUE(stream->CloseSend().ok());
  auto reply = stream->Read();
  ASSERT_TRUE(reply.ok());
  ASSERT_TRUE(reply->has_value());
  EXPECT_EQ((*reply)->text(), "total:10");
  auto result = stream->Finish();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->code, grpc::StatusCode::kOk);
}

TEST_F(TypedGrpcTest, BidiChat) {
  auto stream = stub_->Chat();
  ASSERT_TRUE(stream.ok()) << stream.status().message();
  for (int i = 0; i < 5; i++) {
    Req request;
    request.set_text(absl::StrCat("x", i));
    ASSERT_TRUE(stream->Write(request).ok());
    auto reply = stream->Read();
    ASSERT_TRUE(reply.ok());
    ASSERT_TRUE(reply->has_value());
    EXPECT_EQ((*reply)->text(), absl::StrCat(">x", i));
  }
  ASSERT_TRUE(stream->CloseSend().ok());
  auto end = stream->Read();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());
  auto result = stream->Finish();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->code, grpc::StatusCode::kOk);
}

}  // namespace
}  // namespace test
}  // namespace mustang
