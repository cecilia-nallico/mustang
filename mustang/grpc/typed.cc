#include "mustang/grpc/typed.h"

namespace mustang {
namespace grpc {

absl::StatusCode ToAbslCode(StatusCode code) {
  switch (code) {
    case StatusCode::kOk:
      return absl::StatusCode::kOk;
    case StatusCode::kCancelled:
      return absl::StatusCode::kCancelled;
    case StatusCode::kUnknown:
      return absl::StatusCode::kUnknown;
    case StatusCode::kInvalidArgument:
      return absl::StatusCode::kInvalidArgument;
    case StatusCode::kDeadlineExceeded:
      return absl::StatusCode::kDeadlineExceeded;
    case StatusCode::kNotFound:
      return absl::StatusCode::kNotFound;
    case StatusCode::kAlreadyExists:
      return absl::StatusCode::kAlreadyExists;
    case StatusCode::kPermissionDenied:
      return absl::StatusCode::kPermissionDenied;
    case StatusCode::kResourceExhausted:
      return absl::StatusCode::kResourceExhausted;
    case StatusCode::kFailedPrecondition:
      return absl::StatusCode::kFailedPrecondition;
    case StatusCode::kAborted:
      return absl::StatusCode::kAborted;
    case StatusCode::kOutOfRange:
      return absl::StatusCode::kOutOfRange;
    case StatusCode::kUnimplemented:
      return absl::StatusCode::kUnimplemented;
    case StatusCode::kInternal:
      return absl::StatusCode::kInternal;
    case StatusCode::kUnavailable:
      return absl::StatusCode::kUnavailable;
    case StatusCode::kDataLoss:
      return absl::StatusCode::kDataLoss;
    case StatusCode::kUnauthenticated:
      return absl::StatusCode::kUnauthenticated;
  }
  return absl::StatusCode::kUnknown;
}

absl::Status ToAbslStatus(const CallResult& result) {
  if (result.ok()) {
    return absl::OkStatus();
  }
  return absl::Status(ToAbslCode(result.code), result.message);
}

}  // namespace grpc
}  // namespace mustang
