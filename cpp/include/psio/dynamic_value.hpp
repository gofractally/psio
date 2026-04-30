#pragma once
//
// psio/dynamic_value.hpp — type-erased value representation.
//
// A `dynamic_value` is the runtime counterpart of any value describable
// by a `psio::schema`. The pair (schema, dynamic_value) is the
// type-erased representation that the dynamic codec CPOs (validate_dynamic,
// encode_dynamic, decode_dynamic) and transcode operate on.
//
// Phase 14b scope — the value variant + JSON pass-through:
//
//   psio::dynamic_value         — variant over primitives + record +
//                                   sequence + optional
//   psio::to_dynamic(schema, v) — static value → dynamic_value (uses
//                                   reflection when v is a reflected T)
//   psio::from_dynamic<T>(dv)   — dynamic_value → static T (checked)
//
// Dynamic JSON codec (see dynamic_json.hpp):
//   psio::json_decode_dynamic(schema, bytes) → dynamic_value
//   psio::json_encode_dynamic(schema, dv, sink) → std::string
//
// Cross-format transcode (schema-walking) is the natural next layer and
// stays as a follow-up in 14c. With just JSON↔dynamic in place, users can
// already combine `static→JSON→dynamic` and `dynamic→JSON→static` as a
// canonical pivot, which is the main use case the design calls out.

