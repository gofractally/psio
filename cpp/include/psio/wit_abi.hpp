#pragma once

// Compile-time WIT ABI wire format — layout, serialize, validate, rebase.
//
// All driven by PSIO_REFLECT — no WASM runtime dependency.
// For flat calling-convention dispatch (wit_abi_lower_flat / wit_abi_lift_flat),
// see psizam/canonical_dispatch.hpp.
//
// Layout:
//   wit_abi_size_v<T>             — record byte size
//   wit_abi_align_v<T>            — record alignment
//   wit_abi_field_offset_v<T, N>  — byte offset of field N
//   wit_abi_flat_count_v<T>       — number of flat ABI slots
//
// Memory serialization:
//   wit_abi_lower_fields(value, policy, dest) — store to memory
//   wit_abi_lift_fields<T>(policy, base)      — read from memory
//
// Integrity:
//   wit_abi_validate<T>(buf, size, offset) — bounds/alignment check
//   wit_abi_rebase<T>(buf, offset, delta)  — relocate pointers
//
// Policies:
//   StorePolicy concept — alloc + store_*  (used by wit_abi_lower_fields)
//   LoadPolicy  concept — load_*           (used by wit_abi_lift_fields)
//   buffer_store_policy  — bump allocator into vector<uint8_t>
//   buffer_load_policy   — read from span of bytes

#include <psio/detail/variant_util.hpp>
#include <psio/reflect.hpp>
#include <psio/wit_resource.hpp>

