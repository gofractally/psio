#pragma once
//
// psio/schema_builder.hpp — C++ reflection → Schema IR.
//
// SchemaBuilder is the canonical "C++ enters the multi-format graph
// here" path.  It walks PSIO_REFLECT'd structs, interface_info<T>,
// world_info<W>, and use_info<T> registries to populate a Schema.
//
//    auto s = psio::SchemaBuilder{}
//                .insert<MyRecord>("MyRecord")
//                .insert_world<MyWorld>()
//                .build();
//
// The IR (psio/schema_ir.hpp) is the multi-format pivot; this
// builder is one of the canonical entry points.  Format emitters
// (emit_wit, emit_pssz, emit_gql, …) consume the produced Schema;
// format parsers (wit_parser, fbs_parser, capnp_parser) populate
// it from the other side.
//
// Coverage today:
//
//   Primitives          bool / [u]int{8,16,32,64} / float / double /
//                       char / std::string / std::string_view
//   Collections         std::vector<T> / std::optional<T> /
//                       std::array<T,N> / std::tuple / std::variant /
//                       bounded<T,N> / utf8_string<N> / bitvector/bitlist
//   Resources           wit_resource-derived (own<T>/borrow<T> wrap to
//                       references, with the resource definition picked
//                       up from interface_info<T>)
//   Records             PSIO_REFLECT'd structs (Object form)
//   Wide integers       uint128 / int128 (Int 128), uint256 (Int 256)
//   Time                std::chrono::duration / system_clock time_point
//                       (Custom over the tick-count Int; id from time.hpp)
//   Any JSON            psio::json_value (Custom "json" over a UTF-8 string)
//
// What a schema-driven codec (pssz_ir.hpp, pssz_ir_json.hpp) needs beyond
// the shape is recorded too:
//   - a member `length_bound{.max = N}` on a std::string / std::vector
//     (through std::optional) becomes the bounded form of the member's type
//     (BoundedList, or the string / hex adapter over one), the same IR a
//     Rust BoundedList / BoundedRawString produces. A bound with `exact` or
//     `min`, or on a utf8_string, is the member attribute
//     lengthBound = "exact=N" / "min=A,max=B";
//   - member integer forms: jsonInt / ijsonInt = "number" | "string";
//   - a variant alternative is named by its label (variant_label.hpp), the
//     key of its tagged text form; an untagged variant (untagged.hpp): the
//     Variant attribute `untagged`;
//   - type-level definitionWillNotChange / maxDynamicData / maxFields.
//   - string kinds: std::string is the Custom "string" (raw bytes),
//     utf8_string the Custom "utf8" (validated UTF-8), each over its byte
//     container;
//   - Box<T> is the Custom "box" over T: T on the wire, with no inferred
//     size bound, exactly as the typed codecs treat it.
//   - psio::nested<T> (nested.hpp) is Nested{T}: a byte list holding the
//     PSSZ encoding of a T. A member length_bound on it (its byte count) is
//     always the member attribute lengthBound: there is no bounded Nested.
//   - a transparent type (transparent.hpp) is a named alias: its reflected
//     name, defined as its member's type;
//   - a custom type — one with a text_category adapter, an invariant
//     (invariant.hpp), a schemaId("Id") in its PSIO_REFLECT, or several of
//     these, of ANY shape: record, transparent wrapper,
//     enum, variant, tuple, list — is Custom{wire type, id} (adapter.hpp
//     schema_custom_id), for which the schema-driven codecs take a handler
//     (pssz_ir_custom.hpp).
// Not expressible: a member `as<Tag>` override or a binary adapter (the IR
// has the member's declared type).
//   Envelope            PSIO_INTERFACE → Interface,
//                       PSIO_WORLD     → World (with imports/exports),
//                       PSIO_USE       → Use
//
// Out of scope for now (lands as v3 ports the supporting types):
//   std::bitset,
//   uint128 / int128 / uint256,
//   PackableWrapper unwraps, and the
//   full custom-handler chain.  These are extensions to the same
//   if-constexpr ladder when their types land in v3.

#include <psio/adapter.hpp>
#include <psio/annotate.hpp>
#include <psio/detail/nested_fwd.hpp>
#include <psio/detail/assoc.hpp>
#include <psio/ext_int.hpp>
#include <psio/get_type_name.hpp>
#include <psio/variant_label.hpp>
#include <psio/json_value.hpp>
#include <psio/reflect.hpp>
#include <psio/schema_ir.hpp>
#include <psio/structural.hpp>
#include <psio/time.hpp>
#include <psio/transparent.hpp>
#include <psio/wire_type.hpp>
#include <psio/wit_resource.hpp>
#include <psio/wrappers.hpp>

#include <psio/error.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <tuple>
#include <variant>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace psio
{
   // The name of a reflected record in a schema. By default it is the
   // record's reflected (unqualified) name — and, when two different types
   // of one schema have the same unqualified name, each of them is named as
   // the compiler spells it, with its namespaces and template arguments
   // (`a::Config`, `b::Config`, `WithBlock<Sent>`): SchemaBuilder never
   // makes one node of two types. A spelling is the compiler's; to give a
   // type a name of your own choosing (a published schema whose bytes are
   // hashed, a class template), specialize:
   //
   //    template <> struct psio::schema_type_name<b::Config>
   //    {
   //       static constexpr std::string_view value = "NodeConfig";
   //    };
   //
   // A chosen name is never changed; two types with one chosen name, or a
   // chosen name that another definition of the schema has, is an error
   // (SchemaBuilder::status()).
   template <typename T> struct schema_type_name
   {
   };
} // namespace psio

namespace psio::schema_types
{

   namespace sb_detail
   {
      // Private static identity within one linked image. The address is never
      // serialized; the token avoids RTTI and is not a cross-DSO identity API.
      // The identity of the bounded type a transparent type U makes of its
      // member (transparent.hpp "Bounds on the member").
      template <typename U>
      struct transparent_member_of
      {
      };
      template<typename T> inline unsigned char type_token = 0;
      // ── Std-container detectors (v3-local) ──────────────────────────
      template <typename T>
      struct sb_is_vector : std::false_type
      {
      };
      template <typename E, typename A>
      struct sb_is_vector<std::vector<E, A>> : std::true_type
      {
         using elem = E;
      };

      template <typename T>
      struct sb_is_optional : std::false_type
      {
      };
      template <typename T>
      struct sb_is_optional<std::optional<T>> : std::true_type
      {
         using elem = T;
      };

