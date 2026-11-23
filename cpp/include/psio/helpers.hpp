#pragma once
//
// psio/helpers.hpp — the per-format helper names.
//
// This is the level most code should use. For a format F (pssz, ssz, bin,
// json, key, …) one line in the format's header,
//
//    PSIO_FORMAT_HELPERS(F)
//
// declares in namespace psio:
//
//    to_F(v)                    encode into a fresh buffer of the format's
//                               natural type (std::vector<char>, std::string
//                               for the text formats)
//    to_F(v, sink)              encode appending to an existing buffer
//    from_F<T>(bytes)           validate, then decode
//    from_F<T>(trusted{bytes})  decode without the validation pass
//    validate_F<T>(bytes)       codec_status, never throws or aborts
//    F_size(v)                  size of the encoding of v
//    F_view<T>                  read-only view over borrowed bytes;
//                               F_view<const T> is the same type
//    F_mutable_view<T>          view that writes into borrowed bytes
//    F_buffer<T>                owning typed bytes
//    shared_F<T>                reference-counted typed bytes; copies share them
//
// Each name exists only where the format has the operation (no F_view
// for a format without views, no to_F(v, sink) for a format that cannot
// append): using a missing one is a constraint failure naming the helper.
//
// Failure of the checked decode. `from_F<T>(bytes)` returns a T or does not
// return: on bytes `validate_F<T>` rejects it calls psio::codec_abort,
// which throws psio::codec_exception where exceptions are enabled
// (PSIO_EXCEPTIONS_ENABLED) and calls std::abort where they are not. Code
// that must handle bad input without exceptions asks first:
//
//    if (auto st = psio::validate_pssz<T>(bytes); st.ok())
//       use(psio::from_pssz<T>(psio::trusted{bytes}));
//    else
//       report(st.error());
//
// `trusted` is a statement by the caller that the bytes have already been
// validated as a T in this format (or were produced by to_F in this
// process). For the binary formats, decoding trusted bytes that are not
// valid is undefined behaviour. The text formats and `key` check as they
// read, so their trusted decode still fails through codec_abort.
//
// Everything here is a one-line call of the generic layer in cpo.hpp
// (psio::encode / decode / validate / size_of with the format tag as the
// first argument), which is what generic code that takes the format as a
// template parameter uses.

#include <psio/buffer.hpp>
#include <psio/cpo.hpp>
#include <psio/error.hpp>
#include <psio/format.hpp>
#include <psio/storage.hpp>
#include <psio/trusted.hpp>

#include <concepts>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace psio {

   template <typename T, typename Fmt>
   class mutable_view;

   namespace view_layout {
      template <typename Fmt>
      struct traits;
   }

   namespace detail::helpers {

      // ── What a format tag provides ────────────────────────────────────

      template <typename Fmt, typename T>
      concept encodes = requires(const T& v) { ::psio::encode(Fmt{}, v); };

      template <typename Fmt, typename T, typename Sink>
      concept encodes_into =
         requires(const T& v, Sink& sink) { ::psio::encode(Fmt{}, v, sink); };

      template <typename Fmt, typename T>
      concept sizes = requires(const T& v) { ::psio::size_of(Fmt{}, v); };

      template <typename Fmt, typename T>
      concept decodes = requires(std::span<const char> b) {
         tag_invoke(::psio::decode<T>, Fmt{}, static_cast<T*>(nullptr), b);
      };

      template <typename Fmt, typename T>
      concept validates = requires(std::span<const char> b) {
         { tag_invoke(::psio::validate<T>, Fmt{}, static_cast<T*>(nullptr), b) }
            -> std::same_as<codec_status>;
      };

      // A text format: its fresh buffer is a std::string, and its input may
      // be written as a string literal.
      template <typename Fmt>
      concept text_format =
         requires { requires std::same_as<typename Fmt::buffer_type, std::string>; };

      // A format whose read view exists once its view header is included
      // (psio/pssz_view.hpp, psio/ssz_view.hpp, psio/wit_view.hpp).
      template <typename Fmt>
      concept read_view_format = requires { sizeof(view_layout::traits<Fmt>); };

      // True for the formats psio/mutable_view.hpp can write in place.
      template <typename Fmt>
      inline constexpr bool mutable_view_format_v = false;

      template <typename Fmt>
      concept mutable_view_format = mutable_view_format_v<Fmt>;

      // `F_view<T>`: the format has a read view. X only defers the check to
      // the use of the alias, when the format's view header has been seen.
      template <typename X, typename Fmt>
      concept read_viewable = read_view_format<Fmt>;

      // `F_mutable_view<T>`: the format has a mutable view and T is not
      // const. Writing is never the meaning of a name that does not say so.
      template <typename X, typename Fmt>
      concept mutable_viewable = !std::is_const_v<X> && mutable_view_format<Fmt>;

      // ── Optional hooks on a format tag ────────────────────────────────
      //
      // `Fmt::to_fresh(v)` — the fresh buffer of `to_F(v)` when it is not
      // what `psio::encode(Fmt{}, v)` returns (key: a std::array for a key
      // of fixed size).
      //
      // `Fmt::decode_checked(bytes, out)` → codec_status — validate and
      // decode in one pass, accepting exactly what `validate` accepts. A
      // format that checks as it reads provides it so the checked decode
      // does not read the input twice.

      template <typename Fmt, typename T>
      [[nodiscard]] constexpr auto to_fresh(Fmt fmt, const T& v)
      {
         if constexpr (requires { Fmt::to_fresh(v); })
            return Fmt::to_fresh(v);
         else
            return ::psio::encode(fmt, v);
      }

      template <typename T, typename Fmt>
      [[nodiscard]] T decode_checked(Fmt fmt, std::span<const char> bytes)
      {
         if constexpr (requires(T& out) {
                          { Fmt::decode_checked(bytes, out) } -> std::same_as<codec_status>;
                       })
         {
            T out{};
            if (auto st = Fmt::decode_checked(bytes, out); !st.ok())
               ::psio::codec_abort(st.error());
            return out;
         }
         else
         {
            if (auto st = ::psio::validate<T>(fmt, bytes); !st.ok())
               ::psio::codec_abort(st.error());
            return ::psio::decode<T>(fmt, bytes);
         }
      }

      inline std::span<const char> text(const char* s) noexcept
      {
         const std::string_view v{s};
         return std::span<const char>{v.data(), v.size()};
      }

   }  // namespace detail::helpers

}  // namespace psio

