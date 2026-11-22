// PSSZ cross-version matrix: field evolution through optional suffix fields.
//
// Covers, for pssz8 / pssz16 / pssz32 and the automatic-width profile:
//   - old data -> new reader and new data -> old reader for one, two and
//     three appended optional fields (fixed, string, vector, nested record,
//     optional<optional<T>>);
//   - an evolving record inside a vector, an optional, a variant alternative
//     and as a non-last field of a parent record;
//   - canonical bytes (golden arrays), mandatory pruning, and that a present
//     but empty value is not pruned;
//   - rejection of non-canonical and malformed record frames, with
//     validate / validate_strict / validate_or_throw / decode agreeing;
//   - borrowed views reading evolved records in both directions.

#include "pssz_evolution_fixture.hpp"
#include "pssz_evolution_goldens.hpp"

#include <catch.hpp>

#include <algorithm>
#include <cstring>
#include <span>
#include <type_traits>

namespace
{
   using namespace evo;

   template <typename F>
   struct width_of : std::integral_constant<std::size_t, F::offset_width>
   {
   };
   // Every corpus record holds an unbounded string, so each revision of each
   // lineage selects the same automatic width. That equality is the
   // precondition for a compatible lineage in the automatic profile.
   template <>
   struct width_of<psio::pssz> : std::integral_constant<std::size_t, 4>
   {
   };
   static_assert(psio::auto_pssz_width_v<R0> == 4 && psio::auto_pssz_width_v<R1> == 4 &&
                 psio::auto_pssz_width_v<R2> == 4 && psio::auto_pssz_width_v<R3> == 4 &&
                 psio::auto_pssz_width_v<N1> == 4 && psio::auto_pssz_width_v<P1> == 4 &&
                 psio::auto_pssz_width_v<Inner> == 4 && psio::auto_pssz_width_v<Bad1> == 4 &&
                 psio::auto_pssz_width_v<Parent0> == 4 && psio::auto_pssz_width_v<Parent2> == 4 &&
                 psio::auto_pssz_width_v<VecNew> == 4 && psio::auto_pssz_width_v<VecOld> == 4);

   std::vector<char> from_hex(std::string_view hex)
   {
      auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
      std::vector<char> out;
      for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
         out.push_back(static_cast<char>(nibble(hex[i]) * 16 + nibble(hex[i + 1])));
      return out;
   }

   template <typename F>
   std::vector<char> golden(std::string_view name)
   {
      const std::size_t column = width_of<F>::value == 1 ? 0 : width_of<F>::value == 2 ? 1 : 2;
      auto entry = std::find_if(goldens.begin(), goldens.end(),
                                [&](const Golden& g) { return g.name == name; });
      REQUIRE(entry != goldens.end());
      return from_hex(entry->hex[column]);
   }

   // decode() is the fail-fast path: it throws codec_exception natively.
   template <typename T, typename F>
   bool decodes(std::span<const char> wire)
   {
      try
      {
         (void)psio::decode<T>(F{}, wire);
         return true;
      }
      catch (const psio::codec_exception&)
      {
         return false;
      }
   }

   // A frame `Reader` must accept, yielding `expect`. `unknown` says whether
   // the writer appended optional fields this reader does not know.
   template <typename F, typename Reader>
   void reads(const std::vector<char>& wire, const Reader& expect, bool unknown)
   {
      REQUIRE(psio::validate<Reader>(F{}, wire).ok());
      REQUIRE_NOTHROW(psio::validate_or_throw<Reader>(F{}, wire));
      REQUIRE(decodes<Reader, F>(wire));
      REQUIRE(psio::decode<Reader>(F{}, wire) == expect);

      bool reported = !unknown;
      REQUIRE(psio::validate_pssz_compatible<Reader>(F{}, wire, reported).ok());
      REQUIRE(reported == unknown);
      Reader checked{};
      reported = !unknown;
      REQUIRE(psio::decode_pssz_checked(F{}, wire, checked, reported).ok());
      REQUIRE(reported == unknown);
      REQUIRE(checked == expect);

      // Exact-schema validation rejects unknown fields and nothing else here.
      REQUIRE(psio::validate_strict<Reader>(F{}, wire).ok() == !unknown);
      // One encoding per value: without dropped unknown fields, re-encoding
      // what the reader saw reproduces the input exactly.
      if (!unknown)
         REQUIRE(psio::encode(F{}, expect) == wire);
   }

   // A frame every entry point of `Reader` must reject.
   template <typename F, typename Reader>
   void rejects(const std::vector<char>& wire)
   {
      REQUIRE_FALSE(psio::validate<Reader>(F{}, wire).ok());
      REQUIRE_FALSE(psio::validate_strict<Reader>(F{}, wire).ok());
      REQUIRE_THROWS_AS(psio::validate_or_throw<Reader>(F{}, wire), psio::codec_exception);
      REQUIRE_FALSE(decodes<Reader, F>(wire));
      bool      reported = false;
      REQUIRE_FALSE(psio::validate_pssz_compatible<Reader>(F{}, wire, reported).ok());
      Reader out{};
      REQUIRE_FALSE(psio::decode_pssz_checked(F{}, wire, out, reported).ok());
   }

   // Views borrow: the bytes must outlive the view, so temporaries are
   // refused at compile time.
   template <typename T, typename F>
   auto view_of(const std::vector<char>& bytes)
   {
      return psio::view<T, F, psio::storage::const_borrow>{bytes};
   }
   template <typename T, typename F>
   void view_of(std::vector<char>&&) = delete;

   // Independent little-endian frame builder for hand-made (mostly invalid)
   // frames. `o` writes one W-byte header/offset slot.
   struct Frame
   {
      std::size_t       W;
      std::vector<char> bytes;
      Frame& o(std::size_t value)
      {
         for (std::size_t i = 0; i < W; ++i)
            bytes.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
         return *this;
      }
      Frame& u32(std::uint32_t value)
      {
         for (std::size_t i = 0; i < 4; ++i)
            bytes.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
         return *this;
      }
      Frame& raw(std::string_view text)
      {
         bytes.insert(bytes.end(), text.begin(), text.end());
         return *this;
      }
      Frame& byte(unsigned char value)
      {
         bytes.push_back(static_cast<char>(value));
         return *this;
      }
   };
}  // namespace

