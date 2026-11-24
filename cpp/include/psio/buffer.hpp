#pragma once
//
// psio/buffer.hpp — typed encoded bytes.
//
// `psio::buffer<T, Fmt, Store>` holds the encoding of a T in format Fmt,
// typed at the type-system level with (T, Fmt). `F_buffer<T>` and
// `shared_F<T>` (helpers.hpp) are its owning and shared forms for a format
// F: `psio::pssz_buffer<T>`, `psio::shared_pssz<T>`.
//
// Store controls the backing:
//   - storage::owning       → std::vector<char> (default)
//   - storage::shared       → reference-counted immutable bytes; a copy
//                             of the buffer is another owner of the same
//                             bytes (a block or transaction held by
//                             several owners)
//   - storage::mut_borrow   → std::span<char>
//   - storage::const_borrow → std::span<const char>
//
// An owning or shared buffer is a value that IS the encoding of a T, held
// packed: what `psio::nested<T>` is for a record member, with a choice of
// ownership. It always holds a valid encoding:
//
//   B{}                        the encoding of T{} (no allocation: the
//                              bytes are one process-wide constant)
//   B{value} / B::from(value)  encodes
//   B{bytes}                   validates, then holds them (psio::codec_abort
//                              on bytes that are not a T: an exception
//                              where they are enabled). A std::vector<char>&&
//                              is adopted without a copy.
//   B{psio::trusted{bytes}}    holds bytes the caller vouches for, unchecked
//   B::unchecked(vector)       adopts a vector the caller vouches for
//   B::checked(bytes, out)     validates into `out`, returning a codec_status
//   B::from_checked(value, out) encodes into `out`, returning a codec_status
//
//   b.view() / *b / b.get()    the read-only view of the T; b->member().
//                              It is the TRUSTED view (view.hpp "Safe and
//                              trusted views"): the buffer holds a valid
//                              encoding, so no access checks it again.
//                              psio::as_view(b) is the safe view of the
//                              same bytes
//   b.unpack()                 the T (trusted decode); decode() is the same
//   b.decode_checked(out)      validate, then decode
//   b.validate() / validate_strict()
//   b.bytes() / data() / size()
//   a == b                     same encoding (an encoding is canonical)
//
// There is no null state: an optional packed value is a
// std::optional<psio::shared_pssz<T>>.
//
// What "holds a valid encoding" rests on, per way of construction:
//   B{} / B{value} / B::from(value) / B::from_checked   encoded here
//   B{bytes} / B::checked(bytes, out)                   validated here
//   B{psio::trusted{bytes}} / B::unchecked(vector)      the CALLER's word.
//       Bytes that are not a valid T make view() / unpack() undefined
//       behaviour, as the trusted decode always was; a caller that cannot
//       vouch for them reads through psio::as_view(b), or validate()s.
// A format without a validator (buffer_validates) holds what it is given.
// Writing through psio::mutable_bytes(b) while a view of b is in use is the
// caller's responsibility as well (psio::mutable_view only overwrites
// fixed-size values in place, which keeps the encoding valid).
//
// The borrowing kinds only carry a span and its type: they have the free
// functions below and nothing else.
//
// As a record member a `pssz_buffer<T>` / `shared_pssz<T>` is a
// `psio::nested<T>` in every respect (detail/nested_fwd.hpp): the same
// bytes, key, text and schema node. See docs/helpers.md "Packed values".
//
// Storage-layer operations (`bytes`, `size`, `format_of`, `to_buffer`,
// `to_shared`, `as_view`, `as_mutable_view`) are also free functions in
// `psio::`, which is what generic code over any Store uses.

#include <psio/error.hpp>
#include <psio/storage.hpp>
#include <psio/trusted.hpp>