// PSIO_FORMAT_HELPERS_AS(NAME, TAG) declares the helpers named after NAME
// for the format tag type TAG. Use in namespace psio.
#define PSIO_FORMAT_HELPERS_AS(NAME, TAG)                                                 \
   template <typename T>                                                                  \
      requires ::psio::detail::helpers::encodes<TAG, T>                                   \
   [[nodiscard]] auto to_##NAME(const T& v)                                               \
   {                                                                                      \
      return ::psio::detail::helpers::to_fresh(TAG{}, v);                                 \
   }                                                                                      \
   template <typename T, typename Sink>                                                   \
      requires ::psio::detail::helpers::encodes_into<TAG, T, Sink>                        \
   void to_##NAME(const T& v, Sink& sink)                                                 \
   {                                                                                      \
      ::psio::encode(TAG{}, v, sink);                                                     \
   }                                                                                      \
   template <typename T>                                                                  \
      requires ::psio::detail::helpers::decodes<TAG, T> &&                                \
               ::psio::detail::helpers::validates<TAG, T>                                 \
   [[nodiscard]] T from_##NAME(std::span<const char> bytes)                               \
   {                                                                                      \
      return ::psio::detail::helpers::decode_checked<T>(TAG{}, bytes);                    \
   }                                                                                      \
   template <typename T>                                                                  \
      requires ::psio::detail::helpers::decodes<TAG, T> &&                                \
               ::psio::detail::helpers::validates<TAG, T> &&                              \
               ::psio::detail::helpers::text_format<TAG>                                  \
   [[nodiscard]] T from_##NAME(const char* text)                                          \
   {                                                                                      \
      return ::psio::detail::helpers::decode_checked<T>(                                  \
         TAG{}, ::psio::detail::helpers::text(text));                                     \
   }                                                                                      \
   template <typename T>                                                                  \
      requires ::psio::detail::helpers::decodes<TAG, T>                                   \
   [[nodiscard]] T from_##NAME(::psio::trusted input)                                     \
   {                                                                                      \
      return ::psio::decode<T>(TAG{}, input.bytes);                                       \
   }                                                                                      \
   template <typename T>                                                                  \
      requires ::psio::detail::helpers::validates<TAG, T>                                 \
   [[nodiscard]] ::psio::codec_status validate_##NAME(std::span<const char> bytes) noexcept \
   {                                                                                      \
      return ::psio::validate<T>(TAG{}, bytes);                                           \
   }                                                                                      \
   template <typename T>                                                                  \
      requires ::psio::detail::helpers::validates<TAG, T> &&                              \
               ::psio::detail::helpers::text_format<TAG>                                  \
   [[nodiscard]] ::psio::codec_status validate_##NAME(const char* text) noexcept          \
   {                                                                                      \
      return ::psio::validate<T>(TAG{}, ::psio::detail::helpers::text(text));             \
   }                                                                                      \
   template <typename T>                                                                  \
      requires ::psio::detail::helpers::sizes<TAG, T>                                     \
   [[nodiscard]] std::size_t NAME##_size(const T& v)                                      \
   {                                                                                      \
      return ::psio::size_of(TAG{}, v);                                                   \
   }                                                                                      \
   template <typename X>                                                                  \
      requires ::psio::detail::helpers::read_viewable<X, TAG>                             \
   using NAME##_view =                                                                    \
      ::psio::view<std::remove_const_t<X>, TAG, ::psio::storage::const_borrow>;           \
   template <typename X>                                                                  \
      requires ::psio::detail::helpers::mutable_viewable<X, TAG>                          \
   using NAME##_mutable_view = ::psio::mutable_view<X, TAG>;                              \
   template <typename T>                                                                  \
   using NAME##_buffer = ::psio::buffer<T, TAG, ::psio::storage::owning>;                 \
   template <typename T>                                                                  \
   using shared_##NAME = ::psio::buffer<T, TAG, ::psio::storage::shared>;

// The usual form: the helpers are named after the tag, psio::F.
#define PSIO_FORMAT_HELPERS(F) PSIO_FORMAT_HELPERS_AS(F, ::psio::F)
