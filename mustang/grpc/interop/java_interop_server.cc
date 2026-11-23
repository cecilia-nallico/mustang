// Standalone interop server for the Java compliance suite: serves
// mustang.interop.TestService on an ephemeral port (printed as
// "PORT <n>") until killed by the test harness.

// protobuf first: abseil logging macros must precede photon's (see
// third_party/photon/README.md).
#include "mustang/grpc/interop/interop.grpc.pb.h"

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "mustang/grpc/server.h"
#include "mustang/grpc/typed.h"
#include "mustang/runtime.h"

namespace mustang {
namespace interop {
namespace {

class TestImpl : public TestServiceHandler {
 public:
  absl::StatusOr<SimpleResponse> Unary(grpc::UnaryContext* /*ctx*/, const SimpleRequest& request) override {
    SimpleResponse response;
    response.set_text("echo:" + request.text());
    response.set_n(request.n());
    return response;
  }

  absl::StatusOr<SimpleResponse> Fail(grpc::UnaryContext* /*ctx*/, const SimpleRequest& /*request*/) override {
    return absl::NotFoundError("café 100% missing ☕");
  }

  absl::StatusOr<SimpleResponse> Slow(grpc::UnaryContext* /*ctx*/, const SimpleRequest& request) override {
    photon::thread_usleep(300 * 1000);
    SimpleResponse response;
    response.set_text("late:" + request.text());
    return response;
  }

  void ServerStream(grpc::ServerWriter<SimpleResponse>* writer, const StreamRequest& request) override {
    std::string filler(request.size() > 16 ? request.size() - 16 : 0, 'x');
    for (int i = 0; i < request.count(); i++) {
      SimpleResponse response;
      response.set_text(absl::StrCat(request.text(), i, ":", filler));
      if (!writer->Write(response).ok()) {
        return;
      }
    }
    writer->Close(grpc::StatusCode::kOk);
  }

  absl::StatusOr<SimpleResponse> ClientStream(grpc::ServerReader<SimpleRequest>* reader) override {
    int count = 0;
    int total = 0;
    while (true) {
      auto request = reader->Read();
      if (!request.ok()) {
        return absl::Status(grpc::ToAbslCode(grpc::StatusCode::kInternal), "read failed");
      }
      if (!request->has_value()) {
        break;
      }
      count++;
      total += (*request)->n();
    }
    SimpleResponse response;
    response.set_text(absl::StrCat("count:", count, " total:", total));
    response.set_n(count);
    return response;
  }

  void Chat(grpc::ServerStream<SimpleRequest, SimpleResponse>* stream) override {
    while (true) {
      auto request = stream->Read();
      if (!request.ok()) {
        stream->Close(grpc::StatusCode::kInternal, "read failed");
        return;
      }
      if (!request->has_value()) {
        break;
      }
      SimpleResponse response;
      response.set_text(">" + (*request)->text());
      if (!stream->Write(response).ok()) {
        return;
      }
    }
    stream->Close(grpc::StatusCode::kOk);
  }

  absl::StatusOr<SimpleResponse> Metadata(grpc::UnaryContext* ctx, const MetadataRequest& request) override {
    std::string seen = "<missing>";
    for (const grpc::HeaderField& field : ctx->request_metadata()) {
      if (field.name == request.key()) {
        seen = field.value;
      }
    }
    // Trailer metadata rides along with the framework-written trailers.
    ctx->SetResponseMetadata({grpc::HeaderField{"resp-meta", "value-42"}});
    SimpleResponse response;
    response.set_text("meta:" + seen);
    return response;
  }
};

}  // namespace
}  // namespace interop
}  // namespace mustang

int main() {
  mustang::RuntimeOptions options;
  options.log_level = mustang::kLogLevelWarn;  // keep stdout clean for PORT
  auto runtime = mustang::Runtime::Create(options);
  if (!runtime.ok()) {
    fprintf(stderr, "runtime init failed: %s\n", std::string(runtime.status().message()).c_str());
    return 1;
  }
  mustang::interop::TestImpl impl;
  mustang::grpc::GrpcServer server;
  mustang::interop::RegisterTestServiceService(&server, &impl);
  if (!server.Start(0).ok()) {
    fprintf(stderr, "server start failed\n");
    return 1;
  }
  printf("PORT %u\n", server.endpoint().port);
  fflush(stdout);
  // Serve until the harness kills us.
  while (true) {
    photon::thread_usleep(100 * 1000);
  }
  return 0;
}
