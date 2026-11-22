#pragma once

// Umbrella for PhotonLibOS public headers.
//
// Photon's headers are not -Wextra clean and we must keep mustang code
// warning-free without modifying the vendored tree (styleguide: warnings
// may only be disabled for third-party code). Including them through this
// umbrella silences clang diagnostics *inside* the photon headers only;
// diagnostics pop back on for mustang code that follows.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wdeprecated-copy"

#include "photon/common/alog-stdstring.h"
#include "photon/common/alog.h"
#include "photon/photon.h"
#include "photon/thread/thread.h"
#include "photon/thread/thread11.h"

#pragma clang diagnostic pop
