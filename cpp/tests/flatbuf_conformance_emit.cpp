// Emitter for conformance/flatbuf-vectors.json (conformance/flatbuf-vectors.md):
// FlatBuffers buffers of the values in tests/flatbuf_shapes.hpp, and buffers
// a verifier must refuse.
//
//   psio_flatbuf_conformance_emit <out.json>          write the fixture
//   psio_flatbuf_conformance_emit --check <ref.json>  regenerate and compare
//
// Every accepted row is validated and decoded back to its value before it is
// written, and every rejected row is checked to be refused for the stated
// reason; generation aborts otherwise. Output is deterministic.
#include <psio/flatbuf.hpp>
#include <psio/flatbuf_view.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "flatbuf_shapes.hpp"

namespace
{
   using Bytes = std::vector<char>;

   [[noreturn]] void die(const std::string& message)
   {
      std::fprintf(stderr, "flatbuf conformance emit: %s\n", message.c_str());
      std::exit(2);
   }
   std::string to_hex(std::span<const char> bytes)
   {
      std::string out;
      for (unsigned char c : bytes)
      {
         out += "0123456789abcdef"[c >> 4];
         out += "0123456789abcdef"[c & 15];
      }
      return out;
   }

   struct Row
   {
      std::string id, type, identifier, wire_hex, error;
      bool        size_prefixed = false;
   };
   std::vector<Row> accepted, rejected;

   template <typename T>
   void accept(const std::string& type, const std::string& name, const T& value,
               psio::flatbuf::options opts = {})
   {
      const std::string id    = type + "/" + name;
      const Bytes       bytes = psio::to_flatbuf(value, opts);
      if (auto st = psio::validate_flatbuf<T>(bytes, opts); !st.ok())
         die(id + ": " + std::string(st.error().what));
      const std::span<const char> inner =
         opts.size_prefixed ? psio::flatbuf_size_prefixed(bytes) : std::span<const char>{bytes};
      if (!(psio::from_flatbuf<T>(psio::trusted{inner}) == value))
         die(id + ": does not decode to its value");
      if (!(psio::flatbuf_view<T>{inner}.unpack() == value))
         die(id + ": the view does not unpack to its value");
      accepted.push_back(
         Row{id, type, std::string(opts.file_identifier), to_hex(bytes), {}, opts.size_prefixed});
   }

   template <typename T>
   void reject(const std::string& type, const std::string& name, const Bytes& bytes,
               std::string_view error)
   {
      const std::string id = type + "/" + name;
      const auto        st = psio::validate_flatbuf<T>(bytes);
      if (st.ok())
         die(id + ": is accepted");
      if (st.error().what != error)
         die(id + ": refused with \"" + std::string(st.error().what) + "\", expected \"" +
             std::string(error) + "\"");
      // The safe view reads such bytes without leaving them.
      (void)psio::flatbuf_view<T>{bytes}.template get<0>();
      rejected.push_back(Row{id, type, {}, to_hex(bytes), std::string(error)});
   }

