#pragma once

#include <string>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace mustang {

// Returns "Hello, <name>!" or InvalidArgument if the name is empty.
absl::StatusOr<std::string> Greet(absl::string_view name);

}  // namespace mustang
