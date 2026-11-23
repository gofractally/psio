#pragma once
//
// Rows of conformance/evolution-vectors.json: pairs of types of two Schema
// IRs with the verdicts of the run-time comparison
// (psio/pssz_ir_compare.hpp: psio::pssz_ir::evolves_from / equivalent).
// Shared by the emitter (evolution_conformance_emit.cpp) and the consuming
// test (evolution_conformance_tests.cpp). conformance/evolution-vectors.md
// describes the file for the other languages.

#include <psio/json.hpp>
#include <psio/nested.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir_compare.hpp>
#include <psio/reflect.hpp>
#include <psio/schema_builder.hpp>
#include <psio/schema_ir.hpp>
#include <psio/time.hpp>
#include <psio/to_hex.hpp>
#include <psio/wrappers.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace evo_conf
{
   // ── The file ──────────────────────────────────────────────────────────
   struct SchemaRow
   {
      std::string id, ir_hex;  // the PSSZ encoding of a psio::Schema
   };
   PSIO_REFLECT(SchemaRow, id, ir_hex)

   // ok, or the first rule broken: its code and where (both "" when ok).
   struct Verdict
   {
      bool        ok = false;
      std::string code, path;
      friend bool operator==(const Verdict&, const Verdict&) = default;
   };
   PSIO_REFLECT(Verdict, ok, code, path)

   struct Verdicts
   {
      Verdict wire, contract;
   };
   PSIO_REFLECT(Verdicts, wire, contract)

   // Two types: `old_type` of schema `old` against `new_type` of schema
   // `new` (the names are members of Schema::types).
   struct TypeCase
   {
      std::string name, old_schema, old_type, new_schema, new_type;
      Verdicts    evolves, equivalent;
   };
   PSIO_REFLECT(TypeCase, name, old_schema, old_type, new_schema, new_type, evolves, equivalent)

   // Two interfaces, each named in its schema's `interfaces`.
   struct InterfaceCase
   {
      std::string name, old_schema, old_interface, new_schema, new_interface;
      Verdicts    evolves, equivalent;
   };
   PSIO_REFLECT(InterfaceCase,
                name,
                old_schema,
                old_interface,
                new_schema,
                new_interface,
                evolves,
                equivalent)

   struct Fixture
   {
      std::uint32_t              version = 0;
      std::vector<SchemaRow>     schemas;
      std::vector<TypeCase>      types;
      std::vector<InterfaceCase> interfaces;
   };
   PSIO_REFLECT(Fixture, version, schemas, types, interfaces)

   inline Verdict verdict_of(const psio::pssz_ir::compatibility& c)
   {
      return Verdict{c.ok, std::string{c.code}, c.path};
   }

   // ── The types ─────────────────────────────────────────────────────────
   namespace base
   {
      struct T
      {
         std::uint32_t                id = 0;
         std::optional<std::uint16_t> flags;
         PSIO_REFLECT(T, id, flags, maxDynamicData(200))
      };
   }
   namespace appended
   {
      struct T
      {
         std::uint32_t                id = 0;
         std::optional<std::uint16_t> flags;
         std::optional<std::string>   memo;
         PSIO_REFLECT(T, id, flags, memo, maxDynamicData(200))
      };
   }
   namespace appended_required
   {
      struct T
      {
         std::uint32_t                id = 0;
         std::optional<std::uint16_t> flags;
         std::uint8_t                 kind = 0;
         PSIO_REFLECT(T, id, flags, kind, maxDynamicData(200))
      };
   }
   namespace renamed
   {
      struct T
      {
         std::uint32_t                ident = 0;
         std::optional<std::uint16_t> flags;
         PSIO_REFLECT(T, ident, flags, maxDynamicData(200))
      };
   }
   namespace retyped
   {
      struct T
      {
         std::int32_t                 id = 0;
         std::optional<std::uint16_t> flags;
         PSIO_REFLECT(T, id, flags, maxDynamicData(200))
      };
   }
   namespace shorter
   {
      struct T
      {
         std::uint32_t id = 0;
         PSIO_REFLECT(T, id, maxDynamicData(200))
      };
   }
   // Without a cap an appended field moves the record's width.
   namespace open
   {
      struct T
      {
         std::uint32_t                id = 0;
         std::optional<std::uint16_t> flags;
         PSIO_REFLECT(T, id, flags)
      };
   }
   namespace open_appended
   {
      struct T
      {
         std::uint32_t                id = 0;
         std::optional<std::uint16_t> flags;
         std::optional<std::string>   memo;
         PSIO_REFLECT(T, id, flags, memo)
      };
   }
   namespace fixed
   {
      struct T
      {
         std::uint32_t                id = 0;
         std::optional<std::uint16_t> flags;
         PSIO_REFLECT(T, id, flags, definitionWillNotChange())
      };
   }
   namespace fixed_appended
   {
      struct T
      {
         std::uint32_t                id = 0;
         std::optional<std::uint16_t> flags;
         std::optional<std::uint16_t> more;
         PSIO_REFLECT(T, id, flags, more, definitionWillNotChange())
      };
   }

   // Member annotations and caps.
#define EVO_CONF_BOUNDED(NS, OWNER, TAGS, ...)                                            \
   namespace NS                                                                           \
   {                                                                                      \
      struct T                                                                            \
      {                                                                                   \
         std::uint64_t            id = 0;                                                 \
         std::string              owner;                                                  \
         std::vector<std::string> tags;                                                   \
         PSIO_REFLECT(T, id, attr(owner, OWNER), attr(tags, TAGS) __VA_OPT__(, ) __VA_ARGS__) \
      };                                                                                  \
   }
   EVO_CONF_BOUNDED(bounded, utf8 | max<32>, max<8> | element(max<16>), maxDynamicData(1024))
   EVO_CONF_BOUNDED(owner_wider, utf8 | max<64>, max<8> | element(max<16>), maxDynamicData(1024))
   EVO_CONF_BOUNDED(owner_raw, max<32>, max<8> | element(max<16>), maxDynamicData(1024))
   EVO_CONF_BOUNDED(owner_min, utf8 | max<32> | min<1>, max<8> | element(max<16>),
                    maxDynamicData(1024))
   EVO_CONF_BOUNDED(element_wider, utf8 | max<32>, max<8> | element(max<20>), maxDynamicData(1024))
   EVO_CONF_BOUNDED(count_wider, utf8 | max<32>, max<12> | element(max<16>), maxDynamicData(1024))
   EVO_CONF_BOUNDED(cap_raised, utf8 | max<32>, max<8> | element(max<16>), maxDynamicData(4096))
   EVO_CONF_BOUNDED(cap_lowered, utf8 | max<32>, max<8> | element(max<16>), maxDynamicData(512))
   EVO_CONF_BOUNDED(cap_other_width, utf8 | max<32>, max<8> | element(max<16>),
                    maxDynamicData(100000))
   EVO_CONF_BOUNDED(uncapped, utf8 | max<300>, max<8> | element(max<16>))
   EVO_CONF_BOUNDED(uncapped_wide_list, utf8 | max<300>, max<8> | element(max<40>))
   EVO_CONF_BOUNDED(uncapped_capped, utf8 | max<300>, max<8> | element(max<16>),
                    maxDynamicData(600))
   EVO_CONF_BOUNDED(uncapped_capped_low, utf8 | max<300>, max<8> | element(max<16>),
                    maxDynamicData(400))
#undef EVO_CONF_BOUNDED

   namespace fields3
   {
      struct T
      {
         std::uint32_t                a = 0;
         std::optional<std::uint32_t> b;
         PSIO_REFLECT(T, a, b, maxFields(3), maxDynamicData(64))
      };
   }
   namespace fields3_grown
   {
      struct T
      {
         std::uint32_t                a = 0;
         std::optional<std::uint32_t> b, c;
         PSIO_REFLECT(T, a, b, c, maxFields(3), maxDynamicData(64))
      };
   }
   namespace fields3_raised
   {
      struct T
      {
         std::uint32_t                a = 0;
         std::optional<std::uint32_t> b, c, d;
         PSIO_REFLECT(T, a, b, c, d, maxFields(8), maxDynamicData(64))
      };
   }
   namespace fields3_dropped
   {
      struct T
      {
         std::uint32_t                a = 0;
         std::optional<std::uint32_t> b, c;
         PSIO_REFLECT(T, a, b, c, maxDynamicData(64))
      };
   }

   // Variants.
   using Var2 = std::variant<std::uint32_t, std::string>;
   using Var3 = std::variant<std::uint32_t, std::string, std::uint64_t>;
   using VarSwapped = std::variant<std::string, std::uint32_t>;
   using Small  = std::variant<std::uint8_t, std::uint16_t>;
   using Larger = std::variant<std::uint8_t, std::uint16_t, std::array<std::uint8_t, 300>>;
   template <typename V>
   struct Tight
   {
      V                      v;
      std::array<char, 240>  pad{};
      PSIO_REFLECT(Tight, v, pad)
   };
   template <typename V>
   struct Capped
   {
      V v;
      PSIO_REFLECT(Capped, v, maxDynamicData(1000))
   };

   // Enums (text names over an integer).
   namespace enum1
   {
      enum class Mode : std::uint8_t { keep, match };
      PSIO_REFLECT_ENUM(Mode, keep, match)
   }
   namespace enum2
   {
      enum class Mode : std::uint8_t { keep, match, fresh };
      PSIO_REFLECT_ENUM(Mode, keep, match, fresh)
   }
   namespace enum3
   {
      enum class Mode : std::uint8_t { keep, fresh = 2 };
      PSIO_REFLECT_ENUM(Mode, keep, fresh)
   }

   using Micros = std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>;

   // A nested value: the member stays a byte list, the inner type evolves.
   namespace nested1
   {
      struct Holder
      {
         std::uint64_t          service = 0;
         psio::nested<base::T>  data;
         PSIO_REFLECT(Holder, service, data)
      };
   }
   namespace nested2
   {
      struct Holder
      {
         std::uint64_t             service = 0;
         psio::nested<appended::T> data;
         PSIO_REFLECT(Holder, service, data)
      };
   }
   namespace nested3
   {
      struct Holder
      {
         std::uint64_t            service = 0;
         psio::nested<retyped::T> data;
         PSIO_REFLECT(Holder, service, data)
      };
   }

   // Interfaces.
   namespace svc1
   {
      struct Svc
      {
         void          transfer(std::uint64_t to, std::uint64_t amount);
         std::uint32_t balance(std::uint64_t of);
         void          ping();
      };
      PSIO_REFLECT(Svc,
                   method(transfer, to, amount, maxDynamicData(200)),
                   method(balance, of),
                   method(ping))
      PSIO_INTERFACE(Svc, name("svc"))
   }
   // An appended optional parameter, and a new function.
   namespace svc2
   {
      struct Svc
      {
         void          transfer(std::uint64_t to, std::uint64_t amount, std::optional<std::string> memo);
         std::uint32_t balance(std::uint64_t of);
         void          ping();
         void          close(std::uint64_t account);
      };
      PSIO_REFLECT(Svc,
                   method(transfer, to, amount, memo, maxDynamicData(200)),
                   method(balance, of),
                   method(ping),
                   method(close, account))
      PSIO_INTERFACE(Svc, name("svc"))
   }
   // A retyped result.
   namespace svc3
   {
      struct Svc
      {
         void          transfer(std::uint64_t to, std::uint64_t amount);
         std::uint64_t balance(std::uint64_t of);
         void          ping();
      };
      PSIO_REFLECT(Svc,
                   method(transfer, to, amount, maxDynamicData(200)),
                   method(balance, of),
                   method(ping))
      PSIO_INTERFACE(Svc, name("svc"))
   }
   // A function removed.
   namespace svc4
   {
      struct Svc
      {
         void          transfer(std::uint64_t to, std::uint64_t amount);
         std::uint32_t balance(std::uint64_t of);
      };
      PSIO_REFLECT(Svc, method(transfer, to, amount, maxDynamicData(200)), method(balance, of))
      PSIO_INTERFACE(Svc, name("svc"))
   }
   // A parameter renamed: the same bytes, another text.
   namespace svc6
   {
      struct Svc
      {
         void          transfer(std::uint64_t to, std::uint64_t quantity);
         std::uint32_t balance(std::uint64_t of);
         void          ping();
      };
      PSIO_REFLECT(Svc,
                   method(transfer, to, quantity, maxDynamicData(200)),
                   method(balance, of),
                   method(ping))
      PSIO_INTERFACE(Svc, name("svc"))
   }
   // A result where there was none.
   namespace svc5
   {
      struct Svc
      {
         void          transfer(std::uint64_t to, std::uint64_t amount);
         std::uint32_t balance(std::uint64_t of);
         std::uint8_t  ping();
      };
      PSIO_REFLECT(Svc,
                   method(transfer, to, amount, maxDynamicData(200)),
                   method(balance, of),
                   method(ping))
      PSIO_INTERFACE(Svc, name("svc"))
   }

   // ── The cases ─────────────────────────────────────────────────────────
   //
   // A side of a case: a schema and the root type's name in it. A case is
   // emitted in both directions ("name" and "name~reverse").
   struct Side
   {
      psio::Schema schema;
      std::string  root;
   };
   template <typename T>
   Side side()
   {
      // The root under one name on both sides: a type's own name is not
      // part of the comparison.
      return Side{psio::SchemaBuilder{}.insert<T>("Root").build(), "Root"};
   }
   // The node `name` of a schema, edited in place.
   inline Side edited(Side s, const std::function<void(psio::Schema&)>& edit)
   {
      edit(s.schema);
      return s;
   }

   struct Pair
   {
      std::string name;
      Side        older, newer;
      // What psio::pssz_evolves_from<Old, New> says, forward and reverse,
      // when the pair is two C++ types the compile-time check can compare
      // and the Schema IR records everything it looks at.
      std::optional<bool> compile_time, compile_time_reverse;
   };
   template <typename Old, typename New>
   Pair typed(std::string name)
   {
      return Pair{std::move(name), side<Old>(), side<New>(), psio::pssz_evolves_from_v<Old, New>,
                  psio::pssz_evolves_from_v<New, Old>};
   }
   // Two C++ types the compile-time check does not relate (it compares an
   // enum, an integer or a time point only with itself).
   template <typename Old, typename New>
   Pair untyped(std::string name)
   {
      return Pair{std::move(name), side<Old>(), side<New>(), std::nullopt, std::nullopt};
   }

   // A node as the child of another (Box<AnyType>).
   template <typename Node>
   psio::schema_types::Box<psio::AnyType> boxed(Node node)
   {
      return psio::schema_types::Box<psio::AnyType>{psio::AnyType{std::move(node)}};
   }

   inline std::vector<Pair> pairs()
   {
      namespace st = psio::schema_types;
      std::vector<Pair> out;
      // Records.
      out.push_back(typed<base::T, base::T>("record-same"));
      out.push_back(typed<base::T, appended::T>("record-append-optional"));
      out.push_back(typed<base::T, appended_required::T>("record-append-required"));
      out.push_back(typed<base::T, shorter::T>("record-remove-field"));
      out.push_back(typed<base::T, retyped::T>("record-retype-field"));
      // (the compile-time check does not see names: it agrees with `wire`)
      out.push_back(typed<base::T, renamed::T>("record-rename-field"));
      out.push_back(typed<open::T, open_appended::T>("record-append-moves-width"));
      out.push_back(typed<base::T, fixed::T>("record-dwnc-added"));
      out.push_back(typed<fixed::T, fixed_appended::T>("record-dwnc-gains-field"));
      // Bounds and caps.
      out.push_back(typed<bounded::T, owner_wider::T>("bound-widen-string"));
      out.push_back(typed<bounded::T, owner_raw::T>("utf8-dropped"));
      out.push_back(typed<bounded::T, owner_min::T>("bound-add-min"));
      out.push_back(typed<bounded::T, element_wider::T>("bound-widen-element"));
      out.push_back(typed<bounded::T, count_wider::T>("bound-widen-count"));
      out.push_back(typed<bounded::T, cap_raised::T>("cap-raised"));
      out.push_back(typed<bounded::T, cap_lowered::T>("cap-lowered"));
      out.push_back(typed<bounded::T, cap_other_width::T>("cap-other-width-class"));
      out.push_back(typed<uncapped::T, uncapped_wide_list::T>("bound-moves-list-width"));
      out.push_back(typed<uncapped::T, uncapped_capped::T>("cap-added"));
      out.push_back(typed<uncapped::T, uncapped_capped_low::T>("cap-added-too-low"));
      out.push_back(typed<fields3::T, fields3_grown::T>("max-fields-grown-within"));
      out.push_back(typed<fields3::T, fields3_raised::T>("max-fields-raised"));
      out.push_back(typed<fields3::T, fields3_dropped::T>("max-fields-dropped"));
      // Containers.
      out.push_back(typed<std::vector<base::T>, std::vector<appended::T>>("list-element-evolves"));
      out.push_back(
         typed<std::optional<base::T>, std::optional<appended::T>>("optional-payload-evolves"));
      out.push_back(typed<std::array<base::T, 2>, std::array<base::T, 3>>("array-length"));
      out.push_back(typed<std::tuple<std::uint32_t, base::T>,
                          std::tuple<std::uint32_t, appended::T, std::optional<std::uint8_t>>>(
         "tuple-append-optional"));
      out.push_back(typed<std::map<std::uint32_t, base::T>, std::map<std::uint32_t, appended::T>>(
         "map-value-evolves"));
      out.push_back(typed<std::map<std::uint32_t, base::T>, std::map<std::int32_t, base::T>>(
         "map-key-changed"));
      out.push_back(typed<std::vector<std::uint32_t>, std::set<std::uint32_t>>("list-to-set"));
      out.push_back(typed<std::set<std::string>, std::set<std::uint32_t>>("set-element-changed"));
      out.push_back(typed<std::string, std::vector<std::uint8_t>>("string-to-bytes"));
      out.push_back(typed<bool, std::uint8_t>("bool-to-u8"));
      out.push_back(typed<std::uint32_t, float>("u32-to-f32"));
      out.push_back(typed<std::optional<std::uint32_t>, std::optional<std::string>>(
         "optional-presence-form"));
      // Variants.
      out.push_back(typed<Var2, Var3>("variant-append-alternative"));
      out.push_back(typed<Var2, VarSwapped>("variant-reorder"));
      out.push_back(typed<Tight<Small>, Tight<Larger>>("variant-larger-alternative-moves-width"));
      out.push_back(typed<Capped<Small>, Capped<Larger>>("variant-larger-alternative-under-cap"));
      // Nested values.
      out.push_back(typed<nested1::Holder, nested2::Holder>("nested-inner-evolves"));
      out.push_back(typed<nested1::Holder, nested3::Holder>("nested-inner-retyped"));
      // Text and identity: the bytes are the same.
      out.push_back(untyped<enum1::Mode, enum2::Mode>("enum-append-enumerator"));
      out.push_back(untyped<enum1::Mode, enum3::Mode>("enum-remove-enumerator"));
      out.push_back(untyped<enum1::Mode, std::uint8_t>("enum-to-integer"));
      out.push_back(untyped<Micros, std::int64_t>("time-point-to-integer"));
      out.push_back(untyped<char, std::uint8_t>("char-is-u8"));
      // One byte list, whichever way an older schema spelled its element
      // (docs/spec-evolution-notes.md E-011).
      out.push_back(Pair{
         "byte-list-char-element",
         edited(side<std::vector<std::uint8_t>>(),
                [](psio::Schema& s) {
                   s.insert("Root",
                            st::Custom{boxed(st::List{boxed(st::Custom{boxed(st::Int{8, false}), "char"})}),
                                       "hex"});
                }),
         side<std::vector<std::uint8_t>>(), std::nullopt, std::nullopt});
      // A bound in the type and the same bound on the member.
      out.push_back(Pair{
         "bound-in-type-or-on-member",
         Side{[] {
                 psio::Schema s;
                 s.insert("Root",
                          st::Object{{st::Member{
                                        "tags", boxed(st::BoundedList{boxed(st::Int{32, false}), 8}), {}}},
                                     {}});
                 return s;
              }(),
              "Root"},
         Side{[] {
                 psio::Schema s;
                 s.insert("Root", st::Object{{st::Member{"tags",
                                                         boxed(st::List{boxed(st::Int{32, false})}),
                                                         {st::Attribute{"lengthBound", "max=8"}}}},
                                             {}});
                 return s;
              }(),
              "Root"},
         std::nullopt, std::nullopt});
      // A boxed member: the same bytes, another inferred bound for the holder.
      out.push_back(Pair{"boxed-member", side<base::T>(),
                         edited(side<base::T>(),
                                [](psio::Schema& s) {
                                   auto& record = std::get<st::Object>(
                                      const_cast<psio::AnyType*>(s.get("T"))->value);
                                   *record.members[0].type =
                                      st::Custom{boxed(st::Int{32, false}), "box"};
                                }),
                         std::nullopt, std::nullopt});
      // A recursive type against itself with an appended field.
      {
         auto tree = [](bool grown) {
            psio::Schema s;
            st::Object   node;
            node.members.push_back(st::Member{"value", boxed(st::Int{32, false}), {}});
            node.members.push_back(st::Member{"children", boxed(st::List{boxed(st::Type{"Root"})}), {}});
            if (grown)
               node.members.push_back(
                  st::Member{"label",
                             boxed(st::Option{boxed(st::Custom{
                                boxed(st::List{boxed(st::Int{8, false})}), "string"})}),
                             {}});
            s.insert("Root", std::move(node));
            return Side{std::move(s), "Root"};
         };
         out.push_back(Pair{"recursive-append", tree(false), tree(true), std::nullopt, std::nullopt});
      }
      return out;
   }

   struct InterfacePair
   {
      std::string  name;
      psio::Schema older, newer;
   };
   template <typename Old, typename New>
   InterfacePair interface_pair(std::string name)
   {
      return InterfacePair{std::move(name), psio::SchemaBuilder{}.insert_interface<Old>().build(),
                           psio::SchemaBuilder{}.insert_interface<New>().build()};
   }
   inline std::vector<InterfacePair> interface_pairs()
   {
      std::vector<InterfacePair> out;
      out.push_back(interface_pair<svc1::Svc, svc1::Svc>("interface-same"));
      out.push_back(interface_pair<svc1::Svc, svc2::Svc>("interface-grown"));
      out.push_back(interface_pair<svc1::Svc, svc3::Svc>("interface-result-retyped"));
      out.push_back(interface_pair<svc1::Svc, svc4::Svc>("interface-function-removed"));
      out.push_back(interface_pair<svc1::Svc, svc5::Svc>("interface-result-added"));
      out.push_back(interface_pair<svc1::Svc, svc6::Svc>("interface-parameter-renamed"));
      return out;
   }

   inline std::string hex(const psio::Schema& schema)
   {
      const auto bytes = psio::encode(psio::pssz{}, schema);
      return psio::to_hex(std::span<const char>{bytes});
   }
}  // namespace evo_conf