#include <psio/adapter.hpp>
#include <psio/reflect.hpp>
#include <psio/schema.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace psio {

   struct dynamic_value;

   // Ordered key→value pairs for a dynamic record (preserves field order
   // from the schema, unlike std::unordered_map).
   struct dynamic_record
   {
      std::vector<std::pair<std::string, dynamic_value>> fields;
   };

   struct dynamic_sequence
   {
      std::vector<dynamic_value> elements;
   };

   struct dynamic_optional
   {
      std::unique_ptr<dynamic_value> value;  // nullptr = none
   };

   struct dynamic_value
   {
      using variant_t = std::variant<
         // primitives
         bool,
         std::int8_t, std::uint8_t,
         std::int16_t, std::uint16_t,
         std::int32_t, std::uint32_t,
         std::int64_t, std::uint64_t,
         float, double,
         std::string,
         // composites
         dynamic_record,
         dynamic_sequence,
         dynamic_optional>;

      variant_t v;

      dynamic_value() = default;
      template <typename T>
      dynamic_value(T&& x) : v(std::forward<T>(x))
      {
      }

      template <typename T>
      bool holds() const noexcept
      {
         return std::holds_alternative<T>(v);
      }
      template <typename T>
      const T& as() const
      {
         return std::get<T>(v);
      }
      template <typename T>
      T& as()
      {
         return std::get<T>(v);
      }
   };

   // ── to_dynamic — static T value → dynamic_value ─────────────────────────
   //
   // The schema argument is currently informational; the static walk is
   // driven by T's type. A follow-up will verify schema agreement.

   namespace detail::dyn_impl {

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
      dynamic_value to_dynamic_impl(const T& x)
      {
         // Adapter check first — the dynamic form of a projected
         // type is the adapter's payload bytes held as a string
         // (the dynamic codecs' schema node knows the payload is
         // opaque and routes accordingly).
         if constexpr (::psio::has_adapter_v<T,
                                                 ::psio::binary_category>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                             ::psio::binary_category>;
            std::vector<char> bytes;
            Proj::encode(x, bytes);
            return dynamic_value{std::string(bytes.data(), bytes.size())};
         }
         else if constexpr (::psio::has_adapter_v<
                               T, ::psio::text_category>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                             ::psio::text_category>;
            std::string text;
            Proj::encode(x, text);
            return dynamic_value{std::move(text)};
         }
         else if constexpr (std::is_same_v<T, bool> ||
                       std::is_same_v<T, std::int8_t> ||
                       std::is_same_v<T, std::uint8_t> ||
                       std::is_same_v<T, std::int16_t> ||
                       std::is_same_v<T, std::uint16_t> ||
                       std::is_same_v<T, std::int32_t> ||
                       std::is_same_v<T, std::uint32_t> ||
                       std::is_same_v<T, std::int64_t> ||
                       std::is_same_v<T, std::uint64_t> ||
                       std::is_same_v<T, float> ||
                       std::is_same_v<T, double>)
            return dynamic_value{x};
         else if constexpr (std::is_same_v<T, std::string>)
            return dynamic_value{x};
         else if constexpr (is_std_array<T>::value ||
                            is_std_vector<T>::value)
         {
            dynamic_sequence seq;
            for (const auto& el : x)
               seq.elements.push_back(to_dynamic_impl(el));
            return dynamic_value{std::move(seq)};
         }
         else if constexpr (is_std_optional<T>::value)
         {
            dynamic_optional o;
            if (x.has_value())
               o.value = std::make_unique<dynamic_value>(to_dynamic_impl(*x));
            return dynamic_value{std::move(o)};
         }
         else if constexpr (::psio::Reflected<T>)
         {
            using R = ::psio::reflect<T>;
            dynamic_record rec;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (
                  rec.fields.push_back(
                     {std::string(R::template member_name<Is>),
                      to_dynamic_impl(x.*(R::template member_pointer<Is>))}),
                  ...);
            }(std::make_index_sequence<R::member_count>{});
            return dynamic_value{std::move(rec)};
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio::to_dynamic: unsupported type");
         }
      }

      template <typename T>
      T from_dynamic_impl(const dynamic_value& dv);

      template <typename T>
      T from_dynamic_impl(const dynamic_value& dv)
      {
         if constexpr (::psio::has_adapter_v<T,
                                                 ::psio::binary_category>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                             ::psio::binary_category>;
            const auto& s = dv.as<std::string>();
            return Proj::decode(std::span<const char>(s.data(), s.size()));
         }
         else if constexpr (::psio::has_adapter_v<
                               T, ::psio::text_category>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                             ::psio::text_category>;
            const auto& s = dv.as<std::string>();
            return Proj::decode(std::span<const char>(s.data(), s.size()));
         }
         else if constexpr (std::is_same_v<T, bool> ||
                       std::is_same_v<T, std::int8_t> ||
                       std::is_same_v<T, std::uint8_t> ||
                       std::is_same_v<T, std::int16_t> ||
                       std::is_same_v<T, std::uint16_t> ||
                       std::is_same_v<T, std::int32_t> ||
                       std::is_same_v<T, std::uint32_t> ||
                       std::is_same_v<T, std::int64_t> ||
                       std::is_same_v<T, std::uint64_t> ||
                       std::is_same_v<T, float> ||
                       std::is_same_v<T, double> ||
                       std::is_same_v<T, std::string>)
            return dv.as<T>();
         else if constexpr (is_std_array<T>::value)
         {
            T          out{};
            const auto& seq = dv.as<dynamic_sequence>();
            for (std::size_t i = 0;
                 i < std::tuple_size<T>::value && i < seq.elements.size(); ++i)
               out[i] = from_dynamic_impl<typename T::value_type>(
                  seq.elements[i]);
            return out;
         }
         else if constexpr (is_std_vector<T>::value)
         {
            using E = typename T::value_type;
            T out;
            const auto& seq = dv.as<dynamic_sequence>();
            out.reserve(seq.elements.size());
            for (const auto& el : seq.elements)
               out.push_back(from_dynamic_impl<E>(el));
            return out;
         }
         else if constexpr (is_std_optional<T>::value)
         {
            using V = typename T::value_type;
            const auto& opt = dv.as<dynamic_optional>();
            if (!opt.value)
               return std::optional<V>{};
            return std::optional<V>{from_dynamic_impl<V>(*opt.value)};
         }
         else if constexpr (::psio::Reflected<T>)
         {
            using R        = ::psio::reflect<T>;
            T              out{};
            const auto&    rec = dv.as<dynamic_record>();
            std::map<std::string, const dynamic_value*> by_name;
            for (const auto& kv : rec.fields)
               by_name[kv.first] = &kv.second;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (
                  ([&]
                   {
                      auto key = std::string(R::template member_name<Is>);
                      auto it  = by_name.find(key);
                      if (it != by_name.end())
                         out.*(R::template member_pointer<Is>) =
                            from_dynamic_impl<
                               typename R::template member_type<Is>>(
                               *it->second);
                   }()),
                  ...);
            }(std::make_index_sequence<R::member_count>{});
            return out;
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio::from_dynamic: unsupported type");
         }
      }

   }  // namespace detail::dyn_impl

   template <typename T>
   dynamic_value to_dynamic(const T& x)
   {
      return detail::dyn_impl::to_dynamic_impl(x);
   }

   template <typename T>
   T from_dynamic(const dynamic_value& dv)
   {
      return detail::dyn_impl::from_dynamic_impl<T>(dv);
   }

}  // namespace psio