      template <typename T>
      struct sb_is_array : std::false_type
      {
      };
      template <typename E, std::size_t N>
      struct sb_is_array<std::array<E, N>> : std::true_type
      {
         using elem                       = E;
         static constexpr std::size_t len = N;
      };
      template <typename T> struct sb_is_tuple : std::false_type
      {
      };
      template <typename... T> struct sb_is_tuple<std::tuple<T...>> : std::true_type
      {
      };
      // A std::pair is the 2-tuple of its members.
      template <typename A, typename B> struct sb_is_tuple<std::pair<A, B>> : std::true_type
      {
      };
      template <typename T> struct sb_is_variant : std::false_type
      {
      };
      template <typename... T> struct sb_is_variant<std::variant<T...>> : std::true_type
      {
      };
      template <typename T> struct sb_is_box : std::false_type
      {
      };
      template <typename T> struct sb_is_box<Box<T>> : std::true_type
      {
         using elem = T;
      };
      template <typename T> struct sb_is_bounded : std::false_type
      {
      };
      template <typename T, std::size_t N> struct sb_is_bounded<::psio::bounded<T, N>> : std::true_type
      {
         using elem = T;
         static constexpr auto limit = N;
      };
      template <typename T> struct sb_is_utf8 : std::false_type
      {
         static constexpr bool unbounded = false;
      };
      template <std::size_t N> struct sb_is_utf8<::psio::utf8_string<N>> : std::true_type
      {
         static constexpr auto limit = N;
         static constexpr bool unbounded = N == ::psio::utf8_unbounded;
      };
      // Element types whose sequences are byte sequences (hex in I-JSON).
      template <typename E>
      inline constexpr bool sb_is_byte_v =
          std::is_same_v<E, char> || std::is_same_v<E, unsigned char> || std::is_same_v<E, std::byte>;
      template <typename T> struct sb_is_bitvector : std::false_type
      {
      };
      template <std::size_t N> struct sb_is_bitvector<::psio::bitvector<N>> : std::true_type
      {
         static constexpr auto count = N;
      };
      template <typename T> struct sb_is_bitlist : std::false_type
      {
      };
      template <std::size_t N> struct sb_is_bitlist<::psio::bitlist<N>> : std::true_type
      {
         static constexpr auto limit = N;
      };
   }  // namespace sb_detail

   // ─────────────────────────────────────────────────────────────────────
   // SchemaBuilder
   // ─────────────────────────────────────────────────────────────────────

   class SchemaBuilder
   {
    public:
      SchemaBuilder() = default;

      // Insert a type under a chosen name and return *this for chaining.
      //
      // If the type's automatic name (reflect<T>::name for reflected
      // types, the @-interned id otherwise) happens to equal the
      // user-supplied name, we leave the original definition in place
      // — overwriting it with a Type{} pointing at itself would
      // produce an invalid alias cycle rather than a type definition.
      // A name already used for something else is an error (status()),
      // except the unqualified name of a reflected record that was named
      // automatically: the record gives the name up and is named by its
      // qualified name instead (see psio::schema_type_name).
      template <typename T>
      SchemaBuilder& insert(std::string name) &
      {
         insert_as<T>(std::move(name));
         return *this;
      }

      template <typename T>
      SchemaBuilder&& insert(std::string name) &&
      {
         insert_as<T>(std::move(name));
         return std::move(*this);
      }

      // Insert a type and return its AnyType representation.  Repeated
      // calls for the same T return a Type{} reference back to the
      // first registration so the schema graph stays acyclic on the
      // wire even for self-referential types.
      //
      // Naming policy:
      //   - PSIO_REFLECT'd structs / resources use `reflect<T>::name`
      //     so emitters render `record point { … }` instead of
      //     `record @4 { … }` and downstream tooling can refer to
      //     them by their source-level name. Two different types with
      //     one such name are each named by their qualified spelling
      //     instead (psio::schema_type_name, above): never one node.
      //   - Everything else (primitives, std::vector, std::optional,
      //     std::array, etc.) uses an `@N` interning name; emitters
      //     resolve those inline at the use site rather than emit
      //     them as standalone aliases.
      template <typename T>
      AnyType insert()
      {
         using U = std::remove_cvref_t<T>;

         const void* idx = &sb_detail::type_token<U>;
         if (auto it = id_to_name_.find(idx); it != id_to_name_.end())
            return Type{it->second};

         std::string name;
         if constexpr (Reflected<U>)
            name = record_name<U>();
         else
            name = "@" + std::to_string(id_to_name_.size());

         id_to_name_[idx] = name;
         schema_.insert(name, AnyType{});  // placeholder; overwritten below
         schema_.insert(name, build_anytype<U>());
         return Type{name};
      }

      // Walk a PSIO_INTERFACE'd anchor and add a populated Interface
      // to the schema.  All types listed in `types(...)` are inserted
      // into the schema and their auto-generated names captured into
      // Interface::type_names; all funcs(...) become Funcs.
      template <typename Tag>
      SchemaBuilder& insert_interface() &
      {
         schema_.interfaces.push_back(build_interface<Tag>());
         return *this;
      }

      template <typename Tag>
      SchemaBuilder&& insert_interface() &&
      {
         schema_.interfaces.push_back(build_interface<Tag>());
         return std::move(*this);
      }

      // Walk a PSIO_WORLD'd tag.  Imports may be PSIO_USE tags or
      // bare interface tags; exports are interface tags.  Each
      // interface tag is recursively walked and added to
      // Schema::interfaces; use tags become Schema::uses entries plus
      // an UseRef on World::imports.
      template <typename WorldTag>
      SchemaBuilder& insert_world() &
      {
         schema_.worlds.push_back(build_world<WorldTag>());
         return *this;
      }

      template <typename WorldTag>
      SchemaBuilder&& insert_world() &&
      {
         schema_.worlds.push_back(build_world<WorldTag>());
         return std::move(*this);
      }

      // ── A reflected method outside an Interface ───────────────────────
      //
      // insert_interface<Tag>() writes a method as a Func of an Interface. A
      // schema that lists functions its own way (a map of name ->
      // FunctionType, say) takes the same nodes from here, for method I of
      // the reflected type T:
      //
      //   insert_method_args<T, I>()     the argument list: the node of
      //                                  reflect<T>::method_arg_tuple<I>, with
      //                                  the parameter annotations and the cap
      //   insert_method_result<T, I>()   the result AS returns(...) TYPES IT
      //                                  (the bounded type, `utf8`, element
      //                                  rules): the type whose bytes
      //                                  psio::method_result_to_pssz writes
      //                                  and method_result_from_pssz reads.
      //                                  nullopt for a method that returns
      //                                  nothing. The plain type of a method
      //                                  without returns(...)
      //   insert_method<T, I>()          both, as a FunctionType
      //
      // The result is the wire type of what the method returns: X for a
      // method that returns a psio::pssz_view<X>.
      //
      // A returns(...) item that has no type form — a length bound with
      // `exact` or `min`, a bound on a psio::utf8_string or a nested value,
      // an integer text form — is an attribute of a Func (`result.<name>`,
      // insert_interface). A bare result node cannot carry it, so asking for
      // one is an error (status()) rather than a schema that says less than
      // the declaration.
      template <typename T, std::size_t I>
      AnyType insert_method_args()
      {
         return insert<typename reflect<T>::template method_arg_tuple<I>>();
      }

      template <typename T, std::size_t I>
      std::optional<AnyType> insert_method_result()
      {
         using R        = reflect<T>;
         using Declared = typename R::template method_result<I>;
         if constexpr (std::is_void_v<Declared>)
            return std::nullopt;
         else
         {
            using Result = ::psio::wire_type_t<Declared>;
            Member result{"", Box<AnyType>{insert<Result>()}, {}};
            apply_member_annotations<Result>(result, R::template method_annotations<I>.result);
            for (const auto& attribute : result.attributes)
               errors_.push_back(
                  "psio::SchemaBuilder: returns(...) of " + std::string{R::name} + "::" +
                  std::string{R::template method_name<I>} + " states " + attribute.name +
                  (attribute.value ? " = \"" + *attribute.value + "\"" : std::string{}) +
                  ", which has no type form: only the Func of an Interface carries it "
                  "(insert_interface)");
            return std::move(*result.type);
         }
      }

      template <typename T, std::size_t I>
      FunctionType insert_method()
      {
         FunctionType type{Box<AnyType>{insert_method_args<T, I>()}, std::nullopt};
         if (auto result = insert_method_result<T, I>())
            type.result = Box<AnyType>{std::move(*result)};
         return type;
      }