#define EVO_FORMATS psio::pssz8, psio::pssz16, psio::pssz32, psio::pssz

TEMPLATE_TEST_CASE("pssz evolution corpus encodes to its golden canonical bytes",
                   "[pssz][evolution][golden]", EVO_FORMATS)
{
   using F = TestType;
   std::size_t count = 0;
   for_each_vector([&](const char* name, const auto& value) {
      INFO(name);
      using T          = std::remove_cvref_t<decltype(value)>;
      const auto wire  = psio::encode(F{}, value);
      const auto bytes = golden<F>(name);
      REQUIRE(wire == bytes);
      REQUIRE(psio::size_of(F{}, value) == bytes.size());
      std::vector<char> appended{'p'};
      psio::encode(F{}, value, appended);
      REQUIRE(appended.size() == bytes.size() + 1);
      REQUIRE(std::equal(bytes.begin(), bytes.end(), appended.begin() + 1));
      // decode(encode(x)) == x, and the bytes are accepted by every reader
      // entry point of the writer's own schema.
      reads<F, T>(bytes, value, false);
      ++count;
   });
   REQUIRE(count == goldens.size());
}

TEMPLATE_TEST_CASE("pssz prunes exactly the unset optional tail",
                   "[pssz][evolution][canonical]", EVO_FORMATS)
{
   using F = TestType;
   // An all-unset suffix is byte-identical to the first published revision.
   const auto v1 = golden<F>("r0");
   REQUIRE(golden<F>("r1_none") == v1);
   REQUIRE(golden<F>("r2_none_none") == v1);
   REQUIRE(golden<F>("r3_none") == v1);
   REQUIRE(golden<F>("n1_none") == v1);
   REQUIRE(golden<F>("p1_none") == v1);
   // A partially set suffix prunes only the unset tail.
   REQUIRE(golden<F>("r2_some_none") == golden<F>("r1_some"));
   REQUIRE(golden<F>("r3_mid") == golden<F>("r2_none_some"));
   // An unset field before a set one keeps its slot.
   REQUIRE(golden<F>("r2_none_some").size() > golden<F>("r1_some").size() - 4);
   REQUIRE(golden<F>("r2_none_some") != v1);

   // Present-but-empty values are values, not defaults: they are not pruned
   // and round-trip as present.
   const auto empty_string = golden<F>("r2_none_empty");
   REQUIRE(empty_string != v1);
   auto with_empty_string = psio::decode<R2>(F{}, empty_string);
   REQUIRE(with_empty_string.y.has_value());
   REQUIRE(with_empty_string.y->empty());
   REQUIRE_FALSE(with_empty_string.x.has_value());

   const auto empty_vector = golden<F>("r3_empty_vec");
   REQUIRE(empty_vector != v1);
   auto with_empty_vector = psio::decode<R3>(F{}, empty_vector);
   REQUIRE(with_empty_vector.z.has_value());
   REQUIRE(with_empty_vector.z->empty());
   REQUIRE_FALSE(with_empty_vector.y.has_value());

   // Some(None) of a nested optional is present as well.
   const auto some_none = golden<F>("p1_some_none");
   REQUIRE(some_none != v1);
   auto nested = psio::decode<P1>(F{}, some_none);
   REQUIRE(nested.oo.has_value());
   REQUIRE_FALSE(nested.oo->has_value());
}

TEMPLATE_TEST_CASE("pssz old data is read by newer schemas", "[pssz][evolution][forward]",
                   EVO_FORMATS)
{
   using F = TestType;
   // One, two and three appended optional fields over first-revision data.
   const auto v1 = golden<F>("r0");
   reads<F, R1>(v1, R1{7, "hi", std::nullopt}, false);
   reads<F, R2>(v1, R2{7, "hi", std::nullopt, std::nullopt}, false);
   reads<F, R3>(v1, R3{7, "hi", std::nullopt, std::nullopt, std::nullopt}, false);
   reads<F, N1>(v1, N1{7, "hi", std::nullopt}, false);
   reads<F, P1>(v1, P1{7, "hi", std::nullopt}, false);
   // Intermediate revisions read by later ones.
   reads<F, R2>(golden<F>("r1_some"), R2{7, "hi", 9, std::nullopt}, false);
   reads<F, R3>(golden<F>("r1_some"), R3{7, "hi", 9, std::nullopt, std::nullopt}, false);
   reads<F, R3>(golden<F>("r2_none_some"),
                R3{7, "hi", std::nullopt, std::string("yo"), std::nullopt}, false);
   reads<F, R3>(golden<F>("r2_none_empty"),
                R3{7, "hi", std::nullopt, std::string(), std::nullopt}, false);

   // The evolving record nested in a vector, an optional, a variant
   // alternative, and as a non-last field of a parent record.
   const VecOld old_vector{{1, "a"}, {2, "c"}};
   reads<F, VecNew>(psio::encode(F{}, old_vector),
                    VecNew{{1, "a", std::nullopt, std::nullopt},
                           {2, "c", std::nullopt, std::nullopt}},
                    false);
   reads<F, OptNew>(psio::encode(F{}, OptOld{R0{1, "a"}}),
                    OptNew{R2{1, "a", std::nullopt, std::nullopt}}, false);
   reads<F, OptNew>(psio::encode(F{}, OptOld{}), OptNew{}, false);
   reads<F, VarNew>(psio::encode(F{}, VarOld{R0{1, "a"}}),
                    VarNew{R2{1, "a", std::nullopt, std::nullopt}}, false);
   reads<F, VarNew>(psio::encode(F{}, VarOld{std::uint32_t{5}}), VarNew{std::uint32_t{5}},
                    false);
   reads<F, Parent2>(psio::encode(F{}, Parent0{{1, "a"}, "tail"}),
                     Parent2{{1, "a", std::nullopt, std::nullopt}, "tail"}, false);
}

