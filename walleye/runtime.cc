#include "walleye/runtime.h"

#include <errno.h>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "walleye/photon_headers.h"

namespace walleye {
namespace {
constexpr int kMaxLogLevel = kLogLevelFatal;
}  // namespace

absl::StatusOr<uint64_t> EventEngineFromName(absl::string_view name) {
  if (name == "auto") {
    return photon::INIT_EVENT_DEFAULT;
  }
  if (name == "epoll") {
    return photon::INIT_EVENT_EPOLL;
  }
  if (name == "epoll-ng") {
    return photon::INIT_EVENT_EPOLL_NG;
  }
  if (name == "iouring") {
    return photon::INIT_EVENT_IOURING;
  }
  if (name == "select") {
    return photon::INIT_EVENT_SELECT;
  }
  return absl::InvalidArgumentError(
      absl::StrCat("unknown event engine '", name, "' (want auto|epoll|epoll-ng|iouring|select)"));
}

absl::StatusOr<Runtime> Runtime::Create(const RuntimeOptions& options) {
  if (options.log_level < kLogLevelDebug || options.log_level > kMaxLogLevel) {
    return absl::InvalidArgumentError(
        absl::StrCat("log_level must be in [", kLogLevelDebug, ", ", kMaxLogLevel, "], got ", options.log_level));
  }
  // Photon filters at output time, so the level applies immediately and
  // covers photon::init's own logging below.
  set_log_output_level(options.log_level);

  if (photon::init(options.event_engine, options.io_engine) != 0) {
    return absl::ErrnoToStatus(errno, "photon::init failed");
  }
  return Runtime(/*active=*/true);
}

Runtime::Runtime(Runtime&& other) noexcept : active_(other.active_) { other.active_ = false; }

Runtime::~Runtime() {
  if (active_) {
    photon::fini();
  }
}
}  // namespace walleye