#include <concepts>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace psio {

   // Primary template (undefined) — partial specializations per Store.
   template <typename T, typename Fmt, storage Store = storage::owning>
   class buffer;

   template <typename T, typename Fmt, storage Store>
   class view;

   namespace view_layout {
      template <typename Fmt>
      struct traits;
   }

   namespace detail {

      // `operator->` of something that hands out a view by value.
      template <typename V>
      struct arrow_proxy
      {
         V                      value;
         [[nodiscard]] const V* operator->() const noexcept { return &value; }
      };

      // What a format tag offers (format_tag_base.hpp); a tag without the
      // operation leaves the corresponding buffer member out.
      template <typename Fmt, typename T>
      concept buffer_encodes = requires(const T& v) { Fmt::encode(v); };
      template <typename Fmt, typename T>
      concept buffer_validates = requires(std::span<const char> b) {
         { Fmt::template validate<T>(b) } -> std::same_as<codec_status>;
      };
      template <typename Fmt, typename T>
      concept buffer_decodes = requires(std::span<const char> b) {
         { Fmt::template decode<T>(b) } -> std::same_as<T>;
      };

      // The format of the views of a buffer of format Fmt: Fmt itself,
      // unless the tag is a codec that is viewed as another format
      // (`Fmt::view_format<T>`: psio::pssz_method_result, whose views are
      // those of the result's position).
      template <typename Fmt, typename T>
      struct buffer_view_format
      {
         using type = Fmt;
      };
      template <typename Fmt, typename T>
         requires requires { typename Fmt::template view_format<T>; }
      struct buffer_view_format<Fmt, T>
      {
         using type = typename Fmt::template view_format<T>;
      };
      template <typename Fmt, typename T>
      using buffer_view_format_t = typename buffer_view_format<Fmt, T>::type;

      // A T that could be mistaken for the bytes of an encoding: for it the
      // value constructor is left out (use B::from(value)).
      template <typename T>
      concept looks_like_bytes =
         std::convertible_to<const T&, std::span<const char>> ||
         std::convertible_to<T, std::vector<char>>;

      // The encoding of T{}: what a default-constructed buffer holds.
      // Built once, on first use (no static initialization; a build that
      // never default-constructs a buffer pays nothing).
      template <typename T, typename Fmt>
      [[nodiscard]] const std::vector<char>& default_encoding()
      {
         if constexpr (buffer_encodes<Fmt, T> && std::default_initializable<T>)
         {
            static const std::vector<char> bytes = [] {
               const auto encoded = Fmt::encode(T{});
               return std::vector<char>(encoded.begin(), encoded.end());
            }();
            return bytes;
         }
         else
         {
            static const std::vector<char> none;
            return none;
         }
      }

      [[nodiscard]] inline std::vector<char> copy_bytes(std::span<const char> s)
      {
         return std::vector<char>(s.begin(), s.end());
      }

      // Validates `bytes` as a T (when the format can); codec_abort if not.
      template <typename T, typename Fmt>
      void require_valid(std::span<const char> bytes)
      {
         if constexpr (buffer_validates<Fmt, T>)
            if (auto status = Fmt::template validate<T>(bytes); !status.ok())
               ::psio::codec_abort(status.error());
      }

      // The members an owning and a shared buffer have in common. Derived
      // provides bytes() and adoption of a std::vector<char>.
      template <typename Derived, typename T, typename Fmt>
      class packed_value
      {
         [[nodiscard]] const Derived& self() const noexcept
         {
            return static_cast<const Derived&>(*this);
         }

       public:
         using value_type = T;

         [[nodiscard]] std::size_t size() const noexcept { return self().bytes().size(); }
         [[nodiscard]] const char* data() const noexcept { return self().bytes().data(); }

         // The read-only view of the T: the trusted view, because the
         // buffer holds a valid encoding (see the top of this file for
         // what that rests on when the buffer was built through `trusted`
         // / `unchecked`). It converts implicitly to the safe view,
         // psio::view<T, Fmt>; psio::as_view(buffer) is that directly.
         // (A format whose views have one kind hands that out.)
         [[nodiscard]] auto view() const noexcept
         {
            using VF = buffer_view_format_t<Fmt, T>;
            if constexpr (requires { requires view_layout::traits<VF>::trusted_views; })
               return ::psio::view<T, VF, storage::trusted_borrow>{trusted{self().bytes()}};
            else
               return ::psio::view<T, VF, storage::const_borrow>{self().bytes()};
         }
         [[nodiscard]] auto get() const noexcept { return view(); }
         [[nodiscard]] auto operator*() const noexcept { return view(); }
         [[nodiscard]] auto operator->() const noexcept
         {
            return arrow_proxy<decltype(view())>{view()};
         }

         // The T: a trusted decode of the bytes.
         [[nodiscard]] T unpack() const
            requires buffer_decodes<Fmt, T>
         {
            return Fmt::template decode<T>(self().bytes());
         }
         [[nodiscard]] T decode() const
            requires buffer_decodes<Fmt, T>
         {
            return unpack();
         }
         // Validates, then decodes. `has_unknown`: the bytes carry fields
         // this T does not know (PSSZ; they are not in `out`).
         [[nodiscard]] codec_status decode_checked(T& out, bool* has_unknown = nullptr) const
            requires (buffer_decodes<Fmt, T> && buffer_validates<Fmt, T>)
         {
            if constexpr (requires(bool& unknown) {
                             { Fmt::decode_compatible(self().bytes(), out, unknown) }
                                -> std::same_as<codec_status>;
                          })
            {
               bool unknown = false;
               auto status  = Fmt::decode_compatible(self().bytes(), out, unknown);
               if (status.ok() && has_unknown)
                  *has_unknown = unknown;
               return status;
            }
            else
            {
               if (auto status = Fmt::template validate<T>(self().bytes()); !status.ok())
                  return status;
               out = Fmt::template decode<T>(self().bytes());
               if (has_unknown)
                  *has_unknown = false;
               return codec_ok();
            }
         }
         [[nodiscard]] codec_status validate() const noexcept
            requires buffer_validates<Fmt, T>
         {
            return Fmt::template validate<T>(self().bytes());
         }
         [[nodiscard]] codec_status validate_strict() const noexcept
            requires requires(std::span<const char> b) { Fmt::template validate_strict<T>(b); }
         {
            return Fmt::template validate_strict<T>(self().bytes());
         }

         // The encoding of `value`. Fails fast for a value with no
         // encoding, like psio::encode.
         [[nodiscard]] static Derived from(const T& value)
            requires buffer_encodes<Fmt, T>
         {
            auto encoded = Fmt::encode(value);
            if constexpr (std::is_same_v<decltype(encoded), std::vector<char>>)
               return Derived::unchecked(std::move(encoded));
            else
               return Derived::unchecked(std::vector<char>(encoded.begin(), encoded.end()));
         }
         // The encoding of `value`, or why it has none (`out` is unchanged).
         [[nodiscard]] static codec_status from_checked(const T& value, Derived& out)
            requires buffer_encodes<Fmt, T>
         {
            if constexpr (requires(std::vector<char>& bytes) {
                             { Fmt::encode_checked(value, bytes) } -> std::same_as<codec_status>;
                          })
            {
               std::vector<char> bytes;
               if (auto status = Fmt::encode_checked(value, bytes); !status.ok())
                  return status;
               out = Derived::unchecked(std::move(bytes));
            }
            else
               out = from(value);
            return codec_ok();
         }
         // Holds `bytes` if they are a valid encoding of T; `out` is
         // unchanged otherwise.
         [[nodiscard]] static codec_status checked(std::vector<char>&& bytes, Derived& out)
         {
            if constexpr (buffer_validates<Fmt, T>)
               if (auto status = Fmt::template validate<T>(std::span<const char>{bytes});
                   !status.ok())
                  return status;
            out = Derived::unchecked(std::move(bytes));
            return codec_ok();
         }
         [[nodiscard]] static codec_status checked(std::span<const char> bytes, Derived& out)
         {
            if constexpr (buffer_validates<Fmt, T>)
               if (auto status = Fmt::template validate<T>(bytes); !status.ok())
                  return status;
            out = Derived::unchecked(copy_bytes(bytes));
            return codec_ok();
         }

         // Same encoding. (An encoding is canonical: equal bytes are equal
         // values, and for PSSZ equal values have equal bytes.)
         friend bool operator==(const Derived& a, const Derived& b) noexcept
         {
            const auto x = a.bytes();
            const auto y = b.bytes();
            return x.size() == y.size() &&
                   (x.empty() || std::memcmp(x.data(), y.data(), x.size()) == 0);
         }
      };
   }  // namespace detail

   // ── Owning variant ────────────────────────────────────────────────────
   template <typename T, typename Fmt>
   class buffer<T, Fmt, storage::owning>
       : public detail::packed_value<buffer<T, Fmt, storage::owning>, T, Fmt>
   {
      // Empty: the encoding of T{} (detail::default_encoding), not copied.
      std::vector<char> data_;

      struct adopt_t
      {
      };
      buffer(adopt_t, std::vector<char>&& v) noexcept : data_(std::move(v)) {}

    public:
      using element_type = T;
      using format_type  = Fmt;
      static constexpr storage storage_kind = storage::owning;

      buffer() = default;
      // Validates the bytes, then holds them (no copy of an rvalue).
      explicit buffer(std::vector<char> v) : data_(std::move(v))
      {
         detail::require_valid<T, Fmt>(std::span<const char>{data_});
      }
      explicit buffer(trusted t) : data_(detail::copy_bytes(t.bytes)) {}
      explicit buffer(const T& value)
         requires (detail::buffer_encodes<Fmt, T> && !detail::looks_like_bytes<T>)
         : buffer(buffer::from(value))
      {
      }
      [[nodiscard]] static buffer unchecked(std::vector<char> v) noexcept
      {
         return buffer(adopt_t{}, std::move(v));
      }

      [[nodiscard]] std::span<const char> bytes() const noexcept
      {
         const auto& held = _psio_data();
         return std::span<const char>{held.data(), held.size()};
      }
      // Gives the bytes up; the buffer is left holding T{}.
      [[nodiscard]] std::vector<char> release() &&
      {
         // (Not a conditional expression: it would copy the held bytes.)
         if (data_.empty())
            return detail::default_encoding<T, Fmt>();
         std::vector<char> out = std::move(data_);
         data_.clear();
         return out;
      }

      // Library-internal accessors.
      [[nodiscard]] const std::vector<char>& _psio_data() const noexcept
      {
         return data_.empty() ? detail::default_encoding<T, Fmt>() : data_;
      }
      // Mutable access materialises the bytes of a default-constructed buffer.
      [[nodiscard]] std::vector<char>& _psio_data()
      {
         if (data_.empty())
            data_ = detail::default_encoding<T, Fmt>();
         return data_;
      }
   };

   // ── const_borrow variant ──────────────────────────────────────────────
   template <typename T, typename Fmt>
   class buffer<T, Fmt, storage::const_borrow>
   {
      std::span<const char> data_;

    public:
      using element_type = T;
      using format_type  = Fmt;
      static constexpr storage storage_kind = storage::const_borrow;

      buffer() = default;
      explicit buffer(std::span<const char> s) noexcept : data_(s) {}

      [[nodiscard]] std::span<const char> _psio_data() const noexcept { return data_; }
   };

   // ── mut_borrow variant ────────────────────────────────────────────────
   template <typename T, typename Fmt>
   class buffer<T, Fmt, storage::mut_borrow>
   {
      std::span<char> data_;

    public:
      using element_type = T;
      using format_type  = Fmt;
      static constexpr storage storage_kind = storage::mut_borrow;

      buffer() = default;
      explicit buffer(std::span<char> s) noexcept : data_(s) {}

      [[nodiscard]] std::span<const char> _psio_data() const noexcept
      {
         return std::span<const char>{data_.data(), data_.size()};
      }
      [[nodiscard]] std::span<char> _psio_data() noexcept { return data_; }
   };

   // ── shared variant ────────────────────────────────────────────────────
   //
   // Immutable bytes with shared ownership. Copying the buffer copies a
   // pointer and bumps an atomic count: every copy reads the same bytes,
   // which live until the last copy is destroyed. The bytes are never
   // written after construction, so copies may be read from different
   // threads without synchronization. There is no mutable access: a
   // write through one owner would be seen by all of them.
   template <typename T, typename Fmt>
   class buffer<T, Fmt, storage::shared>
       : public detail::packed_value<buffer<T, Fmt, storage::shared>, T, Fmt>
   {
      // Null: the encoding of T{} (detail::default_encoding).
      std::shared_ptr<const char[]> data_;
      std::size_t                   size_ = 0;

      struct adopt_t
      {
      };
      // Takes over the vector's allocation: no byte is copied.
      buffer(adopt_t, std::vector<char>&& v) : size_(v.size())
      {
         auto owner = std::make_shared<std::vector<char>>(std::move(v));
         data_      = std::shared_ptr<const char[]>(owner, owner->data());
      }
      // Copies the bytes once, into one allocation shared by all copies.
      buffer(adopt_t, std::span<const char> s) : size_(s.size())
      {
         auto block = std::make_shared_for_overwrite<char[]>(s.size());
         if (!s.empty())
            std::memcpy(block.get(), s.data(), s.size());
         data_ = std::move(block);
      }

    public:
      using element_type = T;
      using format_type  = Fmt;
      static constexpr storage storage_kind = storage::shared;

      buffer() = default;

      // Validates the bytes, then takes over the vector's allocation.
      explicit buffer(std::vector<char>&& v)
      {
         detail::require_valid<T, Fmt>(std::span<const char>{v});
         *this = buffer(adopt_t{}, std::move(v));
      }
      // Validates the bytes, then copies them once.
      explicit buffer(std::span<const char> s)
      {
         detail::require_valid<T, Fmt>(s);
         *this = buffer(adopt_t{}, s);
      }
      explicit buffer(trusted t) : buffer(adopt_t{}, t.bytes) {}
      explicit buffer(const T& value)
         requires (detail::buffer_encodes<Fmt, T> && !detail::looks_like_bytes<T>)
         : buffer(buffer::from(value))
      {
      }
      [[nodiscard]] static buffer unchecked(std::vector<char> v)
      {
         return buffer(adopt_t{}, std::move(v));
      }

      [[nodiscard]] std::span<const char> bytes() const noexcept { return _psio_data(); }

      [[nodiscard]] std::span<const char> _psio_data() const noexcept
      {
         if (!data_)
         {
            const auto& held = detail::default_encoding<T, Fmt>();
            return std::span<const char>{held.data(), held.size()};
         }
         return std::span<const char>{data_.get(), size_};
      }
   };

   // ── Free-function storage API ─────────────────────────────────────────
   //
   // These are the canonical user-facing storage operations. Each is a
   // namespace-scope template so `psio::bytes(b)` resolves via
   // qualified lookup.

   template <typename T, typename Fmt>
   [[nodiscard]] std::span<const char>
   bytes(const buffer<T, Fmt, storage::owning>& b) noexcept
   {
      return std::span<const char>{b._psio_data().data(), b._psio_data().size()};
   }
   template <typename T, typename Fmt>
   [[nodiscard]] std::span<const char>
   bytes(const buffer<T, Fmt, storage::const_borrow>& b) noexcept
   {
      return b._psio_data();
   }
   template <typename T, typename Fmt>
   [[nodiscard]] std::span<const char>
   bytes(const buffer<T, Fmt, storage::mut_borrow>& b) noexcept
   {
      return b._psio_data();
   }

   template <typename T, typename Fmt>
   [[nodiscard]] std::span<const char>
   bytes(const buffer<T, Fmt, storage::shared>& b) noexcept
   {
      return b._psio_data();
   }

   template <typename T, typename Fmt>
   [[nodiscard]] std::span<char>
   mutable_bytes(buffer<T, Fmt, storage::owning>& b)
   {
      return std::span<char>{b._psio_data().data(), b._psio_data().size()};
   }
   template <typename T, typename Fmt>
   [[nodiscard]] std::span<char>
   mutable_bytes(buffer<T, Fmt, storage::mut_borrow>& b) noexcept
   {
      return b._psio_data();
   }

   template <typename T, typename Fmt, storage S>
   [[nodiscard]] std::size_t size(const buffer<T, Fmt, S>& b) noexcept
   {
      return bytes(b).size();
   }

   template <typename T, typename Fmt, storage S>
   [[nodiscard]] constexpr Fmt format_of(const buffer<T, Fmt, S>&) noexcept
   {
      return Fmt{};
   }

   // ── Convert any buffer to owning ───────────────────────────────────────
   //
   // The bytes are carried over as they are: a borrowing buffer makes no
   // promise about them, and an owning / shared one has already checked.
   template <typename T, typename Fmt, storage S>
   [[nodiscard]] buffer<T, Fmt, storage::owning>
   to_buffer(const buffer<T, Fmt, S>& b)
   {
      auto span = bytes(b);
      return buffer<T, Fmt, storage::owning>::unchecked(
          std::vector<char>{span.begin(), span.end()});
   }

   // ── Convert any buffer to shared ───────────────────────────────────────
   //
   // An owning rvalue gives up its allocation; anything else is copied
   // once. A shared buffer is returned as another owner of its bytes.
   template <typename T, typename Fmt>
   [[nodiscard]] buffer<T, Fmt, storage::shared>
   to_shared(buffer<T, Fmt, storage::owning>&& b)
   {
      return buffer<T, Fmt, storage::shared>::unchecked(std::move(b).release());
   }
   template <typename T, typename Fmt>
   [[nodiscard]] buffer<T, Fmt, storage::shared>
   to_shared(const buffer<T, Fmt, storage::shared>& b) noexcept
   {
      return b;
   }
   template <typename T, typename Fmt, storage S>
   [[nodiscard]] buffer<T, Fmt, storage::shared>
   to_shared(const buffer<T, Fmt, S>& b)
   {
      return buffer<T, Fmt, storage::shared>{trusted{bytes(b)}};
   }

}  // namespace psio