#include <algorithm>
#include <type_traits>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace psio {

   // ---- Reflect-API adapters (typelist view over psio's index-based reflect) ----
   //
   // wit_abi was originally written for psio1's typelist-based reflect
   // (data_members + apply_members). psio (formerly psio) uses an
   // index-based shape (member_pointer<I>, member_count). These adapters
   // synthesize the typelist API on top of the index-based one so the
   // ABI machinery stays unchanged.

   template <auto... Ms>
   struct MemberList {};

   template <auto... Ms, typename F>
   constexpr decltype(auto) apply_members(MemberList<Ms...>*, F&& f)
   {
      return f(Ms...);
   }

   namespace detail {
      template <typename T, std::size_t... Is>
      constexpr auto data_members_impl(std::index_sequence<Is...>) noexcept
      {
         return static_cast<MemberList<reflect<T>::template member_pointer<Is>...>*>(nullptr);
      }
      template <typename T>
      using data_members_t =
         std::remove_pointer_t<decltype(data_members_impl<T>(
            std::make_index_sequence<reflect<T>::member_count>{}))>;
   }

   // Member-pointer trait: extracts the value type and class type of a
   // pointer-to-member. Specializations cover data, method (mutable),
   // and method (const). Function-pointer-to-member-of-class form was
   // a psio1 oddity not used by wit_abi proper; left out.
   template <typename M>
   struct MemberPtrType;

   template <typename V, typename T>
   struct MemberPtrType<V T::*>
   {
      using ValueType = V;
      using ClassType = T;
   };

   template <typename R, typename T, typename... Args>
   struct MemberPtrType<R (T::*)(Args...)>
   {
      using ValueType = R;
      using ClassType = T;
   };

   template <typename R, typename T, typename... Args>
   struct MemberPtrType<R (T::*)(Args...) const>
   {
      using ValueType = R;
      using ClassType = T;
   };

   namespace detail {
      template <typename T> struct is_psio_own : std::false_type {};
      template <typename T> struct is_psio_own<psio::own<T>> : std::true_type {};
      template <typename T> struct is_psio_borrow : std::false_type {};
      template <typename T> struct is_psio_borrow<psio::borrow<T>> : std::true_type {};

      // Type-shape traits the canonical-ABI walker needs. Mirrors the
      // helpers psio1's wview.hpp / wit_resource.hpp / reflect.hpp
      // provided. They're entirely standalone — no psio reflect
      // dependency — so they're trivially portable.

      template <typename T> struct is_std_string_ct : std::false_type {};
      template <> struct is_std_string_ct<std::string> : std::true_type {};

      template <typename T> struct is_std_vector_ct : std::false_type {};
      template <typename U> struct is_std_vector_ct<std::vector<U>> : std::true_type {};

      template <typename T> struct vector_elem_ct;
      template <typename U> struct vector_elem_ct<std::vector<U>> { using type = U; };

      template <typename T>
      inline constexpr bool is_scalar_v = std::is_arithmetic_v<T> || std::is_enum_v<T>;

      template <typename T> struct is_std_optional_ct : std::false_type {};
      template <typename U> struct is_std_optional_ct<std::optional<U>> : std::true_type {};

      template <typename T> struct optional_elem_ct;
      template <typename U> struct optional_elem_ct<std::optional<U>> { using type = U; };

      template <typename T> struct is_std_expected_ct : std::false_type {};
      template <typename V, typename E> struct is_std_expected_ct<std::expected<V, E>> : std::true_type {};

      template <typename T> struct expected_value_ct;
      template <typename V, typename E> struct expected_value_ct<std::expected<V, E>> { using type = V; };

      template <typename T> struct expected_error_ct;
      template <typename V, typename E> struct expected_error_ct<std::expected<V, E>> { using type = E; };

      template <typename T> struct is_std_tuple : std::false_type {};
      template <typename... Ts> struct is_std_tuple<std::tuple<Ts...>> : std::true_type {};

      // is_std_variant is shared with the format codecs — defined in
      // psio/detail/variant_util.hpp (included transitively by every
      // binary format).  Re-declaring it here would collide if both
      // headers end up in the same TU.
      template <typename T> inline constexpr bool is_std_variant_v = is_std_variant<T>::value;

      // is_own_ct / is_borrow_ct come transitively from psio/wit_resource.hpp.
   }

   // ---- Limits ----

   static constexpr size_t MAX_FLAT_PARAMS  = 16;
   static constexpr size_t MAX_FLAT_RESULTS = 1;

   // ---- Native value (untyped 64-bit slot) ----

   union native_value {
      uint32_t i32;
      uint64_t i64;
      float    f32;
      double   f64;
   };

   // =========================================================================
   // Compile-time WIT ABI layout from C++ types (PSIO1_REFLECT-driven)
   // =========================================================================

   namespace detail_wit_abi {

      using namespace psio::detail;

      // ── wit_abi_align<T>() ──────────────────────────────────────────────

      template <typename T>
      constexpr uint32_t wit_abi_align_impl() {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_void_v<U>)
            return 1;
         else if constexpr (std::is_same_v<U, bool> || std::is_same_v<U, uint8_t> || std::is_same_v<U, int8_t>)
            return 1;
         else if constexpr (std::is_same_v<U, uint16_t> || std::is_same_v<U, int16_t>)
            return 2;
         else if constexpr (std::is_same_v<U, uint32_t> || std::is_same_v<U, int32_t> || std::is_same_v<U, float>)
            return 4;
         else if constexpr (std::is_same_v<U, uint64_t> || std::is_same_v<U, int64_t> || std::is_same_v<U, double>)
            return 8;
         else if constexpr (std::is_enum_v<U>)
            return wit_abi_align_impl<std::underlying_type_t<U>>();
         else if constexpr (is_own_ct<U>::value || is_borrow_ct<U>::value)
            return 4;
         else if constexpr (is_std_string_ct<U>::value)
            return 4;  // (i32 ptr, i32 len)
         else if constexpr (is_std_vector_ct<U>::value)
            return 4;  // (i32 ptr, i32 len)
         else if constexpr (is_psio_own<U>::value || is_psio_borrow<U>::value)
            return 4;
         else if constexpr (is_std_tuple<U>::value) {
            return []<size_t... Is>(std::index_sequence<Is...>) {
               uint32_t m = 1;
               ((m = std::max(m, wit_abi_align_impl<std::tuple_element_t<Is, U>>())), ...);
               return m;
            }(std::make_index_sequence<std::tuple_size_v<U>>{});
         }
         else if constexpr (std::is_same_v<U, std::monostate>)
            return 1;
         else if constexpr (is_std_optional_ct<U>::value) {
            using E = typename optional_elem_ct<U>::type;
            constexpr uint32_t ea = wit_abi_align_impl<E>();
            return ea > 1 ? ea : 1;  // max(1, payload_align)
         }
         else if constexpr (is_std_expected_ct<U>::value) {
            using V = typename expected_value_ct<U>::type;
            using Err = typename expected_error_ct<U>::type;
            constexpr uint32_t ea = wit_abi_align_impl<Err>();
            if constexpr (std::is_void_v<V>)
               return ea > 1 ? ea : 1;
            else {
               constexpr uint32_t va = wit_abi_align_impl<V>();
               return std::max({uint32_t{1}, va, ea});
            }
         }
         else if constexpr (is_std_variant_v<U>) {
            return [&]<size_t... Is>(std::index_sequence<Is...>) {
               constexpr size_t N = sizeof...(Is);
               constexpr uint32_t disc_align = N <= 256 ? 1 : N <= 65536 ? 2 : 4;
               uint32_t max_a = disc_align;
               ((max_a = std::max(max_a, wit_abi_align_impl<std::variant_alternative_t<Is, U>>())), ...);
               return max_a;
            }(std::make_index_sequence<std::variant_size_v<U>>{});
         }
         else if constexpr (std::is_array_v<U>)
            return wit_abi_align_impl<std::remove_extent_t<U>>();
         else if constexpr (Reflected<U>) {
            uint32_t max_align = 1;
            apply_members(
               (detail::data_members_t<U>*)nullptr,
               [&](auto... ptrs) {
                  ((void)(max_align = std::max(max_align,
                     wit_abi_align_impl<
                        std::remove_cvref_t<typename MemberPtrType<decltype(ptrs)>::ValueType>
                     >())), ...);
               }
            );
            return max_align;
         }
         else {
            static_assert(sizeof(U) == 0, "wit_abi_align: unsupported type");
            return 0;
         }
      }

      // ── wit_abi_size<T>() ───────────────────────────────────────────────

      template <typename T>
      constexpr uint32_t wit_abi_size_impl() {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_void_v<U>)
            return 0;
         else if constexpr (std::is_same_v<U, bool> || std::is_same_v<U, uint8_t> || std::is_same_v<U, int8_t>)
            return 1;
         else if constexpr (std::is_same_v<U, uint16_t> || std::is_same_v<U, int16_t>)
            return 2;
         else if constexpr (std::is_same_v<U, uint32_t> || std::is_same_v<U, int32_t> || std::is_same_v<U, float>)
            return 4;
         else if constexpr (std::is_same_v<U, uint64_t> || std::is_same_v<U, int64_t> || std::is_same_v<U, double>)
            return 8;
         else if constexpr (std::is_enum_v<U>)
            return wit_abi_size_impl<std::underlying_type_t<U>>();
         else if constexpr (is_own_ct<U>::value || is_borrow_ct<U>::value)
            return 4;
         else if constexpr (is_std_string_ct<U>::value)
            return 8;  // (i32 ptr, i32 len)
         else if constexpr (is_std_vector_ct<U>::value)
            return 8;  // (i32 ptr, i32 len)
         else if constexpr (is_psio_own<U>::value || is_psio_borrow<U>::value)
            return 4;
         else if constexpr (is_std_tuple<U>::value) {
            return []<size_t... Is>(std::index_sequence<Is...>) {
               uint32_t offset = 0;
               auto add_field = [&]<size_t I>() {
                  constexpr uint32_t fa = wit_abi_align_impl<std::tuple_element_t<I, U>>();
                  constexpr uint32_t fs = wit_abi_size_impl<std::tuple_element_t<I, U>>();
                  offset = (offset + fa - 1) & ~(fa - 1);
                  offset += fs;
               };
               (add_field.template operator()<Is>(), ...);
               constexpr uint32_t a = wit_abi_align_impl<U>();
               return (offset + a - 1) & ~(a - 1);
            }(std::make_index_sequence<std::tuple_size_v<U>>{});
         }
         else if constexpr (std::is_same_v<U, std::monostate>)
            return 0;
         else if constexpr (is_std_optional_ct<U>::value) {
            using E = typename optional_elem_ct<U>::type;
            constexpr uint32_t ea = wit_abi_align_impl<E>();
            constexpr uint32_t es = wit_abi_size_impl<E>();
            constexpr uint32_t disc_padded = (1 + ea - 1) & ~(ea - 1);
            constexpr uint32_t total = disc_padded + es;
            constexpr uint32_t a = ea > 1 ? ea : 1;
            return (total + a - 1) & ~(a - 1);
         }
         else if constexpr (is_std_expected_ct<U>::value) {
            using V = typename expected_value_ct<U>::type;
            using Err = typename expected_error_ct<U>::type;
            constexpr uint32_t era = wit_abi_align_impl<Err>();
            constexpr uint32_t ers = wit_abi_size_impl<Err>();
            if constexpr (std::is_void_v<V>) {
               constexpr uint32_t a = era > 1 ? era : 1;
               constexpr uint32_t dp = (1 + a - 1) & ~(a - 1);
               return (dp + ers + a - 1) & ~(a - 1);
            } else {
               constexpr uint32_t va = wit_abi_align_impl<V>();
               constexpr uint32_t vs = wit_abi_size_impl<V>();
               constexpr uint32_t a = std::max({uint32_t{1}, va, era});
               constexpr uint32_t dp = (1 + a - 1) & ~(a - 1);
               constexpr uint32_t ps = std::max(vs, ers);
               return (dp + ps + a - 1) & ~(a - 1);
            }
         }
         else if constexpr (is_std_variant_v<U>) {
            return []<size_t... Is>(std::index_sequence<Is...>) {
               constexpr size_t N = sizeof...(Is);
               constexpr uint32_t disc_size = N <= 256 ? 1 : N <= 65536 ? 2 : 4;
               constexpr uint32_t disc_align = disc_size;
               uint32_t max_pa = 1;
               ((max_pa = std::max(max_pa, wit_abi_align_impl<std::variant_alternative_t<Is, U>>())), ...);
               uint32_t max_ps = 0;
               ((max_ps = std::max(max_ps, wit_abi_size_impl<std::variant_alternative_t<Is, U>>())), ...);
               uint32_t va = std::max(disc_align, max_pa);
               uint32_t payload_off = (disc_size + max_pa - 1) & ~(max_pa - 1);
               return (payload_off + max_ps + va - 1) & ~(va - 1);
            }(std::make_index_sequence<std::variant_size_v<U>>{});
         }
         else if constexpr (std::is_array_v<U>) {
            using E = std::remove_extent_t<U>;
            constexpr uint32_t n = std::extent_v<U>;
            return n * wit_abi_size_impl<E>();
         }
         else if constexpr (Reflected<U>) {
            uint32_t size = 0;
            uint32_t max_align = 1;
            apply_members(
               (detail::data_members_t<U>*)nullptr,
               [&](auto... ptrs) {
                  auto process = [&](auto ptr) {
                     using FT = std::remove_cvref_t<typename MemberPtrType<decltype(ptr)>::ValueType>;
                     constexpr uint32_t fa = wit_abi_align_impl<FT>();
                     constexpr uint32_t fs = wit_abi_size_impl<FT>();
                     max_align = std::max(max_align, fa);
                     size = ((size + fa - 1) & ~(fa - 1)) + fs;
                  };
                  (process(ptrs), ...);
               }
            );
            return (size + max_align - 1) & ~(max_align - 1);  // trailing pad
         }
         else {
            static_assert(sizeof(U) == 0, "wit_abi_size: unsupported type");
            return 0;
         }
      }

      // ── wit_abi_field_offset<T, I>() ────────────────────────────────────

      template <typename T>
      constexpr auto wit_abi_field_offsets_impl() {
         struct result_t { uint32_t offsets[64] = {}; size_t count = 0; };
         result_t r;
         uint32_t offset = 0;
         apply_members(
            (detail::data_members_t<T>*)nullptr,
            [&](auto... ptrs) {
               auto process = [&](auto ptr) {
                  using FT = std::remove_cvref_t<typename MemberPtrType<decltype(ptr)>::ValueType>;
                  constexpr uint32_t fa = wit_abi_align_impl<FT>();
                  constexpr uint32_t fs = wit_abi_size_impl<FT>();
                  offset = (offset + fa - 1) & ~(fa - 1);
                  r.offsets[r.count++] = offset;
                  offset += fs;
               };
               (process(ptrs), ...);
            }
         );
         return r;
      }

      // ── wit_abi_flat_count<T>() ─────────────────────────────────────────

      template <typename T>
      constexpr size_t wit_abi_flat_count_impl() {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_void_v<U>)
            return 0;
         else if constexpr (is_scalar_v<U>)
            return 1;
         else if constexpr (is_own_ct<U>::value || is_borrow_ct<U>::value)
            return 1;
         else if constexpr (is_std_string_ct<U>::value)
            return 2;  // ptr + len
         else if constexpr (is_std_vector_ct<U>::value)
            return 2;  // ptr + len
         else if constexpr (is_psio_own<U>::value || is_psio_borrow<U>::value)
            return 1;
         else if constexpr (is_std_tuple<U>::value) {
            return []<size_t... Is>(std::index_sequence<Is...>) {
               return (wit_abi_flat_count_impl<std::tuple_element_t<Is, U>>() + ...);
            }(std::make_index_sequence<std::tuple_size_v<U>>{});
         }
         else if constexpr (std::is_same_v<U, std::monostate>)
            return 0;
         else if constexpr (is_std_optional_ct<U>::value) {
            using E = typename optional_elem_ct<U>::type;
            return 1 + wit_abi_flat_count_impl<E>();
         }
         else if constexpr (is_std_expected_ct<U>::value) {
            using V = typename expected_value_ct<U>::type;
            using Err = typename expected_error_ct<U>::type;
            constexpr size_t vc = std::is_void_v<V> ? 0 : wit_abi_flat_count_impl<V>();
            return 1 + std::max(vc, wit_abi_flat_count_impl<Err>());
         }
         else if constexpr (is_std_variant_v<U>) {
            return []<size_t... Is>(std::index_sequence<Is...>) {
               size_t max_count = 0;
               ((max_count = std::max(max_count,
                  wit_abi_flat_count_impl<std::variant_alternative_t<Is, U>>())), ...);
               return 1 + max_count;  // discriminant + max payload
            }(std::make_index_sequence<std::variant_size_v<U>>{});
         }
         else if constexpr (std::is_array_v<U>) {
            using E = std::remove_extent_t<U>;
            return std::extent_v<U> * wit_abi_flat_count_impl<E>();
         }
         else if constexpr (Reflected<U>) {
            size_t n = 0;
            apply_members(
               (detail::data_members_t<U>*)nullptr,
               [&](auto... ptrs) {
                  ((void)(n += wit_abi_flat_count_impl<
                     std::remove_cvref_t<typename MemberPtrType<decltype(ptrs)>::ValueType>
                  >()), ...);
               }
            );
            return n;
         }
         else {
            static_assert(sizeof(U) == 0, "wit_abi_flat_count: unsupported type");
            return 0;
         }
      }

   } // namespace detail_wit_abi

   // ── Public compile-time layout API ────────────────────────────────────────

   template <typename T>
   inline constexpr uint32_t wit_abi_align_v = detail_wit_abi::wit_abi_align_impl<T>();

   template <typename T>
   inline constexpr uint32_t wit_abi_size_v = detail_wit_abi::wit_abi_size_impl<T>();

   template <typename T>
   inline constexpr size_t wit_abi_flat_count_v = detail_wit_abi::wit_abi_flat_count_impl<T>();

   template <typename T, size_t I>
   inline constexpr uint32_t wit_abi_field_offset_v =
      detail_wit_abi::wit_abi_field_offsets_impl<T>().offsets[I];

   // =========================================================================
   // wit_abi_packed_size — compute total buffer size for a value
   // =========================================================================
   //
   // Walks the value tree and sums: record headers + all variable-length
   // data (strings, vectors, nested records).  Used by two-pass pack to
   // allocate the output buffer exactly once.

   namespace detail_wit_abi {

      template <typename T>
      uint32_t packed_size_field(const T& value, uint32_t& bump) {
         using U = std::remove_cvref_t<T>;
         if constexpr (is_scalar_v<U>) {
            return 0;  // scalars are inline, no extra allocation
         }
         else if constexpr (is_own_ct<U>::value || is_borrow_ct<U>::value) {
            return 0;
         }
         else if constexpr (is_std_string_ct<U>::value) {
            bump = (bump + 0) & ~0u;  // align 1
            uint32_t ptr = bump;
            bump += static_cast<uint32_t>(value.size());
            (void)ptr;
            return 0;
         }
         else if constexpr (is_std_vector_ct<U>::value) {
            using E = typename vector_elem_ct<U>::type;
            constexpr uint32_t es = wit_abi_size_impl<E>();
            constexpr uint32_t ea = wit_abi_align_impl<E>();
            uint32_t count = static_cast<uint32_t>(value.size());
            bump = (bump + ea - 1) & ~(ea - 1);
            uint32_t arr = bump;
            bump += count * es;
            // Recurse into elements for nested variable data
            for (uint32_t i = 0; i < count; i++)
               packed_size_field(value[i], bump);
            (void)arr;
            return 0;
         }
         else if constexpr (std::is_same_v<U, std::monostate>) {
            return 0;
         }
         else if constexpr (is_std_optional_ct<U>::value) {
            using E = typename optional_elem_ct<U>::type;
            if (value.has_value())
               packed_size_field(*value, bump);
            return 0;
         }
         else if constexpr (is_std_expected_ct<U>::value) {
            using V = typename expected_value_ct<U>::type;
            if (value.has_value()) {
               if constexpr (!std::is_void_v<V>)
                  packed_size_field(*value, bump);
            } else {
               packed_size_field(value.error(), bump);
            }
            return 0;
         }
         else if constexpr (is_std_variant_v<U>) {
            std::visit([&](const auto& v) {
               using A = std::remove_cvref_t<decltype(v)>;
               if constexpr (!std::is_same_v<A, std::monostate>)
                  packed_size_field(v, bump);
            }, value);
            return 0;
         }
         else if constexpr (Reflected<U>) {
            // Recurse into fields
            apply_members(
               (detail::data_members_t<U>*)nullptr,
               [&](auto... ptrs) {
                  (packed_size_field(value.*ptrs, bump), ...);
               }
            );
            return 0;
         }
         else {
            static_assert(sizeof(U) == 0, "packed_size_field: unsupported type");
            return 0;
         }
      }

   } // namespace detail_wit_abi

   /// Compute the total packed buffer size for a value (record + all variable data).
   template <typename T>
   uint32_t wit_abi_packed_size(const T& value) {
      uint32_t bump = wit_abi_size_v<T>;  // root record
      detail_wit_abi::packed_size_field(value, bump);
      return bump;
   }

   // =========================================================================
   // Policy concepts — memory serialization (wire format)
   // =========================================================================

   /// StorePolicy — write fields to a byte buffer (pack / lower_fields)
   template <typename P>
   concept StorePolicy = requires(P& p, uint32_t offset, uint32_t size, uint32_t align) {
      { p.alloc(align, size) } -> std::same_as<uint32_t>;
      { p.store_u8(offset, uint8_t{}) };
      { p.store_u16(offset, uint16_t{}) };
      { p.store_u32(offset, uint32_t{}) };
      { p.store_u64(offset, uint64_t{}) };
      { p.store_f32(offset, float{}) };
      { p.store_f64(offset, double{}) };
      { p.store_bytes(offset, (const char*)nullptr, size) };
   };

   /// LoadPolicy — read fields from a byte buffer (unpack / lift_fields)
   template <typename P>
   concept LoadPolicy = requires(P& p, uint32_t offset) {
      { p.load_u8(offset) } -> std::same_as<uint8_t>;
      { p.load_u16(offset) } -> std::same_as<uint16_t>;
      { p.load_u32(offset) } -> std::same_as<uint32_t>;
      { p.load_u64(offset) } -> std::same_as<uint64_t>;
      { p.load_f32(offset) } -> std::same_as<float>;
      { p.load_f64(offset) } -> std::same_as<double>;
      { p.load_bytes(offset, uint32_t{}) } -> std::convertible_to<const char*>;
   };

   // =========================================================================
   // wit_abi_lower_fields — store T into memory at dest (recursive)
   // =========================================================================

   template <typename T, StorePolicy Policy>
   void wit_abi_lower_fields(const T& value, Policy& p, uint32_t dest);

   namespace detail_wit_abi {

      template <typename T, StorePolicy Policy>
      void store_field(const T& value, Policy& p, uint32_t dest) {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>)
            p.store_u8(dest, value ? 1 : 0);
         else if constexpr (std::is_same_v<U, uint8_t> || std::is_same_v<U, int8_t>)
            p.store_u8(dest, static_cast<uint8_t>(value));
         else if constexpr (std::is_same_v<U, uint16_t> || std::is_same_v<U, int16_t>)
            p.store_u16(dest, static_cast<uint16_t>(value));
         else if constexpr (std::is_same_v<U, uint32_t> || std::is_same_v<U, int32_t>)
            p.store_u32(dest, static_cast<uint32_t>(value));
         else if constexpr (std::is_same_v<U, uint64_t> || std::is_same_v<U, int64_t>)
            p.store_u64(dest, static_cast<uint64_t>(value));
         else if constexpr (std::is_same_v<U, float>)
            p.store_f32(dest, value);
         else if constexpr (std::is_same_v<U, double>)
            p.store_f64(dest, value);
         else if constexpr (std::is_enum_v<U>)
            store_field(static_cast<std::underlying_type_t<U>>(value), p, dest);
         else if constexpr (is_own_ct<U>::value || is_borrow_ct<U>::value)
            p.store_u32(dest, value.handle);
         else if constexpr (is_std_string_ct<U>::value) {
            uint32_t ptr = p.alloc(1, static_cast<uint32_t>(value.size()));
            p.store_bytes(ptr, value.data(), static_cast<uint32_t>(value.size()));
            p.store_u32(dest, ptr);
            p.store_u32(dest + 4, static_cast<uint32_t>(value.size()));
         }
         else if constexpr (is_std_vector_ct<U>::value) {
            using E = typename vector_elem_ct<U>::type;
            constexpr uint32_t es = wit_abi_size_impl<E>();
            constexpr uint32_t ea = wit_abi_align_impl<E>();
            uint32_t count = static_cast<uint32_t>(value.size());
            uint32_t arr = p.alloc(ea, count * es);
            for (uint32_t i = 0; i < count; i++)
               store_field(value[i], p, arr + i * es);
            p.store_u32(dest, arr);
            p.store_u32(dest + 4, count);
         }
         else if constexpr (is_psio_own<U>::value)
            p.store_u32(dest, value.handle);
         else if constexpr (is_psio_borrow<U>::value)
            p.store_u32(dest, value.handle);
         else if constexpr (is_std_tuple<U>::value) {
            [&]<size_t... Is>(std::index_sequence<Is...>) {
               uint32_t offset = 0;
               auto store_elem = [&]<size_t I>() {
                  using E = std::tuple_element_t<I, U>;
                  constexpr uint32_t fa = wit_abi_align_impl<E>();
                  offset = (offset + fa - 1) & ~(fa - 1);
                  store_field(std::get<I>(value), p, dest + offset);
                  offset += wit_abi_size_impl<E>();
               };
               (store_elem.template operator()<Is>(), ...);
            }(std::make_index_sequence<std::tuple_size_v<U>>{});
         }
         else if constexpr (std::is_same_v<U, std::monostate>) {
         }
         else if constexpr (is_std_optional_ct<U>::value) {
            using E = typename optional_elem_ct<U>::type;
            constexpr uint32_t ea = wit_abi_align_impl<E>();
            constexpr uint32_t payload_offset = (1 + ea - 1) & ~(ea - 1);
            if (value.has_value()) {
               p.store_u8(dest, 1);
               store_field(*value, p, dest + payload_offset);
            } else {
               p.store_u8(dest, 0);
            }
         }
         else if constexpr (is_std_expected_ct<U>::value) {
            using V = typename expected_value_ct<U>::type;
            using Err = typename expected_error_ct<U>::type;
            constexpr uint32_t a = wit_abi_align_impl<U>();
            constexpr uint32_t po = (1 + a - 1) & ~(a - 1);
            if (value.has_value()) {
               p.store_u8(dest, 0);
               if constexpr (!std::is_void_v<V>)
                  store_field(*value, p, dest + po);
            } else {
               p.store_u8(dest, 1);
               store_field(value.error(), p, dest + po);
            }
         }
         else if constexpr (is_std_variant_v<U>) {
            constexpr size_t N = std::variant_size_v<U>;
            constexpr uint32_t disc_size = N <= 256 ? 1 : N <= 65536 ? 2 : 4;
            constexpr uint32_t max_pa = []<size_t... Is>(std::index_sequence<Is...>) {
               uint32_t m = 1;
               ((m = std::max(m, wit_abi_align_impl<std::variant_alternative_t<Is, U>>())), ...);
               return m;
            }(std::make_index_sequence<N>{});
            constexpr uint32_t payload_off = (disc_size + max_pa - 1) & ~(max_pa - 1);

            // Write discriminant
            if constexpr (disc_size == 1)
               p.store_u8(dest, static_cast<uint8_t>(value.index()));
            else if constexpr (disc_size == 2)
               p.store_u16(dest, static_cast<uint16_t>(value.index()));
            else
               p.store_u32(dest, static_cast<uint32_t>(value.index()));

            // Write payload
            std::visit([&](const auto& v) {
               using A = std::remove_cvref_t<decltype(v)>;
               if constexpr (!std::is_same_v<A, std::monostate>)
                  store_field(v, p, dest + payload_off);
            }, value);
         }
         else if constexpr (std::is_array_v<U>) {
            using E = std::remove_extent_t<U>;
            constexpr uint32_t n = std::extent_v<U>;
            constexpr uint32_t es = wit_abi_size_impl<E>();
            for (uint32_t i = 0; i < n; i++)
               store_field(value[i], p, dest + i * es);
         }
         else if constexpr (Reflected<U>) {
            wit_abi_lower_fields(value, p, dest);
         }
         else {
            static_assert(sizeof(U) == 0, "store_field: unsupported type");
         }
      }

   } // namespace detail_wit_abi

   template <typename T, StorePolicy Policy>
   void wit_abi_lower_fields(const T& value, Policy& p, uint32_t dest) {
      if constexpr (Reflected<T>) {
         apply_members(
            (detail::data_members_t<T>*)nullptr,
            [&](auto... ptrs) {
               [&]<size_t... Is>(std::index_sequence<Is...>) {
                  (detail_wit_abi::store_field(value.*ptrs, p,
                     dest + wit_abi_field_offset_v<T, Is>), ...);
               }(std::index_sequence_for<decltype(ptrs)...>{});
            }
         );
      } else {
         detail_wit_abi::store_field(value, p, dest);
      }
   }

   // =========================================================================
   // wit_abi_lift_fields — read T from memory at offset
   // =========================================================================

   template <typename T, LoadPolicy Policy>
   T wit_abi_lift_fields(Policy& p, uint32_t base);

   namespace detail_wit_abi {

      template <typename T, LoadPolicy Policy>
      T load_field(Policy& p, uint32_t offset) {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>)
            return p.load_u8(offset) != 0;
         else if constexpr (std::is_same_v<U, uint8_t>)
            return p.load_u8(offset);
         else if constexpr (std::is_same_v<U, int8_t>)
            return static_cast<int8_t>(p.load_u8(offset));
         else if constexpr (std::is_same_v<U, uint16_t>)
            return p.load_u16(offset);
         else if constexpr (std::is_same_v<U, int16_t>)
            return static_cast<int16_t>(p.load_u16(offset));
         else if constexpr (std::is_same_v<U, uint32_t>)
            return p.load_u32(offset);
         else if constexpr (std::is_same_v<U, int32_t>)
            return static_cast<int32_t>(p.load_u32(offset));
         else if constexpr (std::is_same_v<U, uint64_t>)
            return p.load_u64(offset);
         else if constexpr (std::is_same_v<U, int64_t>)
            return static_cast<int64_t>(p.load_u64(offset));
         else if constexpr (std::is_same_v<U, float>)
            return p.load_f32(offset);
         else if constexpr (std::is_same_v<U, double>)
            return p.load_f64(offset);
         else if constexpr (std::is_enum_v<U>)
            return static_cast<U>(load_field<std::underlying_type_t<U>>(p, offset));
         else if constexpr (is_own_ct<U>::value || is_borrow_ct<U>::value)
            return U{p.load_u32(offset)};
         else if constexpr (is_std_string_ct<U>::value) {
            uint32_t ptr = p.load_u32(offset);
            uint32_t len = p.load_u32(offset + 4);
            const char* data = p.load_bytes(ptr, len);
            return std::string(data, len);
         }
         else if constexpr (is_std_vector_ct<U>::value) {
            using E = typename vector_elem_ct<U>::type;
            constexpr uint32_t es = wit_abi_size_impl<E>();
            uint32_t ptr = p.load_u32(offset);
            uint32_t len = p.load_u32(offset + 4);
            std::vector<E> result;
            result.reserve(len);
            for (uint32_t i = 0; i < len; i++)
               result.push_back(load_field<E>(p, ptr + i * es));
            return result;
         }
         else if constexpr (is_psio_own<U>::value)
            return U{p.load_u32(offset)};
         else if constexpr (is_psio_borrow<U>::value)
            return U{p.load_u32(offset)};
         else if constexpr (is_std_tuple<U>::value) {
            return [&]<size_t... Is>(std::index_sequence<Is...>) {
               uint32_t off = 0;
               auto load_elem = [&]<size_t I>() {
                  using E = std::tuple_element_t<I, U>;
                  constexpr uint32_t fa = wit_abi_align_impl<E>();
                  off = (off + fa - 1) & ~(fa - 1);
                  E val = load_field<E>(p, offset + off);
                  off += wit_abi_size_impl<E>();
                  return val;
               };
               return U{load_elem.template operator()<Is>()...};
            }(std::make_index_sequence<std::tuple_size_v<U>>{});
         }
         else if constexpr (std::is_same_v<U, std::monostate>)
            return std::monostate{};
         else if constexpr (is_std_optional_ct<U>::value) {
            using E = typename optional_elem_ct<U>::type;
            constexpr uint32_t ea = wit_abi_align_impl<E>();
            constexpr uint32_t payload_offset = (1 + ea - 1) & ~(ea - 1);
            uint8_t disc = p.load_u8(offset);
            if (disc)
               return std::optional<E>(load_field<E>(p, offset + payload_offset));
            else
               return std::optional<E>(std::nullopt);
         }
         else if constexpr (is_std_expected_ct<U>::value) {
            using V = typename expected_value_ct<U>::type;
            using Err = typename expected_error_ct<U>::type;
            constexpr uint32_t a = wit_abi_align_impl<U>();
            constexpr uint32_t po = (1 + a - 1) & ~(a - 1);
            uint8_t disc = p.load_u8(offset);
            if (disc == 0) {
               if constexpr (std::is_void_v<V>)
                  return std::expected<V, Err>{};
               else
                  return std::expected<V, Err>{load_field<V>(p, offset + po)};
            } else {
               return std::expected<V, Err>{std::unexpected(load_field<Err>(p, offset + po))};
            }
         }
         else if constexpr (is_std_variant_v<U>) {
            constexpr size_t N = std::variant_size_v<U>;
            constexpr uint32_t disc_size = N <= 256 ? 1 : N <= 65536 ? 2 : 4;
            constexpr uint32_t max_pa = []<size_t... Is>(std::index_sequence<Is...>) {
               uint32_t m = 1;
               ((m = std::max(m, wit_abi_align_impl<std::variant_alternative_t<Is, U>>())), ...);
               return m;
            }(std::make_index_sequence<N>{});
            constexpr uint32_t payload_off = (disc_size + max_pa - 1) & ~(max_pa - 1);

            uint32_t disc;
            if constexpr (disc_size == 1)
               disc = p.load_u8(offset);
            else if constexpr (disc_size == 2)
               disc = p.load_u16(offset);
            else
               disc = p.load_u32(offset);

            // Index-dispatch: load the active alternative without default-constructing U
            std::optional<U> result;
            [&]<size_t... Is>(std::index_sequence<Is...>) {
               auto try_alt = [&]<size_t I>() {
                  if (disc == I) {
                     using Alt = std::variant_alternative_t<I, U>;
                     if constexpr (std::is_same_v<Alt, std::monostate>)
                        result.emplace(std::in_place_index<I>);
                     else
                        result.emplace(std::in_place_index<I>,
                           load_field<Alt>(p, offset + payload_off));
                  }
               };
               (try_alt.template operator()<Is>(), ...);
            }(std::make_index_sequence<N>{});
            return std::move(*result);
         }
         else if constexpr (std::is_array_v<U>) {
            using E = std::remove_extent_t<U>;
            constexpr uint32_t n = std::extent_v<U>;
            constexpr uint32_t es = wit_abi_size_impl<E>();
            U arr;
            for (uint32_t i = 0; i < n; i++)
               arr[i] = load_field<E>(p, offset + i * es);
            return arr;
         }
         else if constexpr (Reflected<U>) {
            return wit_abi_lift_fields<U>(p, offset);
         }
         else {
            static_assert(sizeof(U) == 0, "load_field: unsupported type");
            return {};
         }
      }

   } // namespace detail_wit_abi

   template <typename T, LoadPolicy Policy>
   T wit_abi_lift_fields(Policy& p, uint32_t base) {
      if constexpr (Reflected<T>) {
         T result{};
         apply_members(
            (detail::data_members_t<T>*)nullptr,
            [&](auto... ptrs) {
               [&]<size_t... Is>(std::index_sequence<Is...>) {
                  auto load_member = [&]<size_t I>(auto ptr, std::integral_constant<size_t, I>) {
                     using VT = std::remove_cvref_t<typename MemberPtrType<decltype(ptr)>::ValueType>;
                     if constexpr (std::is_array_v<VT>) {
                        using E = std::remove_extent_t<VT>;
                        constexpr uint32_t n = std::extent_v<VT>;
                        constexpr uint32_t es = detail_wit_abi::wit_abi_size_impl<E>();
                        for (uint32_t j = 0; j < n; j++)
                           (result.*ptr)[j] = detail_wit_abi::load_field<E>(
                              p, base + wit_abi_field_offset_v<T, I> + j * es);
                     } else {
                        result.*ptr = detail_wit_abi::load_field<VT>(
                           p, base + wit_abi_field_offset_v<T, I>);
                     }
                  };
                  (load_member(ptrs, std::integral_constant<size_t, Is>{}), ...);
               }(std::index_sequence_for<decltype(ptrs)...>{});
            }
         );
         return result;
      } else {
         return detail_wit_abi::load_field<T>(p, base);
      }
   }

   // =========================================================================
   // wit_abi_rebase — adjust all pointers in a buffer by delta
   // =========================================================================

   template <typename T>
   void wit_abi_rebase(uint8_t* buf, uint32_t offset, int32_t delta);

   namespace detail_wit_abi {

      template <typename T>
      void rebase_field(uint8_t* buf, uint32_t offset, int32_t delta) {
         using U = std::remove_cvref_t<T>;
         if constexpr (is_scalar_v<U>) {
            // No pointers — nothing to rebase
         }
         else if constexpr (is_own_ct<U>::value || is_borrow_ct<U>::value) {
            // Handle index — no pointers
         }
         else if constexpr (is_std_string_ct<U>::value) {
            uint32_t ptr;
            std::memcpy(&ptr, buf + offset, 4);
            ptr = static_cast<uint32_t>(static_cast<int64_t>(ptr) + delta);
            std::memcpy(buf + offset, &ptr, 4);
         }
         else if constexpr (is_std_vector_ct<U>::value) {
            using E = typename vector_elem_ct<U>::type;
            constexpr uint32_t es = wit_abi_size_impl<E>();
            uint32_t ptr;
            std::memcpy(&ptr, buf + offset, 4);
            uint32_t len;
            std::memcpy(&len, buf + offset + 4, 4);
            uint32_t old_ptr = ptr;
            ptr = static_cast<uint32_t>(static_cast<int64_t>(ptr) + delta);
            std::memcpy(buf + offset, &ptr, 4);
            for (uint32_t i = 0; i < len; i++)
               rebase_field<E>(buf, old_ptr + i * es, delta);
         }
         else if constexpr (std::is_same_v<U, std::monostate>) {
            // Nothing to rebase
         }
         else if constexpr (is_std_optional_ct<U>::value) {
            using E = typename optional_elem_ct<U>::type;
            constexpr uint32_t ea = wit_abi_align_impl<E>();
            constexpr uint32_t payload_offset = (1 + ea - 1) & ~(ea - 1);
            uint8_t disc;
            std::memcpy(&disc, buf + offset, 1);
            if (disc)
               rebase_field<E>(buf, offset + payload_offset, delta);
         }
         else if constexpr (is_std_expected_ct<U>::value) {
            using V = typename expected_value_ct<U>::type;
            using Err = typename expected_error_ct<U>::type;
            constexpr uint32_t a = wit_abi_align_impl<U>();
            constexpr uint32_t po = (1 + a - 1) & ~(a - 1);
            uint8_t disc;
            std::memcpy(&disc, buf + offset, 1);
            if (disc == 0) {
               if constexpr (!std::is_void_v<V>)
                  rebase_field<V>(buf, offset + po, delta);
            } else {
               rebase_field<Err>(buf, offset + po, delta);
            }
         }
         else if constexpr (is_std_variant_v<U>) {
            constexpr size_t N = std::variant_size_v<U>;
            constexpr uint32_t disc_size = N <= 256 ? 1 : N <= 65536 ? 2 : 4;
            constexpr uint32_t max_pa = []<size_t... Is>(std::index_sequence<Is...>) {
               uint32_t m = 1;
               ((m = std::max(m, wit_abi_align_impl<std::variant_alternative_t<Is, U>>())), ...);
               return m;
            }(std::make_index_sequence<N>{});
            constexpr uint32_t payload_off = (disc_size + max_pa - 1) & ~(max_pa - 1);

            uint32_t disc;
            if constexpr (disc_size == 1) {
               uint8_t d; std::memcpy(&d, buf + offset, 1); disc = d;
            } else if constexpr (disc_size == 2) {
               uint16_t d; std::memcpy(&d, buf + offset, 2); disc = d;
            } else {
               std::memcpy(&disc, buf + offset, 4);
            }

            [&]<size_t... Is>(std::index_sequence<Is...>) {
               auto try_alt = [&]<size_t I>() {
                  if (disc == I)
                     rebase_field<std::variant_alternative_t<I, U>>(
                        buf, offset + payload_off, delta);
               };
               (try_alt.template operator()<Is>(), ...);
            }(std::make_index_sequence<N>{});
         }
         else if constexpr (Reflected<U>) {
            wit_abi_rebase<U>(buf, offset, delta);
         }
      }

   } // namespace detail_wit_abi

   template <typename T>
   void wit_abi_rebase(uint8_t* buf, uint32_t offset, int32_t delta) {
      if constexpr (Reflected<T>) {
         apply_members(
            (detail::data_members_t<T>*)nullptr,
            [&](auto... ptrs) {
               [&]<size_t... Is>(std::index_sequence<Is...>) {
                  (detail_wit_abi::rebase_field<
                     std::remove_cvref_t<typename MemberPtrType<decltype(ptrs)>::ValueType>
                  >(buf, offset + wit_abi_field_offset_v<T, Is>, delta), ...);
               }(std::index_sequence_for<decltype(ptrs)...>{});
            }
         );
      } else {
         detail_wit_abi::rebase_field<T>(buf, offset, delta);
      }
   }

   // =========================================================================
   // wit_abi_validate — check bounds and alignment of all pointers
   // =========================================================================

   template <typename T>
   bool wit_abi_validate(const uint8_t* buf, uint32_t buf_size, uint32_t offset);

   namespace detail_wit_abi {

      template <typename T>
      bool validate_field(const uint8_t* buf, uint32_t buf_size, uint32_t offset) {
         using U = std::remove_cvref_t<T>;
         if constexpr (is_scalar_v<U>) {
            return offset + wit_abi_size_impl<U>() <= buf_size;
         }
         else if constexpr (is_std_string_ct<U>::value) {
            if (offset + 8 > buf_size) return false;
            uint32_t ptr, len;
            std::memcpy(&ptr, buf + offset, 4);
            std::memcpy(&len, buf + offset + 4, 4);
            return ptr + len <= buf_size;
         }
         else if constexpr (is_std_vector_ct<U>::value) {
            if (offset + 8 > buf_size) return false;
            using E = typename vector_elem_ct<U>::type;
            constexpr uint32_t es = wit_abi_size_impl<E>();
            constexpr uint32_t ea = wit_abi_align_impl<E>();
            uint32_t ptr, len;
            std::memcpy(&ptr, buf + offset, 4);
            std::memcpy(&len, buf + offset + 4, 4);
            if (ptr + len * es > buf_size) return false;
            if (ea > 1 && (ptr % ea) != 0) return false;
            for (uint32_t i = 0; i < len; i++)
               if (!validate_field<E>(buf, buf_size, ptr + i * es))
                  return false;
            return true;
         }
         else if constexpr (std::is_same_v<U, std::monostate>) {
            return true;
         }
         else if constexpr (is_std_optional_ct<U>::value) {
            using E = typename optional_elem_ct<U>::type;
            constexpr uint32_t ea = wit_abi_align_impl<E>();
            constexpr uint32_t payload_offset = (1 + ea - 1) & ~(ea - 1);
            constexpr uint32_t total = wit_abi_size_impl<U>();
            if (offset + total > buf_size) return false;
            uint8_t disc;
            std::memcpy(&disc, buf + offset, 1);
            if (disc)
               return validate_field<E>(buf, buf_size, offset + payload_offset);
            return true;
         }
         else if constexpr (is_std_expected_ct<U>::value) {
            using V = typename expected_value_ct<U>::type;
            using Err = typename expected_error_ct<U>::type;
            constexpr uint32_t total = wit_abi_size_impl<U>();
            if (offset + total > buf_size) return false;
            constexpr uint32_t a = wit_abi_align_impl<U>();
            constexpr uint32_t po = (1 + a - 1) & ~(a - 1);
            uint8_t disc;
            std::memcpy(&disc, buf + offset, 1);
            if (disc == 0) {
               if constexpr (!std::is_void_v<V>)
                  return validate_field<V>(buf, buf_size, offset + po);
               return true;
            } else {
               return validate_field<Err>(buf, buf_size, offset + po);
            }
         }
         else if constexpr (is_std_variant_v<U>) {
            constexpr size_t N = std::variant_size_v<U>;
            constexpr uint32_t disc_size = N <= 256 ? 1 : N <= 65536 ? 2 : 4;
            constexpr uint32_t total = wit_abi_size_impl<U>();
            if (offset + total > buf_size) return false;

            uint32_t disc;
            if constexpr (disc_size == 1) {
               uint8_t d; std::memcpy(&d, buf + offset, 1); disc = d;
            } else if constexpr (disc_size == 2) {
               uint16_t d; std::memcpy(&d, buf + offset, 2); disc = d;
            } else {
               std::memcpy(&disc, buf + offset, 4);
            }

            if (disc >= N) return false;

            constexpr uint32_t max_pa = []<size_t... Is>(std::index_sequence<Is...>) {
               uint32_t m = 1;
               ((m = std::max(m, wit_abi_align_impl<std::variant_alternative_t<Is, U>>())), ...);
               return m;
            }(std::make_index_sequence<N>{});
            constexpr uint32_t payload_off = (disc_size + max_pa - 1) & ~(max_pa - 1);

            bool ok = true;
            [&]<size_t... Is>(std::index_sequence<Is...>) {
               auto try_alt = [&]<size_t I>() {
                  if (disc == I)
                     ok = validate_field<std::variant_alternative_t<I, U>>(
                        buf, buf_size, offset + payload_off);
               };
               (try_alt.template operator()<Is>(), ...);
            }(std::make_index_sequence<N>{});
            return ok;
         }
         else if constexpr (Reflected<U>) {
            return wit_abi_validate<U>(buf, buf_size, offset);
         }
         else {
            static_assert(sizeof(U) == 0, "validate_field: unsupported type");
            return false;
         }
      }

   } // namespace detail_wit_abi

   template <typename T>
   bool wit_abi_validate(const uint8_t* buf, uint32_t buf_size, uint32_t offset) {
      if (offset + wit_abi_size_v<T> > buf_size)
         return false;
      if constexpr (Reflected<T>) {
         bool ok = true;
         apply_members(
            (detail::data_members_t<T>*)nullptr,
            [&](auto... ptrs) {
               [&]<size_t... Is>(std::index_sequence<Is...>) {
                  ((ok && (ok = detail_wit_abi::validate_field<
                     std::remove_cvref_t<typename MemberPtrType<decltype(ptrs)>::ValueType>
                  >(buf, buf_size, offset + wit_abi_field_offset_v<T, Is>))), ...);
               }(std::index_sequence_for<decltype(ptrs)...>{});
            }
         );
         return ok;
      } else {
         return detail_wit_abi::validate_field<T>(buf, buf_size, offset);
      }
   }

   // =========================================================================
   // Concrete policies
   // =========================================================================

   // ── fixed_store_policy — write into pre-allocated buffer (no resize) ───────
   //
   // Used by two-pass pack: wit_abi_packed_size() computes total size,
   // buffer is allocated once, then fixed_store_policy writes with zero
   // overhead — no vector growth, no bounds checks, no base offset math.

   // ── size_store_policy — count bytes only, no writes ─────────────────────
   //
   // Mirrors lower-fields' alloc/store interface but performs no actual
   // memory writes. Used to compute the total wire size in advance so the
   // real buffer (vector or otherwise) can be pre-allocated.

   struct size_store_policy {
      uint32_t bump = 0;
      uint32_t base = 0;

      uint32_t alloc(uint32_t align, uint32_t size) {
         bump = (bump + align - 1) & ~(align - 1);
         uint32_t ptr = bump;
         bump += size;
         return ptr;
      }

      void store_u8(uint32_t, uint8_t)                {}
      void store_u16(uint32_t, uint16_t)              {}
      void store_u32(uint32_t, uint32_t)              {}
      void store_u64(uint32_t, uint64_t)              {}
      void store_f32(uint32_t, float)                 {}
      void store_f64(uint32_t, double)                {}
      void store_bytes(uint32_t, const char*, uint32_t) {}
   };

   //  One-pass walk that returns the total bytes a buffer_store_policy
   //  would consume to lower `value`.  Useful as a pre-reservation hint
   //  to eliminate repeated vector::resize calls during the actual lower.
   template <typename T>
   inline uint32_t wit_abi_total_bytes(const T& value) noexcept
   {
      size_store_policy p;
      const uint32_t    dest =
         p.alloc(wit_abi_align_v<T>, wit_abi_size_v<T>);
      wit_abi_lower_fields(value, p, dest);
      return p.bump;
   }

   struct fixed_store_policy {
      uint8_t* buf;
      uint32_t bump = 0;

      explicit fixed_store_policy(uint8_t* b) : buf(b) {}

      uint32_t alloc(uint32_t align, uint32_t size) {
         bump = (bump + align - 1) & ~(align - 1);
         uint32_t ptr = bump;
         bump += size;
         return ptr;
      }

      void store_u8(uint32_t off, uint8_t v)   { buf[off] = v; }
      void store_u16(uint32_t off, uint16_t v)  { std::memcpy(buf + off, &v, 2); }
      void store_u32(uint32_t off, uint32_t v)  { std::memcpy(buf + off, &v, 4); }
      void store_u64(uint32_t off, uint64_t v)  { std::memcpy(buf + off, &v, 8); }
      void store_f32(uint32_t off, float v)     { std::memcpy(buf + off, &v, 4); }
      void store_f64(uint32_t off, double v)    { std::memcpy(buf + off, &v, 8); }
      void store_bytes(uint32_t off, const char* data, uint32_t len) {
         if (len > 0) std::memcpy(buf + off, data, len);
      }
   };

   // ── buffer_store_policy — bump allocator into a flat buffer ────────────────
   //
   // Dynamic-sizing variant. Use when the total size is not known ahead of
   // time (e.g., rebase with base offset, or incremental construction).

   struct buffer_store_policy {
      std::vector<uint8_t> buf;
      uint32_t bump = 0;
      uint32_t base = 0;

      buffer_store_policy(uint32_t base_offset = 0) : bump(base_offset), base(base_offset) {}

      uint32_t alloc(uint32_t align, uint32_t size) {
         bump = (bump + align - 1) & ~(align - 1);
         uint32_t ptr = bump;
         bump += size;
         if (buf.size() < bump - base)
            buf.resize(bump - base);
         return ptr;
      }

      void store_u8(uint32_t off, uint8_t v)   { buf[off - base] = v; }
      void store_u16(uint32_t off, uint16_t v)  { std::memcpy(&buf[off - base], &v, 2); }
      void store_u32(uint32_t off, uint32_t v)  { std::memcpy(&buf[off - base], &v, 4); }
      void store_u64(uint32_t off, uint64_t v)  { std::memcpy(&buf[off - base], &v, 8); }
      void store_f32(uint32_t off, float v)     { std::memcpy(&buf[off - base], &v, 4); }
      void store_f64(uint32_t off, double v)    { std::memcpy(&buf[off - base], &v, 8); }
      void store_bytes(uint32_t off, const char* data, uint32_t len) {
         if (len > 0) std::memcpy(&buf[off - base], data, len);
      }
   };

   // ── buffer_load_policy — read from a flat buffer ─────────────────────────

   struct buffer_load_policy {
      const uint8_t* buf;
      uint32_t buf_size;

      buffer_load_policy(const uint8_t* b, uint32_t size)
         : buf(b), buf_size(size) {}

      uint8_t  load_u8(uint32_t off)  { return buf[off]; }
      uint16_t load_u16(uint32_t off) { uint16_t v; std::memcpy(&v, buf + off, 2); return v; }
      uint32_t load_u32(uint32_t off) { uint32_t v; std::memcpy(&v, buf + off, 4); return v; }
      uint64_t load_u64(uint32_t off) { uint64_t v; std::memcpy(&v, buf + off, 8); return v; }
      float    load_f32(uint32_t off) { float v; std::memcpy(&v, buf + off, 4); return v; }
      double   load_f64(uint32_t off) { double v; std::memcpy(&v, buf + off, 8); return v; }
      const char* load_bytes(uint32_t off, uint32_t len) {
         if (static_cast<uint64_t>(off) + len > buf_size) {
#ifdef __cpp_exceptions
            throw std::runtime_error("load_bytes: out of bounds");
#else
            __builtin_trap();
#endif
         }
         return reinterpret_cast<const char*>(buf + off);
      }
   };

} // namespace psio