      // Whether every name of the schema names one thing: ok, or the first
      // naming error — two different types under one chosen name, or a name
      // given to insert<T>(name) that another definition has. The message
      // names both.
      codec_status status() const
      {
         if (errors_.empty())
            return codec_ok();
         return codec_fail_owned(errors_.front(), 0, "schema");
      }

      // The unqualified names that two or more different reflected types
      // of this schema have, each with the names the types got instead:
      // {"Config", {"a::Config", "b::Config"}}. Not an error: for a caller
      // that wants to know (or to refuse) it.
      struct Collision
      {
         std::string              name;
         std::vector<std::string> types;
      };
      std::vector<Collision> collisions() const
      {
         std::vector<Collision> out;
         for (const auto& name : collided_)
         {
            Collision c{name, {}};
            for (const auto& record : records_)
               if (record.plain == name)
                  c.types.push_back(record.qualified);
            out.push_back(std::move(c));
         }
         return out;
      }

      // Finalise.  Sets the schema's package to whatever
      // PSIO_CURRENT_PACKAGE_ resolves to at the most recently
      // inserted world / interface (via the captured package_name_).
      //
      // A naming error (status()) fails through codec_abort: a
      // codec_exception, or abort in a build without exceptions — check
      // status() first there.
      Schema build() &&
      {
         return finish(std::move(schema_));
      }

      Schema build() const&
      {
         return finish(Schema{schema_});
      }

      // ── Internals (public so to_schema customisation can reach in
      //     once that hook lands; today the only paths are the
      //     if-constexpr ladder and the interface/world walkers) ─────

      const Schema& peek() const noexcept { return schema_; }

    private:
      Schema                                   schema_;
      std::unordered_map<const void*, std::string> id_to_name_;
      std::string                              package_name_;
      std::string                              package_version_;

      // ── Names of reflected records ─────────────────────────────────
      //
      // A record is named by its unqualified name while no other type of
      // the schema has it. The second type with a name takes its qualified
      // name at once and the first is renamed when the schema is finished
      // (the references to it are rewritten), so the names do not depend on
      // which of the two was met first.
      struct RecordName
      {
         const void* type;
         std::string plain;      // reflect<T>::name
         std::string qualified;  // the compiler's spelling: a::b::T
         bool        chosen;     // psio::schema_type_name<T>
      };
      std::vector<RecordName>                     records_;
      std::vector<std::string>                    collided_;  // plain names, in order
      std::vector<std::pair<std::string, AnyType>> deferred_;  // insert<T>(name) of a freed name
      std::vector<std::string>                    errors_;

      bool collided(std::string_view plain) const
      {
         return std::find(collided_.begin(), collided_.end(), plain) != collided_.end();
      }
      const RecordName* plain_owner(std::string_view name) const
      {
         for (const auto& record : records_)
            if (!record.chosen && record.plain == name)
               return &record;
         return nullptr;
      }

      template <typename U>
      std::string record_name()
      {
         const void*       type      = &sb_detail::type_token<U>;
         const std::string qualified{::psio::detail::pretty_type_name<U>()};
         if constexpr (requires { std::string_view{::psio::schema_type_name<U>::value}; })
         {
            std::string name{std::string_view{::psio::schema_type_name<U>::value}};
            if (schema_.get(name))
               errors_.push_back("psio::SchemaBuilder: the name \"" + name + "\" chosen for " +
                                 qualified + " (psio::schema_type_name) is already used in " +
                                 "this schema" + other_owner(name));
            records_.push_back(RecordName{type, name, qualified, true});
            return name;
         }
         else
         {
            std::string plain{reflect<U>::name};
            const bool  taken = schema_.get(plain) != nullptr || collided(plain);
            records_.push_back(RecordName{type, plain, qualified, false});
            if (!taken)
               return plain;
            // Another definition has the name. If it is a record that was
            // named automatically, both are named by their spelling.
            if (plain_owner_before(plain, type) && !collided(plain))
               collided_.push_back(plain);
            if (schema_.get(qualified))
               errors_.push_back("psio::SchemaBuilder: two types of this schema are named \"" +
                                 qualified + "\"; give one a name with psio::schema_type_name");
            return qualified;
         }
      }
      // A record other than `type` that has the plain name `name`.
      bool plain_owner_before(std::string_view name, const void* type) const
      {
         for (const auto& record : records_)
            if (!record.chosen && record.plain == name && record.type != type)
               return true;
         return false;
      }
      std::string other_owner(std::string_view name) const
      {
         for (const auto& record : records_)
            if (record.plain == name)
               return " (by " + record.qualified + ")";
         return {};
      }

      template <typename T>
      void insert_as(std::string name)
      {
         AnyType ref = insert<T>();
         // The type's own automatic name: its definition is the root.
         if (auto* t = std::get_if<Type>(&ref.value); t && t->type == name)
            return;
         if (const AnyType* existing = schema_.get(name))
         {
            if (*existing == ref)
               return;  // the same root again
            // The unqualified name of a record that was named automatically:
            // the record is renamed (finish) and the name is this root's.
            if (plain_owner(name) && !deferred_named(name))
            {
               if (!collided(name))
                  collided_.push_back(name);
               deferred_.emplace_back(std::move(name), std::move(ref));
               return;
            }
            errors_.push_back("psio::SchemaBuilder: insert<T>(\"" + name + "\"): the name is " +
                              "already used in this schema for something else" +
                              other_owner(name));
            return;
         }
         if (const AnyType* pending = deferred_named(name))
         {
            if (!(*pending == ref))
               errors_.push_back("psio::SchemaBuilder: insert<T>(\"" + name +
                                 "\"): the name is already used in this schema for something else");
            return;
         }
         schema_.insert(std::move(name), std::move(ref));
      }
      const AnyType* deferred_named(std::string_view name) const
      {
         for (const auto& [taken, type] : deferred_)
            if (taken == name)
               return &type;
         return nullptr;
      }

      // Rewrites the references to renamed types in `type`.
      using Renames = std::unordered_map<std::string, std::string>;
      static void rename(Box<AnyType>& box, const Renames& renames)
      {
         if (box.get())
            rename(*box, renames);
      }
      static void rename(std::vector<Member>& members, const Renames& renames)
      {
         for (auto& member : members)
            rename(member.type, renames);
      }
      static void rename(Func& f, const Renames& renames)
      {
         rename(f.params, renames);
         if (f.result)
            rename(*f.result, renames);
      }
      static void rename(AnyType& type, const Renames& renames)
      {
         std::visit(
            [&](auto& t) {
               using N = std::decay_t<decltype(t)>;
               if constexpr (std::is_same_v<N, Type>)
               {
                  if (auto to = renames.find(t.type); to != renames.end())
                     t.type = to->second;
               }
               else if constexpr (std::is_same_v<N, Struct> || std::is_same_v<N, Object> ||
                                  std::is_same_v<N, Variant>)
                  rename(t.members, renames);
               else if constexpr (std::is_same_v<N, Tuple>)
                  for (auto& member : t.members)
                     rename(member, renames);
               else if constexpr (std::is_same_v<N, Resource>)
                  for (auto& method : t.methods)
                     rename(method, renames);
               else if constexpr (requires { t.type; })
                  rename(t.type, renames);
            },
            type.value);
      }

