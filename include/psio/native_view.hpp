#pragma once

// native_view: zero-cost shim that adapts a live const T& to the same
// view<T, Fmt, Store> accessor surface as the encoded formats (pssz, frac,
// ssz, capnp, fbs, …).
//
// Backing store is the in-memory bytes of the live object — `record_field_span`
// reinterprets the parent span as `const T*` and returns a child span pointing
// at the member's bytes (for arithmetic) or at the member's contained data
// (for std::string, std::vector, etc.). The encoded-shape view templates in
// view.hpp work unchanged.
//
// Use cases:
//   * Multi-index extractors written once against psio::view_of<T> and called
//     on either a live T (via native_view) or an encoded buffer (via the
//     codec-specific from_buffer/as_view) without branching.
//   * Cheap "view over a live object" without re-encoding.
//
// Companion utilities:
//   * native_view(const T&)  — wrap a live object as a view<T, native, const_borrow>.
//   * view_of<V, T>          — concept: V is some view of T.

#include <psio/reflect.hpp>
#include <psio/storage.hpp>
#include <psio/view.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace psio
{
   // ── Format tag ───────────────────────────────────────────────────────────

   struct native;

   namespace native_detail
   {
      // Recover the live const T* from the bytes a parent traits call passed
      // down. Native traits encode "the bytes are a reinterpret_cast of a
      // const T*"; we reverse that here.
      template <typename T>
      const T* live(std::span<const char> s) noexcept
      {
         return reinterpret_cast<const T*>(s.data());
      }

      // Pack a const T* as bytes for chaining through the view machinery.
      template <typename T>
      std::span<const char> as_span(const T& obj) noexcept
      {
         return std::span<const char>{
             reinterpret_cast<const char*>(&obj),
             sizeof(T)};
      }

      // Nth member pointer declared in T's PSIO_REFLECT, as a non-type
      // compile-time value. Sourced from v3's reflect<T>::member_pointer<N>.
      template <typename T, std::size_t N>
      constexpr auto nth_member_pointer_v = reflect<T>::template member_pointer<N>;
   }  // namespace native_detail

   // ── view_layout::traits<native> ──────────────────────────────────────────
   //
   // The native traits return spans that point at the live object's memory
   // for each requested field. For arithmetic fields the span covers the
   // member's bytes verbatim (the arithmetic view reads back via memcpy).
   // For std::string the span IS the string's character data — the
   // view<std::string, native, const_borrow>::view_() then returns a
   // string_view over that span, aliasing the live std::string with no copy.

   namespace view_layout
   {
      template <>
      struct traits<native>
      {
         template <typename T, std::size_t N>
         static std::span<const char>
         record_field_span(std::span<const char> s,
                           std::span<const char> /*root*/ = {}) noexcept
         {
            const T*       obj = native_detail::live<T>(s);
            constexpr auto mp  = native_detail::nth_member_pointer_v<T, N>;
            using F = std::remove_cvref_t<decltype(obj->*mp)>;

            const auto& member = obj->*mp;

            if constexpr (std::is_same_v<F, std::string>)
            {
               return std::span<const char>{member.data(), member.size()};
            }
            else
            {
               // Arithmetic, enum, optional<scalar>, nested reflected, vector,
               // etc. all chain through "bytes are the live object's memory".
               // The shape-specific view picks up from there: arithmetic
               // memcpys, nested records re-enter native traits, and so on.
               return std::span<const char>{
                   reinterpret_cast<const char*>(&member),
                   sizeof(F)};
            }
         }
      };
   }  // namespace view_layout

   // ── native_view(const T&) factory ────────────────────────────────────────

   template <typename T>
   view<T, native, storage::const_borrow> native_view(const T& obj) noexcept
   {
      return view<T, native, storage::const_borrow>{
          native_detail::as_span(obj)};
   }

   // ── view_of<V, T> concept ────────────────────────────────────────────────
   //
   // V is some psio::view of T (any format, any storage). Used to constrain
   // extractors that accept either view<T, native, S>, view<T, pssz, S>, etc.

   namespace view_of_detail
   {
      template <typename V>
      struct view_target
      {
         using type = void;
      };
      template <typename T, typename Fmt, storage S>
      struct view_target<view<T, Fmt, S>>
      {
         using type = T;
      };
   }  // namespace view_of_detail

   template <typename V, typename T>
   concept view_of = std::is_same_v<
       typename view_of_detail::view_target<std::remove_cvref_t<V>>::type, T>;

   // ── is_native_view detection ─────────────────────────────────────────────

   template <typename V>
   struct is_native_view : std::false_type
   {
   };
   template <typename T, storage S>
   struct is_native_view<view<T, native, S>> : std::true_type
   {
   };
   template <typename V>
   constexpr bool is_native_view_v = is_native_view<std::remove_cvref_t<V>>::value;

}  // namespace psio
