#pragma once

// Umbrella for PhotonLibOS public headers.
//
// Photon's headers are not -Wextra clean and we must keep walleye code
// warning-free without modifying the vendored tree (styleguide: warnings
// may only be disabled for third-party code). Including them through this
// umbrella silences clang diagnostics *inside* the photon headers only;
// diagnostics pop back on for walleye code that follows.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wdeprecated-copy"

#include "photon/common/alog-stdstring.h"
#include "photon/common/alog.h"
#include "photon/net/socket.h"
#include "photon/photon.h"
#include "photon/thread/thread.h"
#include "photon/thread/thread11.h"
// Internal photon headers not (yet) exported under include/photon/; they
// resolve through the photon library's exported root include path.
#include "net/http/huffman/codec.h"
#include "net/http/streams.h"

#pragma clang diagnostic pop