      Schema finish(Schema schema) const
      {
         if (auto st = status(); !st.ok())
            ::psio::codec_abort(st.error());
         if (!package_name_.empty())
         {
            schema.package.name    = package_name_;
            schema.package.version = package_version_;
         }
         if (collided_.empty())
            return schema;
         // Every record that still has a collided plain name takes its
         // qualified one.
         Renames renames;
         for (const auto& record : records_)
            if (!record.chosen && collided(record.plain))
               if (auto at = id_to_name_.find(record.type);
                   at != id_to_name_.end() && at->second == record.plain)
                  renames.emplace(record.plain, record.qualified);
         for (auto& named : schema.types)
         {
            if (auto to = renames.find(named.name); to != renames.end())
               named.name = to->second;
            rename(named.type, renames);
         }
         for (auto& in : schema.interfaces)
         {
            for (auto& name : in.type_names)
               if (auto to = renames.find(name); to != renames.end())
                  name = to->second;
            for (auto& f : in.funcs)
               rename(f, renames);
         }
         for (const auto& [name, type] : deferred_)
         {
            AnyType root = type;
            rename(root, renames);
            schema.insert(name, std::move(root));
         }
         return schema;
      }

      // ── Build an AnyType for T ─────────────────────────────────────
      //
      // A custom type (adapter.hpp schema_custom_v: a text presentation,
      // an invariant, a declared schema id) is Custom{its wire type, id},
      // whatever its shape; everything else is its wire type.
      template <typename U>
      AnyType build_anytype()
      {
         if constexpr (::psio::schema_custom_v<U>)
            return Custom{Box<AnyType>{build_plain<U>()},
                          std::string{::psio::schema_custom_id<U>()}};
         else
            return build_plain<U>();
      }

      // The element node of a list / bounded list / array of E. A byte
      // sequence (the "hex" custom) has `u8` elements whichever C++ type
      // spells the byte — char, unsigned char, std::byte — so that
      // std::vector<char> and std::vector<std::uint8_t> are one schema
      // node, the one Rust's Vec<u8> and the JS / Python byte lists build.
      // A lone `char` member is still the "char" custom.
      template <typename E>
      AnyType sequence_element()
      {
         if constexpr (sb_detail::sb_is_byte_v<E>)
            return insert<std::uint8_t>();
         else
            return insert<E>();
      }
      template <typename E, typename Annots>
      AnyType annotated_element(const Annots& annots)
      {
         if constexpr (sb_detail::sb_is_byte_v<E>)
            return insert<std::uint8_t>();
         else
            return annotated_type<E>(annots);
      }

