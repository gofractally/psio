#pragma once
//
// psio/wit_gen.hpp — Runtime WIT generator.
//
// Walks the metadata declared by PSIO_PACKAGE / PSIO_INTERFACE
// (interface_info<Tag>) plus PSIO_REFLECT (records) to produce:
//
//   1. A populated psio::wit_world IR.
//   2. Standards-compliant WIT text (package + interface + world blocks).
//
// Companion to psio/wit_constexpr.hpp (the consteval embedding flow,
// landing next): both consume the same interface_info<Tag>; this one
// runs at runtime and uses the heap.
//
// Usage:
//
//    auto world = psio::generate_wit<wasi_clocks_wall_clock>(
//                    "wasi:clocks", "0.2.3", "imports");
//    auto text  = psio::generate_wit_text<wasi_clocks_wall_clock>(
//                    "wasi:clocks", "0.2.3", "imports");
//
// Type mapping (mirrors v1):
//
//   bool / uint{8,16,32,64}_t / int{8,16,32,64}_t / float / double / char
//      → bool / u8…u64 / s8…s64 / f32 / f64 / char
//   std::string / std::string_view              → string
//   std::vector<T>                              → list<T>
//   std::optional<T>                            → option<T>
//   psio::own<T>                                → own<T-record>
//   psio::borrow<T>                             → borrow<T-record>
//   PSIO_REFLECT'd struct (data members)        → record
//   wit_resource-derived (PSIO_INTERFACE'd)     → resource with methods

#include <psio/reflect.hpp>
#include <psio/structural.hpp>
#include <psio/wit_resource.hpp>
#include <psio/wit_types.hpp>