   void put32(Bytes& b, std::size_t at, std::uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
   void put16(Bytes& b, std::size_t at, std::uint16_t v) { std::memcpy(b.data() + at, &v, 2); }
   std::uint32_t get32(const Bytes& b, std::size_t at)
   {
      std::uint32_t v;
      std::memcpy(&v, b.data() + at, 4);
      return v;
   }
   std::size_t find(const Bytes& b, std::string_view text)
   {
      const auto at = std::string_view(b.data(), b.size()).find(text);
      if (at == std::string_view::npos)
         die("pattern not found");
      return at;
   }

   Tree conformance_tree()
   {
      Tree t;
      t.value = 1;
      t.name  = "root";
      t.children.resize(3);
      t.children[0].value = 2;
      t.children[1].name  = "middle";
      t.children[1].children.resize(2);
      t.children[1].children[1].value = -7;
      t.children[2].value = 2;
      return t;
   }
   Basket conformance_basket()
   {
      Basket b;
      b.id      = 9000000000ull;
      b.owner   = sample_profile();
      b.entries = {{"bolt", 12, 0.25}, {"", 0, 0.0}, {"washer", 1, 1.5}};
      b.tags    = {"a", "", "third"};
      b.codes   = {1, 256, 65535};
      b.bytes   = {0, 1, 254, 255, 7};
      b.total   = 4.5;
      b.paid    = true;
      return b;
   }

   void emit()
   {
      accept("Point", "zero", Point{});
      accept("Point", "values", Point{1, -2});
      accept("Profile", "empty", Profile{});
      accept("Profile", "sample", sample_profile());
      accept("Profile", "identifier", sample_profile(), {.file_identifier = "PROF"});
      accept("Profile", "size_prefixed", sample_profile(), {.size_prefixed = true});
      accept("Numbers", "zero", Numbers{});
      accept("Numbers", "extremes",
             Numbers{true,
                     std::numeric_limits<std::int8_t>::min(),
                     std::numeric_limits<std::uint8_t>::max(),
                     std::numeric_limits<std::int16_t>::min(),
                     std::numeric_limits<std::uint16_t>::max(),
                     std::numeric_limits<std::int32_t>::min(),
                     std::numeric_limits<std::uint32_t>::max(),
                     std::numeric_limits<std::int64_t>::min(),
                     std::numeric_limits<std::uint64_t>::max(),
                     -1.5f,
                     1e300});
      accept("Tree", "empty", Tree{});
      accept("Tree", "nested", conformance_tree());
      accept("Basket", "empty", Basket{});
      accept("Basket", "full", conformance_basket());

      // Shapes beyond the common subset: structs, unions, optionals, sorted
      // vectors, defaults that are not zero, explicit ids, a deprecated field.
      accept("Order", "empty", Order{});
      accept("Order", "sample", sample_order());
      {
         Order o   = sample_order();
         o.payment = Wire{"DE00 1234 5678"};
         o.refund  = Wire{"FR11"};
         o.origin.reset();
         o.discount.reset();
         o.note.reset();
         accept("Order", "wire", o);
         o.payment  = std::monostate{};
         o.note     = std::string{};
         o.discount = 0;
         accept("Order", "none", o);
      }
      accept("Order", "identifier_size_prefixed", sample_order(),
             {.file_identifier = "ORDR", .size_prefixed = true});
      accept("Defaults", "defaults", Defaults{});
      accept("Defaults", "zeros", Defaults{0, false, Color::red, 0.0});
      accept("Renumbered", "sample", Renumbered{5, "five", -5});
      accept("WithDeprecated", "sample", WithDeprecated{1, 0, 3});

      // ── Buffers a verifier must refuse ────────────────────────────────
      const Bytes profile = psio::to_flatbuf(sample_profile());
      reject<Profile>("Profile", "empty_buffer", Bytes{}, "flatbuf: buffer too small");
      reject<Profile>("Profile", "truncated", Bytes(profile.begin(), profile.begin() + 24),
                      "flatbuf: bad table size");
      {
         Bytes b = profile;
         put32(b, 0, 0);
         reject<Profile>("Profile", "null_root", b, "flatbuf: null offset");
         put32(b, 0, static_cast<std::uint32_t>(b.size()));
         reject<Profile>("Profile", "root_past_end", b, "flatbuf: offset out of bounds");
         put32(b, 0, get32(profile, 0) + 2);
         reject<Profile>("Profile", "misaligned_table", b, "flatbuf: misaligned table");
      }
      {
         Bytes b = profile;
         put32(b, get32(b, 0), 0x7fffff00u);  // the table's offset to its vtable
         reject<Profile>("Profile", "vtable_out_of_bounds", b, "flatbuf: vtable out of bounds");
      }
      {
         Bytes             b    = profile;
         const std::size_t root = get32(b, 0);
         std::int32_t      soff;
         std::memcpy(&soff, b.data() + root, 4);
         const std::size_t vt = root - soff;
         put16(b, vt, 3);
         reject<Profile>("Profile", "vtable_size_odd", b, "flatbuf: bad vtable size");
         b = profile;
         put16(b, vt + 4, 0x7000);  // the vtable entry of `id`
         reject<Profile>("Profile", "field_outside_table", b, "flatbuf: field outside its table");
      }
      {
         Bytes             b  = profile;
         const std::size_t at = find(b, "Alice Example");
         put32(b, at - 4, 0x00100000);
         reject<Profile>("Profile", "string_length", b, "flatbuf: string out of bounds");
         b              = profile;
         b[at + 13]     = '!';
         reject<Profile>("Profile", "string_not_terminated", b,
                         "flatbuf: string is not NUL-terminated");
      }
      {
         Bytes b = psio::to_flatbuf(conformance_basket());
         // The `codes` vector: count 3, then 1, 256, 65535.
         const unsigned char pattern[] = {3, 0, 0, 0, 1, 0, 0, 1, 0xff, 0xff};
         const std::size_t   at =
            find(b, std::string_view(reinterpret_cast<const char*>(pattern), sizeof(pattern)));
         put32(b, at, 0x40000000);
         reject<Basket>("Basket", "vector_length", b, "flatbuf: vector out of bounds");
      }
      {
         Tree  t;
         Tree* cur = &t;
         for (int i = 0; i < 64; ++i)
         {
            cur->children.resize(1);
            cur = &cur->children[0];
         }
         reject<Tree>("Tree", "too_deep", psio::to_flatbuf(t), "flatbuf: tables nested too deeply");
      }
      {
         Order o;
         o.ids   = {1, 2, 3};
         Bytes b = psio::to_flatbuf(o);
         const unsigned char pattern[] = {3, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0};
         const std::size_t   at =
            find(b, std::string_view(reinterpret_cast<const char*>(pattern), sizeof(pattern)));
         put32(b, at + 12, 2);
         reject<Order>("Order", "set_not_ascending", b,
                       "flatbuf: set keys are not in ascending order");
      }
      {
         Order o;
         o.payment               = Card{"1", 2};
         Bytes             b     = psio::to_flatbuf(o);
         const std::size_t root  = get32(b, 0);
         std::int32_t      soff;
         std::memcpy(&soff, b.data() + root, 4);
         const std::size_t vt  = root - soff;
         const std::size_t tvo = psio::flatbuf::layout<Order>::vtable_offset(5);
         std::uint16_t     off;
         std::memcpy(&off, b.data() + vt + tvo, 2);
         b[root + off] = 9;
         reject<Order>("Order", "unknown_union_type", b, "flatbuf: unknown union type");
      }
   }

   std::string quote(const std::string& s)
   {
      std::string out = "\"";
      for (const char c : s)
      {
         if (c == '"' || c == '\\')
            out += '\\';
         out += c;
      }
      return out + "\"";
   }

   std::string render()
   {
      std::string out = "{\"version\":1,\n\"accept\":[\n";
      for (std::size_t i = 0; i < accepted.size(); ++i)
      {
         const Row& r = accepted[i];
         out += "{\"id\":" + quote(r.id) + ",\"type\":" + quote(r.type);
         if (!r.identifier.empty())
            out += ",\"file_identifier\":" + quote(r.identifier);
         if (r.size_prefixed)
            out += ",\"size_prefixed\":true";
         out += ",\"wire_hex\":" + quote(r.wire_hex) + "}";
         out += i + 1 < accepted.size() ? ",\n" : "\n";
      }
      out += "],\n\"reject\":[\n";
      for (std::size_t i = 0; i < rejected.size(); ++i)
      {
         const Row& r = rejected[i];
         out += "{\"id\":" + quote(r.id) + ",\"type\":" + quote(r.type) + ",\"error\":" +
                quote(r.error) + ",\"wire_hex\":" + quote(r.wire_hex) + "}";
         out += i + 1 < rejected.size() ? ",\n" : "\n";
      }
      out += "]}\n";
      return out;
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
   emit();
   const std::string text = render();
   if (check)
   {
      std::ifstream     in(argv[2], std::ios::binary);
      std::stringstream buf;
      buf << in.rdbuf();
      if (!in || buf.str() != text)
      {
         std::fprintf(stderr,
                      "flatbuf conformance emit: %s is not what the implementation generates; "
                      "regenerate it only for an intended format change\n",
                      argv[2]);
         return 1;
      }
      return 0;
   }
   std::ofstream out(argv[1], std::ios::binary);
   out << text;
   std::printf("wrote %zu accepted and %zu rejected buffers to %s\n", accepted.size(),
               rejected.size(), argv[1]);
   return out ? 0 : 1;
}
