#include "mustang/grpc/server.h"

#include <utility>

#include "absl/strings/str_cat.h"

#include "mustang/photon_headers.h"

namespace mustang {
namespace grpc {
namespace {

absl::optional<absl::string_view> Find(absl::string_view name, const std::vector<HeaderField>& headers) {
  for (const HeaderField& field : headers) {
    if (field.name == name) {
      return field.value;
    }
  }
  return std::nullopt;
}

}  // namespace

GrpcServer::GrpcServer(GrpcServerOptions options) : options_(std::move(options)) {}

GrpcServer::~GrpcServer() { Stop(); }

void GrpcServer::Register(std::string path, Handler handler) { handlers_[std::move(path)] = std::move(handler); }

absl::Status GrpcServer::Start(uint16_t port) {
  if (listener_ != nullptr) {
    return absl::InternalError("grpc server: already started");
  }
  listener_ = photon::net::new_tcp_socket_server();
  if (listener_ == nullptr) {
    return absl::InternalError("grpc server: socket creation failed");
  }
  if (listener_->bind(port) != 0 || listener_->listen() != 0) {
    return absl::InternalError("grpc server: bind/listen failed");
  }
  endpoint_ = listener_->getsockname();
  accept_thread_ = photon::thread_create11([this]() {
    while (true) {
      photon::net::ISocketStream* sock = listener_->accept();
      if (sock == nullptr) {
        return;  // listener terminated
      }
      photon::thread* th = photon::thread_create11([this, sock]() { ServeConnection(sock); });
      photon::join_handle* jh = photon::thread_enable_join(th, true);
      SCOPED_LOCK(mu_);
      joins_.push_back(jh);
    }
  });
  // The accept coroutine runs in the background; keep it joinable too.
  photon::join_handle* accept_join = photon::thread_enable_join(accept_thread_, true);
  SCOPED_LOCK(mu_);
  joins_.push_back(accept_join);
  return absl::OkStatus();
}

void GrpcServer::ServeConnection(photon::net::ISocketStream* sock) {
  // The photon accept loop deletes the socket after this handler returns,
  // so the Connection must not take ownership; closing it first is fine.
  Connection conn(sock, /*is_client=*/false, options_.connection, /*ownership=*/false);
  {
    SCOPED_LOCK(mu_);
    connections_.push_back(&conn);
  }
  std::vector<photon::join_handle*> stream_joins;
  if (conn.Start().ok()) {
    while (true) {
      absl::StatusOr<Stream*> accepted = conn.AcceptStream();
      if (!accepted.ok()) {
        break;
      }
      photon::thread* th = photon::thread_create11([this, &conn, stream = *accepted]() { ServeStream(&conn, stream); });
      stream_joins.push_back(photon::thread_enable_join(th, true));
    }
  }
  // Streams reference this connection's state: they must be done first.
  for (photon::join_handle* jh : stream_joins) {
    photon::thread_join(jh);
  }
  {
    SCOPED_LOCK(mu_);
    for (size_t i = 0; i < connections_.size(); i++) {
      if (connections_[i] == &conn) {
        connections_.erase(connections_.begin() + i);
        break;
      }
    }
  }
}

absl::Status GrpcServer::ReplyTrailersOnly(Stream* stream, StatusCode code, const std::string& message) {
  std::vector<HeaderField> fields;
  fields.push_back(HeaderField{":status", "200"});
  fields.push_back(HeaderField{"content-type", "application/grpc"});
  fields.push_back(HeaderField{"grpc-status", absl::StrCat(static_cast<int>(code))});
  if (!message.empty()) {
    fields.push_back(HeaderField{"grpc-message", PercentEncode(message)});
  }
  return stream->SendHeaders(std::move(fields), /*end_stream=*/true);
}

void GrpcServer::ServeStream(Connection* /*conn*/, Stream* stream) {
  bool end = false;
  absl::StatusOr<std::vector<HeaderField>> headers = stream->RecvHeaders(&end);
  if (!headers.ok()) {
    stream->Done();
    return;
  }

  // R3: anything not starting with application/grpc gets plain HTTP 415 so
  // non-gRPC HTTP/2 clients do not read our status-200 error as success.
  absl::optional<absl::string_view> content_type = Find("content-type", *headers);
  if (!content_type.has_value() || content_type->substr(0, 16) != absl::string_view("application/grpc")) {
    (void)stream->SendHeaders({HeaderField{":status", "415"}}, /*end_stream=*/true);
    stream->Done();
    return;
  }

  absl::optional<absl::Time> deadline;
  if (absl::optional<absl::string_view> timeout = Find("grpc-timeout", *headers)) {
    absl::StatusOr<absl::Duration> parsed = ParseTimeout(*timeout);  // R4
    if (parsed.ok()) {
      deadline = absl::Now() + *parsed;
    }
  }

  absl::optional<absl::string_view> path = Find(":path", *headers);
  Handler handler;
  if (path.has_value()) {
    auto it = handlers_.find(std::string(*path));
    if (it != handlers_.end()) {
      handler = it->second;
    }
  }
  if (!handler) {
    (void)ReplyTrailersOnly(stream, StatusCode::kUnimplemented, "unknown method");
    stream->Done();
    return;
  }

  ServerCall call(stream, std::move(*headers), deadline, options_.max_message_size);
  handler(call);
  if (!call.finished()) {
    // Handlers must finish exactly once; enforce a sane fallback (R13).
    (void)call.Finish(StatusCode::kInternal, "handler returned without finishing");
  }
  stream->Done();
}

void GrpcServer::Stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  if (listener_ != nullptr) {
    listener_->terminate();
  }
  // terminate() closes the listening fd, but a photon accept() blocked in
  // the event loop is not unblocked by that alone: interrupt it as well.
  if (accept_thread_ != nullptr) {
    photon::thread_interrupt(accept_thread_);
  }
  {
    SCOPED_LOCK(mu_);
    for (Connection* conn : connections_) {
      (void)conn->Shutdown();  // R22: GOAWAY, in-flight RPCs continue
    }
  }
  std::vector<photon::join_handle*> joins;
  {
    SCOPED_LOCK(mu_);
    joins.swap(joins_);
  }
  for (photon::join_handle* jh : joins) {
    photon::thread_join(jh);
  }
  accept_thread_ = nullptr;
  if (listener_ != nullptr) {
    delete listener_;
    listener_ = nullptr;
  }
}

// ---------------------------------------------------------------------------
// GrpcClient
// ---------------------------------------------------------------------------

GrpcClient::GrpcClient(photon::net::EndPoint endpoint, ConnectionOptions options)
    : endpoint_(endpoint), options_(options) {}

absl::Status GrpcClient::Connect() {
  if (connection_.has_value()) {
    return absl::InternalError("grpc client: already connected");
  }
  dialer_ = photon::net::new_tcp_socket_client();
  if (dialer_ == nullptr) {
    return absl::InternalError("grpc client: socket creation failed");
  }
  photon::net::ISocketStream* sock = dialer_->connect(endpoint_);
  if (sock == nullptr) {
    return absl::UnavailableError("grpc client: connect failed");
  }
  connection_.emplace(sock, /*is_client=*/true, options_, /*ownership=*/true);
  absl::Status st = connection_->Start();
  if (!st.ok()) {
    return st;
  }
  return absl::OkStatus();
}

absl::StatusOr<ClientCall> GrpcClient::Call(std::string path,
                                            std::vector<HeaderField> metadata,
                                            absl::optional<absl::Duration> timeout) {
  if (!connection_.has_value()) {
    return absl::InternalError("grpc client: not connected");
  }
  absl::StatusOr<Stream*> stream = connection_->OpenStream();
  if (!stream.ok()) {
    return stream.status();
  }

  std::vector<HeaderField> headers;
  headers.push_back(HeaderField{":method", "POST"});
  headers.push_back(HeaderField{":scheme", "http"});
  headers.push_back(HeaderField{":path", path});
  headers.push_back(HeaderField{":authority", "mustang"});
  headers.push_back(HeaderField{"te", "trailers"});  // R2
  headers.push_back(HeaderField{"content-type", "application/grpc"});
  headers.push_back(HeaderField{"user-agent", "grpc-mustang/0.1"});
  if (timeout.has_value()) {
    headers.push_back(HeaderField{"grpc-timeout", FormatTimeout(*timeout)});  // R4
  }
  for (auto& field : metadata) {
    headers.push_back(std::move(field));
  }

  absl::Status st = (*stream)->SendHeaders(std::move(headers), false);
  if (!st.ok()) {
    return st;
  }
  absl::optional<absl::Time> deadline =
      timeout.has_value() ? absl::optional<absl::Time>(absl::Now() + *timeout) : std::nullopt;
  return ClientCall(*stream, deadline);
}

}  // namespace grpc
}  // namespace mustang