TEMPLATE_TEST_CASE("pssz new data is read by older schemas", "[pssz][evolution][backward]",
                   EVO_FORMATS)
{
   using F = TestType;
   // Each newer frame read by every older revision of its lineage. The
   // unknown appended slots are skipped but still delimit `s`.
   for_each_vector([&](const char* name, const auto& value) {
      INFO(name);
      using T          = std::remove_cvref_t<decltype(value)>;
      const auto bytes = golden<F>(name);
      if constexpr (std::is_same_v<T, R1>)
         reads<F, R0>(bytes, old_of(value), value.x.has_value());
      else if constexpr (std::is_same_v<T, R2>)
      {
         const bool wrote_y = value.y.has_value();
         reads<F, R0>(bytes, old_of(value), value.x.has_value() || wrote_y);
         reads<F, R1>(bytes, mid_of(value), wrote_y);
      }
      else if constexpr (std::is_same_v<T, R3>)
      {
         const bool wrote_z = value.z.has_value();
         const bool wrote_y = value.y.has_value() || wrote_z;
         reads<F, R0>(bytes, old_of(value), value.x.has_value() || wrote_y);
         reads<F, R1>(bytes, mid_of(value), wrote_y);
         reads<F, R2>(bytes, mid2_of(value), wrote_z);
      }
      else if constexpr (std::is_same_v<T, N1>)
         reads<F, R0>(bytes, old_of(value), value.in.has_value());
      else if constexpr (std::is_same_v<T, P1>)
         reads<F, R0>(bytes, old_of(value), value.oo.has_value());
   });

   // Unknown fields are reported from inside containers and parents too.
   reads<F, VecOld>(golden<F>("vec_new"), VecOld{{1, "a"}, {2, "c"}}, true);
   reads<F, OptOld>(golden<F>("opt_new"), OptOld{R0{1, "a"}}, true);
   reads<F, VarOld>(golden<F>("var_new"), VarOld{R0{1, "a"}}, true);
   reads<F, Parent0>(golden<F>("parent_new"), Parent0{{1, "a"}, "tail"}, true);
   // A newer writer whose suffix happens to be unset is indistinguishable
   // from an old writer.
   reads<F, VecOld>(psio::encode(F{}, VecNew{{2, "c", std::nullopt, std::nullopt}}),
                    VecOld{{2, "c"}}, false);
}

TEMPLATE_TEST_CASE("pssz rejects unpruned and malformed record frames",
                   "[pssz][evolution][malformed]", EVO_FORMATS)
{
   using F               = TestType;
   constexpr std::size_t W  = width_of<F>::value;
   constexpr std::size_t K0 = 4 + W;      // R0 fixed region: a + slot(s)
   constexpr std::size_t K1 = 4 + 2 * W;  // R1: + slot(x)
   constexpr std::size_t K2 = 4 + 3 * W;  // R2: + slot(y)

   SECTION("a stored trailing None is non-canonical for every reader that knows the field")
   {
      // R1 with x stored as None (optional<u32> None is the empty span).
      auto fixed_none = Frame{W}.o(K1).u32(7).o(K1).o(K1 + 2).raw("hi").bytes;
      rejects<F, R1>(fixed_none);
      rejects<F, R2>(fixed_none);
      rejects<F, R3>(fixed_none);
      // R2 with x present and y stored as None (optional<string> None is 00).
      auto selector_none =
         Frame{W}.o(K2).u32(7).o(K2).o(K2 + 2).o(K2 + 6).raw("hi").u32(9).byte(0).bytes;
      rejects<F, R2>(selector_none);
      rejects<F, R3>(selector_none);
      // Both suffix fields stored as None.
      auto all_none = Frame{W}.o(K2).u32(7).o(K2).o(K2 + 2).o(K2 + 2).raw("hi").byte(0).bytes;
      rejects<F, R2>(all_none);
      rejects<F, R3>(all_none);
      // The canonical forms of the same values are accepted.
      reads<F, R1>(golden<F>("r0"), R1{7, "hi", std::nullopt}, false);
      reads<F, R2>(golden<F>("r1_some"), R2{7, "hi", 9, std::nullopt}, false);

      // An older reader cannot interpret the writer's last slot, so it
      // cannot judge pruning there: it accepts and reports unknown fields.
      reads<F, R0>(fixed_none, R0{7, "hi"}, true);
      reads<F, R1>(selector_none, R1{7, "hi", 9}, true);
   }

   SECTION("unpruned records are rejected wherever they are nested")
   {
      const auto unpruned = Frame{W}.o(K1).u32(7).o(K1).o(K1 + 2).raw("hi").bytes;
      // vector<R1> with one element: a one-slot table, then the element.
      auto in_vector = Frame{W}.o(W).bytes;
      in_vector.insert(in_vector.end(), unpruned.begin(), unpruned.end());
      rejects<F, std::vector<R1>>(in_vector);
      // optional<R1>: selector, then the record.
      std::vector<char> in_optional{1};
      in_optional.insert(in_optional.end(), unpruned.begin(), unpruned.end());
      rejects<F, std::optional<R1>>(in_optional);
      // variant<u32, R1>: alternative index, then the record.
      std::vector<char> in_variant{1};
      in_variant.insert(in_variant.end(), unpruned.begin(), unpruned.end());
      rejects<F, std::variant<std::uint32_t, R1>>(in_variant);
      // The canonical element is accepted in each position.
      const auto canonical = golden<F>("r0");
      auto       ok_vector = Frame{W}.o(W).bytes;
      ok_vector.insert(ok_vector.end(), canonical.begin(), canonical.end());
      reads<F, std::vector<R1>>(ok_vector, std::vector<R1>{{7, "hi", std::nullopt}}, false);
   }

   SECTION("the header must cover the non-optional prefix on a field boundary")
   {
      // Smaller than the non-optional prefix: `s` has no slot.
      rejects<F, R0>(Frame{W}.o(4).u32(7).bytes);
      rejects<F, R1>(Frame{W}.o(4).u32(7).bytes);
      rejects<F, R0>(Frame{W}.o(0).bytes);
      // Not on a field boundary: cuts `a`.
      rejects<F, R0>(Frame{W}.o(2).byte(7).byte(0).bytes);
      rejects<F, R3>(Frame{W}.o(2).byte(7).byte(0).bytes);
      // Truncated header and empty input.
      rejects<F, R0>(std::vector<char>{});
      if (W > 1)
      {
         rejects<F, R0>(std::vector<char>{5});
         // Cuts an optional slot (known reader) / leaves a partial unknown
         // slot (older reader): the missing suffix is not whole slots.
         auto partial = Frame{W}.o(K0 + 1).u32(7).o(K0 + 1).byte(0).raw("hi").bytes;
         rejects<F, R0>(partial);
         rejects<F, R1>(partial);
         rejects<F, R2>(partial);
      }
   }

   SECTION("the header and offsets must stay inside the frame")
   {
      // Header larger than the frame.
      rejects<F, R0>(Frame{W}.o(200).u32(7).o(200).raw("hi").bytes);
      rejects<F, R1>(Frame{W}.o(200).u32(7).o(200).raw("hi").bytes);
      rejects<F, R0>(Frame{W}.o(K0 + W).u32(7).o(K0 + W).bytes);  // unknown slot missing
      // First payload must begin exactly at the end of the fixed region.
      rejects<F, R0>(Frame{W}.o(K0).u32(7).o(K0 + 1).raw("xhi").bytes);
      rejects<F, R0>(Frame{W}.o(K0).u32(7).o(K0 - 1).raw("hi").bytes);
      // Offsets past the frame, and an unknown suffix offset running backwards.
      rejects<F, R0>(Frame{W}.o(K0).u32(7).o(K0 + 9).raw("hi").bytes);
      rejects<F, R0>(Frame{W}.o(K1).u32(7).o(K1 + 1).o(K1).raw("hi").bytes);
      rejects<F, R1>(Frame{W}.o(K1).u32(7).o(K1 + 1).o(K1).raw("hi").bytes);
      rejects<F, R0>(Frame{W}.o(K1).u32(7).o(K1).o(K1 + 3).raw("hi").bytes);
   }

   SECTION("an appended non-optional field is not a compatible revision")
   {
      // Old data lacks the required field's bytes: the new reader rejects.
      rejects<F, Bad1>(golden<F>("r0"));
      // A frame written with an appended optional is rejected too whenever
      // the slot and the required field differ in size. At four-byte offsets
      // an offset slot and a u32 are the same size, and since PSSZ carries no
      // schema on the wire the two layouts are indistinguishable there: that
      // is why the rule (append optionals only) is a schema-upgrade check,
      // not something a reader can enforce from bytes.
      if (W < 4)
         rejects<F, Bad1>(golden<F>("r1_some"));
      // Its own data is fine.
      const Bad1 value{7, "hi", 9};
      reads<F, Bad1>(psio::encode(F{}, value), value, false);
   }

   SECTION("truncations and single-byte corruptions never decode after failing validation")
   {
      // validate() is the gate for decode(): whenever it accepts, decode
      // succeeds and re-validates; frame-level failures are shared.
      for_each_vector([&](const char*, const auto& value) {
         using T          = std::remove_cvref_t<decltype(value)>;
         const auto bytes = psio::encode(F{}, value);
         for (std::size_t length = 0; length < bytes.size(); ++length)
         {
            const std::vector<char> prefix(bytes.begin(), bytes.begin() + length);
            if (psio::validate<T>(F{}, prefix).ok())
               REQUIRE(decodes<T, F>(prefix));
         }
         for (std::size_t i = 0; i < bytes.size(); ++i)
         {
            auto mutated = bytes;
            mutated[i]   = static_cast<char>(mutated[i] ^ 0x5a);
            if (psio::validate<T>(F{}, mutated).ok())
            {
               REQUIRE(decodes<T, F>(mutated));
               // Accepted means canonical or carrying unknown fields; in
               // this same-schema corpus re-encoding must reproduce it.
               bool unknown = false;
               REQUIRE(psio::validate_pssz_compatible<T>(F{}, mutated, unknown).ok());
               if (!unknown)
                  REQUIRE(psio::encode(F{}, psio::decode<T>(F{}, mutated)) == mutated);
            }
         }
      });
   }
}

