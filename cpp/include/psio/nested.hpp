#pragma once
//
// psio/nested.hpp — psio::nested<T>: a value that IS the PSSZ encoding of a
// T, carried as a byte list (docs/pssz-spec.md, "Nested values").
//
//   struct Action
//   {
//      AccountNumber             sender;
//      MethodNumber              method;
//      psio::nested<TransferArgs> args;   // on the wire: the bytes of a vector<u8>
//      PSIO_REFLECT(Action, sender, method, args)
//   };
//
// On the wire, in a key and in a schema-less reader's eyes a nested<T> is
// exactly a std::vector<std::uint8_t> holding encode(psio::pssz{}, T): the
// other side may declare the member as bytes and forward it untouched. What
// the type adds is that the bytes are known to be a T:
//
//   pssz        the bytes; every checking reader of the enclosing value
//               (validate, validate_strict, validate_pssz_compatible,
//               decode_pssz_checked, validate_or_throw) also validates them
//               as T, in its own mode, one node level below the member
//   key         the key of the byte list; key validate / decode validate
//               the bytes as T
//   json/ijson  the inner value's own text ({"args": {"to": "bob"}}), not
//               hex: read as a T and stored as its canonical encoding;
//               written by decoding the bytes
//   schema      schema_types::Nested{T}
//   view        view<nested<T>, pssz>: bytes(), and get() / operator* -> the
//               view<T, pssz> over the bytes
//
// A nested<T> owns its bytes. Built through anything but `unchecked` it
// holds a valid encoding of a T, which is why encoding an enclosing value
// writes the bytes without looking at them.
//
// (To write the member of an enclosing value from a T without this buffer
// of its own, give the enclosing record psio::nested_of(value): pssz.hpp
// "Packing in place".)
//
//   nested<T>::from(value)         encode (fail-fast, like psio::encode)
//   nested<T>::from_checked(v, n)  encode, reporting a codec_status
//   nested<T>::checked(bytes, n)   adopt bytes after validating them as T
//   nested<T>::unchecked(bytes)    adopt bytes the caller vouches for
//   n.bytes() / n.size()           the encoding
//   std::move(n).release()         the encoding, as a vector
//   n.view()                       psio::view<T, psio::pssz> over the bytes
//   n.decode()                     T (trusted, like psio::decode)
//   n.decode_checked(out)          validate, then decode
//   n.validate() / validate_strict()
//
// nested<T, nested_validation::deferred> has the same bytes, key, text and
// schema, but a reader of the enclosing value treats its bytes as opaque:
// the holder calls validate() itself before view() / decode(). Use it for a
// payload that is checked later, by someone else, or not at all on this
// path (a router forwarding a message whose body only the destination
// interprets).
//
// Text round trips are value round trips: reading text produces the
// canonical encoding of the T it describes, so fields a newer writer
// appended to the inner record, which a typed read drops, are not carried
// through text. A holder that must preserve foreign bytes exactly declares
// a byte list (hex in ijson) instead.