      // The wire type of T: the if-constexpr ladder.
      template <typename U>
      AnyType build_plain()
      {
         if constexpr (std::is_same_v<U, bool>)
            return Custom{Box<AnyType>{Int{1, false}}, "bool"};
         else if constexpr (std::is_same_v<U, char>)
            return Custom{Box<AnyType>{Int{8, false}}, "char"};
         else if constexpr (std::is_same_v<U, ::psio::json_value>)
            // Any JSON value: on the wire a UTF-8 string holding its JSON
            // text; the id says its text form is that value, inline.
            return Custom{
                Box<AnyType>{AnyType{Custom{
                    Box<AnyType>{List{Box<AnyType>{insert<std::uint8_t>()}}}, "utf8"}}},
                "json"};
         else if constexpr (std::is_integral_v<U>)
            return Int{static_cast<std::uint32_t>(8 * sizeof(U)),
                       std::is_signed_v<U>};
         else if constexpr (std::is_same_v<U, ::psio::uint128>)
            return Int{128, false};
         else if constexpr (std::is_same_v<U, ::psio::int128>)
            return Int{128, true};
         else if constexpr (std::is_same_v<U, ::psio::uint256>)
            return Int{256, false};
         else if constexpr (std::is_floating_point_v<U> &&
                            std::numeric_limits<U>::is_iec559)
         {
            // Exponent field width: max_exponent is 2^(bits-1), so
            // 8 for binary32 and 11 for binary64 (the Rust builder's
            // {8, 24} / {11, 53}).
            std::uint32_t exp = 1;
            for (auto e = std::numeric_limits<U>::max_exponent; e > 1;
                 e >>= 1)
               ++exp;
            return Float{exp, static_cast<std::uint32_t>(
                                 std::numeric_limits<U>::digits)};
         }
         else if constexpr (std::is_enum_v<U>)
         {
            // A plain C++ enum is its underlying integer type in the schema,
            // matching its wire form. With PSIO_REFLECT_ENUM it is an Enum
            // over that integer, carrying the enumerator names and values.
            if constexpr (::psio::ReflectedEnum<U>)
            {
               using R = ::psio::reflect_enum<U>;
               Enum e;
               // The integer itself, whatever the underlying type is
               // spelled as (char, bool): an Enum sits directly on an Int.
               e.type = Box<AnyType>{Int{static_cast<std::uint32_t>(8 * sizeof(U)),
                                         std::is_signed_v<std::underlying_type_t<U>>}};
               for (std::size_t i = 0; i < R::count; ++i)
                  e.members.push_back(Enumerator{
                      std::string{R::names[i]},
                      static_cast<std::uint64_t>(static_cast<std::int64_t>(
                          static_cast<std::underlying_type_t<U>>(R::values[i])))});
               return e;
            }
            else
               return build_anytype<std::underlying_type_t<U>>();
         }
         else if constexpr (::psio::TimeValue<U>)
            // A duration / time point is its tick-count integer carrying the
            // time marker: "time-point-micros", "duration-seconds", ...
            return Custom{Box<AnyType>{Int{
                             static_cast<std::uint32_t>(8 * sizeof(::psio::time_rep_t<U>)),
                             std::is_signed_v<::psio::time_rep_t<U>>}},
                          ::psio::time_schema_id<U>()};
         else if constexpr (std::is_same_v<U, std::string> ||
                            std::is_same_v<U, std::string_view>)
            return Custom{Box<AnyType>{List{Box<AnyType>{insert<std::uint8_t>()}}},
                          "string"};
         else if constexpr (sb_detail::sb_is_box<U>::value)
            // The inner type on the wire, marked: a boxed value has no
            // inferred size bound in the typed codecs, and a schema reader
            // must derive the same offset widths.
            return Custom{Box<AnyType>{insert<typename sb_detail::sb_is_box<U>::elem>()}, "box"};
         else if constexpr (::psio::detail::is_nested_v<U>)
            // A byte list holding the PSSZ encoding of the inner type. The
            // validation policy (eager / deferred) is not part of the schema.
            return Nested{Box<AnyType>{insert<typename ::psio::detail::is_nested<U>::inner>()}};
         else if constexpr (::psio::detail::is_assoc_v<U>)
            // A map / set: the list of its entries (K, V) / elements, in
            // strictly ascending key order.
            return Custom{
                Box<AnyType>{List{Box<AnyType>{insert<::psio::detail::assoc_entry_t<U>>()}}},
                ::psio::detail::is_map_v<U> ? "map" : "set"};
         else if constexpr (requires { typename U::psio_annotated_tuple; })
         {
            // A tuple with element annotations and a cap (a method's
            // argument tuple). A Tuple node has no attributes, so it is
            // written as what a tuple is on the wire — the extensible record
            // of its elements (docs/pssz-spec.md 3.2) — which can carry the
            // cap: an Object whose members are the elements. Its `tuple`
            // attribute says what it is — positional on the wire like any
            // record, and in text an argument list (docs/json.md "Method
            // arguments"): "named" when the method names every parameter
            // (the members carry the names; the text is an object, and a
            // reader also takes the positional array), "positional"
            // otherwise (the members are named "0", "1", ...; the text is
            // the array).
            using Tuple = typename U::tuple_type;
            PSIO_REQUIRE_TUPLE_SIZE(std::tuple_size_v<Tuple>);
            Object o;
            [&]<std::size_t... I>(std::index_sequence<I...>)
            {
               auto name = [](std::size_t i) -> std::string
               {
                  if constexpr (::psio::annotated_tuple_named_v<U>)
                     return std::string{U::meta_type::names[i]};
                  else
                     return std::to_string(i);
               };
               (o.members.push_back(
                   Member{name(I),
                          Box<AnyType>{annotated_type<std::tuple_element_t<I, Tuple>>(
                             ::psio::detail::ann::at_specs<I>(U::psio_annotations))},
                          {}}),
                ...);
            }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
            o.attributes.push_back(Attribute{
               "tuple", ::psio::annotated_tuple_named_v<U> ? "named" : "positional"});
            if constexpr (::psio::max_fields_v<U>.has_value())
               o.attributes.push_back(
                  Attribute{"maxFields", std::to_string(::psio::max_fields_v<U>.value())});
            if constexpr (::psio::max_dynamic_data_v<U>.has_value())
               o.attributes.push_back(Attribute{
                  "maxDynamicData", std::to_string(::psio::max_dynamic_data_v<U>.value())});
            return o;
         }
         else if constexpr (sb_detail::sb_is_tuple<U>::value)
            return [&]<std::size_t... I>(std::index_sequence<I...>)
            {
               PSIO_REQUIRE_TUPLE_SIZE(sizeof...(I));
               return Tuple{{Box<AnyType>{insert<std::tuple_element_t<I, U>>()}...}};
            }(std::make_index_sequence<std::tuple_size_v<U>>{});
         else if constexpr (sb_detail::sb_is_variant<U>::value)
            return build_variant<U>([&]<std::size_t I>() {
               return insert<std::variant_alternative_t<I, U>>();
            });
         else if constexpr (sb_detail::sb_is_bounded<U>::value)
         {
            using E = typename sb_detail::sb_is_bounded<U>::elem;
            AnyType list = BoundedList{Box<AnyType>{sequence_element<E>()},
                                       sb_detail::sb_is_bounded<U>::limit};
            if constexpr (sb_detail::sb_is_byte_v<E>)
               return Custom{Box<AnyType>{std::move(list)}, "hex"};
            return list;
         }
         // utf8_string is "utf8" (validated UTF-8); std::string above is
         // "string" (raw bytes).
         else if constexpr (sb_detail::sb_is_utf8<U>::unbounded)
            // Unbounded UTF-8: over an unbounded byte list, with no
            // fabricated maximum.
            return Custom{Box<AnyType>{List{Box<AnyType>{insert<std::uint8_t>()}}},
                          "utf8"};
         else if constexpr (sb_detail::sb_is_utf8<U>::value)
            return Custom{Box<AnyType>{AnyType{BoundedList{Box<AnyType>{insert<std::uint8_t>()},
                                                           sb_detail::sb_is_utf8<U>::limit}}},
                          "utf8"};
         else if constexpr (sb_detail::sb_is_bitvector<U>::value)
            return Custom{Box<AnyType>{AnyType{Array{Box<AnyType>{Int{1, false}},
                                                     sb_detail::sb_is_bitvector<U>::count}}},
                          "bitvector"};
         else if constexpr (sb_detail::sb_is_bitlist<U>::value)
            return Custom{Box<AnyType>{AnyType{BoundedList{Box<AnyType>{Int{1, false}},
                                                           sb_detail::sb_is_bitlist<U>::limit}}},
                          "bitlist"};
         else if constexpr (sb_detail::sb_is_optional<U>::value)
            return Option{Box<AnyType>{insert<typename sb_detail::sb_is_optional<U>::elem>()}};
         else if constexpr (sb_detail::sb_is_vector<U>::value)
         {
            using E = typename sb_detail::sb_is_vector<U>::elem;
            // vector<char>/vector<unsigned char>/vector<std::byte> → Custom hex
            if constexpr (sb_detail::sb_is_byte_v<E>)
               return Custom{Box<AnyType>{List{Box<AnyType>{sequence_element<E>()}}},
                             "hex"};
            else
               return List{Box<AnyType>{insert<E>()}};
         }
         else if constexpr (sb_detail::sb_is_array<U>::value)
         {
            using E = typename sb_detail::sb_is_array<U>::elem;
            AnyType arr = Array{Box<AnyType>{sequence_element<E>()},
                                sb_detail::sb_is_array<U>::len};
            if constexpr (sb_detail::sb_is_byte_v<E>)
               return Custom{Box<AnyType>{std::move(arr)}, "hex"};
            return arr;
         }
         else if constexpr (::psio::is_transparent_v<U>)
            // A transparent type is its member: a named alias of the
            // member's type — of the bounded type, when it bounds its member
            // (attr(text, utf8 | max<80>) is the node of utf8_string<80>).
            // The bounded type is an anonymous type of its own that the
            // alias refers to, as a wrapper type is:
            //    Memo -> @1,  @1 = Custom{BoundedList{u8, 80}, "utf8"}
            if constexpr (::psio::transparent_member_checked_v<U>)
            {
               const void* idx = &sb_detail::type_token<sb_detail::transparent_member_of<U>>;
               if (auto it = id_to_name_.find(idx); it != id_to_name_.end())
                  return Type{it->second};
               std::string name = "@" + std::to_string(id_to_name_.size());
               id_to_name_[idx] = name;
               schema_.insert(name, AnyType{});  // placeholder, as in insert()
               schema_.insert(name, annotated_type<::psio::transparent_inner_t<U>>(
                                       ::psio::transparent_member_annotations<U>::value));
               return Type{name};
            }
            else
               return insert<::psio::transparent_inner_t<U>>();
         else if constexpr (is_wit_resource_v<U>)
         {
            // Resource: opaque handle — methods come from
            // interface_info<U> if the user PSIO_INTERFACE'd it
            // (the resource type itself acts as its own tag).
            Resource r;
            r.name = std::string{reflect<U>::name};
            if constexpr (requires { ::psio::detail::interface_info<U>::name; })
               r.methods = build_funcs<U>();
            return r;
         }
         else if constexpr (Reflected<U>)
         {
            Object o;
            constexpr auto N = reflect<U>::member_count;
            build_record_members<U>(o.members, std::make_index_sequence<N>{});
            // Surface type-level caps as IR attributes so they round-
            // trip through emit_pssz / emit_wit / emit_capnp etc.
            if constexpr (::psio::is_dwnc_v<U>)
               o.attributes.push_back(
                  Attribute{"definitionWillNotChange", ""});
            if constexpr (::psio::max_fields_v<U>.has_value())
               o.attributes.push_back(Attribute{
                  "maxFields",
                  std::to_string(::psio::max_fields_v<U>.value())});
            if constexpr (::psio::max_dynamic_data_v<U>.has_value())
               o.attributes.push_back(Attribute{
                  "maxDynamicData",
                  std::to_string(
                     ::psio::max_dynamic_data_v<U>.value())});
            return o;
         }
         else if constexpr (std::is_same_v<U, std::monostate>)
            // The unit type: a variant alternative without a value
            // (FlatBuffers `NONE`, Cap'n Proto `Void`).
            return Tuple{};
         else
         {
            // Fall back to Custom-tagged "unknown" so the IR keeps the
            // type slot but downstream emitters can flag it.
            return Custom{Box<AnyType>{Int{32, false}}, "unknown"};
         }
      }

