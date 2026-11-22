#pragma once
//
// psio/buffer.hpp — typed byte container.
//
// `psio::buffer<T, Fmt, Store>` holds encoded bytes, typed at the
// type-system level with (T, Fmt). Canonical return of
// `psio::encode(Fmt{}, value)`, canonical input to
// `psio::decode<T>(Fmt{}, buf)` / `psio::as_view(buf)`.
//
// Store controls the backing:
//   - storage::owning       → std::vector<char> (default)
//   - storage::mut_borrow   → std::span<char>
//   - storage::const_borrow → std::span<const char>
//   - storage::shared       → reference-counted immutable bytes; a copy
//                             of the buffer is another owner of the same
//                             bytes (a block or transaction held by
//                             several owners)
//
// Storage-layer operations (`bytes`, `size`, `format_of`, `to_buffer`,
// `as_view`) are free functions in `psio::`. Buffer's `.` surface
// exposes zero user-relevant methods — just the constructor and an
// underscore-prefixed library accessor for internal use by the
// namespace-scope storage templates. This satisfies the "access-
// surface rule" from design § 5.5: free-function storage API + no
// library-named methods on the public surface (the reserved-name
// `_psio_data` is a detail accessor, not a public method, and is
// hidden from name-based method detection used by the rule check).

#include <psio/storage.hpp>

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

   // ── Owning variant ────────────────────────────────────────────────────
   template <typename T, typename Fmt>
   class buffer<T, Fmt, storage::owning>
   {
      std::vector<char> data_;

    public:
      using element_type = T;
      using format_type  = Fmt;
      static constexpr storage storage_kind = storage::owning;

      buffer() = default;
      explicit buffer(std::vector<char> v) noexcept : data_(std::move(v)) {}

      // Library-internal accessor (underscore-prefixed so method-name
      // detection used by the access-surface rule won't mistake it for
      // a user-facing surface method).
      [[nodiscard]] const std::vector<char>& _psio_data() const noexcept { return data_; }
      [[nodiscard]] std::vector<char>&       _psio_data() noexcept       { return data_; }
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
   {
      std::shared_ptr<const char[]> data_;
      std::size_t                   size_ = 0;

    public:
      using element_type = T;
      using format_type  = Fmt;
      static constexpr storage storage_kind = storage::shared;

      buffer() = default;

      // Takes over the vector's allocation: no byte is copied.
      explicit buffer(std::vector<char>&& v) : size_(v.size())
      {
         auto owner = std::make_shared<std::vector<char>>(std::move(v));
         data_      = std::shared_ptr<const char[]>(owner, owner->data());
      }

      // Copies the bytes once, into one allocation shared by all copies.
      explicit buffer(std::span<const char> s) : size_(s.size())
      {
         auto block = std::make_shared_for_overwrite<char[]>(s.size());
         if (!s.empty())
            std::memcpy(block.get(), s.data(), s.size());
         data_ = std::move(block);
      }

      [[nodiscard]] std::span<const char> _psio_data() const noexcept
      {
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
   mutable_bytes(buffer<T, Fmt, storage::owning>& b) noexcept
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
   template <typename T, typename Fmt, storage S>
   [[nodiscard]] buffer<T, Fmt, storage::owning>
   to_buffer(const buffer<T, Fmt, S>& b)
   {
      auto span = bytes(b);
      return buffer<T, Fmt, storage::owning>{
          std::vector<char>{span.begin(), span.end()}};
   }

   // ── Convert any buffer to shared ───────────────────────────────────────
   //
   // An owning rvalue gives up its allocation; anything else is copied
   // once. A shared buffer is returned as another owner of its bytes.
   template <typename T, typename Fmt>
   [[nodiscard]] buffer<T, Fmt, storage::shared>
   to_shared(buffer<T, Fmt, storage::owning>&& b)
   {
      return buffer<T, Fmt, storage::shared>{std::move(b._psio_data())};
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
      return buffer<T, Fmt, storage::shared>{bytes(b)};
   }

}  // namespace psio
