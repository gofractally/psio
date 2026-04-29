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
//                       std::array<T,N>
//   Resources           wit_resource-derived (own<T>/borrow<T> wrap to
//                       references, with the resource definition picked
//                       up from interface_info<T>)
//   Records             PSIO_REFLECT'd structs (Object form)
//   Envelope            PSIO_INTERFACE → Interface,
//                       PSIO_WORLD     → World (with imports/exports),
//                       PSIO_USE       → Use
//
// Out of scope for now (lands as v3 ports the supporting types):
//   bounded_list, bitvector, bitlist, std::bitset, std::variant,
//   uint128 / int128 / uint256, chrono::duration / time_point,
//   FracPack-nested wrappers, PackableWrapper unwraps, and the
//   full custom-handler chain.  These are extensions to the same
//   if-constexpr ladder when their types land in v3.

#include <psio/annotate.hpp>
#include <psio/reflect.hpp>
#include <psio/schema_ir.hpp>
#include <psio/structural.hpp>
#include <psio/wit_resource.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace psio::schema_types
{

   namespace sb_detail
   {
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
   }  // namespace detail

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
      // produce a cycle that resolve() walks until the stack runs out.
      template <typename T>
      SchemaBuilder& insert(std::string name) &
      {
         AnyType ref = insert<T>();
         if (auto* t = std::get_if<Type>(&ref.value); t && t->type == name)
            return *this;
         schema_.insert(std::move(name), std::move(ref));
         return *this;
      }

      template <typename T>
      SchemaBuilder&& insert(std::string name) &&
      {
         AnyType ref = insert<T>();
         if (auto* t = std::get_if<Type>(&ref.value); t && t->type == name)
            return std::move(*this);
         schema_.insert(std::move(name), std::move(ref));
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
      //     them by their source-level name.
      //   - Everything else (primitives, std::vector, std::optional,
      //     std::array, etc.) uses an `@N` interning name; emitters
      //     resolve those inline at the use site rather than emit
      //     them as standalone aliases.
      template <typename T>
      AnyType insert()
      {
         using U = std::remove_cvref_t<T>;

         auto idx = std::type_index(typeid(U));
         if (auto it = id_to_name_.find(idx); it != id_to_name_.end())
            return Type{it->second};

         std::string name;
         if constexpr (Reflected<U>)
            name = std::string{reflect<U>::name};
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

      // Finalise.  Sets the schema's package to whatever
      // PSIO_CURRENT_PACKAGE_ resolves to at the most recently
      // inserted world / interface (via the captured package_name_).
      Schema build() &&
      {
         if (!package_name_.empty())
         {
            schema_.package.name    = package_name_;
            schema_.package.version = package_version_;
         }
         return std::move(schema_);
      }

      Schema build() const&
      {
         Schema copy = schema_;
         if (!package_name_.empty())
         {
            copy.package.name    = package_name_;
            copy.package.version = package_version_;
         }
         return copy;
      }

      // ── Internals (public so to_schema customisation can reach in
      //     once that hook lands; today the only paths are the
      //     if-constexpr ladder and the interface/world walkers) ─────

      const Schema& peek() const noexcept { return schema_; }

    private:
      Schema                                   schema_;
      std::unordered_map<std::type_index, std::string> id_to_name_;
      std::string                              package_name_;
      std::string                              package_version_;

      // ── Build an AnyType for T, for the if-constexpr ladder ────────
      template <typename U>
      AnyType build_anytype()
      {
         if constexpr (std::is_same_v<U, bool>)
            return Custom{Box<AnyType>{Int{1, false}}, "bool"};
         else if constexpr (std::is_same_v<U, char>)
            return Custom{Box<AnyType>{Int{8, false}}, "char"};
         else if constexpr (std::is_integral_v<U>)
            return Int{static_cast<std::uint32_t>(8 * sizeof(U)),
                       std::is_signed_v<U>};
         else if constexpr (std::is_floating_point_v<U> &&
                            std::numeric_limits<U>::is_iec559)
         {
            // exponent bits = ceil(log2(max_exponent))
            std::uint32_t exp = 0;
            for (auto e = std::numeric_limits<U>::max_exponent; e > 1;
                 e >>= 1)
               ++exp;
            return Float{exp, static_cast<std::uint32_t>(
                                 std::numeric_limits<U>::digits)};
         }
         else if constexpr (std::is_same_v<U, std::string> ||
                            std::is_same_v<U, std::string_view>)
            return Custom{Box<AnyType>{List{Box<AnyType>{insert<std::uint8_t>()}}},
                          "string"};
         else if constexpr (sb_detail::sb_is_optional<U>::value)
            return Option{Box<AnyType>{insert<typename sb_detail::sb_is_optional<U>::elem>()}};
         else if constexpr (sb_detail::sb_is_vector<U>::value)
         {
            using E = typename sb_detail::sb_is_vector<U>::elem;
            // vector<char>/vector<unsigned char> → Custom hex
            if constexpr (std::is_same_v<E, char> ||
                          std::is_same_v<E, unsigned char>)
               return Custom{Box<AnyType>{List{Box<AnyType>{insert<E>()}}},
                             "hex"};
            else
               return List{Box<AnyType>{insert<E>()}};
         }
         else if constexpr (sb_detail::sb_is_array<U>::value)
         {
            using E = typename sb_detail::sb_is_array<U>::elem;
            AnyType arr = Array{Box<AnyType>{insert<E>()},
                                sb_detail::sb_is_array<U>::len};
            if constexpr (std::is_same_v<E, char> ||
                          std::is_same_v<E, unsigned char>)
               return Custom{Box<AnyType>{std::move(arr)}, "hex"};
            return arr;
         }
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
         else
         {
            // Fall back to Custom-tagged "unknown" so the IR keeps the
            // type slot but downstream emitters can flag it.
            return Custom{Box<AnyType>{Int{32, false}}, "unknown"};
         }
      }

      template <typename U, std::size_t... Is>
      void build_record_members(std::vector<Member>& out,
                                std::index_sequence<Is...>)
      {
         ((out.push_back(Member{
              std::string{reflect<U>::template member_name<Is>},
              Box<AnyType>{insert<typename reflect<U>::template member_type<Is>>()},
              {}})),
          ...);
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
            package_name_    = std::string{pkg::name};
            package_version_ = std::string{pkg::version};
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
         using info       = ::psio::detail::interface_info<Tag>;
         using func_types = typename info::func_types;
         (iface.funcs.push_back(
             build_func<std::tuple_element_t<Is, func_types>>(
                info::func_names[Is], info::param_names[Is])),
          ...);
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

      template <typename FnPtr>
      struct fn_decomp;
      template <typename R, typename... Args>
      struct fn_decomp<R (*)(Args...)>
      {
         using ret                          = R;
         using args                         = std::tuple<Args...>;
         static constexpr std::size_t arity = sizeof...(Args);
      };

      template <typename FnPtr>
      Func build_func(std::string_view                       name,
                      const std::initializer_list<const char*>& pnames)
      {
         using D = fn_decomp<FnPtr>;
         Func f;
         f.name = std::string{name};
         build_params<typename D::args>(
            f.params, pnames, std::make_index_sequence<D::arity>{});
         if constexpr (!std::is_void_v<typename D::ret>)
            f.result = Box<AnyType>{insert<std::remove_cvref_t<typename D::ret>>()};
         return f;
      }

      template <typename ArgsTuple, std::size_t... Is>
      void build_params(std::vector<Member>&                       out,
                        const std::initializer_list<const char*>& pnames,
                        std::index_sequence<Is...>)
      {
         auto it = pnames.begin();
         ((out.push_back(Member{
              std::string{it != pnames.end() ? *it++ : ""},
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