      // A variant: `alternative<I>()` gives the type of alternative I.
      template <typename U, typename Alternative>
      AnyType build_variant(Alternative&& alternative)
      {
         constexpr auto count = std::variant_size_v<U>;
         static_assert(count <= 256, "schema variant exceeds u8 selector capacity");
         // An alternative's name is its label (variant_label.hpp): the
         // key of its tagged text form.
         PSIO_REQUIRE_VARIANT_LABELS(U);
         return [&]<std::size_t... I>(std::index_sequence<I...>)
         {
            // An untagged variant (untagged.hpp) says so: its text is the
            // bare value of the alternative.
            std::vector<Attribute> flags;
            if constexpr (::psio::is_untagged_v<U>)
               flags.push_back(Attribute{"untagged", ""});
            return Variant{
                {Member{std::string{::psio::variant_label<U, I>()},
                        Box<AnyType>{alternative.template operator()<I>()}, {}}...},
                std::move(flags)};
         }(std::make_index_sequence<count>{});
      }

      // The type of an F at a position that carries `annots` — a member's
      // effective annotations, or what element(...) / keys(...) /
      // values(...) / at<I>(...) state for a position below one
      // (detail/annotation_tree.hpp). A `max<N>` becomes the bounded form of
      // a string / list / map / set (BoundedList, or the string / hex / map /
      // set adapter over one), `utf8` the "utf8" string: the nodes a wrapper
      // type (psio::bounded, psio::utf8_string) or a Rust BoundedList /
      // BoundedString produces. A position with nothing to say is insert<F>().
      //
      // `use_bound` false: the position's own length bound is carried
      // elsewhere (the member attribute lengthBound).
      template <typename F, typename Annots>
      AnyType annotated_type(const Annots& annots, bool use_bound = true)
      {
         namespace ann = ::psio::detail::ann;
         using A = std::remove_cvref_t<Annots>;
         if constexpr (!ann::has_tree_specs_v<A>)
            return insert<F>();
         else if constexpr (sb_detail::sb_is_optional<F>::value)
         {
            using E     = typename sb_detail::sb_is_optional<F>::elem;
            auto  inner = annotated_type<E>(annots, use_bound);
            if (inner == insert<E>())
               return insert<F>();  // nothing to say about the payload
            return Option{Box<AnyType>{std::move(inner)}};
         }
         else
         {
            std::optional<std::uint64_t> max;
            if (use_bound)
               if (auto lb = ::psio::find_spec<::psio::length_bound>(annots);
                   lb && lb->max && !lb->exact && !lb->min)
                  max = *lb->max;
            auto list_of = [&](AnyType element) -> AnyType {
               if (max)
                  return BoundedList{Box<AnyType>{std::move(element)}, *max};
               return List{Box<AnyType>{std::move(element)}};
            };
            if constexpr (std::is_same_v<F, std::string>)
            {
               const bool utf8 = ann::enforces_utf8(annots);
               if (!max && !utf8)
                  return insert<F>();
               return Custom{Box<AnyType>{list_of(insert<std::uint8_t>())},
                             utf8 ? "utf8" : "string"};
            }
            else if constexpr (::psio::detail::is_map_v<F>)
            {
               const auto key_annots   = ann::key_specs(annots);
               const auto value_annots = ann::value_specs(annots);
               constexpr bool below =
                  ann::has_tree_specs_v<std::remove_cvref_t<decltype(key_annots)>> ||
                  ann::has_tree_specs_v<std::remove_cvref_t<decltype(value_annots)>>;
               if (!max && !below)
                  return insert<F>();
               AnyType entry;
               if constexpr (below)
               {
                  // (in this order: the key's types are registered first)
                  auto key   = annotated_type<::psio::detail::assoc_key_t<F>>(key_annots);
                  auto value = annotated_type<typename F::mapped_type>(value_annots);
                  entry      = Tuple{{Box<AnyType>{std::move(key)}, Box<AnyType>{std::move(value)}}};
               }
               else
                  entry = insert<::psio::detail::assoc_entry_t<F>>();
               return Custom{Box<AnyType>{list_of(std::move(entry))}, "map"};
            }
            else if constexpr (::psio::detail::is_set_v<F>)
            {
               const auto inner = ann::element_specs(annots);
               if constexpr (!ann::has_tree_specs_v<std::remove_cvref_t<decltype(inner)>>)
                  if (!max)
                     return insert<F>();
               return Custom{Box<AnyType>{list_of(
                                annotated_type<::psio::detail::assoc_key_t<F>>(inner))},
                             "set"};
            }
            else if constexpr (sb_detail::sb_is_vector<F>::value ||
                               sb_detail::sb_is_bounded<F>::value ||
                               sb_detail::sb_is_array<F>::value)
            {
               using E          = typename F::value_type;
               const auto inner = ann::element_specs(annots);
               constexpr bool below =
                  ann::has_tree_specs_v<std::remove_cvref_t<decltype(inner)>>;
               AnyType sequence;
               if constexpr (sb_detail::sb_is_vector<F>::value)
               {
                  if constexpr (!below)
                     if (!max)
                        return insert<F>();
                  sequence = list_of(annotated_element<E>(inner));
               }
               else if constexpr (!below)
                  // The wrapper's bound / the array's length is in its type.
                  return insert<F>();
               else if constexpr (sb_detail::sb_is_bounded<F>::value)
                  sequence = BoundedList{Box<AnyType>{annotated_element<E>(inner)},
                                         sb_detail::sb_is_bounded<F>::limit};
               else
                  sequence = Array{Box<AnyType>{annotated_element<E>(inner)},
                                   sb_detail::sb_is_array<F>::len};
               if constexpr (sb_detail::sb_is_byte_v<E>)
                  return Custom{Box<AnyType>{std::move(sequence)}, "hex"};
               return sequence;
            }
            else if constexpr (sb_detail::sb_is_tuple<F>::value)
            {
               constexpr bool below = ann::has_position_specs_v<A>;
               if constexpr (!below)
                  return insert<F>();
               else
                  return [&]<std::size_t... I>(std::index_sequence<I...>)
                  {
                     Tuple tuple;
                     // (braced: the elements are built in order)
                     (tuple.members.push_back(Box<AnyType>{
                         annotated_type<std::remove_const_t<std::tuple_element_t<I, F>>>(
                            ann::at_specs<I>(annots))}),
                      ...);
                     return AnyType{std::move(tuple)};
                  }(std::make_index_sequence<std::tuple_size_v<F>>{});
            }
            else if constexpr (sb_detail::sb_is_variant<F>::value)
            {
               if constexpr (!ann::has_position_specs_v<A>)
                  return insert<F>();
               else
                  return build_variant<F>([&]<std::size_t I>() {
                     return annotated_type<std::variant_alternative_t<I, F>>(
                        ann::at_specs<I>(annots));
                  });
            }
            else
               return insert<F>();
         }
      }

      // Whether a `max<N>` on a member of type F (through std::optional) has
      // a bounded type form; when it has none (a psio::utf8_string, a nested
      // value) or the bound uses exact / min, it is the member attribute
      // lengthBound.
      template <typename F>
      static constexpr bool has_bounded_form()
      {
         if constexpr (sb_detail::sb_is_optional<F>::value)
            return has_bounded_form<typename sb_detail::sb_is_optional<F>::elem>();
         else
            return std::is_same_v<F, std::string> || sb_detail::sb_is_vector<F>::value ||
                   ::psio::detail::is_assoc_v<F>;
      }

