#include "walleye/grpc/hpack.h"

#include <cstdint>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

#include "walleye/photon_headers.h"

namespace walleye {
namespace grpc {
namespace {

struct StaticEntry {
  const char* name;
  const char* value;
};

// RFC 7541 Appendix A, HPACK static table (index = position + 1).
constexpr StaticEntry kStaticTable[] = {
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
};
constexpr size_t kStaticTableSize = sizeof(kStaticTable) / sizeof(kStaticTable[0]);
// Well past any sane table/list size; guards against integer overflow abuse.
constexpr uint64_t kMaxInteger = 1ull << 32;

int StaticExactIndex(absl::string_view name, absl::string_view value) {
  for (size_t i = 0; i < kStaticTableSize; i++) {
    if (name == kStaticTable[i].name && value == kStaticTable[i].value) {
      return static_cast<int>(i + 1);
    }
  }
  return 0;
}

int StaticNameIndex(absl::string_view name) {
  for (size_t i = 0; i < kStaticTableSize; i++) {
    if (name == kStaticTable[i].name) {
      return static_cast<int>(i + 1);
    }
  }
  return 0;
}

void AppendInteger(std::string* out, uint8_t first_byte, uint8_t prefix_mask, uint64_t v) {
  if (v < prefix_mask) {
    out->push_back(static_cast<char>(first_byte | v));
    return;
  }
  out->push_back(static_cast<char>(first_byte | prefix_mask));
  v -= prefix_mask;
  while (v >= 128) {
    out->push_back(static_cast<char>((v & 0x7f) | 0x80));
    v >>= 7;
  }
  out->push_back(static_cast<char>(v));
}

void AppendString(std::string* out, absl::string_view s) {
  size_t hlen = photon::net::http::huffman_encoded_length(s);
  if (hlen < s.size()) {
    AppendInteger(out, 0x80, 0x7f, hlen);
    size_t old_size = out->size();
    out->resize(old_size + hlen);
    photon::net::http::huffman_encode(s, out->data() + old_size, hlen);
  } else {
    AppendInteger(out, 0x00, 0x7f, s.size());
    out->append(s.data(), s.size());
  }
}

absl::Status Truncated(std::string_view what) {
  return absl::InvalidArgumentError(absl::StrCat("truncated HPACK ", what));
}

absl::StatusOr<uint64_t> TakeInteger(absl::string_view* in, uint8_t prefix_mask) {
  uint8_t b = static_cast<uint8_t>(in->front());
  in->remove_prefix(1);
  uint64_t v = b & prefix_mask;
  if (v < prefix_mask) {
    return v;
  }
  int shift = 0;
  while (true) {
    if (in->empty()) {
      return Truncated("integer");
    }
    uint8_t c = static_cast<uint8_t>(in->front());
    in->remove_prefix(1);
    v += static_cast<uint64_t>(c & 0x7f) << shift;
    if (v > kMaxInteger) {
      return absl::InvalidArgumentError("HPACK integer too large");
    }
    if ((c & 0x80) == 0) {
      return v;
    }
    shift += 7;
    if (shift > 56) {
      return absl::InvalidArgumentError("HPACK integer overflow");
    }
  }
}

absl::StatusOr<std::string> TakeString(absl::string_view* in) {
  if (in->empty()) {
    return Truncated("string");
  }
  bool huffman = (static_cast<uint8_t>(in->front()) & 0x80) != 0;
  absl::StatusOr<uint64_t> len = TakeInteger(in, 0x7f);
  if (!len.ok()) {
    return len.status();
  }
  if (*len > in->size()) {
    return Truncated("string");
  }
  absl::string_view raw = in->substr(0, *len);
  in->remove_prefix(*len);
  if (!huffman) {
    return std::string(raw);
  }
  // Shortest huffman code is 5 bits, so decoded size is at most 8/5 of the
  // encoded size; 2x + slack is a safe buffer.
  std::string out;
  out.resize(static_cast<size_t>(*len) * 2 + 8);
  ssize_t n = photon::net::http::huffman_decode(raw, out.data(), out.size());
  if (n < 0) {
    return absl::InvalidArgumentError("invalid HPACK huffman string");
  }
  out.resize(static_cast<size_t>(n));
  return out;
}

}  // namespace

absl::StatusOr<std::string> HpackEncode(absl::Span<const HeaderField> fields) {
  std::string out;
  for (const HeaderField& field : fields) {
    int exact = StaticExactIndex(field.name, field.value);
    if (exact > 0) {
      AppendInteger(&out, 0x80, 0x7f, exact);
      continue;
    }
    int name_index = StaticNameIndex(field.name);
    AppendInteger(&out, 0x00, 0x0f, name_index);
    if (name_index == 0) {
      AppendString(&out, field.name);
    }
    AppendString(&out, field.value);
  }
  return out;
}

HpackDecoder::HpackDecoder(size_t max_header_table_size, size_t max_header_list_size)
    : max_header_list_size_(max_header_list_size), table_capacity_(max_header_table_size) {}

void HpackDecoder::InsertTable(const HeaderField& field) {
  size_t size = field.WireSize();
  if (size > table_capacity_) {
    // RFC 7541 4.4: an entry larger than the table empties it and is not
    // inserted, but is still delivered to the application.
    while (!table_.empty()) {
      table_size_ -= table_.back().size;
      table_.pop_back();
    }
    return;
  }
  while (table_size_ + size > table_capacity_) {
    table_size_ -= table_.back().size;
    table_.pop_back();
  }
  table_.push_front(Entry{field.name, field.value, size});
  table_size_ += size;
}

absl::Status HpackDecoder::LookupIndex(uint64_t index, HeaderField* out) const {
  if (index == 0) {
    return absl::InvalidArgumentError("HPACK index 0 is invalid");
  }
  if (index <= kStaticTableSize) {
    out->name = kStaticTable[index - 1].name;
    out->value = kStaticTable[index - 1].value;
    return absl::OkStatus();
  }
  uint64_t dynamic = index - kStaticTableSize - 1;  // 62 -> most recent
  if (dynamic >= table_.size()) {
    return absl::InvalidArgumentError(absl::StrCat("HPACK index ", index, " beyond tables"));
  }
  const Entry& entry = table_[dynamic];
  out->name = entry.name;
  out->value = entry.value;
  return absl::OkStatus();
}

absl::Status HpackDecoder::Decode(absl::string_view block, std::vector<HeaderField>* out) {
  absl::string_view in = block;
  size_t list_size = 0;
  while (!in.empty()) {
    uint8_t b = static_cast<uint8_t>(in.front());

    if ((b & 0xE0) == 0x20) {  // dynamic table size update
      absl::StatusOr<uint64_t> size = TakeInteger(&in, 0x1f);
      if (!size.ok()) {
        return size.status();
      }
      if (*size > table_capacity_) {
        // May not exceed the SETTINGS_HEADER_TABLE_SIZE we advertise.
        return absl::InvalidArgumentError("HPACK table size update above setting");
      }
      while (table_size_ > *size) {
        table_size_ -= table_.back().size;
        table_.pop_back();
      }
      continue;
    }

    HeaderField field;
    bool incremental_indexing = (b & 0xC0) == 0x40;
    uint8_t name_mask = incremental_indexing ? 0x3f : (b & 0x80) ? 0x7f : 0x0f;

    if (b & 0x80) {  // indexed header field
      absl::StatusOr<uint64_t> index = TakeInteger(&in, 0x7f);
      if (!index.ok()) {
        return index.status();
      }
      absl::Status st = LookupIndex(*index, &field);
      if (!st.ok()) {
        return st;
      }
    } else {  // literal (with/without indexing, never-indexed)
      absl::StatusOr<uint64_t> index = TakeInteger(&in, name_mask);
      if (*index > 0) {
        absl::Status st = LookupIndex(*index, &field);
        if (!st.ok()) {
          return st;
        }
        field.value.clear();  // name-only reference
      } else {
        absl::StatusOr<std::string> name = TakeString(&in);
        if (!name.ok()) {
          return name.status();
        }
        field.name = *std::move(name);
      }
      absl::StatusOr<std::string> value = TakeString(&in);
      if (!value.ok()) {
        return value.status();
      }
      field.value = *std::move(value);
      if (incremental_indexing) {
        InsertTable(field);
      }
    }

    list_size += field.WireSize();
    if (list_size > max_header_list_size_) {
      return absl::ResourceExhaustedError("header list size above limit (R7)");
    }
    out->push_back(std::move(field));
  }
  return absl::OkStatus();
}

}  // namespace grpc
}  // namespace walleye
