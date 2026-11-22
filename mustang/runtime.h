#pragma once

#include <cstdint>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

#include "mustang/photon_headers.h"

namespace mustang {
// PhotonLibOS log levels (ALOG_* values from photon/common/alog.h).
inline constexpr int kLogLevelDebug = 0;
inline constexpr int kLogLevelInfo = 1;
inline constexpr int kLogLevelWarn = 2;
inline constexpr int kLogLevelError = 3;
inline constexpr int kLogLevelFatal = 4;

struct RuntimeOptions {
  int log_level = kLogLevelInfo;
  // "auto" resolves to photon's default mask, which lets photon fall back
  // (io_uring -> epoll -> select) when an engine is unavailable.
  uint64_t event_engine = photon::INIT_EVENT_DEFAULT;
  uint64_t io_engine = photon::INIT_IO_DEFAULT;
};

// Maps an event engine name ("auto", "epoll", "epoll-ng", "iouring",
// "select") to the photon engine bit mask.
absl::StatusOr<uint64_t> EventEngineFromName(absl::string_view name);

// RAII guard over photon::init()/photon::fini(). Photon ties the runtime to
// the calling thread, so a Runtime must be created, used and destroyed on
// the same thread.
class Runtime {
 public:
  static absl::StatusOr<Runtime> Create(const RuntimeOptions& options);

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&& other) noexcept;
  ~Runtime();

 private:
  explicit Runtime(bool active) : active_(active) {}

  bool active_ = false;
};
}  // namespace mustang