      // Whether a member length_bound constrains F at all (the typed codecs
      // apply it to strings and lists, through std::optional).
      template <typename F>
      static constexpr bool length_bound_applies()
      {
         if constexpr (sb_detail::sb_is_optional<F>::value)
            return length_bound_applies<typename sb_detail::sb_is_optional<F>::elem>();
         else
            return std::is_same_v<F, std::string> || sb_detail::sb_is_vector<F>::value ||
                   sb_detail::sb_is_utf8<F>::value || ::psio::detail::is_nested_v<F> ||
                   ::psio::detail::is_assoc_v<F>;
      }

      // The schema's view of member annotations (shared by record members
      // and method parameters): the annotated type, and the attributes that
      // have no type form.
      template <typename F, typename Annots>
      void apply_member_annotations(Member& member, const Annots& annots)
      {
         using eff_t = std::remove_cvref_t<Annots>;
         if constexpr (::psio::detail::ann::has_tree_specs_v<eff_t> &&
                       !::psio::has_binary_as_override_v<F, eff_t>)
         {
            bool as_attribute = false;
            if constexpr (::psio::has_spec_v<::psio::length_bound, eff_t>)
            {
               const ::psio::length_bound lb = std::get<::psio::length_bound>(annots);
               if constexpr (length_bound_applies<F>())
               {
                  if (!(lb.max && !lb.exact && !lb.min && has_bounded_form<F>()))
                  {
                     as_attribute = true;
                     std::string text;
                     auto item = [&](const char* key, const std::optional<std::uint32_t>& v) {
                        if (!v) return;
                        if (!text.empty()) text += ',';
                        text += key;
                        text += '=';
                        text += std::to_string(*v);
                     };
                     item("exact", lb.exact);
                     item("min", lb.min);
                     item("max", lb.max);
                     if (!text.empty())
                        member.attributes.push_back(Attribute{"lengthBound", std::move(text)});
                  }
               }
               else
                  as_attribute = true;  // the bound constrains nothing here
            }
            *member.type = annotated_type<F>(annots, !as_attribute);
         }
         if constexpr (::psio::has_spec_v<::psio::json_int_spec, eff_t>)
            member.attributes.push_back(Attribute{
                "jsonInt", std::get<::psio::json_int_spec>(annots).form ==
                                 ::psio::int_text::string
                              ? "string"
                              : "number"});
         if constexpr (::psio::has_spec_v<::psio::ijson_int_spec, eff_t>)
            member.attributes.push_back(Attribute{
                "ijsonInt", std::get<::psio::ijson_int_spec>(annots).form ==
                                  ::psio::int_text::string
                               ? "string"
                               : "number"});
      }

      template <typename U, std::size_t I>
      Member build_record_member()
      {
         using R   = reflect<U>;
         using F   = typename R::template member_type<I>;
         using eff = ::psio::effective_annotations_for<U, F, R::template member_pointer<I>>;
         Member member{std::string{R::template member_name<I>}, Box<AnyType>{insert<F>()}, {}};
         apply_member_annotations<F>(member, eff::value);
         // What the numbered formats (protobuf, Cap'n Proto, FlatBuffers)
         // read: the member's field number when it is stated
         // (psio/detail/field_number.hpp: else its 1-based position), a
         // retired member, and the member initialiser of a scalar when it is
         // not zero (the FlatBuffers / Cap'n Proto default of the field).
         using eff_t = std::remove_cvref_t<decltype(eff::value)>;
         if constexpr (::psio::has_spec_v<::psio::field_num_spec, eff_t>)
            member.attributes.push_back(Attribute{
                "field", std::to_string(std::get<::psio::field_num_spec>(eff::value).value)});
         if constexpr (::psio::has_spec_v<::psio::deprecated_spec, eff_t>)
            member.attributes.push_back(Attribute{
                "deprecated",
                std::string{std::get<::psio::deprecated_spec>(eff::value).version}});
         // How protobuf writes the member's integers (annotate.hpp).
         if constexpr (::psio::has_spec_v<::psio::pb_fixed_spec, eff_t>)
            member.attributes.push_back(Attribute{"protobuf", "fixed"});
         else if constexpr (::psio::has_spec_v<::psio::pb_sint_spec, eff_t>)
            member.attributes.push_back(Attribute{"protobuf", "sint"});
         using V = std::remove_cvref_t<F>;
         if constexpr ((std::is_arithmetic_v<V> || std::is_enum_v<V>) && sizeof(V) <= 8 &&
                       requires { U{}; })
         {
            const U    object{};
            const V    value = object.*(R::template member_pointer<I>);
            const auto text  = schema_scalar_text(value);
            if (!text.empty())
               member.attributes.push_back(Attribute{"default", text});
         }
         return member;
      }

      // The text of a scalar default (the `default` member attribute):
      // empty for zero (no attribute), else `true`, a decimal integer (an
      // enum is its value), or the shortest decimal text that reads back as
      // the same float (`nan`, `inf`, `-inf`, `-0` for those values).
      template <typename V>
      static std::string schema_scalar_text(V value)
      {
         if constexpr (std::is_same_v<V, bool>)
            return value ? "true" : "";
         else if constexpr (std::is_enum_v<V>)
            return schema_scalar_text(static_cast<std::underlying_type_t<V>>(value));
         else if constexpr (std::is_floating_point_v<V>)
         {
            if (value != value)
               return "nan";
            if (value == std::numeric_limits<V>::infinity())
               return "inf";
            if (value == -std::numeric_limits<V>::infinity())
               return "-inf";
            if (value == 0)
               return std::signbit(value) ? "-0" : "";
            char       buffer[64];
            const auto r = std::to_chars(buffer, buffer + sizeof(buffer), value);
            return std::string(buffer, r.ptr);
         }
         else if constexpr (std::is_signed_v<V>)
            return value == 0 ? "" : std::to_string(static_cast<long long>(value));
         else
            return value == 0 ? "" : std::to_string(static_cast<unsigned long long>(value));
      }

      template <typename U, std::size_t... Is>
      void build_record_members(std::vector<Member>& out,
                                std::index_sequence<Is...>)
      {
         (out.push_back(build_record_member<U, Is>()), ...);
      }

      // ── interface_info<Tag> walk → Interface ──────────────────────
      template <typename Tag>
      Interface build_interface()
      {
         using info       = ::psio::detail::interface_info<Tag>;
         using types_t    = typename info::types;
         using func_types = typename info::func_types;

         // Capture package while we're here; SchemaBuilder reports the
         // last-walked package on the resulting Schema.
         using pkg = typename info::package;
         if constexpr (requires { pkg::name; })
         {
            // An interface with no package (a global tag) leaves the
            // package captured so far in place.
            if (!std::string_view{pkg::name}.empty())
            {
               package_name_    = std::string{pkg::name};
               package_version_ = std::string{pkg::version};
            }
         }

         Interface iface;
         iface.name = std::string{info::name};

         add_interface_types<types_t>(
            iface,
            std::make_index_sequence<std::tuple_size_v<types_t>>{});

         constexpr auto N = std::tuple_size_v<func_types>;
         add_interface_funcs<Tag>(iface, std::make_index_sequence<N>{});
         return iface;
      }

      template <typename TypesTuple, std::size_t... Is>
      void add_interface_types(Interface& iface, std::index_sequence<Is...>)
      {
         (iface.type_names.push_back(
             type_name_for<std::tuple_element_t<Is, TypesTuple>>()),
          ...);
      }

