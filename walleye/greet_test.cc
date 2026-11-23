#include "walleye/greet.h"

#include "absl/status/status.h"
#include "gtest/gtest.h"

namespace walleye {
namespace {
TEST(GreetTest, GreetsByName) {
  absl::StatusOr<std::string> greeting = Greet("photon");
  ASSERT_TRUE(greeting.ok());
  EXPECT_EQ(*greeting, "Hello, photon!");
}

TEST(GreetTest, RejectsEmptyName) {
  absl::StatusOr<std::string> greeting = Greet("");
  ASSERT_FALSE(greeting.ok());
  EXPECT_EQ(greeting.status().code(), absl::StatusCode::kInvalidArgument);
}
}  // namespace
}  // namespace walleye