TEMPLATE_TEST_CASE("pssz views read evolved records in both directions",
                   "[pssz][evolution][view]", EVO_FORMATS)
{
   using F = TestType;

   // New data, old reader: unknown slots bound the last known payload.
   const auto newest = golden<F>("r3_all");
   const auto g_r2_none_some = golden<F>("r2_none_some");
   const auto g_r2_none_empty = golden<F>("r2_none_empty");
   const auto g_n1_some = golden<F>("n1_some");
   const auto g_vec_new = golden<F>("vec_new");
   const auto g_parent_new = golden<F>("parent_new");
   {
      auto old = view_of<R0, F>(newest);
      REQUIRE(old.template get<0>() == 7);
      REQUIRE(old.template get<1>().view_() == "hi");
      auto mid = view_of<R1, F>(newest);
      REQUIRE(mid.template get<1>().view_() == "hi");
      REQUIRE(mid.template get<2>().has_value());
      REQUIRE((*mid.template get<2>()).get() == 9);
   }
   // Same schema: every field.
   {
      auto same = view_of<R3, F>(newest);
      REQUIRE(same.template get<0>() == 7);
      REQUIRE(same.template get<1>().view_() == "hi");
      REQUIRE((*same.template get<2>()).get() == 9);
      REQUIRE((*same.template get<3>()).view_() == "yo");
      auto z = *same.template get<4>();
      REQUIRE(z.size() == 2);
      REQUIRE(z[0].get() == 1);
      REQUIRE(z[1].get() == 2);
   }
   // Old data, new reader: pruned fields read as absent.
   const auto oldest = golden<F>("r0");
   {
      auto upgraded = view_of<R3, F>(oldest);
      REQUIRE(upgraded.template get<0>() == 7);
      REQUIRE(upgraded.template get<1>().view_() == "hi");
      REQUIRE_FALSE(upgraded.template get<2>().has_value());
      REQUIRE_FALSE(upgraded.template get<3>().has_value());
      REQUIRE_FALSE(upgraded.template get<4>().has_value());
      auto nested = view_of<N1, F>(oldest);
      REQUIRE_FALSE(nested.template get<2>().has_value());
   }
   // Partially written suffix: an unset middle field and a set later one.
   {
      auto partial = view_of<R3, F>(g_r2_none_some);
      REQUIRE_FALSE(partial.template get<2>().has_value());
      REQUIRE((*partial.template get<3>()).view_() == "yo");
      REQUIRE_FALSE(partial.template get<4>().has_value());
      auto older = view_of<R1, F>(g_r2_none_some);
      REQUIRE(older.template get<1>().view_() == "hi");
      REQUIRE_FALSE(older.template get<2>().has_value());
      auto present_empty = view_of<R2, F>(g_r2_none_empty);
      REQUIRE(present_empty.template get<3>().has_value());
      REQUIRE((*present_empty.template get<3>()).view_().empty());
   }
   // Nested record suffix.
   {
      auto nested = view_of<N1, F>(g_n1_some);
      REQUIRE(nested.template get<2>().has_value());
      REQUIRE((*nested.template get<2>()).template get<0>() == 3);
      REQUIRE((*nested.template get<2>()).template get<1>().view_() == "t");
   }
   // Evolving record inside a vector and as a non-last parent field.
   {
      auto elements = view_of<VecOld, F>(g_vec_new);
      REQUIRE(elements.size() == 2);
      REQUIRE(elements[0].template get<0>() == 1);
      REQUIRE(elements[0].template get<1>().view_() == "a");
      REQUIRE(elements[1].template get<1>().view_() == "c");
      auto parent = view_of<Parent0, F>(g_parent_new);
      REQUIRE(parent.template get<0>().template get<0>() == 1);
      REQUIRE(parent.template get<0>().template get<1>().view_() == "a");
      REQUIRE(parent.template get<1>().view_() == "tail");
      const auto old_parent = psio::encode(F{}, Parent0{{1, "a"}, "tail"});
      auto       upgraded   = view_of<Parent2, F>(old_parent);
      REQUIRE(upgraded.template get<0>().template get<1>().view_() == "a");
      REQUIRE_FALSE(upgraded.template get<0>().template get<2>().has_value());
      REQUIRE(upgraded.template get<1>().view_() == "tail");
   }
   // Checked construction validates the frame and reports unknown fields.
   {
      std::optional<psio::view<R0, F, psio::storage::const_borrow>> checked;
      bool                                                         unknown = false;
      REQUIRE(psio::validated_pssz_view<R0>(F{}, newest, checked, unknown).ok());
      REQUIRE(unknown);
      REQUIRE(checked->template get<1>().view_() == "hi");
      std::optional<psio::view<R1, F, psio::storage::const_borrow>> refused;
      const auto unpruned =
         Frame{width_of<F>::value}
            .o(4 + 2 * width_of<F>::value)
            .u32(7)
            .o(4 + 2 * width_of<F>::value)
            .o(6 + 2 * width_of<F>::value)
            .raw("hi")
            .bytes;
      REQUIRE_FALSE(psio::validated_pssz_view<R1>(F{}, unpruned, refused, unknown).ok());
      REQUIRE_FALSE(refused.has_value());
   }
}

