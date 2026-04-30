#pragma once
//
// psio/schema.hpp — runtime schema value (Phase 14a).
//
// The schema value is the type-erased description of a reflected type.
// Given a `psio::schema` and some bytes-plus-format-tag, dynamic codec
// operations (`validate_dynamic`, `encode_dynamic`, `decode_dynamic`,
// `transcode`) can walk the value without a compile-time T — enabling
// wire-format-agnostic validation, pretty-printing, and format-to-format
// conversion. All of those consumer operations build on this single
// representation.
//
// Phase 14a scope — the schema value itself and the static bridge:
//
//   psio::primitive_kind  — enum of primitive wire types
//   psio::field_descriptor — one field of a record
//   psio::record_descriptor — ordered field list + name
//   psio::schema          — std::variant of primitive_kind and
//                             record_descriptor (sequence / optional are
//                             captured as record_descriptor compositions
//                             for now; a dedicated sequence/optional
//                             kind lands in 14b)
//   psio::schema_of<T>()  — consteval bridge — turns a reflected T
//                             into a constexpr schema
//
// Design spec: `.issues/psio-v2-design.md` § 5.2.6.

#include <psio/adapter.hpp>
#include <psio/reflect.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace psio {

   // ── Kinds ──────────────────────────────────────────────────────────────
   enum class primitive_kind : std::uint8_t
   {
      Bool,
      Int8,
      Uint8,
      Int16,
      Uint16,
      Int32,
      Uint32,
      Int64,
      Uint64,
      Float32,
      Float64,
      String,
      Bytes,  // Opaque byte sequence — wire shape of binary adapters.
   };

   // Presentation category carried in projected schema nodes, mirroring
   // the binary/text dichotomy in adapter.hpp.
   enum class presentation_category : std::uint8_t
   {
      Binary,
      Text,
   };

   // Forward declare so schema can own nested records by value.
   struct schema;

   // ── Field descriptor ────────────────────────────────────────────────────
   struct field_descriptor
   {
      std::string_view      name;
      std::uint32_t         field_number{};
      std::shared_ptr<schema> type;  // shared so schemas can be copied cheaply
   };

   // ── Record descriptor ───────────────────────────────────────────────────
   struct record_descriptor
   {
      std::string_view              name;
      std::vector<field_descriptor> fields;
   };

   // ── Sequence descriptor (variable-length list of a single element type) ─
   struct sequence_descriptor
   {
      std::shared_ptr<schema> element;
      // std::nullopt for unbounded vector; fixed for std::array<T,N>.
      std::optional<std::uint32_t> fixed_count;
   };

   // ── Optional descriptor ─────────────────────────────────────────────────
   struct optional_descriptor
   {
      std::shared_ptr<schema> value_type;
   };

   // ── Projected descriptor ────────────────────────────────────────────────
   //
   // A type whose wire form is produced by a adapter (see
   // adapter.hpp / design § 5.3.7). The schema carries the logical
   // type name, the presentation category, and the schema of the
   // *presentation shape* — the wire form as the format sees it, e.g.
   // `primitive_kind::String` for a text adapter, `primitive_kind::Bytes`
   // for a binary adapter. Dynamic codecs treat this as "it's whatever
   // the shape is, plus a label" — they serialize the shape but carry
   // the logical/presentation identity so transcoders can preserve it.
   struct adapted_descriptor
   {
      std::string_view                 logical_name;
      presentation_category            category;
      std::shared_ptr<schema>          presentation_shape;
   };

   // ── The schema value itself ─────────────────────────────────────────────
   struct schema
   {
      std::variant<primitive_kind,
                   record_descriptor,
                   sequence_descriptor,
                   optional_descriptor,
                   adapted_descriptor>
         v;

      // Convenience accessors.
      bool is_primitive() const noexcept
      {
         return std::holds_alternative<primitive_kind>(v);
      }
      bool is_record() const noexcept
      {
         return std::holds_alternative<record_descriptor>(v);
      }
      bool is_sequence() const noexcept
      {
         return std::holds_alternative<sequence_descriptor>(v);
      }
      bool is_optional() const noexcept
      {
         return std::holds_alternative<optional_descriptor>(v);
      }
      bool is_projected() const noexcept
      {
         return std::holds_alternative<adapted_descriptor>(v);
      }

      primitive_kind          as_primitive() const { return std::get<primitive_kind>(v); }
      const record_descriptor& as_record() const { return std::get<record_descriptor>(v); }
      const sequence_descriptor& as_sequence() const
      {
         return std::get<sequence_descriptor>(v);
      }
      const optional_descriptor& as_optional() const
      {
         return std::get<optional_descriptor>(v);
      }
      const adapted_descriptor& as_projected() const
      {
         return std::get<adapted_descriptor>(v);
      }
   };

   // ── schema_of<T>() bridge ──────────────────────────────────────────────

   namespace detail::schema_impl {

      template <typename T>
      struct is_std_array : std::false_type
      {
      };
      template <typename T, std::size_t N>
      struct is_std_array<std::array<T, N>> : std::true_type
      {
      };

      template <typename T>
      struct is_std_vector : std::false_type
      {
      };
      template <typename T, typename A>
      struct is_std_vector<std::vector<T, A>> : std::true_type
      {
      };

      template <typename T>
      struct is_std_optional : std::false_type
      {
      };
      template <typename T>
      struct is_std_optional<std::optional<T>> : std::true_type
      {
      };

      template <typename T>
      std::shared_ptr<schema> schema_ptr_of();

      template <typename T>
      schema schema_of()
      {
         // Adapter check first — a projected type hides its
         // internal layout behind a presentation shape. Binary
         // adapters surface as Bytes; text adapters as String.
         if constexpr (::psio::has_adapter_v<T,
                                                 ::psio::binary_category>)
         {
            adapted_descriptor pd;
            if constexpr (::psio::Reflected<T>)
               pd.logical_name = ::psio::reflect<T>::name;
            else
               pd.logical_name = {};
            pd.category = presentation_category::Binary;
            pd.presentation_shape =
               std::make_shared<schema>(schema{primitive_kind::Bytes});
            return {std::move(pd)};
         }
         else if constexpr (::psio::has_adapter_v<
                               T, ::psio::text_category>)
         {
            adapted_descriptor pd;
            if constexpr (::psio::Reflected<T>)
               pd.logical_name = ::psio::reflect<T>::name;
            else
               pd.logical_name = {};
            pd.category = presentation_category::Text;
            pd.presentation_shape =
               std::make_shared<schema>(schema{primitive_kind::String});
            return {std::move(pd)};
         }
         else if constexpr (std::is_same_v<T, bool>)
            return {primitive_kind::Bool};
         else if constexpr (std::is_same_v<T, std::int8_t>)
            return {primitive_kind::Int8};
         else if constexpr (std::is_same_v<T, std::uint8_t>)
            return {primitive_kind::Uint8};
         else if constexpr (std::is_same_v<T, std::int16_t>)
            return {primitive_kind::Int16};
         else if constexpr (std::is_same_v<T, std::uint16_t>)
            return {primitive_kind::Uint16};
         else if constexpr (std::is_same_v<T, std::int32_t>)
            return {primitive_kind::Int32};
         else if constexpr (std::is_same_v<T, std::uint32_t>)
            return {primitive_kind::Uint32};
         else if constexpr (std::is_same_v<T, std::int64_t>)
            return {primitive_kind::Int64};
         else if constexpr (std::is_same_v<T, std::uint64_t>)
            return {primitive_kind::Uint64};
         else if constexpr (std::is_same_v<T, float>)
            return {primitive_kind::Float32};
         else if constexpr (std::is_same_v<T, double>)
            return {primitive_kind::Float64};
         else if constexpr (std::is_same_v<T, std::string>)
            return {primitive_kind::String};
         else if constexpr (is_std_array<T>::value)
         {
            using E = typename T::value_type;
            return {sequence_descriptor{
               schema_ptr_of<E>(),
               static_cast<std::uint32_t>(std::tuple_size<T>::value)}};
         }
         else if constexpr (is_std_vector<T>::value)
         {
            using E = typename T::value_type;
            return {sequence_descriptor{schema_ptr_of<E>(), std::nullopt}};
         }
         else if constexpr (is_std_optional<T>::value)
         {
            using V = typename T::value_type;
            return {optional_descriptor{schema_ptr_of<V>()}};
         }
         else if constexpr (::psio::Reflected<T>)
         {
            using R = ::psio::reflect<T>;
            record_descriptor r{R::name, {}};
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (
                  ([&]
                   {
                      using F = typename R::template member_type<Is>;
                      r.fields.push_back(field_descriptor{
                         R::template member_name<Is>,
                         R::template field_number<Is>,
                         schema_ptr_of<F>()});
                   }()),
                  ...);
            }(std::make_index_sequence<R::member_count>{});
            return {std::move(r)};
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio::schema_of: unsupported type");
         }
      }

      template <typename T>
      std::shared_ptr<schema> schema_ptr_of()
      {
         return std::make_shared<schema>(schema_of<T>());
      }

   }  // namespace detail::schema_impl

   template <typename T>
   schema schema_of()
   {
      return detail::schema_impl::schema_of<T>();
   }

}  // namespace psio
