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
      std::string   id, type, identifier, wire_hex, error;
      bool          size_prefixed = false;
      std::uint32_t at            = 0;  // rejected rows: where the check failed
   };
   std::vector<Row> accepted, rejected, accepted_corrupt;

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
               std::string_view error, psio::flatbuf::options opts = {})
   {
      const std::string id = type + "/" + name;
      const auto        st = psio::validate_flatbuf<T>(bytes, opts);
      if (st.ok())
         die(id + ": is accepted");
      if (st.error().what != error)
         die(id + ": refused with \"" + std::string(st.error().what) + "\", expected \"" +
             std::string(error) + "\"");
      // The safe view reads such bytes without leaving them.
      (void)psio::flatbuf_view<T>{bytes}.template get<0>();
      rejected.push_back(Row{id, type, std::string(opts.file_identifier), to_hex(bytes),
                             std::string(error), opts.size_prefixed, st.error().byte_offset});
   }

   // A buffer that is not the encoding of an intended value and that the
   // verifier accepts all the same: what it decodes to re-encodes to a valid
   // buffer.
   template <typename T>
   void accept_corrupt(const std::string& type, const std::string& name, const Bytes& bytes)
   {
      const std::string id = type + "/" + name;
      if (auto st = psio::validate_flatbuf<T>(bytes); !st.ok())
         die(id + ": " + std::string(st.error().what));
      const T    value = psio::from_flatbuf<T>(psio::trusted{bytes});
      const auto again = psio::to_flatbuf(value);
      if (!psio::validate_flatbuf<T>(again).ok())
         die(id + ": its value does not re-encode to a valid buffer");
      accepted_corrupt.push_back(Row{id, type, {}, to_hex(bytes), {}, false});
   }

   // splitmix64: the same sequence on every platform.
   struct Rng
   {
      std::uint64_t state;
      std::uint64_t next()
      {
         std::uint64_t z = (state += 0x9e3779b97f4a7c15ull);
         z               = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
         z               = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
         return z ^ (z >> 31);
      }
   };

   // One corruption of `base`: one or two edits (the edits of
   // flatbuf_fuzz_tests.cpp), drawn from `rng` in this order.
   Bytes corrupt(const Bytes& base, Rng& rng)
   {
      Bytes     b     = base;
      const int edits = 1 + static_cast<int>(rng.next() % 2);
      for (int e = 0; e < edits && !b.empty(); ++e)
      {
         const std::size_t at = rng.next() % b.size();
         switch (rng.next() % 6)
         {
            case 0: b[at] = static_cast<char>(rng.next()); break;
            case 1: b[at] = static_cast<char>(b[at] + 4); break;
            case 2: b[at] = static_cast<char>(b[at] ^ (1u << (rng.next() % 8))); break;
            case 3:
            {
               // A 32-bit word: an offset or a length.
               const std::size_t   w = at & ~std::size_t{3};
               const std::uint32_t v =
                  rng.next() % 2 ? static_cast<std::uint32_t>(rng.next() % (b.size() + 8))
                                 : static_cast<std::uint32_t>(rng.next());
               if (w + 4 <= b.size())
                  std::memcpy(b.data() + w, &v, 4);
               break;
            }
            case 4:
            {
               // A 16-bit word: a vtable entry.
               const std::size_t   w = at & ~std::size_t{1};
               const std::uint16_t v = static_cast<std::uint16_t>(rng.next() % 96);
               if (w + 2 <= b.size())
                  std::memcpy(b.data() + w, &v, 2);
               break;
            }
            default: b.resize(at); break;
         }
      }
      return b;
   }

   struct Digest
   {
      std::string   type, base;
      std::uint64_t seed = 0, count = 0, accepted = 0, digest = 0;
   };
   std::vector<Digest> digests;

   // The verdicts of the verifier on `count` corruptions of the accepted
   // row `base_id`, folded into one number (FNV-1a over, per buffer: 1 when
   // it is accepted; else 0, the message, and the position as 4 bytes,
   // little-endian). A consumer draws the same corruptions and must arrive
   // at the same number: every verdict, reason and position agrees.
   template <typename T>
   void digest(const std::string& type, const std::string& name, const T& value,
               std::uint64_t seed, std::uint64_t count)
   {
      const Bytes   base = psio::to_flatbuf(value);
      Rng           rng{seed};
      std::uint64_t h    = 1469598103934665603ull;
      const auto    mix  = [&](unsigned char c) { h = (h ^ c) * 1099511628211ull; };
      Digest        d{type, type + "/" + name, seed, count, 0, 0};
      for (std::uint64_t i = 0; i < count; ++i)
      {
         const Bytes b  = corrupt(base, rng);
         const auto  st = psio::validate_flatbuf<T>(b);
         if (st.ok())
         {
            mix(1);
            ++d.accepted;
            continue;
         }
         mix(0);
         for (const char c : st.error().what)
            mix(static_cast<unsigned char>(c));
         for (int k = 0; k < 4; ++k)
            mix(static_cast<unsigned char>(st.error().byte_offset >> (8 * k)));
      }
      d.digest = h;
      digests.push_back(d);
   }

   // Deterministic corruptions of `base`:
   // the first `rejects` that the verifier refuses, with its message, and
   // the first `accepts` that it accepts.
   template <typename T>
   void mutate(const std::string& type, const Bytes& base, std::uint64_t seed, int rejects,
               int accepts)
   {
      Rng rng{seed};
      int n_rejected = 0, n_accepted = 0;
      for (int iter = 0; iter < 100000 && (n_rejected < rejects || n_accepted < accepts); ++iter)
      {
         const Bytes b = corrupt(base, rng);
         if (b == base)
            continue;
         const auto st = psio::validate_flatbuf<T>(b);
         char       name[32];
         if (st.ok())
         {
            if (n_accepted >= accepts)
               continue;
            std::snprintf(name, sizeof name, "mutated_valid_%03d", n_accepted++);
            accept_corrupt<T>(type, name, b);
         }
         else
         {
            if (n_rejected >= rejects)
               continue;
            std::snprintf(name, sizeof name, "mutated_%03d", n_rejected++);
            reject<T>(type, name, b, st.error().what);
         }
      }
      if (n_rejected < rejects || n_accepted < accepts)
         die(type + ": not enough mutations");
   }

   void put32(Bytes& b, std::size_t at, std::uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
   void put16(Bytes& b, std::size_t at, std::uint16_t v) { std::memcpy(b.data() + at, &v, 2); }
   std::uint32_t get32(const Bytes& b, std::size_t at)
   {
      std::uint32_t v;
      std::memcpy(&v, b.data() + at, 4);
      return v;
   }
   std::uint16_t get16(const Bytes& b, std::size_t at)
   {
      std::uint16_t v;
      std::memcpy(&v, b.data() + at, 2);
      return v;
   }
   // The vtable of the table at `table`.
   std::size_t vtable_of(const Bytes& b, std::size_t table)
   {
      std::int32_t soff;
      std::memcpy(&soff, b.data() + table, 4);
      return table - soff;
   }
   // The position of the field of member `member` of the root table (a T).
   template <typename T>
   std::size_t root_field(const Bytes& b, std::size_t member)
   {
      const std::size_t   root = get32(b, 0);
      const std::uint16_t off =
         get16(b, vtable_of(b, root) + psio::flatbuf::layout<T>::vtable_offset(member));
      if (off == 0)
         die("the root table does not store the member");
      return root + off;
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
      accept("Order", "compact", compact_order());
      accept("Scalars", "zero", Scalars{});
      accept("Scalars", "sample",
             Scalars{true, -1, 2, -3, 4, -5, 6, -7, 8, 1.5f, -2.25, Color::blue, 'x'});
      accept("Sparse", "rows",
             Sparse{{{1, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 6}, {0, 2, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0}, {}}});
      accept("Longs", "empty", Longs{});
      accept("Longs", "sample", Longs{{1, 0xffffffffffffffffull, 3}, {0.5, -0.0}});

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

      // ── One row for every other reason the verifier has ───────────────
      {
         Bytes b = psio::to_flatbuf(sample_profile(), {.file_identifier = "PROF"});
         reject<Profile>("Profile", "wrong_identifier", b, "flatbuf: wrong file identifier",
                         {.file_identifier = "ORDR"});
         b = psio::to_flatbuf(sample_profile(), {.size_prefixed = true});
         b.push_back(0);
         reject<Profile>("Profile", "size_prefix_mismatch", b,
                         "flatbuf: size prefix does not match the buffer", {.size_prefixed = true});
      }
      {
         // `name` (member 1) is an offset; `id` (member 0) a 64-bit scalar.
         const std::size_t root = get32(profile, 0);
         const std::size_t vt   = vtable_of(profile, root);
         Bytes             b    = profile;
         put16(b, vt + psio::flatbuf::layout<Profile>::vtable_offset(1), 5);
         reject<Profile>("Profile", "misaligned_offset", b, "flatbuf: misaligned offset");
         b = profile;
         put16(b, vt + psio::flatbuf::layout<Profile>::vtable_offset(0), 5);
         reject<Profile>("Profile", "misaligned_field", b, "flatbuf: misaligned field");
         b                      = profile;
         const std::size_t cell = root_field<Profile>(b, 1);
         put32(b, cell, get32(b, cell) + 2);
         reject<Profile>("Profile", "misaligned_string", b, "flatbuf: misaligned string");
      }
      {
         Bytes             b    = psio::to_flatbuf(conformance_basket());
         const std::size_t cell = root_field<Basket>(b, 4);  // codes
         put32(b, cell, get32(b, cell) + 2);
         reject<Basket>("Basket", "misaligned_vector", b, "flatbuf: misaligned vector");
      }
      {
         // The offset moved 4 bytes into the first element, whose low word
         // then reads as a count of 1: elements at a position that is not a
         // multiple of 8.
         Bytes             b    = psio::to_flatbuf(Longs{{1, 2, 3}, {}});
         const std::size_t cell = root_field<Longs>(b, 0);
         put32(b, cell, get32(b, cell) + 4);
         reject<Longs>("Longs", "misaligned_elements", b, "flatbuf: misaligned vector elements");
      }
      {
         const Bytes order = psio::to_flatbuf(compact_order());
         Bytes       b     = order;
         {
            const unsigned char pattern[] = {4, 0, 0, 0, 1, 0, 2, 0, 3, 0, 4, 0};  // quad
            const std::size_t   at =
               find(b, std::string_view(reinterpret_cast<const char*>(pattern), sizeof(pattern)));
            put32(b, at, 3);
            reject<Order>("Order", "fixed_vector_length", b,
                          "flatbuf: a fixed-length vector has the wrong length");
         }
         b = order;
         {
            // The keys of `stock` are "k" and "m".
            const unsigned char pattern[] = {1, 0, 0, 0, 'k', 0};
            const std::size_t   at =
               find(b, std::string_view(reinterpret_cast<const char*>(pattern), sizeof(pattern)));
            b[at + 4] = 'z';
            reject<Order>("Order", "map_not_ascending", b,
                          "flatbuf: map keys are not in ascending order");
         }
         b = order;
         {
            // The first entry of `stock`: its vtable loses the key.
            const std::size_t vec   = root_field<Order>(b, 13);
            const std::size_t list  = vec + get32(b, vec);
            const std::size_t entry = list + 4 + get32(b, list + 4);
            put16(b, vtable_of(b, entry) + 4, 0);
            reject<Order>("Order", "map_entry_without_key", b, "flatbuf: map entry has no key");
         }
         b = order;
         {
            const std::size_t root = get32(b, 0);
            const std::size_t vt   = vtable_of(b, root);
            const std::size_t tvo  = psio::flatbuf::layout<Order>::vtable_offset(5);  // payment
            b[root + get16(b, vt + tvo)] = 0;
            reject<Order>("Order", "union_none_with_value", b,
                          "flatbuf: union is NONE but has a value");
            b = order;
            put16(b, vt + tvo + 2, 0);
            reject<Order>("Order", "union_type_without_value", b,
                          "flatbuf: union has a type but no value");
         }
      }
      {
         // A Tree whose `children` vector holds 40 offsets to ONE child, and
         // so on for 12 levels: 40^12 tables if every path were walked
         // (flatbuf_fuzz_tests.cpp explains the construction).
         const std::uint32_t fan = 40, levels = 12;
         const std::size_t   vtable = 4, first = 16;
         const std::size_t   stride = 8 + 4 + std::size_t{fan} * 4;
         Bytes               b(first + stride * levels + 12, 0);
         put32(b, 0, static_cast<std::uint32_t>(first));
         put16(b, vtable, 10);
         put16(b, vtable + 2, 8);
         put16(b, vtable + 8, 4);
         for (std::uint32_t l = 0; l <= levels; ++l)
         {
            const std::size_t tbl = first + stride * l;
            put32(b, tbl, static_cast<std::uint32_t>(tbl - vtable));
            if (l == levels)
            {
               put32(b, tbl + 4, 4);
               break;
            }
            const std::size_t vec = tbl + 8;
            put32(b, tbl + 4, static_cast<std::uint32_t>(vec - (tbl + 4)));
            put32(b, vec, fan);
            const std::size_t next = first + stride * (l + 1);
            for (std::uint32_t i = 0; i < fan; ++i)
            {
               const std::size_t cell = vec + 4 + std::size_t{i} * 4;
               put32(b, cell, static_cast<std::uint32_t>(next - cell));
            }
         }
         reject<Tree>("Tree", "shared_subobjects", b, "flatbuf: too many tables");
      }

      // ── Corrupted buffers the verifier accepts ────────────────────────
      {
         // FlatBuffers strings are bytes: C++ `std::string` holds these, the
         // Rust `String` cannot (its verifier refuses the row; `RawString`
         // accepts it).
         Bytes             b  = profile;
         const std::size_t at = find(b, "Alice Example");
         b[at]                = static_cast<char>(0xff);
         accept_corrupt<Profile>("Profile", "name_not_utf8", b);
      }
      {
         // An enum is its underlying integer: C++ reads any value, a Rust
         // enum without a catch-all variant does not have this one.
         Bytes b = psio::to_flatbuf(compact_order());
         b[root_field<Order>(b, 10)] = 9;  // color
         accept_corrupt<Order>("Order", "color_out_of_range", b);
      }

      // ── Deterministic corruptions, with the verdict of this verifier ──
      mutate<Profile>("Profile", profile, 1, 40, 8);
      mutate<Tree>("Tree", psio::to_flatbuf(conformance_tree()), 2, 40, 8);
      mutate<Basket>("Basket", psio::to_flatbuf(conformance_basket()), 3, 60, 10);
      mutate<Order>("Order", psio::to_flatbuf(compact_order()), 4, 240, 30);
      mutate<Longs>("Longs", psio::to_flatbuf(Longs{{1, 2, 3}, {0.5}}), 5, 20, 4);

      // ── The verdicts on many more corruptions, as one number each ─────
      digest<Profile>("Profile", "sample", sample_profile(), 11, 30000);
      digest<Tree>("Tree", "nested", conformance_tree(), 12, 30000);
      digest<Basket>("Basket", "full", conformance_basket(), 13, 40000);
      digest<Order>("Order", "compact", compact_order(), 14, 60000);
      digest<Order>("Order", "sample", sample_order(), 15, 60000);
      digest<Order>("Order", "empty", Order{}, 16, 20000);
      digest<Scalars>("Scalars", "sample",
                      Scalars{true, -1, 2, -3, 4, -5, 6, -7, 8, 1.5f, -2.25, Color::blue, 'x'}, 17,
                      20000);
      digest<Longs>("Longs", "sample", Longs{{1, 0xffffffffffffffffull, 3}, {0.5, -0.0}}, 18, 20000);
      digest<Renumbered>("Renumbered", "sample", Renumbered{5, "five", -5}, 19, 10000);
      digest<Sparse>("Sparse", "rows",
                     Sparse{{{1, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 6}, {0, 2, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0}, {}}},
                     20, 10000);
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
      std::string out = "{\"version\":2,\n\"accept\":[\n";
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
         out += "{\"id\":" + quote(r.id) + ",\"type\":" + quote(r.type);
         if (!r.identifier.empty())
            out += ",\"file_identifier\":" + quote(r.identifier);
         if (r.size_prefixed)
            out += ",\"size_prefixed\":true";
         out += ",\"error\":" + quote(r.error) + ",\"at\":" + std::to_string(r.at) +
                ",\"wire_hex\":" + quote(r.wire_hex) + "}";
         out += i + 1 < rejected.size() ? ",\n" : "\n";
      }
      out += "],\n\"accept_corrupt\":[\n";
      for (std::size_t i = 0; i < accepted_corrupt.size(); ++i)
      {
         const Row& r = accepted_corrupt[i];
         out += "{\"id\":" + quote(r.id) + ",\"type\":" + quote(r.type) + ",\"wire_hex\":" +
                quote(r.wire_hex) + "}";
         out += i + 1 < accepted_corrupt.size() ? ",\n" : "\n";
      }
      out += "],\n\"mutation_digests\":[\n";
      for (std::size_t i = 0; i < digests.size(); ++i)
      {
         const Digest& d = digests[i];
         char          hex[17];
         std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(d.digest));
         out += "{\"type\":" + quote(d.type) + ",\"base\":" + quote(d.base) +
                ",\"seed\":" + std::to_string(d.seed) + ",\"count\":" + std::to_string(d.count) +
                ",\"accepted\":" + std::to_string(d.accepted) + ",\"digest\":" + quote(hex) + "}";
         out += i + 1 < digests.size() ? ",\n" : "\n";
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
   std::printf("wrote %zu accepted, %zu rejected and %zu corrupted-but-accepted buffers to %s\n",
               accepted.size(), rejected.size(), accepted_corrupt.size(), argv[1]);
   return out ? 0 : 1;
}
