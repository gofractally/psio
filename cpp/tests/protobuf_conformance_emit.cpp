// Emitter for conformance/protobuf-vectors.json: protobuf messages of the
// types in tests/protobuf_shapes.hpp, and bytes a reader must refuse.
//
//   psio_protobuf_conformance_emit <out.json>          write the fixture
//   psio_protobuf_conformance_emit --check <ref.json>  regenerate and compare
//
//   accept          bytes psio writes (what the official library writes for
//                   the value): a reader decodes them, and its writer gives
//                   the same bytes back
//   accept_other    bytes another writer may produce (unpacked, reordered,
//                   repeated, with unknown fields): accepted, and the value
//                   re-encodes to `canonical_hex`
//   reject          bytes that are refused, with the message and the offset
//   mutation_digests  the verdict (and message and offset) on `count`
//                   corruptions of an accepted row, folded into one number
//
// Corruptions (the same in every language): the generator is pb_rng of
// tests/protobuf_random.hpp — x = seed * 0x9e3779b97f4a7c15 + 1; next():
// x ^= x << 13; x ^= x >> 7; x ^= x << 17; below(n) = next() % n — and each
// corruption starts from the row's bytes and applies 1 + below(3) edits,
// each chosen by below(5): 0 replace (at = below(len), then value =
// below(256)); 1 flip (at = below(len), then bit = below(8)); 2 truncate to
// below(len + 1); 3 insert (at = below(len + 1), then value = below(256));
// 4 remove at below(len). An edit of an empty buffer other than 2 and 3
// draws nothing. The digest is FNV-1a 64 over, per corruption: 1 and
// nothing else when accepted; 0, the message's bytes, and the offset as
// four little-endian bytes when refused.
//
// Every row is checked before it is written; output is deterministic.

#include <psio/protobuf.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "protobuf_random.hpp"
#include "protobuf_shapes.hpp"

namespace
{
   using Bytes = std::vector<char>;

   [[noreturn]] void die(const std::string& message)
   {
      std::fprintf(stderr, "protobuf conformance emit: %s\n", message.c_str());
      std::exit(2);
   }
   std::string to_hex(const Bytes& bytes)
   {
      std::string out;
      for (unsigned char c : bytes)
      {
         out += "0123456789abcdef"[c >> 4];
         out += "0123456789abcdef"[c & 15];
      }
      return out;
   }
   Bytes raw(std::initializer_list<unsigned> il)
   {
      Bytes out;
      for (unsigned v : il)
         out.push_back(static_cast<char>(v));
      return out;
   }

   std::string accept_rows, other_rows, reject_rows, digest_rows;
   std::size_t accepted = 0, others = 0, rejected = 0;

   void row(std::string& to, const std::string& text)
   {
      to += (to.empty() ? "\n    " : ",\n    ") + text;
   }

   template <typename T>
   void accept(const char* type, const std::string& name, const T& v)
   {
      const Bytes b = psio::to_protobuf(v);
      T           back{};
      if (!psio::protobuf::decode_checked(std::span<const char>{b}, back).ok() ||
          psio::to_protobuf(back) != b)
         die(std::string{type} + "/" + name + " does not round-trip");
      row(accept_rows, "{\"id\": \"" + std::string{type} + "/" + name + "\", \"type\": \"" + type +
                          "\", \"wire_hex\": \"" + to_hex(b) + "\"}");
      ++accepted;
   }

   template <typename T>
   void other(const char* type, const char* name, const Bytes& b)
   {
      T v{};
      if (!psio::protobuf::decode_checked(std::span<const char>{b}, v).ok())
         die(std::string{type} + "/" + name + " is not accepted");
      row(other_rows, "{\"id\": \"" + std::string{type} + "/" + name + "\", \"type\": \"" + type +
                         "\", \"wire_hex\": \"" + to_hex(b) + "\", \"canonical_hex\": \"" +
                         to_hex(psio::to_protobuf(v)) + "\"}");
      ++others;
   }

   template <typename T>
   void reject(const char* type, const char* name, const Bytes& b)
   {
      const auto st = psio::validate_protobuf<T>(std::span<const char>{b});
      if (st.ok())
         die(std::string{type} + "/" + name + " is accepted");
      row(reject_rows, "{\"id\": \"" + std::string{type} + "/" + name + "\", \"type\": \"" + type +
                          "\", \"error\": \"" + std::string{st.error().what} +
                          "\", \"at\": " + std::to_string(st.error().byte_offset) +
                          ", \"wire_hex\": \"" + to_hex(b) + "\"}");
      ++rejected;
   }

