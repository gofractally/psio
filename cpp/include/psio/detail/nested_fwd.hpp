#pragma once
//
// psio/detail/nested_fwd.hpp — forward declaration of psio::nested<T> and
// the trait the codecs dispatch on. The type itself is psio/nested.hpp; a
// codec header includes only this, so nothing here depends on a format.

#include <psio/error.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace psio
{
   // What a checking reader of an enclosing value does with the bytes of a
   // nested member (nested.hpp).
   enum class nested_validation : std::uint8_t
   {
      eager,     // validates them as T, in the reader's own mode (the default)
      deferred,  // treats them as opaque bytes; the holder validates later
   };

   template <typename T, nested_validation V = nested_validation::eager>
   class nested;

   namespace detail
   {
      template <typename T>
      struct is_nested : std::false_type
      {
      };
      template <typename T, nested_validation V>
      struct is_nested<::psio::nested<T, V>> : std::true_type
      {
         using inner                    = T;
         static constexpr bool descends = V == nested_validation::eager;
      };
      template <typename T>
      inline constexpr bool is_nested_v = is_nested<T>::value;

      // The pssz walks of a nested value that need the inner type's codec.
      // Declared here for pssz.hpp's ladders; defined in nested.hpp, which a
      // translation unit naming psio::nested has included.
      namespace nested_impl
      {
         // Encoder preflight of a nested member at node depth `depth`.
         template <typename N>
         codec_status pssz_check_encode(const N& value, std::size_t depth);

         // Validation of the payload `bytes` of a nested member at node
         // depth `depth`. `Flags` is pssz_impl::validate_flags.
         template <typename N, typename Flags>
         codec_status pssz_validate(std::span<const char> bytes, std::size_t depth,
                                    Flags* flags) noexcept;
      }  // namespace nested_impl
   }     // namespace detail
}  // namespace psio
