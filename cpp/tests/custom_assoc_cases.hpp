// Transparent types, custom text forms and invariants together with maps,
// sets, pairs and psio::nested<T>:
//
//   - an invariant runs on a map's keys and mapped values and on a set's
//     elements, inside nested bytes, and through a transparent wrapper, in
//     pssz, key, json / ijson and — given the handlers — the schema-driven
//     codecs;
//   - a transparent wrapper of a map, a set, a pair or a nested value is
//     that value;
//   - a map keyed by a transparent wrapper of a string, or by a type with a
//     custom text form, is a JSON object.
//
// Shared by a Catch test and by the executable built with exceptions and
// RTTI disabled: no case here may throw or abort.
//
// run() returns the number of checks that did not hold (each is printed).

#pragma once

#include "custom_conformance_types.hpp"

#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/nested.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/pssz_view.hpp>
#include <psio/schema_builder.hpp>

#include <cstdio>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace custom_assoc_cases
{
   using namespace custom_conf;
   namespace ir = psio::pssz_ir;
   namespace pi = psio::detail::pssz_impl;
   using Frame  = std::vector<char>;

   // custom_conf::Tag is a transparent wrapper of a string: a string in every
   // format, so a map keyed by it is a JSON object.
   // The same with an invariant: lower-case letters, not empty.
   struct Slug
   {
      std::string text;
      friend bool operator==(const Slug&, const Slug&) = default;
      friend auto operator<=>(const Slug&, const Slug&) = default;
      static psio::codec_status check(const Slug& s) noexcept
      {
         bool ok = !s.text.empty();
         for (char c : s.text)
            ok = ok && c >= 'a' && c <= 'z';
         return ok ? psio::codec_ok()
                   : psio::codec_fail("Slug: lower-case letters only", 0, "Slug");
      }
      PSIO_REFLECT(Slug, transparent(), invariant(check), text)
   };
   inline constexpr std::string_view slug_reason  = "Slug: lower-case letters only";
   inline constexpr std::string_view name_reason  = "Name: value is not a canonical name";
   inline constexpr std::string_view range_reason = "Range: lo is greater than hi";
   inline constexpr std::string_view level_reason = "Level: not a listed level";
   inline constexpr std::string_view ref_reason   = "Ref: a name is not empty";

   // Transparent wrappers of a map, a set, a pair and a nested value.
   struct Totals
   {
      std::map<std::string, std::uint32_t> lines;
      friend bool operator==(const Totals&, const Totals&) = default;
      PSIO_REFLECT(Totals, transparent(), lines)
   };
   // ... a map with an invariant of its own: the lines add up to at most 100.
   struct Budget
   {
      std::map<Tag, std::uint32_t> lines;
      friend bool operator==(const Budget&, const Budget&) = default;
      static psio::codec_status check(const Budget& b) noexcept
      {
         std::uint64_t sum = 0;
         for (const auto& line : b.lines)
            sum += line.second;
         return sum <= 100 ? psio::codec_ok()
                           : psio::codec_fail("Budget: over 100", 0, "Budget");
      }
      PSIO_REFLECT(Budget, transparent(), invariant(check), lines)
   };
   inline constexpr std::string_view budget_reason = "Budget: over 100";
   struct Members
   {
      std::set<Name> names;
      friend bool operator==(const Members&, const Members&) = default;
      PSIO_REFLECT(Members, transparent(), names)
   };
   struct Edge
   {
      std::pair<Name, Tag> ends;
      friend bool operator==(const Edge&, const Edge&) = default;
      PSIO_REFLECT(Edge, transparent(), ends)
   };
   struct Sealed
   {
      psio::nested<Range> window;
      friend bool operator==(const Sealed&, const Sealed&) = default;
      PSIO_REFLECT(Sealed, transparent(), window)
   };

   // Level's wire type as an enum of its own, without the invariant.
   enum class RawLevel : std::uint8_t {};

   using ByName   = std::map<Name, Range>;                 // invariant key and value
   using Levels   = std::set<Level>;                       // invariant element (an enum)
   using ByTag    = std::map<Tag, std::uint32_t>;          // transparent string key
   using SlugMap  = std::map<Slug, std::uint32_t>;         // ... with an invariant
   using ByBlob   = std::map<Blob, std::uint8_t>;          // custom text key (transparent)
   using ByPeer   = std::map<Endpoint, std::uint8_t>;      // custom text key (a variant)
   using ByRef    = std::map<Ref, std::uint8_t>;           // custom text and invariant
   using Packed   = psio::nested<Range>;
   using PackedBy = std::map<std::string, psio::nested<Name>>;
   using Later    = psio::nested<Range, psio::nested_validation::deferred>;

   struct Everything
   {
      ByName   by_name;
      Levels   levels;
      ByTag    by_tag;
      SlugMap  by_slug;
      ByBlob   by_blob;
      ByPeer   by_peer;
      ByRef    by_ref;
      Packed   packed;
      PackedBy packed_by;
      Totals   totals;
      Budget   budget;
      Members  members;
      Edge     edge;
      Sealed   sealed;
      friend bool operator==(const Everything&, const Everything&) = default;
      PSIO_REFLECT(Everything, by_name, levels, by_tag, by_slug, by_blob, by_peer, by_ref,
                   packed, packed_by, totals, budget, members, edge, sealed)
   };

   static_assert(psio::contains_invariant_v<ByName> && psio::contains_invariant_v<Levels>);
   static_assert(psio::contains_invariant_v<Packed> && psio::contains_invariant_v<PackedBy>);
   static_assert(psio::contains_invariant_v<Sealed> && psio::contains_invariant_v<Everything>);
   static_assert(!psio::contains_invariant_v<ByTag> && !psio::contains_invariant_v<Totals>);
   static_assert(psio::detail::json_impl::text_key_v<psio::json, Tag> &&
                 psio::detail::json_impl::text_key_v<psio::ijson, Slug> &&
                 psio::detail::json_impl::text_key_v<psio::json, Blob> &&
                 psio::detail::json_impl::text_key_v<psio::json, Endpoint> &&
                 !psio::detail::json_impl::text_key_v<psio::json, Name> &&
                 !psio::detail::json_impl::text_key_v<psio::json, Id>);
   // A transparent wrapper is its member: same widths, sizes and evolution.
   static_assert(psio::auto_pssz_width_v<Totals> ==
                 psio::auto_pssz_width_v<std::map<std::string, std::uint32_t>>);
   static_assert(psio::pssz_evolves_from<Totals, Totals> &&
                 psio::pssz_evolves_from<Sealed, Sealed>);

   inline int& failures()
   {
      static int count = 0;
      return count;
   }
   // The case being run (the line of the call in violations()).
   inline int& context()
   {
      static int line = 0;
      return line;
   }
#define CA_CHECK(...)                                                                    \
   do                                                                                     \
   {                                                                                      \
      if (!(__VA_ARGS__))                                                                 \
      {                                                                                   \
         ++::custom_assoc_cases::failures();                                              \
         std::fprintf(stderr, "%s:%d (case at line %d): failed: %s\n", __FILE__, __LINE__,  \
                      ::custom_assoc_cases::context(), #__VA_ARGS__);                     \
      }                                                                                   \
   } while (false)

   inline bool is(const psio::codec_status& status, std::string_view reason)
   {
      if (!status.ok() && status.error().what == reason)
         return true;
      std::fprintf(stderr, "   status: %s\n",
                   status.ok() ? "ok" : std::string(status.error().what).c_str());
      return false;
   }
   inline std::span<const char> sp(const Frame& b) { return {b.data(), b.size()}; }
   inline std::span<const char> sp(const std::string& s) { return {s.data(), s.size()}; }

   inline ir::CustomHandlers handlers()
   {
      ir::CustomHandlers h;
      for_each_custom([&]<typename T>(std::type_identity<T>) { h.template add<T>(); });
      h.add<Slug>().add<Budget>();
      return h;
   }
   template <typename T>
   psio::Schema schema_of()
   {
      return psio::SchemaBuilder{}.insert<T>("T").build();
   }
   template <typename T>
   Frame frame_of(const T& value)
   {
      Frame out;
      CA_CHECK(psio::encode_pssz_checked(psio::pssz{}, value, out).ok());
      return out;
   }
   // The frame a writer that does not check invariants produces.
   template <typename T>
   Frame unchecked_frame(const T& value)
   {
      Frame out;
      CA_CHECK(pi::encode_ignoring_invariants(psio::pssz{}, value, out).ok());
      return out;
   }
   inline Packed packed_unchecked(const Range& range)
   {
      return Packed::unchecked(unchecked_frame(range));
   }

   inline Everything sample()
   {
      Everything e;
      e.by_name   = {{Name{7}, Range{1, 2}}, {Name{3}, Range{0, 0}}};
      e.levels    = {Level::top, Level::low};
      e.by_tag    = {{Tag{"b"}, 2}, {Tag{"a"}, 1}};
      e.by_slug   = {{Slug{"two"}, 2}, {Slug{"one"}, 1}};
      e.by_blob   = {{Blob{{1, 2, 3}}, 1}, {Blob{{}}, 0}};
      e.by_peer   = {{Endpoint{V4{{127, 0, 0, 1}, 80}}, 4}, {Endpoint{V6{{}, 443}}, 6}};
      e.by_ref    = {{Ref{std::string{"alice"}}, 1}, {Ref{std::uint64_t{9}}, 2}};
      e.packed    = Packed::from(Range{3, 9});
      e.packed_by = {{"n", psio::nested<Name>::from(Name{5})}};
      e.totals    = Totals{{{"b", 2}, {"a", 1}}};
      e.budget    = Budget{{{Tag{"rent"}, 60}, {Tag{"food"}, 40}}};
      e.members   = Members{{Name{2}, Name{1}}};
      e.edge      = Edge{{Name{1}, Tag{"to"}}};
      e.sealed    = Sealed{Packed::from(Range{1, 1})};
      return e;
   }

   // ── A valid value: every codec, typed and schema-driven, agrees ─────────
   inline void valid_values()
   {
      const Everything value = sample();
      const Frame      bytes = frame_of(value);
      CA_CHECK(psio::validate<Everything>(psio::pssz{}, sp(bytes)).ok());
      bool       unknown = true;
      Everything back;
      CA_CHECK(psio::decode_pssz_checked(psio::pssz{}, sp(bytes), back, unknown).ok() && !unknown);
      CA_CHECK(back == value);

      // A transparent wrapper is its member, whatever the member is.
      CA_CHECK(frame_of(value.totals) == frame_of(value.totals.lines));
      CA_CHECK(frame_of(value.budget) == frame_of(value.budget.lines));
      CA_CHECK(frame_of(value.members) == frame_of(value.members.names));
      CA_CHECK(frame_of(value.edge) ==
               frame_of(std::tuple<std::uint64_t, std::string>{1, "to"}));
      CA_CHECK(frame_of(value.sealed) == frame_of(value.sealed.window));
      CA_CHECK(frame_of(value.by_tag) ==
               frame_of(std::map<std::string, std::uint32_t>{{"a", 1}, {"b", 2}}));
      const psio::view<Everything, psio::pssz> view{sp(bytes)};
      CA_CHECK(view.get<9>().size() == 2);              // Totals: the map's view
      CA_CHECK(view.get<11>().size() == 2);             // Members: the set's view
      CA_CHECK(view.get<13>().get().get<1>() == 1u);    // Sealed: the nested view
      CA_CHECK(view.get<12>().second().view_() == "to");  // Edge: the pair's view

      // Keys: a transparent wrapper has its member's key.
      const auto key = psio::to_key(value);
      CA_CHECK(psio::validate<Everything>(psio::key{}, std::span<const char>{key}).ok());
      CA_CHECK(psio::to_key(value.totals) == psio::to_key(value.totals.lines));
      CA_CHECK(psio::to_key(value.sealed) == psio::to_key(value.sealed.window));
      CA_CHECK(psio::to_key(value.members) ==
               psio::to_key(std::vector<std::uint64_t>{1, 2}));

      // Text: a map keyed by a transparent string or by a custom text form
      // is an object; by a record or a transparent integer, pairs.
      for (int pass = 0; pass < 2; ++pass)
      {
         auto text_of = [&](const auto& v) {
            std::string out;
            CA_CHECK((pass ? psio::encode_json_checked(psio::ijson{}, v, out)
                           : psio::encode_json_checked(psio::json{}, v, out))
                        .ok());
            return out;
         };
         CA_CHECK(text_of(value.by_tag) == R"({"a":1,"b":2})");
         CA_CHECK(text_of(value.by_slug) == R"({"one":1,"two":2})");
         CA_CHECK(text_of(value.by_blob) == R"({"":0,"AQID":1})");
         CA_CHECK(text_of(value.by_peer) ==
                  R"({"127.0.0.1:80":4,"[0000:0000:0000:0000:0000:0000:0000:0000]:443":6})");
         CA_CHECK(text_of(value.by_ref) == R"({"#9":2,"@alice":1})");
         CA_CHECK(text_of(value.totals) == R"({"a":1,"b":2})");
         CA_CHECK(text_of(value.budget) == R"({"food":40,"rent":60})");
         CA_CHECK(text_of(value.levels) == R"(["low","top"])");
         CA_CHECK(text_of(value.packed) == R"({"lo":3,"hi":9})");
         CA_CHECK(text_of(value.sealed) == R"({"lo":1,"hi":1})");
         CA_CHECK(text_of(std::map<Id, std::uint8_t>{{Id{2}, 1}}) == "[[2,1]]");

         const std::string text = text_of(value);
         Everything        read;
         CA_CHECK((pass ? psio::decode_json_checked(psio::ijson{}, sp(text), read)
                        : psio::decode_json_checked(psio::json{}, sp(text), read))
                     .ok());
         CA_CHECK(read == value);

         // Schema-driven: the same text with the handlers, both ways, and
         // the same bytes.
         const auto         schema = schema_of<Everything>();
         const auto         h      = handlers();
         std::string        dynamic;
         Frame              again;
         CA_CHECK((pass ? ir::pssz_to_json(schema, "T", psio::ijson{}, sp(bytes), dynamic,
                                           nullptr, &h)
                        : ir::pssz_to_json(schema, "T", psio::json{}, sp(bytes), dynamic,
                                           nullptr, &h))
                     .ok());
         CA_CHECK(dynamic == text);
         CA_CHECK((pass ? ir::json_to_pssz(schema, "T", psio::ijson{}, sp(text), again, nullptr,
                                           psio::json_parser::automatic, &h)
                        : ir::json_to_pssz(schema, "T", psio::json{}, sp(text), again, nullptr,
                                           psio::json_parser::automatic, &h))
                     .ok());
         CA_CHECK(again == bytes);
      }

      // Schema-driven binary: with no registry, with the handlers, and with
      // a strict registry that has them all.
      const auto schema = schema_of<Everything>();
      auto       h      = handlers();
      CA_CHECK(ir::validate_pssz(schema, "T", sp(bytes)).ok());
      CA_CHECK(ir::validate_pssz(schema, "T", sp(bytes), nullptr, &h).ok());
      h.require_all = true;
      CA_CHECK(ir::validate_pssz(schema, "T", sp(bytes), nullptr, &h).ok());
      ir::Value dynamic;
      CA_CHECK(ir::decode_auto(schema, "T", sp(bytes), dynamic, unknown, &h).ok());
      Frame again, dynamic_key;
      CA_CHECK(ir::encode_auto(schema, "T", dynamic, again, &h).ok());
      CA_CHECK(again == bytes);
      CA_CHECK(ir::to_key(schema, "T", dynamic, dynamic_key, &h).ok());
      CA_CHECK(dynamic_key == Frame(key.begin(), key.end()));
   }

   // Every checked typed pssz entry point on a value that violates `reason`,
   // and the schema-driven ones: accepted without the handlers (the node is
   // its wire type), rejected with them, with the invariant's own message.
   template <typename T>
   void violation(const T& bad, std::string_view reason, bool encoder_sees_it = true)
   {
      const Frame bytes = unchecked_frame(bad);
      CA_CHECK(is(psio::validate<T>(psio::pssz{}, sp(bytes)), reason));
#if defined(__cpp_exceptions)
      CA_CHECK(is(psio::validate_strict<T>(psio::pssz{}, sp(bytes)), reason));
#endif
      bool unknown = false;
      CA_CHECK(is(psio::validate_pssz_compatible<T>(psio::pssz{}, sp(bytes), unknown), reason));
      T out{};
      CA_CHECK(is(psio::decode_pssz_checked(psio::pssz{}, sp(bytes), out, unknown), reason));
      if (encoder_sees_it)
      {
         Frame sink{'x'};
         CA_CHECK(is(psio::encode_pssz_checked(psio::pssz{}, bad, sink), reason));
         CA_CHECK(sink == Frame{'x'});
         std::string text = "x";
         CA_CHECK(is(psio::encode_json_checked(psio::json{}, bad, text), reason));
         CA_CHECK(is(psio::encode_json_checked(psio::ijson{}, bad, text), reason));
         CA_CHECK(text == "x");
      }
      // An explicit-width frame.
      Frame wide;
      CA_CHECK(pi::encode_ignoring_invariants(psio::pssz_<4>{}, bad, wide).ok());
      CA_CHECK(is(psio::validate<T>(psio::pssz_<4>{}, sp(wide)), reason));

      const auto schema = schema_of<T>();
      auto       h      = handlers();
      CA_CHECK(ir::validate_pssz(schema, "T", sp(bytes)).ok());
      CA_CHECK(is(ir::validate_pssz(schema, "T", sp(bytes), nullptr, &h), reason));
      ir::Value value;
      CA_CHECK(is(ir::decode_auto(schema, "T", sp(bytes), value, unknown, &h), reason));
      CA_CHECK(is(ir::decode_exact<4>(schema, "T", sp(wide), value, &h), reason));
      // The dynamic value itself (decoded without the handlers) cannot be
      // written with them: bytes, key or text.
      CA_CHECK(ir::decode_auto(schema, "T", sp(bytes), value, unknown).ok());
      Frame sink{'x'};
      CA_CHECK(is(ir::encode_auto(schema, "T", value, sink, &h), reason));
      CA_CHECK(is(ir::encode<4>(schema, "T", value, sink, &h), reason));
      CA_CHECK(sink == Frame{'x'});
      std::string text = "x";
      CA_CHECK(is(ir::value_to_json(schema, "T", psio::ijson{}, value, text, &h), reason));
      CA_CHECK(is(ir::pssz_to_json(schema, "T", psio::json{}, sp(bytes), text, nullptr, &h),
                  reason));
      CA_CHECK(text == "x");
      // Without the handlers the text is the wire type's, and reads back to
      // the same bytes; with them that text is the violation again.
      std::string plain;
      CA_CHECK(ir::pssz_to_json(schema, "T", psio::ijson{}, sp(bytes), plain).ok());
      Frame again;
      CA_CHECK(ir::json_to_pssz(schema, "T", psio::ijson{}, sp(plain), again).ok());
      CA_CHECK(again == bytes);
   }

   // Text that reads as a value violating `reason`: typed, and schema-driven
   // with the handlers.
   template <typename T>
   void text_violation(std::string_view text, std::string_view reason,
                       std::string_view ijson_text = {})
   {
      if (ijson_text.empty())
         ijson_text = text;
      T out{};
      CA_CHECK(is(psio::decode_json_checked(psio::json{}, text, out), reason));
      CA_CHECK(is(psio::decode_json_checked(psio::ijson{}, ijson_text, out), reason));
      CA_CHECK(is(psio::validate<T>(psio::json{}, std::span<const char>{text}), reason));
      const auto schema = schema_of<T>();
      const auto h      = handlers();
      Frame      sink{'x'};
      for (const auto parser : {psio::json_parser::automatic, psio::json_parser::text})
      {
         CA_CHECK(is(ir::json_to_pssz(schema, "T", psio::json{}, text, sink, nullptr, parser, &h),
                     reason));
         CA_CHECK(is(ir::json_to_pssz(schema, "T", psio::ijson{}, ijson_text, sink, nullptr,
                                      parser, &h),
                     reason));
      }
      CA_CHECK(sink == Frame{'x'});
   }

   // A key (the bytes of the list twin) whose value violates `reason`.
   template <typename T, typename Twin>
   void key_violation(const Twin& twin, std::string_view reason)
   {
      const auto key = psio::to_key(twin);
      CA_CHECK(psio::validate<Twin>(psio::key{}, std::span<const char>{key}).ok());
      CA_CHECK(is(psio::validate<T>(psio::key{}, std::span<const char>{key}), reason));
      CA_CHECK(is(psio::validate_strict<T>(psio::key{}, std::span<const char>{key}), reason));
   }

   inline void violations()
   {
      const Name  bad_name{std::uint64_t{1} << 60};
      const Range bad_range{5, 1};
      const auto  bad_level = static_cast<Level>(9);

      // ── Map keys, mapped values, set elements ───────────────────────────
      context() = __LINE__;
      violation(ByName{{bad_name, Range{0, 1}}}, name_reason);
      context() = __LINE__;
      violation(ByName{{Name{1}, Range{0, 1}}, {Name{2}, bad_range}}, range_reason);
      context() = __LINE__;
      violation(Levels{Level::low, bad_level}, level_reason);
      context() = __LINE__;
      violation(SlugMap{{Slug{"ok"}, 1}, {Slug{"Not ok"}, 2}}, slug_reason);
      context() = __LINE__;
      violation(ByRef{{Ref{std::string{}}, 1}}, ref_reason);
      context() = __LINE__;
      violation(std::unordered_map<std::uint8_t, std::vector<Name>>{{1, {Name{1}, bad_name}}},
                name_reason);
      context() = __LINE__;
      violation(std::map<std::string, std::set<Level>>{{"k", {bad_level}}}, level_reason);
      // The key is checked before its mapped value (wire order).
      context() = __LINE__;
      violation(ByName{{bad_name, bad_range}}, name_reason);

      context() = __LINE__;
      key_violation<ByName>(
         std::vector<std::tuple<std::uint64_t, std::tuple<std::uint32_t, std::uint32_t>>>{
            {1, {5, 1}}},
         range_reason);
      context() = __LINE__;
      key_violation<ByName>(
         std::vector<std::tuple<std::uint64_t, std::tuple<std::uint32_t, std::uint32_t>>>{
            {std::uint64_t{1} << 60, {0, 1}}},
         name_reason);
      context() = __LINE__;
      key_violation<Levels>(std::vector<RawLevel>{RawLevel{0}, RawLevel{9}}, level_reason);
      context() = __LINE__;
      key_violation<SlugMap>(std::vector<std::tuple<std::string, std::uint32_t>>{{"A", 1}},
                             slug_reason);

      context() = __LINE__;
      text_violation<ByName>(R"([[{"value":1152921504606846976},{"lo":0,"hi":1}]])", name_reason,
                             R"([[{"value":"1152921504606846976"},{"lo":0,"hi":1}]])");
      context() = __LINE__;
      text_violation<ByName>(R"([[{"value":1},{"lo":5,"hi":1}]])", range_reason);
      context() = __LINE__;
      text_violation<Levels>(R"(["low",9])", level_reason);
      // A key read from an object member name is a value like any other.
      context() = __LINE__;
      text_violation<SlugMap>(R"({"ok":1,"Not ok":2})", slug_reason);
      context() = __LINE__;
      text_violation<ByRef>(R"({"@":1})", ref_reason);
      context() = __LINE__;
      text_violation<Budget>(R"({"rent":60,"food":41})", budget_reason);

      // ── Inside nested bytes ─────────────────────────────────────────────
      context() = __LINE__;
      violation(packed_unchecked(bad_range), range_reason, false);
      context() = __LINE__;
      violation(PackedBy{{"n", psio::nested<Name>::unchecked(unchecked_frame(bad_name))}},
                name_reason, false);
      context() = __LINE__;
      text_violation<Packed>(R"({"lo":5,"hi":1})", range_reason);
      context() = __LINE__;
      text_violation<PackedBy>(R"({"n":{"value":1152921504606846976}})", name_reason,
                               R"({"n":{"value":"1152921504606846976"}})");
      {
         // The value type's own checks.
         Packed      out;
         const Frame inner = unchecked_frame(bad_range);
         CA_CHECK(is(Packed::from_checked(bad_range, out), range_reason));
         CA_CHECK(is(Packed::checked(sp(inner), out), range_reason));
         CA_CHECK(out == Packed{});
         const Packed adopted = Packed::unchecked(inner);
         CA_CHECK(is(adopted.validate(), range_reason));
         CA_CHECK(is(adopted.validate_strict(), range_reason));
         Range decoded;
         CA_CHECK(is(adopted.decode_checked(decoded), range_reason));
         // Its key: the reader validates the bytes as the inner type.
         const auto key = psio::to_key(std::vector<std::uint8_t>(inner.begin(), inner.end()));
         CA_CHECK(!psio::validate<Packed>(psio::key{}, std::span<const char>{key}).ok());
         // Deferred: an enclosing reader does not look inside.
         CA_CHECK(psio::validate<Later>(psio::pssz{}, sp(frame_of(std::vector<std::uint8_t>(
                                                         inner.begin(), inner.end()))))
                     .ok());
         // Schema-driven, told the bytes are opaque: not checked either.
         const auto schema = schema_of<Packed>();
         const auto h      = handlers();
         const Frame outer = unchecked_frame(adopted);
         CA_CHECK(ir::validate_pssz(schema, "T", sp(outer), nullptr, &h,
                                    ir::nested_bytes::opaque)
                     .ok());
      }

      // ── Through transparent wrappers ────────────────────────────────────
      context() = __LINE__;
      violation(Members{{Name{1}, bad_name}}, name_reason);
      context() = __LINE__;
      violation(Edge{{bad_name, Tag{"x"}}}, name_reason);
      context() = __LINE__;
      violation(Sealed{packed_unchecked(bad_range)}, range_reason, false);
      context() = __LINE__;
      violation(Budget{{{Tag{"a"}, 60}, {Tag{"b"}, 41}}}, budget_reason);
      context() = __LINE__;
      violation(std::map<std::string, Budget>{{"q1", Budget{{{Tag{"a"}, 101}}}}}, budget_reason);
      context() = __LINE__;
      violation(psio::nested<Budget>::unchecked(unchecked_frame(Budget{{{Tag{"a"}, 101}}})),
                budget_reason, false);
   }

   // A strict registry: a custom node with no handler is an error wherever
   // it sits — a map key, a set element, nested bytes.
   inline void strict_registry()
   {
      ir::CustomHandlers strict;
      strict.require_all = true;
      constexpr std::string_view missing = "pssz IR: no handler registered for a custom type";
      auto                       check   = [&]<typename T>(const T& value) {
         const auto  schema = schema_of<T>();
         const Frame bytes  = frame_of(value);
         CA_CHECK(is(ir::validate_pssz(schema, "T", sp(bytes), nullptr, &strict), missing));
         std::string text;
         CA_CHECK(is(ir::pssz_to_json(schema, "T", psio::ijson{}, sp(bytes), text, nullptr,
                                      &strict),
                     missing));
      };
      check(ByName{{Name{1}, Range{0, 1}}});
      check(Levels{Level::low});
      check(Packed::from(Range{0, 1}));
      check(PackedBy{{"n", psio::nested<Name>::from(Name{1})}});
      check(SlugMap{{Slug{"a"}, 1}});
   }

   inline int run()
   {
      failures() = 0;
      valid_values();
      violations();
      strict_registry();
      return failures();
   }
}  // namespace custom_assoc_cases