#include <cctype>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace psio
{

   namespace detail
   {

      // ── Primitive map ───────────────────────────────────────────────
      template <typename T>
      struct wit_prim_map
      {
         static constexpr bool has_value = false;
      };
#define PSIO_WIT_PRIM_MAP_(CPP_TYPE, WIT_PRIM)                       \
   template <>                                                        \
   struct wit_prim_map<CPP_TYPE>                                      \
   {                                                                  \
      static constexpr bool     has_value = true;                     \
      static constexpr wit_prim value     = WIT_PRIM;                 \
   };
      PSIO_WIT_PRIM_MAP_(bool, wit_prim::bool_)
      PSIO_WIT_PRIM_MAP_(std::uint8_t, wit_prim::u8)
      PSIO_WIT_PRIM_MAP_(std::int8_t, wit_prim::s8)
      PSIO_WIT_PRIM_MAP_(std::uint16_t, wit_prim::u16)
      PSIO_WIT_PRIM_MAP_(std::int16_t, wit_prim::s16)
      PSIO_WIT_PRIM_MAP_(std::uint32_t, wit_prim::u32)
      PSIO_WIT_PRIM_MAP_(std::int32_t, wit_prim::s32)
      PSIO_WIT_PRIM_MAP_(std::uint64_t, wit_prim::u64)
      PSIO_WIT_PRIM_MAP_(std::int64_t, wit_prim::s64)
      PSIO_WIT_PRIM_MAP_(float, wit_prim::f32)
      PSIO_WIT_PRIM_MAP_(double, wit_prim::f64)
      PSIO_WIT_PRIM_MAP_(char, wit_prim::char_)
#undef PSIO_WIT_PRIM_MAP_

      // ── Compound-type detectors ─────────────────────────────────────
      template <typename T>
      struct is_std_string : std::false_type
      {
      };
      template <>
      struct is_std_string<std::string> : std::true_type
      {
      };
      template <>
      struct is_std_string<std::string_view> : std::true_type
      {
      };

      template <typename T>
      struct is_std_vector : std::false_type
      {
      };
      template <typename E, typename A>
      struct is_std_vector<std::vector<E, A>> : std::true_type
      {
         using elem = E;
      };

      template <typename T>
      struct is_std_optional : std::false_type
      {
      };
      template <typename T>
      struct is_std_optional<std::optional<T>> : std::true_type
      {
         using elem = T;
      };

      // ── Interface-info presence check (resources opt in via PSIO_INTERFACE) ──
      template <typename T, typename = void>
      struct has_interface_info : std::false_type
      {
      };
      template <typename T>
      struct has_interface_info<T, std::void_t<decltype(interface_info<T>::name)>>
         : std::true_type
      {
      };

      // ── Function-pointer decomposition ──────────────────────────────
      template <typename F>
      struct fn_decompose;
      template <typename R, typename... Args>
      struct fn_decompose<R (*)(Args...)>
      {
         using ret                        = R;
         using args                       = std::tuple<Args...>;
         static constexpr std::size_t arity = sizeof...(Args);
      };

      // ── Kebab-case ──────────────────────────────────────────────────
      //
      // snake_case / camelCase / PascalCase → kebab-case.  Mirrors v1's
      // wit_gen_ctx::to_kebab_case so identifiers round-trip the way
      // existing WIT consumers expect.
      inline std::string to_kebab_case(std::string_view s)
      {
         std::string out;
         out.reserve(s.size() + 4);
         for (std::size_t i = 0; i < s.size(); ++i)
         {
            char c = s[i];
            if (c == '_')
            {
               if (!out.empty() && out.back() != '-')
                  out.push_back('-');
            }
            else if (std::isupper(static_cast<unsigned char>(c)))
            {
               if (!out.empty() && out.back() != '-')
                  out.push_back('-');
               out.push_back(static_cast<char>(
                  std::tolower(static_cast<unsigned char>(c))));
            }
            else
            {
               out.push_back(c);
            }
         }
         return out;
      }

      // ─────────────────────────────────────────────────────────────────
      // wit_gen_ctx — accumulates types into a wit_world, deduplicating
      // by std::type_index.
      // ─────────────────────────────────────────────────────────────────

      struct wit_gen_ctx
      {
         wit_world                                       world;
         std::unordered_map<std::type_index, std::int32_t> type_cache;

         std::int32_t add_type(wit_type_def&& td)
         {
            auto idx = static_cast<std::int32_t>(world.types.size());
            world.types.push_back(std::move(td));
            return idx;
         }

         template <typename T>
         std::int32_t resolve_type()
         {
            using U = std::remove_cvref_t<T>;

            // 1. Primitives.
            if constexpr (wit_prim_map<U>::has_value)
            {
               return wit_prim_idx(wit_prim_map<U>::value);
            }
            // 2. std::string{,_view} → string primitive.
            else if constexpr (is_std_string<U>::value)
            {
               return wit_prim_idx(wit_prim::string_);
            }
            // 3. std::vector<E> → list<E>.
            else if constexpr (is_std_vector<U>::value)
            {
               auto key = std::type_index(typeid(U));
               if (auto it = type_cache.find(key); it != type_cache.end())
                  return it->second;
               auto         elem_idx = resolve_type<typename is_std_vector<U>::elem>();
               wit_type_def td;
               td.kind             = static_cast<std::uint8_t>(wit_type_kind::list_);
               td.element_type_idx = elem_idx;
               auto idx            = add_type(std::move(td));
               type_cache[key]     = idx;
               return idx;
            }
            // 4. std::optional<E> → option<E>.
            else if constexpr (is_std_optional<U>::value)
            {
               auto key = std::type_index(typeid(U));
               if (auto it = type_cache.find(key); it != type_cache.end())
                  return it->second;
               auto         elem_idx = resolve_type<typename is_std_optional<U>::elem>();
               wit_type_def td;
               td.kind             = static_cast<std::uint8_t>(wit_type_kind::option_);
               td.element_type_idx = elem_idx;
               auto idx            = add_type(std::move(td));
               type_cache[key]     = idx;
               return idx;
            }
            // 5. own<R> / borrow<R> → handle wrappers around a resource.
            else if constexpr (is_own_ct<U>::value || is_borrow_ct<U>::value)
            {
               return resolve_handle<U>();
            }
            // 6. PSIO_REFLECT'd record (data-member walk).
            else if constexpr (Reflected<U>)
            {
               return resolve_record_or_resource<U>();
            }
            else
            {
               // Unknown type — fall back to u32 like v1 did.  Better
               // than a hard error: keeps the generator total even when
               // a stub binding hasn't been wired up yet.
               return wit_prim_idx(wit_prim::u32);
            }
         }

         // ── own<T> / borrow<T> handle resolution ────────────────────
         //
         // The wrapped type T must be a wit_resource.  Resolve T first
         // (which may emit a `resource` block via interface_info<T>),
         // then wrap with the own_/borrow_ kind.
         template <typename Handle>
         std::int32_t resolve_handle()
         {
            auto key = std::type_index(typeid(Handle));
            if (auto it = type_cache.find(key); it != type_cache.end())
               return it->second;

            using Res = typename handle_inner<Handle>::type;
            static_assert(is_wit_resource_v<Res>,
                          "own<T>/borrow<T> requires T : psio::wit_resource");

            auto         res_idx = resolve_type<Res>();
            wit_type_def td;
            td.kind = is_own_ct<Handle>::value
                         ? static_cast<std::uint8_t>(wit_type_kind::own_)
                         : static_cast<std::uint8_t>(wit_type_kind::borrow_);
            td.element_type_idx = res_idx;
            auto idx            = add_type(std::move(td));
            type_cache[key]     = idx;
            return idx;
         }

         template <typename H>
         struct handle_inner;
         template <typename T>
         struct handle_inner<own<T>>
         {
            using type = T;
         };
         template <typename T>
         struct handle_inner<borrow<T>>
         {
            using type = T;
         };

         // ── PSIO_REFLECT'd type → record or resource ────────────────
         template <typename U>
         std::int32_t resolve_record_or_resource()
         {
            auto key = std::type_index(typeid(U));
            if (auto it = type_cache.find(key); it != type_cache.end())
               return it->second;

            // Reserve the slot first so recursive references resolve.
            auto idx        = static_cast<std::int32_t>(world.types.size());
            type_cache[key] = idx;
            world.types.emplace_back();

            wit_type_def td;
            td.name = to_kebab_case(reflect<U>::name);

            if constexpr (is_wit_resource_v<U>)
            {
               td.kind = static_cast<std::uint8_t>(wit_type_kind::resource_);
               // Methods come from a PSIO_INTERFACE specialisation on U
               // itself.  Without one, emit a bare `resource T;`.
               if constexpr (has_interface_info<U>::value)
                  populate_resource_methods<U>(td);
            }
            else
            {
               td.kind = static_cast<std::uint8_t>(wit_type_kind::record_);
               populate_record_fields<U>(td);
            }

            world.types[idx] = std::move(td);
            return idx;
         }

         // ── Record fields via reflect<U>::for_each_field ────────────
         template <typename U>
         void populate_record_fields(wit_type_def& td)
         {
            constexpr auto N = reflect<U>::member_count;
            populate_record_fields_impl<U>(td, std::make_index_sequence<N>{});
         }
         template <typename U, std::size_t... Is>
         void populate_record_fields_impl(wit_type_def& td,
                                          std::index_sequence<Is...>)
         {
            ((td.fields.push_back(
                  {to_kebab_case(reflect<U>::template member_name<Is>),
                   resolve_type<typename reflect<U>::template member_type<Is>>()})),
             ...);
         }

         // ── Resource methods via interface_info<U> ──────────────────
         template <typename U>
         void populate_resource_methods(wit_type_def& td)
         {
            using info       = interface_info<U>;
            using func_types = typename info::func_types;
            constexpr auto N = std::tuple_size_v<func_types>;
            populate_resource_methods_impl<U>(td, std::make_index_sequence<N>{});
         }
         template <typename U, std::size_t... Is>
         void populate_resource_methods_impl(wit_type_def& td,
                                             std::index_sequence<Is...>)
         {
            using info       = interface_info<U>;
            using func_types = typename info::func_types;
            (emit_function<std::tuple_element_t<Is, func_types>>(
                info::func_names[Is], info::param_names[Is],
                [&](std::uint32_t fi) { td.method_func_idxs.push_back(fi); }),
             ...);
         }

         // ── Emit one wit_func, append to world.funcs, run callback ──
         template <typename FnPtr, typename Sink>
         void emit_function(std::string_view                       name,
                            const std::initializer_list<const char*>& pnames,
                            Sink&&                                  sink)
         {
            using Decomp     = fn_decompose<FnPtr>;
            using ArgsTuple  = typename Decomp::args;
            constexpr auto A = Decomp::arity;

            wit_func f;
            f.name = to_kebab_case(name);

            collect_params<ArgsTuple>(f, pnames, std::make_index_sequence<A>{});

            if constexpr (!std::is_void_v<typename Decomp::ret>)
            {
               using R = std::remove_cvref_t<typename Decomp::ret>;
               f.results.push_back({"", resolve_type<R>()});
            }

            auto fi = static_cast<std::uint32_t>(world.funcs.size());
            world.funcs.push_back(std::move(f));
            sink(fi);
         }

         template <typename ArgsTuple, std::size_t... Is>
         void collect_params(wit_func&                                  f,
                             const std::initializer_list<const char*>& pnames,
                             std::index_sequence<Is...>)
         {
            auto it = pnames.begin();
            ((f.params.push_back(
                  {to_kebab_case(it != pnames.end() ? *it++ : ""),
                   resolve_type<std::remove_cvref_t<
                      std::tuple_element_t<Is, ArgsTuple>>>()})),
             ...);
         }
      };

      // ── Text emission ───────────────────────────────────────────────

      inline std::string wit_type_name(const wit_world& w, std::int32_t idx)
      {
         if (is_prim_idx(idx))
         {
            switch (idx_to_prim(idx))
            {
               case wit_prim::bool_:   return "bool";
               case wit_prim::u8:      return "u8";
               case wit_prim::s8:      return "s8";
               case wit_prim::u16:     return "u16";
               case wit_prim::s16:     return "s16";
               case wit_prim::u32:     return "u32";
               case wit_prim::s32:     return "s32";
               case wit_prim::u64:     return "u64";
               case wit_prim::s64:     return "s64";
               case wit_prim::f32:     return "f32";
               case wit_prim::f64:     return "f64";
               case wit_prim::char_:   return "char";
               case wit_prim::string_: return "string";
            }
            return "u32";
         }
         auto i = static_cast<std::size_t>(idx);
         if (i >= w.types.size())
            return "u32";
         const auto& td = w.types[i];
         switch (static_cast<wit_type_kind>(td.kind))
         {
            case wit_type_kind::list_:
               return "list<" + wit_type_name(w, td.element_type_idx) + ">";
            case wit_type_kind::option_:
               return "option<" + wit_type_name(w, td.element_type_idx) + ">";
            case wit_type_kind::result_:
               return "result<" + wit_type_name(w, td.element_type_idx) + ", " +
                      wit_type_name(w, td.error_type_idx) + ">";
            case wit_type_kind::tuple_:
            {
               std::string s = "tuple<";
               for (std::size_t j = 0; j < td.fields.size(); ++j)
               {
                  if (j > 0)
                     s += ", ";
                  s += wit_type_name(w, td.fields[j].type_idx);
               }
               s += ">";
               return s;
            }
            case wit_type_kind::own_:
               return "own<" + wit_type_name(w, td.element_type_idx) + ">";
            case wit_type_kind::borrow_:
               return "borrow<" + wit_type_name(w, td.element_type_idx) + ">";
            default:
               return td.name.empty() ? "u32" : td.name;
         }
      }

      inline void wit_emit_func(std::ostringstream& os,
                                const wit_world&    w,
                                const wit_func&     f,
                                const std::string&  indent)
      {
         os << indent << f.name << ": func(";
         for (std::size_t i = 0; i < f.params.size(); ++i)
         {
            if (i > 0)
               os << ", ";
            os << f.params[i].name << ": "
               << wit_type_name(w, f.params[i].type_idx);
         }
         os << ")";
         if (!f.results.empty())
         {
            if (f.results.size() == 1 && f.results[0].name.empty())
            {
               os << " -> " << wit_type_name(w, f.results[0].type_idx);
            }
            else
            {
               os << " -> (";
               for (std::size_t i = 0; i < f.results.size(); ++i)
               {
                  if (i > 0)
                     os << ", ";
                  if (!f.results[i].name.empty())
                     os << f.results[i].name << ": ";
                  os << wit_type_name(w, f.results[i].type_idx);
               }
               os << ")";
            }
         }
         os << ";\n";
      }

      inline void wit_emit_type(std::ostringstream&  os,
                                const wit_world&     w,
                                const wit_type_def&  td,
                                const std::string&   indent)
      {
         switch (static_cast<wit_type_kind>(td.kind))
         {
            case wit_type_kind::record_:
               os << indent << "record " << td.name << " {\n";
               for (const auto& f : td.fields)
                  os << indent << "  " << f.name << ": "
                     << wit_type_name(w, f.type_idx) << ",\n";
               os << indent << "}\n";
               break;
            case wit_type_kind::variant_:
               os << indent << "variant " << td.name << " {\n";
               for (const auto& f : td.fields)
               {
                  os << indent << "  " << f.name;
                  if (f.type_idx != 0)
                     os << "(" << wit_type_name(w, f.type_idx) << ")";
                  os << ",\n";
               }
               os << indent << "}\n";
               break;
            case wit_type_kind::enum_:
               os << indent << "enum " << td.name << " {\n";
               for (const auto& f : td.fields)
                  os << indent << "  " << f.name << ",\n";
               os << indent << "}\n";
               break;
            case wit_type_kind::flags_:
               os << indent << "flags " << td.name << " {\n";
               for (const auto& f : td.fields)
                  os << indent << "  " << f.name << ",\n";
               os << indent << "}\n";
               break;
            case wit_type_kind::resource_:
               if (td.method_func_idxs.empty())
               {
                  os << indent << "resource " << td.name << ";\n";
               }
               else
               {
                  os << indent << "resource " << td.name << " {\n";
                  for (auto fi : td.method_func_idxs)
                     if (fi < w.funcs.size())
                        wit_emit_func(os, w, w.funcs[fi], indent + "  ");
                  os << indent << "}\n";
               }
               break;
            default:
               break;  // list/option/own/borrow/tuple/result are inline
         }
      }

      inline void wit_emit_interface(std::ostringstream&  os,
                                     const wit_world&     w,
                                     const wit_interface& iface,
                                     const std::string&   indent)
      {
         os << indent << "interface " << iface.name << " {\n";
         auto inner = indent + "  ";
         for (auto ti : iface.type_idxs)
            if (ti < w.types.size() && !w.types[ti].name.empty())
            {
               wit_emit_type(os, w, w.types[ti], inner);
               os << "\n";
            }
         for (auto fi : iface.func_idxs)
            if (fi < w.funcs.size())
               wit_emit_func(os, w, w.funcs[fi], inner);
         os << indent << "}\n";
      }

      // ── Walk a single interface tag into the ctx ────────────────────
      template <typename Tag>
      void register_interface(wit_gen_ctx& ctx, wit_interface& iface)
      {
         using info       = interface_info<Tag>;
         using types_t    = typename info::types;
         using func_types = typename info::func_types;

         iface.name = to_kebab_case(info::name);

         // Resolve each type listed in types(...) and tag it as owned by
         // this interface.  Resolve records/resources up front so they
         // appear in the interface block in declaration order, not
         // discovery order.
         resolve_owned_types<Tag, types_t>(
            ctx, iface, std::make_index_sequence<std::tuple_size_v<types_t>>{});

         // Functions.
         constexpr auto N = std::tuple_size_v<func_types>;
         register_funcs<Tag>(ctx, iface, std::make_index_sequence<N>{});
      }

      template <typename Tag, typename TypesTuple, std::size_t... Is>
      void resolve_owned_types(wit_gen_ctx&    ctx,
                               wit_interface&  iface,
                               std::index_sequence<Is...>)
      {
         (void)iface;
         ((iface.type_idxs.push_back(static_cast<std::uint32_t>(
              ctx.template resolve_type<std::tuple_element_t<Is, TypesTuple>>()))),
          ...);
      }

      template <typename Tag, std::size_t... Is>
      void register_funcs(wit_gen_ctx&    ctx,
                          wit_interface&  iface,
                          std::index_sequence<Is...>)
      {
         using info       = interface_info<Tag>;
         using func_types = typename info::func_types;
         (ctx.template emit_function<std::tuple_element_t<Is, func_types>>(
             info::func_names[Is], info::param_names[Is],
             [&](std::uint32_t fi) { iface.func_idxs.push_back(fi); }),
          ...);
      }

   }  // namespace detail

   // =====================================================================
   // Public API
   // =====================================================================

   /// Build a wit_world for a single exported interface tag.
   ///
   /// The tag is the user-authored interface anchor — the same struct
   /// passed to PSIO_INTERFACE.  Its declared types(...) become record /
   /// resource definitions; its declared funcs(...) become function
   /// signatures.
   ///
   /// @param ns       Package namespace, e.g. "wasi"
   /// @param name     Package name, e.g. "clocks"
   /// @param version  Package version, e.g. "0.2.3" (optional, may be empty)
   /// @param world_name  World identifier; defaults to the tag's kebab-case name.
   template <typename Tag>
   wit_world generate_wit(std::string_view ns,
                          std::string_view name,
                          std::string_view version,
                          std::string_view world_name = {})
   {
      detail::wit_gen_ctx ctx;
      {
         std::string pkg{ns};
         pkg += ':';
         pkg += name;
         if (!version.empty())
         {
            pkg += '@';
            pkg += version;
         }
         ctx.world.package = std::move(pkg);
      }
      ctx.world.name = world_name.empty()
                          ? detail::to_kebab_case(detail::interface_info<Tag>::name)
                          : std::string{world_name};

      wit_interface iface;
      detail::register_interface<Tag>(ctx, iface);
      ctx.world.exports.push_back(std::move(iface));
      return ctx.world;
   }

   /// Convenience overload: pre-formatted "ns:name@version" package id.
   template <typename Tag>
   wit_world generate_wit(const std::string& package,
                          std::string_view   world_name = {})
   {
      detail::wit_gen_ctx ctx;
      ctx.world.package = package;
      ctx.world.name    = world_name.empty()
                             ? detail::to_kebab_case(detail::interface_info<Tag>::name)
                             : std::string{world_name};

      wit_interface iface;
      detail::register_interface<Tag>(ctx, iface);
      ctx.world.exports.push_back(std::move(iface));
      return ctx.world;
   }

   /// Render a wit_world as standards-compliant WIT text.
   inline std::string wit_to_text(const wit_world& w)
   {
      std::ostringstream os;
      if (!w.package.empty())
         os << "package " << w.package << ";\n\n";

      for (const auto& iface : w.exports)
      {
         detail::wit_emit_interface(os, w, iface, "");
         os << "\n";
      }
      for (const auto& iface : w.imports)
      {
         detail::wit_emit_interface(os, w, iface, "");
         os << "\n";
      }

      os << "world " << (w.name.empty() ? "unnamed" : w.name) << " {\n";
      for (const auto& iface : w.imports)
         os << "  import " << iface.name << ";\n";
      for (const auto& iface : w.exports)
         os << "  export " << iface.name << ";\n";
      os << "}\n";
      return os.str();
   }

   /// Render WIT text directly from an interface tag.
   template <typename Tag>
   std::string generate_wit_text(std::string_view ns,
                                 std::string_view name,
                                 std::string_view version,
                                 std::string_view world_name = {})
   {
      return wit_to_text(generate_wit<Tag>(ns, name, version, world_name));
   }

   template <typename Tag>
   std::string generate_wit_text(const std::string& package,
                                 std::string_view   world_name = {})
   {
      return wit_to_text(generate_wit<Tag>(package, world_name));
   }

}  // namespace psio
