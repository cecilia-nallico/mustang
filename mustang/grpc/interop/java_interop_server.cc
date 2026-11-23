// Standalone interop server for the Java compliance suite: serves
// mustang.interop.TestService on an ephemeral port (printed as
// "PORT <n>") until killed by the test harness.

#include <cstdio>
#include <memory>
#include <string>

// protobuf first: abseil logging macros must precede photon's (see
// third_party/photon/README.md).
#include "mustang/grpc/interop/interop.grpc.pb.h"

#include "absl/strings/str_cat.h"

#include "mustang/grpc/call.h"
#include "mustang/grpc/server.h"
#include "mustang/grpc/typed.h"
#include "mustang/runtime.h"

namespace mustang {
namespace interop {
namespace {

using grpc::ServerCall;
using grpc::StatusCode;

class TestImpl : public TestServiceHandler {
 public:
  void Unary(ServerCall& call, const SimpleRequest& request) override {
    SimpleResponse response;
    response.set_text("echo:" + request.text());
    response.set_n(request.n());
    (void)grpc::WriteMessage(call, response);
    (void)call.Finish(StatusCode::kOk);
  }

  void Fail(ServerCall& call, const SimpleRequest& /*request*/) override {
    (void)call.Finish(StatusCode::kNotFound, "café 100% missing ☕");
  }

  void Slow(ServerCall& call, const SimpleRequest& request) override {
    photon::thread_usleep(300 * 1000);
    SimpleResponse response;
    response.set_text("late:" + request.text());
    (void)grpc::WriteMessage(call, response);
    (void)call.Finish(StatusCode::kOk);
  }

  void ServerStream(ServerCall& call, const StreamRequest& request) override {
    std::string filler(request.size() > 16 ? request.size() - 16 : 0, 'x');
    for (int i = 0; i < request.count(); i++) {
      SimpleResponse response;
      response.set_text(absl::StrCat(request.text(), i, ":", filler));
      (void)grpc::WriteMessage(call, response);
    }
    (void)call.Finish(StatusCode::kOk);
  }

  void ClientStream(ServerCall& call) override {
    int count = 0;
    int total = 0;
    while (true) {
      auto request = grpc::ReadMessage<SimpleRequest>(call);
      if (!request.ok()) {
        (void)call.Finish(StatusCode::kInternal, "read failed");
        return;
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
    (void)grpc::WriteMessage(call, response);
    (void)call.Finish(StatusCode::kOk);
  }

  void Chat(ServerCall& call) override {
    while (true) {
      auto request = grpc::ReadMessage<SimpleRequest>(call);
      if (!request.ok()) {
        (void)call.Finish(StatusCode::kInternal, "read failed");
        return;
      }
      if (!request->has_value()) {
        break;
      }
      SimpleResponse response;
      response.set_text(">" + (*request)->text());
      (void)grpc::WriteMessage(call, response);
    }
    (void)call.Finish(StatusCode::kOk);
  }

  void Metadata(ServerCall& call, const MetadataRequest& request) override {
    std::string seen = "<missing>";
    for (const grpc::HeaderField& field : call.request_metadata()) {
      if (field.name == request.key()) {
        seen = field.value;
      }
    }
    SimpleResponse response;
    response.set_text("meta:" + seen);
    (void)grpc::WriteMessage(call, response);
    grpc::HeaderField reply{"resp-meta", "value-42"};
    (void)call.Finish(StatusCode::kOk, "", {reply});
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
  mustang::interop::RegisterTestServiceService(server, &impl);
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