#include <psio/detail/nested_fwd.hpp>
#include <psio/format.hpp>
#include <psio/get_type_name.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/shapes.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace psio
{
   template <typename T, nested_validation V>
   class nested
   {
      static_assert(std::is_object_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>,
                    "psio::nested<T>: T is a value type (no reference, no cv-qualifier)");

      std::vector<char> bytes_;

      struct adopt_t
      {
      };
      nested(adopt_t, std::vector<char>&& bytes) noexcept : bytes_(std::move(bytes)) {}

     public:
      using value_type                              = T;
      static constexpr nested_validation validation = V;

      // The encoding of T{}. (Not a constraint: T may still be incomplete
      // where a recursive type declares its nested<T> member.)
      nested() : bytes_(::psio::encode(::psio::pssz{}, T{})) {}
      // The encoding of `value`. Fails fast (codec_exception, or abort
      // without exceptions) for a value with no encoding, like psio::encode.
      explicit nested(const T& value) : bytes_(::psio::encode(::psio::pssz{}, value)) {}

      static nested from(const T& value) { return nested(value); }

      // The encoding of `value`, or why it has none (`out` is unchanged).
      [[nodiscard]] static codec_status from_checked(const T& value, nested& out)
      {
         std::vector<char> bytes;
         if (auto status = ::psio::encode_pssz_checked(::psio::pssz{}, value, bytes);
             !status.ok())
            return status;
         out.bytes_ = std::move(bytes);
         return codec_ok();
      }

      // Adopts bytes the caller vouches for: they are not looked at. Bytes
      // that are not a valid encoding of T make view() / decode() undefined
      // and are rejected by every reader of an enclosing value (unless V is
      // deferred).
      [[nodiscard]] static nested unchecked(std::vector<char> bytes) noexcept
      {
         return nested(adopt_t{}, std::move(bytes));
      }

      // Adopts `bytes` if they are a valid encoding of T (the compatible
      // rule of psio::validate: unknown appended optional fields are
      // accepted); `out` is unchanged otherwise.
      [[nodiscard]] static codec_status checked(std::vector<char>&& bytes, nested& out)
      {
         if (auto status = ::psio::validate<T>(::psio::pssz{}, std::span<const char>{bytes});
             !status.ok())
            return status;
         out.bytes_ = std::move(bytes);
         return codec_ok();
      }
      [[nodiscard]] static codec_status checked(std::span<const char> bytes, nested& out)
      {
         if (auto status = ::psio::validate<T>(::psio::pssz{}, bytes); !status.ok())
            return status;
         out.bytes_.assign(bytes.begin(), bytes.end());
         return codec_ok();
      }

      [[nodiscard]] std::span<const char> bytes() const noexcept
      {
         return std::span<const char>{bytes_};
      }
      // Number of bytes: what a member length_bound counts.
      [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

      // Gives the bytes up; the object is left holding nothing useful.
      [[nodiscard]] std::vector<char> release() && noexcept { return std::move(bytes_); }

      // Zero-copy access. As for every view, the bytes are assumed valid.
      [[nodiscard]] ::psio::view<T, ::psio::pssz, storage::const_borrow> view() const noexcept
      {
         return ::psio::view<T, ::psio::pssz, storage::const_borrow>{bytes()};
      }

      // Trusted, like psio::decode: the bytes are assumed valid.
      [[nodiscard]] T decode() const { return ::psio::decode<T>(::psio::pssz{}, bytes()); }

      // Validates, then decodes. `has_unknown`: the bytes carry optional
      // fields this T does not know (they are not in `out`).
      [[nodiscard]] codec_status decode_checked(T& out, bool* has_unknown = nullptr) const
      {
         bool unknown = false;
         auto status  = ::psio::decode_pssz_checked(::psio::pssz{}, bytes(), out, unknown);
         if (status.ok() && has_unknown)
            *has_unknown = unknown;
         return status;
      }

      [[nodiscard]] codec_status validate() const noexcept
      {
         return ::psio::validate<T>(::psio::pssz{}, bytes());
      }
      [[nodiscard]] codec_status validate(bool& has_unknown) const noexcept
      {
         return ::psio::validate_pssz_compatible<T>(::psio::pssz{}, bytes(), has_unknown);
      }
      [[nodiscard]] codec_status validate_strict() const noexcept
      {
         return ::psio::validate_strict<T>(::psio::pssz{}, bytes());
      }

      // Bytes: an encoding is canonical, so equal bytes are equal values.
      friend bool operator==(const nested&, const nested&) = default;
   };

   template <typename T>
   nested(const T&) -> nested<T>;

   // A byte list: a member length_bound applies to it (its byte count).
   template <typename T, nested_validation V>
   struct is_byte_string<nested<T, V>> : std::true_type
   {
   };

   // ── Variant alternative name ("nested_<T>") ────────────────────────────
   template <typename T, nested_validation V>
   constexpr const char* get_type_name(const nested<T, V>*);

   namespace detail::nested_impl
   {
      template <typename T>
      constexpr auto type_name = [] {
         constexpr std::string_view inner = get_type_name(static_cast<const T*>(nullptr));
         std::array<char, 7 + inner.size() + 1> out{'n', 'e', 's', 't', 'e', 'd', '_'};
         for (std::size_t i = 0; i < inner.size(); ++i)
            out[7 + i] = inner[i];
         return out;
      }();
   }  // namespace detail::nested_impl

   template <typename T, nested_validation V>
   constexpr const char* get_type_name(const nested<T, V>*)
   {
      return detail::nested_impl::type_name<T>.data();
   }
   template <typename T, storage S>
      requires (S == storage::owning || S == storage::shared)
   constexpr const char* get_type_name(const buffer<T, ::psio::pssz, S>*)
   {
      return detail::nested_impl::type_name<T>.data();
   }

   // (The pssz walks of a nested value — detail::nested_impl — are defined
   // at the end of pssz.hpp: a pssz_buffer / shared_pssz member needs them
   // without this header.)

   // ── View ───────────────────────────────────────────────────────────────
   //
   // The member's span is the inner encoding. Whatever profile the
   // enclosing view reads, the inner value is in the automatic profile.
   template <typename T, nested_validation V, typename Fmt, storage Store>
      requires detail::is_pssz_format_v<Fmt>
   class view<nested<T, V>, Fmt, Store>
   {
      std::span<const char> data_;

     public:
      using element_type                    = nested<T, V>;
      using format_type                     = Fmt;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);
      // The kind of the inner value's view. Validating the enclosing value
      // validates an eager member's bytes, so trust carries over; it never
      // looks at a deferred member's, so the inner view of one is the safe
      // view whatever the enclosing view is.
      static constexpr storage inner_storage =
         V == nested_validation::eager ? Store : storage::const_borrow;
      using inner_view = view<T, ::psio::pssz, inner_storage>;

      view() = default;
      explicit view(detail::view_source<Store> s) noexcept : data_(detail::view_source_bytes(s)) {}
      view(detail::view_source<Store> s, std::span<const char>) noexcept
         : data_(detail::view_source_bytes(s))
      {
      }
      [[nodiscard]] std::span<const char> _psio_data() const noexcept { return data_; }
      [[nodiscard]] std::span<const char> bytes() const noexcept { return data_; }
      [[nodiscard]] inner_view get() const noexcept
      {
         return inner_view{detail::view_src<inner_storage>(data_)};
      }
      [[nodiscard]] inner_view operator*() const noexcept { return get(); }
      // `member->field()`: the inner value's view.
      [[nodiscard]] detail::arrow_proxy<inner_view> operator->() const noexcept
      {
         return {get()};
      }
      [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
      // The nested<T> (a copy of the bytes); the inner value is get().unpack().
      PSIO_VIEW_UNPACK_
      PSIO_VIEW_TO_SAFE_(_psio_data())
   };

   // The view of a pssz_buffer<T> / shared_pssz<T> member is the view of
   // the nested<T> it is on the wire; unpack() gives the member's own type.
   template <typename T, storage Held, typename Fmt, storage Store>
      requires (detail::is_pssz_format_v<Fmt> &&
                (Held == storage::owning || Held == storage::shared))
   class view<buffer<T, ::psio::pssz, Held>, Fmt, Store> : public view<nested<T>, Fmt, Store>
   {
      using base = view<nested<T>, Fmt, Store>;

    public:
      using element_type = buffer<T, ::psio::pssz, Held>;
      using format_type  = Fmt;
      using base::base;
      view() = default;
      [[nodiscard]] std::span<const char> _psio_data() const noexcept { return base::_psio_data(); }
      static constexpr storage storage_kind = Store;
      PSIO_VIEW_UNPACK_
      PSIO_VIEW_TO_SAFE_(_psio_data())
   };

}  // namespace psio
