#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace mustang {
namespace grpc {
// Ordered metadata field. gRPC requires that order and duplicate names are
// preserved end to end (protocol rule R5), so this is a list, not a map.
struct HeaderField {
  std::string name;
  std::string value;

  // gRPC header-list accounting (rule R7): name + value + 32 bytes.
  size_t WireSize() const { return name.size() + value.size() + 32; }
};

// Stateless HPACK encoder (RFC 7541): exact static-table matches become
// indexed fields, everything else is a literal with static name indexing
// when possible, strings huffman-coded when shorter. It never inserts into
// the dynamic table, so the encoder needs no state and every conforming
// decoder stays in sync with it.
absl::StatusOr<std::string> HpackEncode(absl::Span<const HeaderField> fields);

// Stateful HPACK decoder, one instance per connection. Complete: indexed
// fields (static and dynamic), literals with and without incremental
// indexing, never-indexed literals, dynamic-table size updates, huffman
// strings, insertion with eviction, header-list size limit (R7).
class HpackDecoder {
 public:
  // max_header_table_size: the SETTINGS_HEADER_TABLE_SIZE we advertise;
  // larger table size updates are a decoding error. max_header_list_size:
  // R7 cap on the decoded list, 8 KiB per the gRPC spec default.
  explicit HpackDecoder(size_t max_header_table_size = 4096, size_t max_header_list_size = 8192);

  HpackDecoder(const HpackDecoder&) = delete;
  HpackDecoder& operator=(const HpackDecoder&) = delete;

  // Appends decoded fields to *out (caller clears between blocks if
  // desired); the dynamic table persists across blocks.
  absl::Status Decode(absl::string_view block, std::vector<HeaderField>* out);

 private:
  struct Entry {
    std::string name;
    std::string value;
    size_t size;  // name + value + 32
  };

  void InsertTable(const HeaderField& field);
  absl::Status LookupIndex(uint64_t index, HeaderField* out) const;

  const size_t max_header_list_size_;
  size_t table_capacity_;  // current limit (settable via size updates)
  size_t table_size_ = 0;
  // front = most recently inserted = HPACK index 62.
  std::deque<Entry> table_;
};

}  // namespace grpc
}  // namespace mustang