   Bytes corrupt(const Bytes& base, pb_rng& r)
   {
      Bytes               c     = base;
      const std::uint64_t edits = 1 + r.below(3);
      for (std::uint64_t e = 0; e < edits; ++e)
      {
         const std::uint64_t kind = r.below(5);
         if (kind == 2)
            c.resize(r.below(c.size() + 1));
         else if (kind == 3)
         {
            const std::uint64_t at    = r.below(c.size() + 1);
            const std::uint64_t value = r.below(256);
            c.insert(c.begin() + static_cast<std::ptrdiff_t>(at), static_cast<char>(value));
         }
         else if (c.empty())
            continue;
         else if (kind == 0)
         {
            const std::uint64_t at    = r.below(c.size());
            const std::uint64_t value = r.below(256);
            c[at]                     = static_cast<char>(value);
         }
         else if (kind == 1)
         {
            const std::uint64_t at  = r.below(c.size());
            const std::uint64_t bit = r.below(8);
            c[at]                   = static_cast<char>(c[at] ^ (1u << bit));
         }
         else
            c.erase(c.begin() + static_cast<std::ptrdiff_t>(r.below(c.size())));
      }
      return c;
   }

   template <typename T>
   void digest(const char* type, const char* base_name, const T& base_value, std::uint64_t seed,
               unsigned count)
   {
      const Bytes   base = psio::to_protobuf(base_value);
      pb_rng        r{seed};
      std::uint64_t h    = 0xcbf29ce484222325ull;
      std::uint64_t ok   = 0;
      const auto    fold = [&](unsigned char b) {
         h ^= b;
         h *= 0x100000001b3ull;
      };
      for (unsigned i = 0; i < count; ++i)
      {
         const Bytes c  = corrupt(base, r);
         const auto  st = psio::validate_protobuf<T>(std::span<const char>{c});
         if (st.ok())
         {
            fold(1);
            ++ok;
            continue;
         }
         fold(0);
         for (unsigned char ch : st.error().what)
            fold(ch);
         for (int k = 0; k < 4; ++k)
            fold(static_cast<unsigned char>(st.error().byte_offset >> (8 * k)));
      }
      char hex[17];
      std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
      row(digest_rows, "{\"type\": \"" + std::string{type} + "\", \"base\": \"" + type + "/" +
                          base_name + "\", \"seed\": " + std::to_string(seed) +
                          ", \"count\": " + std::to_string(count) + ", \"accepted\": " +
                          std::to_string(ok) + ", \"digest\": \"" + hex + "\"}");
   }

   // `count` random values (the first is the base of the type's digest).
   template <typename T>
   void family(const char* type, std::uint64_t seed, unsigned count)
   {
      accept(type, "zero", [] {
         T v{};
         psio::detail::pb_impl::clear(v);
         return v;
      }());
      pb_rng r{seed};
      T      richest{};
      for (unsigned i = 0; i < count; ++i)
      {
         const T v = pb_random<T>(r);
         accept(type, "random_" + std::to_string(i), v);
         if (psio::protobuf_size(v) > psio::protobuf_size(richest))
            richest = v;
      }
      accept(type, "base", richest);
      digest(type, "base", richest, seed + 100, 30000);
   }