// The automatic profile keeps each type's own width, so an evolving child
// with a pinned narrow budget keeps one-byte offsets inside a wide parent.
namespace evo_auto
{
   struct Small0
   {
      std::uint8_t id;
      std::string  label;
      friend bool  operator==(const Small0&, const Small0&) = default;
   };
   PSIO_REFLECT(Small0, id, label)
   struct Small1
   {
      std::uint8_t                id;
      std::string                 label;
      std::optional<std::uint8_t> flag;
      friend bool                 operator==(const Small1&, const Small1&) = default;
   };
   PSIO_REFLECT(Small1, id, label, flag)
   struct Wide0
   {
      Small0      child;
      std::string note;
      friend bool operator==(const Wide0&, const Wide0&) = default;
   };
   PSIO_REFLECT(Wide0, child, note)
   struct Wide1
   {
      Small1      child;
      std::string note;
      friend bool operator==(const Wide1&, const Wide1&) = default;
   };
   PSIO_REFLECT(Wide1, child, note)
}  // namespace evo_auto
PSIO_TYPE_ATTRS(evo_auto::Small0, psio::max_dynamic_data_spec{.value = 64})
PSIO_TYPE_ATTRS(evo_auto::Small1, psio::max_dynamic_data_spec{.value = 64})

TEST_CASE("automatic pssz evolves a pinned narrow child inside a wide parent",
          "[pssz][evolution][type-width]")
{
   using namespace evo_auto;
   using F = psio::pssz;
   STATIC_REQUIRE(psio::auto_pssz_width_v<Small0> == 1);
   STATIC_REQUIRE(psio::auto_pssz_width_v<Small1> == 1);
   STATIC_REQUIRE(psio::auto_pssz_width_v<Wide0> == 4);
   STATIC_REQUIRE(psio::auto_pssz_width_v<Wide1> == 4);

   // Golden: parent header and offsets are four bytes, the child's are one.
   const std::vector<char> v0{8, 0, 0, 0, 8, 0, 0, 0, 13, 0, 0, 0,  // header, child, note
                              2, 3, 2, 'a', 'b',                    // Small0{3,"ab"}
                              'n'};
   const std::vector<char> v1{8, 0, 0, 0, 8, 0, 0, 0, 15, 0, 0, 0,  // header, child, note
                              3, 3, 3, 5, 'a', 'b', 1,              // Small1{3,"ab",1}
                              'n'};
   REQUIRE(psio::encode(F{}, Wide0{{3, "ab"}, "n"}) == v0);
   REQUIRE(psio::encode(F{}, Wide1{{3, "ab", std::nullopt}, "n"}) == v0);
   REQUIRE(psio::encode(F{}, Wide1{{3, "ab", std::uint8_t{1}}, "n"}) == v1);
   reads<F, Wide1>(v0, Wide1{{3, "ab", std::nullopt}, "n"}, false);
   reads<F, Wide0>(v1, Wide0{{3, "ab"}, "n"}, true);
   reads<F, Wide1>(v1, Wide1{{3, "ab", std::uint8_t{1}}, "n"}, false);
   // The child's stored None is rejected at the child's own width.
   const std::vector<char> unpruned{8, 0, 0, 0, 8, 0, 0, 0, 14, 0, 0, 0,
                                    3, 3, 3, 5, 'a', 'b',
                                    'n'};
   rejects<F, Wide1>(unpruned);
   // Views select each type's width independently.
   psio::view<Wide0, F, psio::storage::const_borrow> old_view{v1};
   REQUIRE(old_view.get<0>().get<0>() == 3);
   REQUIRE(old_view.get<0>().get<1>().view_() == "ab");
   REQUIRE(old_view.get<1>().view_() == "n");
   psio::view<Wide1, F, psio::storage::const_borrow> new_view{v0};
   REQUIRE_FALSE(new_view.get<0>().get<2>().has_value());
   REQUIRE(new_view.get<1>().view_() == "n");
   // The declared budget still bounds the evolved child.
   REQUIRE_THROWS_AS(psio::encode(F{}, Wide1{{3, std::string(100, 'x'), std::nullopt}, "n"}),
                     psio::codec_exception);
}

