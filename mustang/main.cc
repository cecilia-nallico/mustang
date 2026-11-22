#include <string>
#include <utility>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/status/status.h"

#include "mustang/greet.h"
#include "mustang/photon_headers.h"
#include "mustang/runtime.h"

ABSL_FLAG(std::string, name, "world",
          "Name that mustang greets on startup.");
ABSL_FLAG(int, log_level, mustang::kLogLevelInfo,
          "PhotonLibOS log level: 0=debug 1=info 2=warn 3=error 4=fatal.");
ABSL_FLAG(std::string, event_engine, "auto",
          "PhotonLibOS event engine: auto|epoll|epoll-ng|iouring|select.");

namespace mustang {
namespace {
absl::Status RealMain() {
  RuntimeOptions options;
  options.log_level = absl::GetFlag(FLAGS_log_level);
  absl::StatusOr<uint64_t> event_engine =
      EventEngineFromName(absl::GetFlag(FLAGS_event_engine));
  if (!event_engine.ok()) {
    return event_engine.status();
  }
  options.event_engine = *event_engine;

  absl::StatusOr<Runtime> runtime = Runtime::Create(options);
  if (!runtime.ok()) {
    return runtime.status();
  }

  // Application work runs as a photon coroutine on the runtime we just
  // initialized; the main thread only joins it.
  const std::string name = absl::GetFlag(FLAGS_name);
  std::string greeting;
  absl::Status coroutine_status = absl::OkStatus();
  photon::thread* worker = photon::thread_create11([&]() {
    absl::StatusOr<std::string> result = Greet(name);
    if (!result.ok()) {
      LOG_ERROR("greet failed: `", std::string(result.status().message()));
      coroutine_status = std::move(result).status();
      return;
    }
    greeting = std::move(*result);
    LOG_INFO("coroutine produced greeting: `", greeting);
    photon::thread_usleep(1000);
  });
  photon::join_handle* worker_join = photon::thread_enable_join(worker, true);
  photon::thread_join(worker_join);

  if (!coroutine_status.ok()) {
    return coroutine_status;
  }
  LOG_INFO("mustang says: `", greeting);
  return absl::OkStatus();
}
}  // namespace
}  // namespace mustang

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);

  absl::Status status = mustang::RealMain();
  if (!status.ok()) {
    LOG_ERROR("mustang exiting: `", std::string(status.message()));
    return 1;
  }
  return 0;
}
