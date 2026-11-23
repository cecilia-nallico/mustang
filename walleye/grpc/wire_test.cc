#include "walleye/grpc/wire.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "gtest/gtest.h"

namespace walleye {
namespace grpc {
namespace {

std::string Bytes(std::initializer_list<int> bytes) {
  std::string out;
  for (int b : bytes) {
    out.push_back(static_cast<char>(b));
  }
  return out;
}

// --- grpc-timeout (R4) ---

TEST(ParseTimeoutTest, AcceptsValidValues) {
  EXPECT_EQ(ParseTimeout("1H").value(), absl::Hours(1));
  EXPECT_EQ(ParseTimeout("2M").value(), absl::Minutes(2));
  EXPECT_EQ(ParseTimeout("1S").value(), absl::Seconds(1));
  EXPECT_EQ(ParseTimeout("1500m").value(), absl::Milliseconds(1500));
  EXPECT_EQ(ParseTimeout("1500000u").value(), absl::Microseconds(1500000));
  EXPECT_EQ(ParseTimeout("99999999n").value(), absl::Nanoseconds(99999999));
  EXPECT_EQ(ParseTimeout("99999999S").value(), absl::Seconds(99999999));  // 8 digits max
  EXPECT_EQ(ParseTimeout("01S").value(), absl::Seconds(1));               // leading zeros are fine
}

TEST(ParseTimeoutTest, RejectsInvalidValues) {
  const char* kBad[] = {
      "",
      "1",
      "1X",
      "0S",
      "1s",
      "-1S",
      "+1S",
      "1.5S",
      " 1S",
      "1S ",
      "123456789S",  // 9 digits
      "1H1",
      "S1",
      "1u1",
  };
  for (const char* bad : kBad) {
    EXPECT_FALSE(ParseTimeout(bad).ok()) << bad;
  }
}

TEST(FormatTimeoutTest, PicksExactLargestUnitWithinEightDigits) {
  EXPECT_EQ(FormatTimeout(absl::Seconds(1)), "1S");
  EXPECT_EQ(FormatTimeout(absl::Milliseconds(1500)), "1500m");
  EXPECT_EQ(FormatTimeout(absl::Hours(3)), "3H");
  EXPECT_EQ(FormatTimeout(absl::Nanoseconds(250)), "250n");
  EXPECT_EQ(FormatTimeout(absl::ZeroDuration()), "1n");  // clamped, positive
  EXPECT_EQ(FormatTimeout(absl::Nanoseconds(1500)), "1500n");
  EXPECT_EQ(FormatTimeout(absl::Seconds(99999999)), "99999999S");
  // 99999999S + 1s exceeds the wire format entirely: clamps to the max.
  EXPECT_EQ(FormatTimeout(absl::Seconds(100000000)), "99999999H");
  EXPECT_EQ(FormatTimeout(absl::Hours(100000000)), "99999999H");
}

TEST(TimeoutRoundtripTest, ParseFormatAgree) {
  const absl::Duration values[] = {
      absl::Nanoseconds(1),
      absl::Microseconds(42),
      absl::Milliseconds(1200),
      absl::Seconds(30),
      absl::Minutes(7),
      absl::Hours(12),
      absl::Seconds(99999999),
  };
  for (absl::Duration value : values) {
    std::string formatted = FormatTimeout(value);
    absl::StatusOr<absl::Duration> parsed = ParseTimeout(formatted);
    ASSERT_TRUE(parsed.ok()) << formatted;
    EXPECT_EQ(*parsed, value) << formatted;
  }
}

// --- grpc-message percent-encoding (R16) ---

TEST(PercentTest, EncodeEscapesReservedBytes) {
  // %x20-%24 and %x26-%7E stay raw: that includes space (unlike URI
  // encoding) and excludes '%'.
  EXPECT_EQ(PercentEncode("hello world"), "hello world");
  EXPECT_EQ(PercentEncode("100%"), "100%25");
  EXPECT_EQ(PercentEncode("a/b"), "a/b");
  EXPECT_EQ(PercentEncode("tab\t"), "tab%09");
  EXPECT_EQ(PercentEncode("caf\xc3\xa9"), "caf%C3%A9");
  EXPECT_EQ(PercentEncode("$&"), "$&");
}

TEST(PercentTest, DecodeNeverErrors) {
  EXPECT_EQ(PercentDecode("hello%20world"), "hello world");
  EXPECT_EQ(PercentDecode("caf%C3%A9"), "caf\xc3\xa9");
  EXPECT_EQ(PercentDecode("caf%c3%a9"), "caf\xc3\xa9");  // lowercase hex ok
  // Invalid escapes pass through untouched (R16: must not error).
  EXPECT_EQ(PercentDecode("bad%zz"), "bad%zz");
  EXPECT_EQ(PercentDecode("bad%2"), "bad%2");
  EXPECT_EQ(PercentDecode("bad%"), "bad%");
  EXPECT_EQ(PercentDecode("%20%"), " %");  // %20 decodes, lone % stays raw
}

TEST(PercentTest, Roundtrip) {
  std::string weird = "\x01\x02 tip\n%caf\xc3\xa9~\x7f";
  EXPECT_EQ(PercentDecode(PercentEncode(weird)), weird);
}

// --- Length-Prefixed-Message framing (R8, R9) ---

TEST(FramingTest, EncodeMatchesWireFormat) {
  std::string frame = EncodeFrame("hi", false);
  EXPECT_EQ(frame, Bytes({0x00, 0x00, 0x00, 0x00, 0x02, 'h', 'i'}));
  std::string zframe = EncodeFrame("z", true);
  EXPECT_EQ(zframe, Bytes({0x01, 0x00, 0x00, 0x00, 0x01, 'z'}));
  std::string empty = EncodeFrame("", false);
  EXPECT_EQ(empty, Bytes({0x00, 0x00, 0x00, 0x00, 0x00}));
}

TEST(FramingTest, DecodesSingleAndMultipleMessages) {
  MessageDecoder decoder;
  std::string data = EncodeFrame("one") + EncodeFrame("") + EncodeFrame("three");
  absl::string_view in = data;

  absl::StatusOr<std::optional<MessageFrame>> f1 = decoder.Next(&in);
  ASSERT_TRUE(f1.ok());
  ASSERT_TRUE(f1->has_value());
  EXPECT_EQ((*f1)->payload, "one");

  absl::StatusOr<std::optional<MessageFrame>> f2 = decoder.Next(&in);
  ASSERT_TRUE(f2.ok());
  ASSERT_TRUE(f2->has_value());
  EXPECT_EQ((*f2)->payload, "");

  absl::StatusOr<std::optional<MessageFrame>> f3 = decoder.Next(&in);
  ASSERT_TRUE(f3.ok());
  ASSERT_TRUE(f3->has_value());
  EXPECT_EQ((*f3)->payload, "three");
  EXPECT_TRUE(in.empty());
}

TEST(FramingTest, ReassemblesAcrossArbitraryChunkBoundaries) {
  // R9: feed one byte at a time; the 5-byte prefix and payload may split
  // anywhere.
  MessageDecoder decoder;
  std::string data = EncodeFrame(std::string(1000, 'x'), true) + EncodeFrame("t");
  absl::string_view in = data;
  int completions = 0;
  while (!in.empty()) {
    absl::string_view one = in.substr(0, 1);
    in.remove_prefix(1);
    absl::StatusOr<std::optional<MessageFrame>> frame = decoder.Next(&one);
    ASSERT_TRUE(frame.ok());
    if (frame->has_value()) {
      if (completions == 0) {
        EXPECT_TRUE((*frame)->compressed);
        EXPECT_EQ((*frame)->payload.size(), 1000u);
      } else {
        EXPECT_EQ((*frame)->payload, "t");
      }
      completions++;
    }
  }
  EXPECT_EQ(completions, 2);
}

TEST(FramingTest, NeedsMoreDataReturnsNullopt) {
  MessageDecoder decoder;
  std::string data = EncodeFrame("hello");
  absl::string_view partial = absl::string_view(data).substr(0, 6);
  absl::StatusOr<std::optional<MessageFrame>> frame = decoder.Next(&partial);
  ASSERT_TRUE(frame.ok());
  EXPECT_EQ(frame->has_value(), false);
}

TEST(FramingTest, OversizeMessageRejected) {
  MessageDecoder decoder(/*max_message_size=*/100);
  std::string data = EncodeFrame(std::string(200, 'x'));
  absl::string_view in = absl::string_view(data).substr(0, 5);  // header alone announces 200
  absl::StatusOr<std::optional<MessageFrame>> frame = decoder.Next(&in);
  ASSERT_FALSE(frame.ok());
  EXPECT_EQ(frame.status().code(), absl::StatusCode::kResourceExhausted);
}

// --- :path (R1) ---

TEST(ParsePathTest, SplitsServiceAndMethod) {
  absl::StatusOr<PathParts> parts = ParsePath("/pkg.Cache/Get");
  ASSERT_TRUE(parts.ok());
  EXPECT_EQ(parts->service, "pkg.Cache");
  EXPECT_EQ(parts->method, "Get");

  parts = ParsePath("/Svc/m");
  ASSERT_TRUE(parts.ok());
  EXPECT_EQ(parts->service, "Svc");
  EXPECT_EQ(parts->method, "m");
}

TEST(ParsePathTest, RejectsMalformedPaths) {
  const char* kBad[] = {"", "/", "no-slash", "/onlyservice/", "/onlymethod", "//m", "/a/b/c"};
  for (const char* bad : kBad) {
    EXPECT_FALSE(ParsePath(bad).ok()) << bad;
  }
}

// --- error mappings (R18, R20) ---

TEST(ErrorMappingTest, RstErrorCodesMapToGrpcStatus) {
  EXPECT_EQ(StatusCodeFromRstError(0), StatusCode::kInternal);            // NO_ERROR
  EXPECT_EQ(StatusCodeFromRstError(1), StatusCode::kInternal);            // PROTOCOL_ERROR
  EXPECT_EQ(StatusCodeFromRstError(2), StatusCode::kInternal);            // INTERNAL_ERROR
  EXPECT_EQ(StatusCodeFromRstError(3), StatusCode::kInternal);            // FLOW_CONTROL_ERROR
  EXPECT_EQ(StatusCodeFromRstError(4), StatusCode::kInternal);            // SETTINGS_TIMEOUT
  EXPECT_EQ(StatusCodeFromRstError(6), StatusCode::kInternal);            // FRAME_SIZE_ERROR
  EXPECT_EQ(StatusCodeFromRstError(7), StatusCode::kUnavailable);         // REFUSED_STREAM
  EXPECT_EQ(StatusCodeFromRstError(8), StatusCode::kCancelled);           // CANCEL
  EXPECT_EQ(StatusCodeFromRstError(9), StatusCode::kInternal);            // COMPRESSION_ERROR
  EXPECT_EQ(StatusCodeFromRstError(10), StatusCode::kInternal);           // CONNECT_ERROR
  EXPECT_EQ(StatusCodeFromRstError(11), StatusCode::kResourceExhausted);  // ENHANCE_YOUR_CALM
  EXPECT_EQ(StatusCodeFromRstError(12), StatusCode::kPermissionDenied);   // INADEQUATE_SECURITY
  EXPECT_EQ(StatusCodeFromRstError(99), StatusCode::kInternal);           // unknown
}

TEST(ErrorMappingTest, HttpStatusMapsWhenGrpcStatusMissing) {
  EXPECT_EQ(StatusCodeFromHttp(400), StatusCode::kInternal);
  EXPECT_EQ(StatusCodeFromHttp(401), StatusCode::kUnauthenticated);
  EXPECT_EQ(StatusCodeFromHttp(403), StatusCode::kPermissionDenied);
  EXPECT_EQ(StatusCodeFromHttp(404), StatusCode::kUnimplemented);
  EXPECT_EQ(StatusCodeFromHttp(429), StatusCode::kUnavailable);
  EXPECT_EQ(StatusCodeFromHttp(502), StatusCode::kUnavailable);
  EXPECT_EQ(StatusCodeFromHttp(503), StatusCode::kUnavailable);
  EXPECT_EQ(StatusCodeFromHttp(504), StatusCode::kUnavailable);
  EXPECT_EQ(StatusCodeFromHttp(200), StatusCode::kUnknown);
  EXPECT_EQ(StatusCodeFromHttp(418), StatusCode::kUnknown);
}

}  // namespace
}  // namespace grpc
}  // namespace walleye
