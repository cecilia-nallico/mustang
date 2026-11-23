#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "walleye/grpc/call.h"
#include "walleye/grpc/connection.h"

namespace walleye {
namespace grpc {

struct GrpcServerOptions {
  ConnectionOptions connection;
  size_t max_message_size = size_t{4} << 20;
};

// A gRPC server: listens with a photon TCP socket, one Connection per
// accepted socket, one photon coroutine per RPC stream.
class GrpcServer {
 public:
  using Handler = std::function<void(ServerCall*)>;

  explicit GrpcServer(GrpcServerOptions options = GrpcServerOptions());
  ~GrpcServer();

  GrpcServer(const GrpcServer&) = delete;
  GrpcServer& operator=(const GrpcServer&) = delete;

  // Registers a handler for ":path" == /Service/Method. Call before Start.
  void Register(std::string path, Handler handler);

  // Binds (port 0 = ephemeral) and starts serving.
  absl::Status Start(uint16_t port = 0);
  photon::net::EndPoint endpoint() const { return endpoint_; }

  // Graceful stop: closes the listener, GOAWAYs every connection and waits
  // for in-flight RPCs to finish (R22).
  void Stop();

 private:
  void ServeConnection(photon::net::ISocketStream* sock);
  void ServeStream(Connection* conn, Stream* stream);
  absl::Status ReplyTrailersOnly(Stream* stream, StatusCode code, const std::string& message);

  photon::net::ISocketServer* listener_ = nullptr;
  photon::thread* accept_thread_ = nullptr;
  bool stopped_ = false;
  GrpcServerOptions options_;
  std::map<std::string, Handler> handlers_;
  photon::mutex mu_;
  std::vector<Connection*> connections_;     // guarded by mu_
  std::vector<photon::join_handle*> joins_;  // connection coroutines
  photon::net::EndPoint endpoint_{};
};

// Minimal channel: one h2c connection to one endpoint (reconnect with
// backoff and subchannel machinery are deliberately out of scope for v1).
class GrpcClient {
 public:
  GrpcClient(photon::net::EndPoint endpoint, ConnectionOptions options = ConnectionOptions());

  GrpcClient(const GrpcClient&) = delete;
  GrpcClient& operator=(const GrpcClient&) = delete;

  absl::Status Connect();
  Connection& connection() { return *connection_; }

  // Starts a call; timeout (grpc-timeout, R4) is optional. The returned
  // call owns the request/response exchange; finish it with Finish()/Cancel.
  absl::StatusOr<ClientCall> Call(std::string path,
                                  std::vector<HeaderField> metadata = {},
                                  absl::optional<absl::Duration> timeout = std::nullopt);

 private:
  photon::net::EndPoint endpoint_;
  ConnectionOptions options_;
  photon::net::ISocketClient* dialer_ = nullptr;
  std::optional<Connection> connection_;
};

}  // namespace grpc
}  // namespace walleye