   std::string generate()
   {
      family<PbPoint>("PbPoint", 1, 6);
      family<PbScalars>("PbScalars", 2, 24);
      family<PbEncodings>("PbEncodings", 3, 24);
      family<PbText>("PbText", 4, 12);
      family<PbOptionals>("PbOptionals", 5, 24);
      family<PbRepeated>("PbRepeated", 6, 24);
      family<PbMaps>("PbMaps", 7, 24);
      family<PbChoice>("PbChoice", 8, 24);
      family<PbRenumbered>("PbRenumbered", 9, 8);
      family<PbTree>("PbTree", 10, 16);
      family<PbDefaults>("PbDefaults", 11, 6);
      family<PbOrder>("PbOrder", 12, 24);
      accept("PbDefaults", "initialisers", PbDefaults{});
      accept("PbBounded", "at_limits", pb_bounded_at_limits());
      accept("PbBounded", "empty", PbBounded{});
      digest("PbBounded", "at_limits", pb_bounded_at_limits(), 113, 30000);

      // What another writer may produce.
      other<PbPoint>("PbPoint", "zero_written", raw({0x08, 0, 0x10, 0}));
      other<PbPoint>("PbPoint", "reversed", raw({0x10, 2, 0x08, 1}));
      other<PbPoint>("PbPoint", "last_wins", raw({0x08, 1, 0x08, 2}));
      other<PbPoint>("PbPoint", "overlong_varint", raw({0x08, 0x81, 0x80, 0x00}));
      other<PbPoint>("PbPoint", "int32_from_64_bits", raw({0x08, 0x81, 0x80, 0x80, 0x80, 0x10}));
      other<PbPoint>("PbPoint", "unknown_fields",
                     raw({0x08, 5, 0x98, 0x06, 1, 0x99, 0x06, 1, 2, 3, 4, 5, 6, 7, 8, 0x9a, 0x06, 2,
                          'h', 'i', 0x9d, 0x06, 1, 2, 3, 4, 0x9b, 0x06, 0x08, 1, 0x13, 0x14, 0x9c,
                          0x06}));
      other<PbPoint>("PbPoint", "another_wire_type", raw({0x08, 5, 0x15, 1, 0, 0, 0}));
      other<PbRepeated>("PbRepeated", "unpacked", raw({0x08, 1, 0x08, 2, 0x08, 3}));
      other<PbRepeated>("PbRepeated", "packed_and_unpacked", raw({0x08, 1, 0x0a, 2, 2, 3, 0x08, 4}));
      other<PbRepeated>("PbRepeated", "empty_packed", raw({0x0a, 0}));
      other<PbRepeated>("PbRepeated", "unpacked_floats", raw({0x25, 0, 0, 0x80, 0x3f}));
      other<PbRepeated>("PbRepeated", "bool_any_non_zero", raw({0x12, 3, 0, 2, 0x7f}));
      other<PbRepeated>("PbRepeated", "set_out_of_order", raw({0x4a, 3, 9, 1, 9}));
      other<PbMaps>("PbMaps", "value_then_key", raw({0x0a, 5, 0x10, 9, 0x0a, 1, 'k'}));
      other<PbMaps>("PbMaps", "no_key", raw({0x0a, 2, 0x10, 4}));
      other<PbMaps>("PbMaps", "no_value", raw({0x0a, 3, 0x0a, 1, 'z'}));
      other<PbMaps>("PbMaps", "empty_entry", raw({0x0a, 0}));
      other<PbMaps>("PbMaps", "key_twice",
                    raw({0x0a, 5, 0x0a, 1, 'k', 0x10, 1, 0x0a, 5, 0x0a, 1, 'k', 0x10, 2}));
      other<PbMaps>("PbMaps", "unknown_in_entry", raw({0x0a, 7, 0x0a, 1, 'k', 0x18, 1, 0x10, 5}));
      other<PbMaps>("PbMaps", "unordered",
                    raw({0x0a, 5, 0x0a, 1, 'b', 0x10, 1, 0x0a, 5, 0x0a, 1, 'a', 0x10, 2}));
      other<PbChoice>("PbChoice", "last_alternative", raw({0x32, 1, 'x', 0x28, 7}));
      other<PbChoice>("PbChoice", "merged_alternative",
                      raw({0x3a, 2, 0x08, 1, 0x3a, 2, 0x10, 2}));
      other<PbChoice>("PbChoice", "none_set", raw({0x08, 1}));
      other<PbOrder>("PbOrder", "merged_record", raw({0x12, 2, 0x08, 1, 0x12, 2, 0x10, 2}));
      other<PbOrder>("PbOrder", "empty_record_written", raw({0x12, 0, 0x42, 0}));
      other<PbOptionals>("PbOptionals", "present_zeros", raw({0x08, 0, 0x10, 0, 0x1a, 0, 0x2a, 0}));
      other<PbOptionals>("PbOptionals", "optional_twice", raw({0x2a, 2, 0x08, 1, 0x2a, 2, 0x10, 2}));
      other<PbScalars>("PbScalars", "open_enum", raw({0x68, 0x4d, 0x70, 0xc0, 0xc4, 0x07}));
      other<PbScalars>("PbScalars", "negative_int8",
                       raw({0x10, 0x80, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}));
      other<PbText>("PbText", "zero_quad_written", raw({0x22, 4, 0, 0, 0, 0}));
      other<PbText>("PbText", "bytes_not_utf8", raw({0x12, 2, 0xc3, 0x28}));
      other<PbTree>("PbTree", "group_100_deep", [] {
         Bytes b;
         for (int i = 0; i < 100; ++i)
            b.push_back(0x23);
         for (int i = 0; i < 100; ++i)
            b.push_back(0x24);
         return b;
      }());

      // What is refused.
      reject<PbPoint>("PbPoint", "truncated_tag", raw({0x88}));
      reject<PbPoint>("PbPoint", "truncated_varint", raw({0x08}));
      reject<PbPoint>("PbPoint", "truncated_varint_2", raw({0x08, 0x80}));
      reject<PbPoint>("PbPoint", "field_number_0", raw({0x00, 0x00}));
      reject<PbPoint>("PbPoint", "field_number_0_fixed", raw({0x05, 1, 2, 3, 4}));
      reject<PbPoint>("PbPoint", "wire_type_6", raw({0x0e}));
      reject<PbPoint>("PbPoint", "wire_type_7", raw({0x0f}));
      reject<PbPoint>("PbPoint", "varint_11_bytes",
                      raw({0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x81, 0x00}));
      reject<PbPoint>("PbPoint", "varint_65_bits",
                      raw({0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02}));
      reject<PbPoint>("PbPoint", "tag_33_bits", raw({0x88, 0x80, 0x80, 0x80, 0x10, 0x01}));
      reject<PbPoint>("PbPoint", "tag_6_bytes", raw({0x88, 0x80, 0x80, 0x80, 0x80, 0x00, 0x01}));
      reject<PbPoint>("PbPoint", "unknown_truncated_fixed64", raw({0x99, 0x06, 1, 2, 3}));
      reject<PbPoint>("PbPoint", "unknown_truncated_fixed32", raw({0x9d, 0x06, 1, 2, 3}));
      reject<PbPoint>("PbPoint", "unknown_length_past_end", raw({0x9a, 0x06, 5, 1, 2}));
      reject<PbPoint>("PbPoint", "unterminated_group", raw({0x9b, 0x06, 0x08, 1}));
      reject<PbPoint>("PbPoint", "group_ends_with_another_number", raw({0x9b, 0x06, 0x94, 0x06}));
      reject<PbPoint>("PbPoint", "end_group_alone", raw({0x0c}));
      reject<PbPoint>("PbPoint", "group_field_number_0", raw({0x9b, 0x06, 0x00}));
      reject<PbText>("PbText", "string_not_utf8", raw({0x0a, 2, 0xc3, 0x28}));
      reject<PbText>("PbText", "string_surrogate", raw({0x0a, 3, 0xed, 0xa0, 0x80}));
      reject<PbText>("PbText", "string_overlong", raw({0x0a, 2, 0xc0, 0x80}));
      reject<PbText>("PbText", "string_above_10ffff", raw({0x0a, 4, 0xf4, 0x90, 0x80, 0x80}));
      reject<PbText>("PbText", "string_truncated_sequence", raw({0x0a, 1, 0xe2}));
      reject<PbText>("PbText", "length_2_gib", raw({0x12, 0xff, 0xff, 0xff, 0xff, 0x07, 1, 2}));
      reject<PbText>("PbText", "length_past_end", raw({0x12, 3, 1, 2}));
      reject<PbText>("PbText", "length_6_bytes", raw({0x12, 0x81, 0x80, 0x80, 0x80, 0x80, 0x00, 1}));
      reject<PbText>("PbText", "quad_of_3", raw({0x22, 3, 1, 2, 3}));
      reject<PbText>("PbText", "quad_of_5", raw({0x22, 5, 1, 2, 3, 4, 5}));
      reject<PbText>("PbText", "quad_empty", raw({0x22, 0}));
      reject<PbScalars>("PbScalars", "int8_128", raw({0x10, 0x80, 0x01}));
      reject<PbScalars>("PbScalars", "int16_minus_32769",
                        raw({0x18, 0xff, 0xff, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}));
      reject<PbScalars>("PbScalars", "uint8_256", raw({0x30, 0x80, 0x02}));
      reject<PbScalars>("PbScalars", "uint16_65536", raw({0x38, 0x80, 0x80, 0x04}));
      reject<PbScalars>("PbScalars", "char_256", raw({0x60, 0x80, 0x02}));
      reject<PbScalars>("PbScalars", "enum_of_8_bits_256", raw({0x68, 0x80, 0x02}));
      reject<PbScalars>("PbScalars", "enum_of_8_bits_negative",
                        raw({0x68, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}));
      reject<PbScalars>("PbScalars", "truncated_float", raw({0x55, 0, 0, 0}));
      reject<PbScalars>("PbScalars", "truncated_double", raw({0x59, 0, 0, 0, 0, 0, 0, 0}));
      reject<PbEncodings>("PbEncodings", "sint16_out_of_range", raw({0x38, 0x80, 0x80, 0x04}));
      reject<PbEncodings>("PbEncodings", "truncated_sfixed32", raw({0x1d, 1, 2, 3}));
      reject<PbEncodings>("PbEncodings", "truncated_fixed64", raw({0x31, 1, 2, 3, 4, 5, 6, 7}));
      reject<PbEncodings>("PbEncodings", "packed_fixed64_of_12", raw({0x4a, 12, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}));
      reject<PbEncodings>("PbEncodings", "packed_sfixed32_of_3", raw({0x5a, 3, 1, 2, 3}));
      reject<PbRepeated>("PbRepeated", "packed_truncated_varint", raw({0x0a, 2, 1, 0x80}));
      reject<PbRepeated>("PbRepeated", "packed_floats_of_3", raw({0x22, 3, 0, 0, 0}));
      reject<PbRepeated>("PbRepeated", "packed_length_past_end", raw({0x0a, 0xf0, 0xff, 0xff, 0x7f, 1}));
      reject<PbRepeated>("PbRepeated", "string_element_not_utf8", raw({0x2a, 1, 0xff}));
      reject<PbRepeated>("PbRepeated", "record_element_truncated", raw({0x32, 2, 0x08}));
      reject<PbRepeated>("PbRepeated", "packed_enum_out_of_range", raw({0x3a, 2, 0x80, 0x02}));
      reject<PbMaps>("PbMaps", "entry_past_end", raw({0x0a, 9, 0x0a, 1, 'k'}));
      reject<PbMaps>("PbMaps", "key_not_utf8", raw({0x0a, 3, 0x0a, 1, 0xff}));
      reject<PbMaps>("PbMaps", "entry_field_number_0", raw({0x0a, 2, 0x00, 0x00}));
      reject<PbMaps>("PbMaps", "value_truncated", raw({0x1a, 4, 0x08, 1, 0x12, 5}));
      reject<PbMaps>("PbMaps", "entry_end_group", raw({0x0a, 1, 0x0c}));
      reject<PbChoice>("PbChoice", "alternative_truncated", raw({0x3a, 3, 0x08}));
      reject<PbChoice>("PbChoice", "alternative_not_utf8", raw({0x32, 1, 0x80}));
      reject<PbChoice>("PbChoice", "double_truncated", raw({0x59, 0, 0, 0, 0}));
      reject<PbOptionals>("PbOptionals", "optional_string_not_utf8", raw({0x1a, 1, 0xff}));
      reject<PbOptionals>("PbOptionals", "optional_enum_out_of_range", raw({0x30, 0x80, 0x02}));
      reject<PbOrder>("PbOrder", "nested_field_number_0", raw({0x12, 2, 0x00, 0x00}));
      reject<PbOrder>("PbOrder", "nested_length_past_end", raw({0x12, 3, 0x08, 1}));
      // A value outside what its type declares. Each message is valid
      // protobuf; it is written from `PbLoose`, the same members without the
      // declarations.
      {
         if (psio::to_protobuf(pb_loose_at_limits()) != psio::to_protobuf(pb_bounded_at_limits()))
            die("PbLoose and PbBounded do not have the same bytes");
         auto outside = [&](const char* name, std::string_view error, auto&& change) {
            PbLoose v = pb_loose_at_limits();
            change(v);
            const Bytes b  = psio::to_protobuf(v);
            const auto  st = psio::validate_protobuf<PbBounded>(std::span<const char>{b});
            if (st.ok() || st.error().what != error)
               die(std::string{"PbBounded/"} + name + " is not refused with \"" +
                   std::string{error} + "\"");
            reject<PbBounded>("PbBounded", name, b);
         };
         static constexpr std::string_view length = "protobuf: member length_bound violated";
         outside("ids_over_max", length, [](PbLoose& v) { v.ids.push_back(11); });
         outside("name_over_max", length, [](PbLoose& v) { v.name += '!'; });
         outside("tags_over_max", length, [](PbLoose& v) { v.tags.push_back("d"); });
         outside("tag_element_over_max", length, [](PbLoose& v) { v.tags[1] = "bbbbb"; });
         outside("attrs_over_max", length, [](PbLoose& v) { v.attrs["z"] = ""; });
         outside("attrs_key_over_max", length, [](PbLoose& v) {
            v.attrs.erase("k1");
            v.attrs["long"] = "v";
         });
         outside("attrs_value_over_max", length, [](PbLoose& v) { v.attrs["k1"] = "sixsix"; });
         outside("ranks_not_sorted", "sorted: elements are not in order",
                 [](PbLoose& v) { v.ranks = {1, 5, 4}; });
         outside("codes_not_unique", "sorted: elements are not in strictly ascending order",
                 [](PbLoose& v) { v.codes = {2, 3, 3}; });
         outside("slot_code_over_max", length, [](PbLoose& v) { v.slots[1].code = "fives"; });
         outside("spare_code_over_max", length, [](PbLoose& v) { v.spare->code = "fives"; });
         outside("span_invariant", "PbSpan: lo is greater than hi",
                 [](PbLoose& v) { v.span = {4, 3}; });
         outside("first_violation_reported", "sorted: elements are not in order", [](PbLoose& v) {
            v.ranks = {2, 1};
            v.span  = {4, 3};
         });
         // The declarations are held against the finished value. `name`
         // (field 2) written twice: the last one is the value.
         other<PbBounded>("PbBounded", "name_over_max_then_replaced",
                          raw({0x12, 9, 'n', 'i', 'n', 'e', 'c', 'h', 'a', 'r', 's', 0x12, 2, 'o', 'k'}));
         // `ids` (field 1, packed) written twice: the two parts are one list
         // of three.
         reject<PbBounded>("PbBounded", "ids_over_max_in_two_parts", raw({0x0a, 2, 7, 9, 0x0a, 1, 11}));
         // `span` (field 8) written twice merges: lo from the first, hi from
         // the second.
         reject<PbBounded>("PbBounded", "span_invariant_after_merge",
                           raw({0x42, 2, 0x08, 4, 0x42, 2, 0x10, 3}));
         // The structure is checked first.
         reject<PbBounded>("PbBounded", "structure_first", raw({0x0a, 3, 7, 9, 11, 0x12, 9, 'x'}));
      }
      reject<PbTree>("PbTree", "nested_101_deep", [] {
         Bytes b;
         for (int i = 0; i < 101; ++i)
         {
            Bytes       outer{0x12};
            std::size_t n = b.size();
            for (; n >= 0x80; n >>= 7)
               outer.push_back(static_cast<char>(n | 0x80));
            outer.push_back(static_cast<char>(n));
            outer.insert(outer.end(), b.begin(), b.end());
            b = std::move(outer);
         }
         return b;
      }());
      reject<PbTree>("PbTree", "group_101_deep", [] {
         Bytes b;
         for (int i = 0; i < 101; ++i)
            b.push_back(0x23);
         for (int i = 0; i < 101; ++i)
            b.push_back(0x24);
         return b;
      }());

      return "{\n  \"version\": 1,\n  \"accept\": [" + accept_rows + "\n  ],\n  \"accept_other\": [" +
             other_rows + "\n  ],\n  \"reject\": [" + reject_rows +
             "\n  ],\n  \"mutation_digests\": [" + digest_rows + "\n  ]\n}\n";
   }
}  // namespace

int main(int argc, char** argv)
{
   const bool check = argc == 3 && std::string_view(argv[1]) == "--check";
   if (!check && argc != 2)
   {
      std::fprintf(stderr, "usage: %s <out.json> | --check <ref.json>\n", argv[0]);
      return 2;
   }
   const std::string text = generate();
   if (check)
   {
      std::ifstream     in(argv[2], std::ios::binary);
      std::stringstream ref;
      ref << in.rdbuf();
      if (!in || ref.str() != text)
      {
         std::fprintf(stderr, "%s is not what psio_protobuf_conformance_emit writes; regenerate it\n",
                      argv[2]);
         return 1;
      }
      return 0;
   }
   std::ofstream out(argv[1], std::ios::binary);
   out << text;
   std::printf("%zu accepted, %zu accepted from other writers, %zu rejected -> %s\n", accepted,
               others, rejected, argv[1]);
   return out ? 0 : 1;
}