// ── Budgeted lineages on the automatic (production) entry point ──────────
//
// The offset width is part of the logical type: maxDynamicData(N) fixes it
// for the whole lineage and caps how far optional extension may grow it.
// One lineage per width class (<= 255, <= 65535, larger).
#define EVO_BUDGET_LINEAGE(NS, BUDGET)                                                  \
   namespace NS                                                                         \
   {                                                                                    \
      struct V1                                                                         \
      {                                                                                 \
         std::uint32_t a;                                                               \
         std::string   s;                                                               \
         friend bool   operator==(const V1&, const V1&) = default;                      \
      };                                                                                \
      PSIO_REFLECT(V1, a, s)                                    \
      struct V2                                                                         \
      {                                                                                 \
         std::uint32_t                a;                                                \
         std::string                  s;                                                \
         std::optional<std::uint32_t> x;                                                \
         friend bool                  operator==(const V2&, const V2&) = default;       \
      };                                                                                \
      PSIO_REFLECT(V2, a, s, x)                                 \
      struct V3                                                                         \
      {                                                                                 \
         std::uint32_t                a;                                                \
         std::string                  s;                                                \
         std::optional<std::uint32_t> x;                                                \
         std::optional<std::string>   y;                                                \
         friend bool                  operator==(const V3&, const V3&) = default;       \
      };                                                                                \
      PSIO_REFLECT(V3, a, s, x, y)                              \
      struct Lineage                                                                    \
      {                                                                                 \
         using v1 = V1;                                                                 \
         using v2 = V2;                                                                 \
         using v3 = V3;                                                                 \
         static constexpr std::size_t budget = BUDGET;                                  \
      };                                                                                \
   }                                                                                    \
   PSIO_TYPE_ATTRS(NS::V1, psio::max_dynamic_data_spec{.value = BUDGET})                \
   PSIO_TYPE_ATTRS(NS::V2, psio::max_dynamic_data_spec{.value = BUDGET})                \
   PSIO_TYPE_ATTRS(NS::V3, psio::max_dynamic_data_spec{.value = BUDGET})
EVO_BUDGET_LINEAGE(evo_w1, 200)
EVO_BUDGET_LINEAGE(evo_w2, 60000)
EVO_BUDGET_LINEAGE(evo_w4, 1000000)

TEMPLATE_TEST_CASE("automatic pssz lineages keep their budgeted width while evolving",
                   "[pssz][evolution][type-width]", evo_w1::Lineage, evo_w2::Lineage,
                   evo_w4::Lineage)
{
   using L  = TestType;
   using V1 = typename L::v1;
   using V2 = typename L::v2;
   using V3 = typename L::v3;
   using F  = psio::pssz;
   constexpr std::size_t W = L::budget <= 0xff ? 1 : L::budget <= 0xffff ? 2 : 4;
   // Appending optionals does not move the width: the budget owns it.
   STATIC_REQUIRE(psio::auto_pssz_width_v<V1> == W);
   STATIC_REQUIRE(psio::auto_pssz_width_v<V2> == W);
   STATIC_REQUIRE(psio::auto_pssz_width_v<V3> == W);
   constexpr std::size_t K1 = 4 + W, K2 = 4 + 2 * W, K3 = 4 + 3 * W;

   // Canonical bytes, built independently of the codec.
   const auto b1 = Frame{W}.o(K1).u32(7).o(K1).raw("hi").bytes;
   const auto b2 = Frame{W}.o(K2).u32(7).o(K2).o(K2 + 2).raw("hi").u32(9).bytes;
   const auto b3 = Frame{W}.o(K3).u32(7).o(K3).o(K3 + 2).o(K3 + 6).raw("hi").u32(9).byte(1)
                      .raw("yo").bytes;
   const auto b3_gap = Frame{W}.o(K3).u32(7).o(K3).o(K3 + 2).o(K3 + 2).raw("hi").byte(1)
                          .raw("yo").bytes;
   if (W == 1)
   {
      // Literal goldens for the narrow class.
      REQUIRE(b1 == std::vector<char>{5, 7, 0, 0, 0, 5, 'h', 'i'});
      REQUIRE(b3 == std::vector<char>{7, 7, 0, 0, 0, 7, 9, 13, 'h', 'i', 9, 0, 0, 0, 1, 'y', 'o'});
   }
   if (W == 4)
      REQUIRE(b1 == std::vector<char>{8, 0, 0, 0, 7, 0, 0, 0, 8, 0, 0, 0, 'h', 'i'});

   REQUIRE(psio::encode(F{}, V1{7, "hi"}) == b1);
   REQUIRE(psio::encode(F{}, V2{7, "hi", std::nullopt}) == b1);
   REQUIRE(psio::encode(F{}, V3{7, "hi", std::nullopt, std::nullopt}) == b1);
   REQUIRE(psio::encode(F{}, V2{7, "hi", 9}) == b2);
   REQUIRE(psio::encode(F{}, V3{7, "hi", 9, std::nullopt}) == b2);
   REQUIRE(psio::encode(F{}, V3{7, "hi", 9, std::string("yo")}) == b3);
   REQUIRE(psio::encode(F{}, V3{7, "hi", std::nullopt, std::string("yo")}) == b3_gap);
   REQUIRE(psio::size_of(F{}, V3{7, "hi", 9, std::nullopt}) == b2.size());

   // Old data -> new readers.
   reads<F, V1>(b1, V1{7, "hi"}, false);
   reads<F, V2>(b1, V2{7, "hi", std::nullopt}, false);
   reads<F, V3>(b1, V3{7, "hi", std::nullopt, std::nullopt}, false);
   reads<F, V3>(b2, V3{7, "hi", 9, std::nullopt}, false);
   // New data -> old readers.
   reads<F, V1>(b2, V1{7, "hi"}, true);
   reads<F, V1>(b3, V1{7, "hi"}, true);
   reads<F, V2>(b3, V2{7, "hi", 9}, true);
   reads<F, V2>(b3_gap, V2{7, "hi", std::nullopt}, true);
   reads<F, V3>(b3, V3{7, "hi", 9, std::string("yo")}, false);
   // In the in-place vector element decode path as well.
   const std::vector<V3> list{{1, "a", 9, std::string("b")}, {2, "c", std::nullopt, std::nullopt}};
   const auto            list_bytes = psio::encode(F{}, list);
   reads<F, std::vector<V3>>(list_bytes, list, false);
   reads<F, std::vector<V1>>(list_bytes, std::vector<V1>{{1, "a"}, {2, "c"}}, true);
   reads<F, std::vector<V3>>(psio::encode(F{}, std::vector<V1>{{1, "a"}}),
                             std::vector<V3>{{1, "a", std::nullopt, std::nullopt}}, false);

   // Mandatory pruning on the automatic path.
   rejects<F, V2>(Frame{W}.o(K2).u32(7).o(K2).o(K2 + 2).raw("hi").bytes);
   rejects<F, V3>(Frame{W}.o(K2).u32(7).o(K2).o(K2 + 2).raw("hi").bytes);
   rejects<F, V3>(
      Frame{W}.o(K3).u32(7).o(K3).o(K3 + 2).o(K3 + 6).raw("hi").u32(9).byte(0).bytes);
   // Malformed headers.
   rejects<F, V1>(Frame{W}.o(4).u32(7).bytes);
   rejects<F, V2>(Frame{W}.o(2).byte(7).byte(0).bytes);
   rejects<F, V1>(Frame{W}.o(K1 + 60).u32(7).o(K1 + 60).raw("hi").bytes);

   // Views, both directions.
   auto old_view = view_of<V1, F>(b3);
   REQUIRE(old_view.template get<0>() == 7);
   REQUIRE(old_view.template get<1>().view_() == "hi");
   auto new_view = view_of<V3, F>(b1);
   REQUIRE(new_view.template get<1>().view_() == "hi");
   REQUIRE_FALSE(new_view.template get<2>().has_value());
   REQUIRE_FALSE(new_view.template get<3>().has_value());
   auto full_view = view_of<V3, F>(b3);
   REQUIRE((*full_view.template get<2>()).get() == 9);
   REQUIRE((*full_view.template get<3>()).view_() == "yo");
}