      // Insert T (returning Type{} into the schema) and return its
      // assigned name; Interface::type_names carries names, not
      // AnyType payloads.
      template <typename T>
      std::string type_name_for()
      {
         auto a = insert<T>();
         if (auto* t = std::get_if<Type>(&a.value))
            return t->type;
         // Should not happen given insert<>() always returns a Type{}
         // reference; defend defensively.
         return std::string{reflect<std::remove_cvref_t<T>>::name};
      }

      template <typename Tag, std::size_t... Is>
      void add_interface_funcs(Interface& iface, std::index_sequence<Is...>)
      {
         (iface.funcs.push_back(build_interface_func<Tag, Is>()), ...);
      }

      // Function I of an interface. One whose functions are reflect<Tag>'s
      // methods carries what method(...) lists: parameter annotations (the
      // bounded parameter types), returns(...) and the argument cap.
      template <typename Tag, std::size_t I>
      Func build_interface_func()
      {
         using info       = ::psio::detail::interface_info<Tag>;
         using func_types = typename info::func_types;
         Func f = build_func<std::tuple_element_t<I, func_types>>(info::func_names[I],
                                                                  info::param_names[I]);
         if constexpr (requires { typename info::reflected_tag; })
         {
            using R      = reflect<typename info::reflected_tag>;
            using Params = typename R::template method_params<I>;
            const auto& meta = R::template method_annotations<I>;
            [&]<std::size_t... Ps>(std::index_sequence<Ps...>) {
               (apply_member_annotations<std::tuple_element_t<Ps, Params>>(
                   f.params[Ps], std::get<Ps>(meta.params)),
                ...);
            }(std::make_index_sequence<
                 std::tuple_size_v<std::remove_cvref_t<decltype(meta.params)>>>{});
            if constexpr (!std::is_void_v<typename R::template method_result<I>>)
            {
               using Result = std::remove_cvref_t<typename R::template method_result<I>>;
               if (f.result)
               {
                  Member result{"", std::move(*f.result), {}};
                  apply_member_annotations<Result>(result, meta.result);
                  f.result = std::move(result.type);
                  for (auto& attribute : result.attributes)
                     f.attributes.push_back(
                        Attribute{"result." + attribute.name, std::move(attribute.value)});
               }
            }
            // The cap of the argument tuple: its encoded size limit and its
            // offset width.
            if (meta.max_fields)
               f.attributes.push_back(Attribute{"maxFields", std::to_string(*meta.max_fields)});
            if (meta.max_dynamic_data)
               f.attributes.push_back(
                  Attribute{"maxDynamicData", std::to_string(*meta.max_dynamic_data)});
         }
         return f;
      }

      // Build a Resource's methods (same shape as a free interface's
      // funcs, but reached through the resource's PSIO_INTERFACE).
      template <typename Resource_T>
      std::vector<Func> build_funcs()
      {
         using info       = ::psio::detail::interface_info<Resource_T>;
         using func_types = typename info::func_types;
         constexpr auto N = std::tuple_size_v<func_types>;
         std::vector<Func> out;
         out.reserve(N);
         add_resource_funcs<Resource_T>(out, std::make_index_sequence<N>{});
         return out;
      }

      template <typename Resource_T, std::size_t... Is>
      void add_resource_funcs(std::vector<Func>& out,
                              std::index_sequence<Is...>)
      {
         using info       = ::psio::detail::interface_info<Resource_T>;
         using func_types = typename info::func_types;
         (out.push_back(build_func<std::tuple_element_t<Is, func_types>>(
             info::func_names[Is], info::param_names[Is])),
          ...);
      }

      // ── Function-pointer decomposition + Func construction ────────

      // A static member (function pointer) or an instance member function:
      // the implicit object is not a parameter.
      template <typename FnPtr>
      Func build_func(std::string_view                       name,
                      const std::initializer_list<const char*>& pnames)
      {
         using D = ::psio::method_traits<FnPtr>;
         Func f;
         f.name = std::string{name};
         build_params<typename D::args>(
            f.params, pnames, std::make_index_sequence<D::arity>{});
         if constexpr (!std::is_void_v<typename D::result>)
            f.result = Box<AnyType>{insert<std::remove_cvref_t<typename D::result>>()};
         return f;
      }

      template <typename ArgsTuple, std::size_t... Is>
      void build_params(std::vector<Member>&                       out,
                        const std::initializer_list<const char*>& pnames,
                        std::index_sequence<Is...>)
      {
         // A function that names every parameter carries the names; one
         // that does not (none, or not all of them) is positional and its
         // parameters are "0", "1", ... — the names of the members of a
         // `tuple = "positional"` argument list (docs/json.md "Method
         // arguments"). Never an empty or a repeated name: the parameters
         // are the members of a record.
         const bool named = pnames.size() == sizeof...(Is);
         auto       it    = pnames.begin();
         ((out.push_back(Member{
              named ? std::string{*it++} : std::to_string(Is),
              Box<AnyType>{
                 insert<std::remove_cvref_t<std::tuple_element_t<Is, ArgsTuple>>>()},
              {}})),
          ...);
      }

      // ── world_info<Tag> walk → World ──────────────────────────────
      template <typename WorldTag>
      World build_world()
      {
         using winfo   = ::psio::detail::world_info<WorldTag>;
         using imports = typename winfo::imports;
         using exports = typename winfo::exports;

         using pkg = typename winfo::package;
         if constexpr (requires { pkg::name; })
         {
            package_name_    = std::string{pkg::name};
            package_version_ = std::string{pkg::version};
         }

         World w;
         w.name = std::string{winfo::name};

         add_world_imports<imports>(
            w, std::make_index_sequence<std::tuple_size_v<imports>>{});
         add_world_exports<exports>(
            w, std::make_index_sequence<std::tuple_size_v<exports>>{});
         return w;
      }

      template <typename ImportsTuple, std::size_t... Is>
      void add_world_imports(World& w, std::index_sequence<Is...>)
      {
         (handle_world_import<std::tuple_element_t<Is, ImportsTuple>>(w),
          ...);
      }

      template <typename ExportsTuple, std::size_t... Is>
      void add_world_exports(World& w, std::index_sequence<Is...>)
      {
         (handle_world_export<std::tuple_element_t<Is, ExportsTuple>>(w),
          ...);
      }

      // Imports: a use-tag → Use entry + UseRef; an interface tag →
      // walk it as a free interface.
      template <typename T>
      void handle_world_import(World& w)
      {
         if constexpr (requires { ::psio::detail::use_info<T>::package; })
         {
            using uinfo = ::psio::detail::use_info<T>;
            schema_.uses.push_back(
               Use{std::string{uinfo::package},
                   std::string{uinfo::interface_name},
                   std::string{uinfo::version},
                   {}});
            w.imports.push_back(UseRef{std::string{uinfo::package},
                                       std::string{uinfo::interface_name}});
         }
         else if constexpr (requires { ::psio::detail::interface_info<T>::name; })
         {
            // Walk the interface so its types/funcs land in the schema
            // even if the world only references it.
            schema_.interfaces.push_back(build_interface<T>());
         }
      }

      template <typename T>
      void handle_world_export(World& w)
      {
         if constexpr (requires { ::psio::detail::interface_info<T>::name; })
         {
            schema_.interfaces.push_back(build_interface<T>());
            w.exports.push_back(
               std::string{::psio::detail::interface_info<T>::name});
         }
      }
   };

}  // namespace psio::schema_types

namespace psio
{
   using schema_types::SchemaBuilder;
}
