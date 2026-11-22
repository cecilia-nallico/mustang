#include "gtest/gtest.h"

#include <cstdint>

#include "absl/status/status.h"

#include "mustang/photon_headers.h"

#include "mustang/runtime.h"

namespace mustang {
namespace {

TEST(RuntimeTest, RejectsInvalidLogLevel) {
  RuntimeOptions options;
  options.log_level = 42;
  absl::StatusOr<Runtime> runtime = Runtime::Create(options);
  ASSERT_FALSE(runtime.ok());
  EXPECT_EQ(runtime.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(RuntimeTest, MapsEventEngineNames) {
  EXPECT_EQ(EventEngineFromName("auto").value_or(0),
            photon::INIT_EVENT_DEFAULT);
  EXPECT_EQ(EventEngineFromName("iouring").value_or(0),
            photon::INIT_EVENT_IOURING);
  EXPECT_EQ(EventEngineFromName("epoll").value_or(0), photon::INIT_EVENT_EPOLL);
  EXPECT_FALSE(EventEngineFromName("io_uring!").ok());
}

TEST(RuntimeTest, RunsCoroutines) {
  absl::StatusOr<Runtime> runtime = Runtime::Create(RuntimeOptions{});
  ASSERT_TRUE(runtime.ok());

  // The coroutine sleeps on the photon timer wheel (woken by the event
  // loop), so this exercises scheduler, timer and logging at once.
  int counter = 0;
  photon::thread* worker = photon::thread_create11([&counter]() {
    photon::thread_usleep(10 * 1000);
    counter += 1;
    LOG_INFO("coroutine finished, counter `", counter);
  });
  photon::join_handle* worker_join = photon::thread_enable_join(worker, true);
  photon::thread_join(worker_join);

  EXPECT_EQ(counter, 1);
}

}  // namespace
}  // namespace mustang