TEST_CASE("automatic pssz enforces the lineage budget on evolved values",
          "[pssz][evolution][type-width]")
{
   using namespace evo_w1;  // budget 200, one-byte offsets
   using F = psio::pssz;
   // V1 frame: header 1 + a 4 + slot 1 + s. V3 adds two slots, x (4) and
   // y (selector + bytes).
   const std::string s(185, 's');
   // V1 uses 191 bytes; appending x makes 196; y="ab" makes 200 exactly.
   const V3 at_budget{7, s, 9, std::string("ab")};
   const auto wire = psio::encode(F{}, at_budget);
   REQUIRE(wire.size() == 200);
   reads<F, V3>(wire, at_budget, false);
   reads<F, V1>(wire, V1{7, s}, true);

   // One more byte in the appended optional exceeds the type's budget.
   const V3 over{7, s, 9, std::string("abc")};
   std::vector<char> destination{'p'};
   REQUIRE_THROWS_AS(psio::encode(F{}, over), psio::codec_exception);
   REQUIRE_THROWS_AS(psio::encode(F{}, over, destination), psio::codec_exception);
   REQUIRE(destination == std::vector<char>{'p'});
   REQUIRE_THROWS_AS(psio::size_of(F{}, over), psio::codec_exception);
   // The same value without the suffix is fine: only the extension overflowed.
   REQUIRE(psio::encode(F{}, V3{7, s, std::nullopt, std::nullopt}).size() == 191);

   // The over-budget frame itself, built by hand, is rejected by every reader
   // of the lineage (the budget belongs to the type, not to a revision).
   auto too_big = Frame{1}.o(7).u32(7).o(7).o(192).o(196).raw(s).u32(9).byte(1).raw("abc").bytes;
   REQUIRE(too_big.size() == 201);
   rejects<F, V3>(too_big);
   rejects<F, V2>(too_big);
   rejects<F, V1>(too_big);
   // One byte shorter is the accepted at-budget frame.
   auto just_fits = Frame{1}.o(7).u32(7).o(7).o(192).o(196).raw(s).u32(9).byte(1).raw("ab").bytes;
   REQUIRE(just_fits == wire);
}

// ── Parent and child at different widths ─────────────────────────────────
namespace evo_mixed
{
   // Wide child (four-byte offsets) inside a narrow-header parent.
   struct Big0
   {
      std::uint8_t id;
      friend bool  operator==(const Big0&, const Big0&) = default;
   };
   PSIO_REFLECT(Big0, id)
   struct Big1
   {
      std::uint8_t                id;
      std::optional<std::uint8_t> flag;
      friend bool                 operator==(const Big1&, const Big1&) = default;
   };
   PSIO_REFLECT(Big1, id, flag)
   struct Narrow0  // parent, first revision
   {
      Big0          child;
      std::uint32_t sequence;
      friend bool   operator==(const Narrow0&, const Narrow0&) = default;
   };
   PSIO_REFLECT(Narrow0, child, sequence)
   struct NarrowChild1  // same parent, evolved child
   {
      Big1          child;
      std::uint32_t sequence;
      friend bool   operator==(const NarrowChild1&, const NarrowChild1&) = default;
   };
   PSIO_REFLECT(NarrowChild1, child, sequence)
   struct NarrowParent1  // evolved parent, original child
   {
      Big0                        child;
      std::uint32_t               sequence;
      std::optional<std::string>  memo;
      friend bool operator==(const NarrowParent1&, const NarrowParent1&) = default;
   };
   PSIO_REFLECT(NarrowParent1, child, sequence, memo)
}  // namespace evo_mixed
PSIO_TYPE_ATTRS(evo_mixed::Big0, psio::max_dynamic_data_spec{.value = 1000000})
PSIO_TYPE_ATTRS(evo_mixed::Big1, psio::max_dynamic_data_spec{.value = 1000000})
PSIO_TYPE_ATTRS(evo_mixed::Narrow0, psio::max_dynamic_data_spec{.value = 64})
PSIO_TYPE_ATTRS(evo_mixed::NarrowChild1, psio::max_dynamic_data_spec{.value = 64})
PSIO_TYPE_ATTRS(evo_mixed::NarrowParent1, psio::max_dynamic_data_spec{.value = 64})

