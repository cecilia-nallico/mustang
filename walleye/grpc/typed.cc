#include "walleye/grpc/typed.h"

#include "absl/strings/str_cat.h"

#include "walleye/photon_headers.h"

namespace walleye {
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

StatusCode ToGrpcCode(absl::StatusCode code) {
  switch (code) {
    case absl::StatusCode::kOk:
      return StatusCode::kOk;
    case absl::StatusCode::kCancelled:
      return StatusCode::kCancelled;
    case absl::StatusCode::kUnknown:
      return StatusCode::kUnknown;
    case absl::StatusCode::kInvalidArgument:
      return StatusCode::kInvalidArgument;
    case absl::StatusCode::kDeadlineExceeded:
      return StatusCode::kDeadlineExceeded;
    case absl::StatusCode::kNotFound:
      return StatusCode::kNotFound;
    case absl::StatusCode::kAlreadyExists:
      return StatusCode::kAlreadyExists;
    case absl::StatusCode::kPermissionDenied:
      return StatusCode::kPermissionDenied;
    case absl::StatusCode::kResourceExhausted:
      return StatusCode::kResourceExhausted;
    case absl::StatusCode::kFailedPrecondition:
      return StatusCode::kFailedPrecondition;
    case absl::StatusCode::kAborted:
      return StatusCode::kAborted;
    case absl::StatusCode::kOutOfRange:
      return StatusCode::kOutOfRange;
    case absl::StatusCode::kUnimplemented:
      return StatusCode::kUnimplemented;
    case absl::StatusCode::kInternal:
      return StatusCode::kInternal;
    case absl::StatusCode::kUnavailable:
      return StatusCode::kUnavailable;
    case absl::StatusCode::kDataLoss:
      return StatusCode::kDataLoss;
    case absl::StatusCode::kUnauthenticated:
      return StatusCode::kUnauthenticated;
    case absl::StatusCode::kDoNotUseReservedForFutureExpansionUseDefaultInSwitchInstead_:
      break;
  }
  return StatusCode::kUnknown;
}

void FinishQuietly(ServerCall* call, StatusCode code, const std::string& message, std::vector<HeaderField> metadata) {
  absl::Status st = call->Finish(code, message, std::move(metadata));
  if (!st.ok()) {
    // Transport-level failure while writing final trailers: the call is
    // over either way, log and move on.
    LOG_WARN("grpc: finishing call failed: `", std::string(st.message()));
  }
}

absl::Status ToAbslStatus(const CallResult& result) {
  if (result.ok()) {
    return absl::OkStatus();
  }
  return absl::Status(ToAbslCode(result.code), result.message);
}

}  // namespace grpc
}  // namespace walleye
