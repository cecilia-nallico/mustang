#include "mustang/greet.h"

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

namespace mustang {
namespace {
constexpr char kEmptyNameError[] = "name must not be empty";
}  // namespace

absl::StatusOr<std::string> Greet(absl::string_view name) {
  if (name.empty()) {
    return absl::InvalidArgumentError(kEmptyNameError);
  }
  return absl::StrCat("Hello, ", name, "!");
}
}  // namespace mustang