TEST_CASE("automatic pssz evolves across a width boundary between parent and child",
          "[pssz][evolution][type-width]")
{
   using namespace evo_mixed;
   using F = psio::pssz;
   STATIC_REQUIRE(psio::auto_pssz_width_v<Big0> == 4);
   STATIC_REQUIRE(psio::auto_pssz_width_v<Big1> == 4);
   STATIC_REQUIRE(psio::auto_pssz_width_v<Narrow0> == 1);
   STATIC_REQUIRE(psio::auto_pssz_width_v<NarrowChild1> == 1);
   STATIC_REQUIRE(psio::auto_pssz_width_v<NarrowParent1> == 1);

   // Parent: one-byte header and slot. Child: four-byte header.
   const std::vector<char> base{5, 5, 17, 0, 0, 0,  // header, child slot, sequence
                                1, 0, 0, 0, 42};    // Big0{42}
   REQUIRE(psio::encode(F{}, Narrow0{{42}, 17}) == base);
   REQUIRE(psio::encode(F{}, NarrowChild1{{42, std::nullopt}, 17}) == base);
   REQUIRE(psio::encode(F{}, NarrowParent1{{42}, 17, std::nullopt}) == base);

   // Evolving the wide child.
   const std::vector<char> child_new{5, 5, 17, 0, 0, 0,
                                     5, 0, 0, 0, 42, 5, 0, 0, 0, 1};  // Big1{42, 1}
   REQUIRE(psio::encode(F{}, NarrowChild1{{42, std::uint8_t{1}}, 17}) == child_new);
   reads<F, NarrowChild1>(child_new, NarrowChild1{{42, std::uint8_t{1}}, 17}, false);
   reads<F, Narrow0>(child_new, Narrow0{{42}, 17}, true);
   reads<F, NarrowChild1>(base, NarrowChild1{{42, std::nullopt}, 17}, false);
   // The child's unpruned None, at the child's four-byte width.
   rejects<F, NarrowChild1>(
      std::vector<char>{5, 5, 17, 0, 0, 0, 5, 0, 0, 0, 42, 5, 0, 0, 0});

   // Evolving the narrow parent: the new slot bounds the child's payload.
   const std::vector<char> parent_new{6, 6, 17, 0, 0, 0, 11,  // header, child, sequence, memo
                                      1, 0, 0, 0, 42,         // Big0{42}
                                      1, 'm'};                // Some("m")
   REQUIRE(psio::encode(F{}, NarrowParent1{{42}, 17, std::string("m")}) == parent_new);
   reads<F, NarrowParent1>(parent_new, NarrowParent1{{42}, 17, std::string("m")}, false);
   reads<F, Narrow0>(parent_new, Narrow0{{42}, 17}, true);
   reads<F, NarrowParent1>(base, NarrowParent1{{42}, 17, std::nullopt}, false);
   rejects<F, NarrowParent1>(
      std::vector<char>{6, 6, 17, 0, 0, 0, 11, 1, 0, 0, 0, 42, 0});  // stored None

   // Views across the boundary.
   auto old_view = view_of<Narrow0, F>(child_new);
   REQUIRE(old_view.get<0>().get<0>() == 42);
   REQUIRE(old_view.get<1>() == 17);
   auto new_view = view_of<NarrowChild1, F>(base);
   REQUIRE(new_view.get<0>().get<0>() == 42);
   REQUIRE_FALSE(new_view.get<0>().get<1>().has_value());
   auto parent_view = view_of<Narrow0, F>(parent_new);
   REQUIRE(parent_view.get<0>().get<0>() == 42);
   auto memo_view = view_of<NarrowParent1, F>(parent_new);
   REQUIRE((*memo_view.get<2>()).view_() == "m");
}

// ── A lineage without a declared budget ──────────────────────────────────
//
// This pins today's behavior; it is not a policy. Without maxDynamicData the
// width is inferred from the current fields, so appending an optional can
// move a type across a width threshold. The two revisions then disagree about
// the header and slot width and are NOT wire compatible. The codec cannot
// detect this from the bytes: per docs/pssz-type-width.md a consuming SDK
// must record the compiled width in its ABI manifest and reject the change.
namespace evo_unbudgeted
{
   struct U0
   {
      std::array<std::uint8_t, 251> blob;
      friend bool                   operator==(const U0&, const U0&) = default;
   };
   PSIO_REFLECT(U0, blob)
   struct U1
   {
      std::array<std::uint8_t, 251> blob;
      std::optional<std::uint8_t>   extra;
      friend bool                   operator==(const U1&, const U1&) = default;
   };
   PSIO_REFLECT(U1, blob, extra)
}  // namespace evo_unbudgeted

TEST_CASE("automatic pssz without a budget can change width when a field is appended",
          "[pssz][evolution][type-width]")
{
   using namespace evo_unbudgeted;
   using F = psio::pssz;
   STATIC_REQUIRE(psio::max_encoded_size<U0>() == std::optional<std::size_t>{255});
   STATIC_REQUIRE(psio::auto_pssz_width_v<U0> == 1);
   STATIC_REQUIRE(psio::auto_pssz_width_v<U1> == 2);  // crossed the 255 threshold

   U0 old_value{};
   old_value.blob[0] = 9;
   const auto old_bytes = psio::encode(F{}, old_value);
   REQUIRE(old_bytes.size() == 252);  // one-byte header
   U1 new_value{old_value.blob, std::nullopt};
   const auto new_bytes = psio::encode(F{}, new_value);
   REQUIRE(new_bytes.size() == 253);  // two-byte header, suffix pruned
   // Same logical content, different bytes: the revisions do not interoperate.
   REQUIRE(old_bytes != new_bytes);
   REQUIRE_FALSE(psio::validate<U1>(F{}, old_bytes).ok());
   REQUIRE_FALSE(psio::validate<U0>(F{}, new_bytes).ok());
   // Under an explicit shared width the same two schemas are compatible,
   // which is what a declared budget provides.
   REQUIRE(psio::encode(psio::pssz16{}, old_value) == psio::encode(psio::pssz16{}, new_value));
   REQUIRE(psio::validate<U1>(psio::pssz16{}, psio::encode(psio::pssz16{}, old_value)).ok());
}
