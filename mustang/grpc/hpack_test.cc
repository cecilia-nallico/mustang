#include "mustang/grpc/hpack.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "gtest/gtest.h"

namespace mustang {
namespace grpc {
namespace {

// Fresh decoder per test; table state persists across Decode calls, like a
// connection.
struct Hpack {
  HpackDecoder decoder;
  std::vector<HeaderField> fields;

  absl::Status Decode(absl::string_view block) { return decoder.Decode(block, &fields); }
};

HeaderField F(std::string name, std::string value) { return HeaderField{std::move(name), std::move(value)}; }

std::string Bytes(std::initializer_list<int> bytes) {
  std::string out;
  for (int b : bytes) {
    out.push_back(static_cast<char>(b));
  }
  return out;
}

// Literal-with-indexing insert of a (name, value) pair of small strings.
std::string Insert(std::string name, std::string value) {
  std::string block{char(0x40)};
  block.push_back(static_cast<char>(name.size()));
  block += name;
  block.push_back(static_cast<char>(value.size()));
  block += value;
  return block;
}

TEST(HpackEncodeTest, StaticExactMatchBecomesIndexedField) {
  absl::StatusOr<std::string> block = HpackEncode({F(":method", "POST")});
  ASSERT_TRUE(block.ok());
  // Indexed field, static index 3 (:method POST) => single byte 0x83.
  EXPECT_EQ(*block, Bytes({0x83}));
}

TEST(HpackEncodeTest, LiteralUsesNameIndex) {
  absl::StatusOr<std::string> block = HpackEncode({F(":path", "/pkg.Service/Get")});
  ASSERT_TRUE(block.ok());
  // Literal without indexing, name index 4 (:path): 0000 0100.
  EXPECT_EQ((*block)[0], '\x04');
}

TEST(HpackEncodeDecodeTest, RoundtripPreservesOrderAndDuplicates) {
  const std::vector<HeaderField> in = {
      F(":method", "POST"),
      F(":path", "/svc/M"),
      F("k", "v"),
      F("k", "v2"),
      F("x-bin", "AAAA"),
  };
  absl::StatusOr<std::string> block = HpackEncode(in);
  ASSERT_TRUE(block.ok());
  Hpack h;
  EXPECT_TRUE(h.Decode(*block).ok());
  ASSERT_EQ(h.fields.size(), in.size());
  for (size_t i = 0; i < in.size(); i++) {
    EXPECT_EQ(h.fields[i].name, in[i].name);
    EXPECT_EQ(h.fields[i].value, in[i].value);
  }
}

TEST(HpackDecodeTest, EmptyBlockYieldsNoFields) {
  Hpack h;
  EXPECT_TRUE(h.Decode("").ok());
  EXPECT_TRUE(h.fields.empty());
}

TEST(HpackDecodeTest, StaticIndexedField) {
  Hpack h;
  // 0x82 => static index 2 (:method GET); 0x84 => index 4 (:path "/").
  EXPECT_TRUE(h.Decode(Bytes({0x82, 0x84})).ok());
  ASSERT_EQ(h.fields.size(), 2u);
  EXPECT_EQ(h.fields[0].name, ":method");
  EXPECT_EQ(h.fields[0].value, "GET");
  EXPECT_EQ(h.fields[1].name, ":path");
  EXPECT_EQ(h.fields[1].value, "/");
}

TEST(HpackDecodeTest, ZeroIndexRejected) {
  Hpack h;
  EXPECT_FALSE(h.Decode(Bytes({0x80})).ok());
}

TEST(HpackDecodeTest, IndexBeyondTablesRejected) {
  Hpack h;
  // 0xFF 0xA0 => index 127 + 32*128 = 4223, beyond static + empty dynamic.
  EXPECT_FALSE(h.Decode(Bytes({0xFF, 0xA0})).ok());
}

TEST(HpackDecodeTest, LiteralWithIncrementalIndexingInserts) {
  Hpack h;
  EXPECT_TRUE(h.Decode(Insert("ab", "1")).ok());
  ASSERT_EQ(h.fields.size(), 1u);
  EXPECT_EQ(h.fields[0].name, "ab");
  EXPECT_EQ(h.fields[0].value, "1");

  // Indexed field 62 must now resolve to the inserted entry.
  h.fields.clear();
  EXPECT_TRUE(h.Decode(Bytes({0xBE})).ok());
  ASSERT_EQ(h.fields.size(), 1u);
  EXPECT_EQ(h.fields[0].name, "ab");
  EXPECT_EQ(h.fields[0].value, "1");
}

TEST(HpackDecodeTest, DynamicTableEviction) {
  Hpack h;
  // (aa,1), (bb,2), (cc,3): entry size 2+1+32 = 35 each, total 105.
  EXPECT_TRUE(h.Decode(Insert("aa", "1")).ok());
  EXPECT_TRUE(h.Decode(Insert("bb", "2")).ok());
  EXPECT_TRUE(h.Decode(Insert("cc", "3")).ok());
  h.fields.clear();
  // 64 = (aa,1), 63 = (bb,2), 62 = (cc,3): higher index = older entry.
  EXPECT_TRUE(h.Decode(Bytes({0xC0, 0xBF, 0xBE})).ok());
  ASSERT_EQ(h.fields.size(), 3u);
  EXPECT_EQ(h.fields[0].value, "1");
  EXPECT_EQ(h.fields[1].value, "2");
  EXPECT_EQ(h.fields[2].value, "3");

  // Insert an entry larger than the whole table (2 + 4070 + 32 > 4096):
  // per RFC 7541 4.4 the table is emptied and the entry is NOT inserted,
  // but the field is still delivered. String length 4070 encodes as
  // 7-bit continuation 0x7F 0xE7 0x1E (4070 = 127 + 103*128 + 30*128^2).
  std::string big(4070, 'x');
  std::string big_block = Bytes({0x40, 0x02, 'z', 'z', 0x7F, 0xE7, 0x1E});
  big_block += big;
  h.fields.clear();
  EXPECT_TRUE(h.Decode(big_block).ok());
  ASSERT_EQ(h.fields.size(), 1u);
  EXPECT_EQ(h.fields[0].name, "zz");

  // Everything older was evicted by the oversized insert.
  h.fields.clear();
  EXPECT_FALSE(h.Decode(Bytes({0xBE})).ok());
}

TEST(HpackDecodeTest, TableSizeUpdateAcceptedAndApplied) {
  Hpack h;
  EXPECT_TRUE(h.Decode(Insert("aa", "1")).ok());
  // Dynamic table size update to 0 (0x20): evicts everything.
  EXPECT_TRUE(h.Decode(Bytes({0x20})).ok());
  h.fields.clear();
  EXPECT_FALSE(h.Decode(Bytes({0xBE})).ok());
}

TEST(HpackDecodeTest, TableSizeUpdateAboveSettingRejected) {
  Hpack h;
  // Size update to 5000 > 4096: 5000 = 31 + 4969*1... encoded 0x3F then
  // 7-bit continuation of (5000-31): 4969 & 0x7F = 0x69 => 0xE9, 4969>>7 = 38.
  EXPECT_FALSE(h.Decode(Bytes({0x3F, 0xE9, 0x26})).ok());
}

TEST(HpackDecodeTest, NeverIndexedLiteral) {
  Hpack h;
  // Never-indexed literal, name index 1 (:authority): 0001 0001, value "x".
  EXPECT_TRUE(h.Decode(Bytes({0x11, 0x01, 'x'})).ok());
  ASSERT_EQ(h.fields.size(), 1u);
  EXPECT_EQ(h.fields[0].name, ":authority");
  EXPECT_EQ(h.fields[0].value, "x");
}

TEST(HpackDecodeTest, HuffmanStringFromRfcVectors) {
  Hpack h;
  // RFC 7541 C.1: huffman("www.example.com") = f1 e3 c2 e5 f2 3a 6b a0 ab
  // 90 f4 ff (12 bytes). Literal without indexing, new name "m".
  EXPECT_TRUE(
      h.Decode(Bytes({0x00, 0x01, 'm', 0x8C, 0xF1, 0xE3, 0xC2, 0xE5, 0xF2, 0x3A, 0x6B, 0xA0, 0xAB, 0x90, 0xF4, 0xFF}))
          .ok());
  ASSERT_EQ(h.fields.size(), 1u);
  EXPECT_EQ(h.fields[0].name, "m");
  EXPECT_EQ(h.fields[0].value, "www.example.com");
}

TEST(HpackDecodeTest, InvalidHuffmanRejected) {
  Hpack h;
  // Huffman flag, 6 bytes of 0xFF: a run of ones longer than 7 bits is
  // the EOS prefix and must never appear inside a string (RFC 7541 5.2).
  EXPECT_FALSE(h.Decode(Bytes({0x00, 0x01, 'm', 0x86, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF})).ok());
}

TEST(HpackDecodeTest, TruncatedBlockRejected) {
  Hpack h;
  EXPECT_FALSE(h.Decode(Bytes({0x40, 0x05, 'a'})).ok());             // name len 5, 1 byte
  EXPECT_FALSE(h.Decode(Bytes({0x40, 0x02, 'a', 'b', 0x7F})).ok());  // value len 127
}

TEST(HpackDecodeTest, HeaderListSizeLimitEnforced) {
  Hpack h;
  // Three 4000-byte inserts in one block: 3 * (1 + 4000 + 32) > 8192 (R7).
  std::string block;
  for (int i = 0; i < 3; i++) {
    block += Bytes({0x40, 0x01, 'k', 0x7F, 0xA1, 0x1E});  // len 4000 = 127 + 33*128 + 30
    block.append(4000, 'v');
  }
  EXPECT_EQ(h.Decode(block).code(), absl::StatusCode::kResourceExhausted);
}

}  // namespace
}  // namespace grpc
}  // namespace mustang
