#pragma once
//
// psio/flatbuf_view.hpp — views of a FlatBuffers buffer, and writing its
// fixed-size fields in place.
//
//   psio::flatbuf_view<Order> v{bytes};       // the safe view: any bytes
//   v.id();                                   // std::uint64_t
//   v.customer().name();                      // a string: std::string_view
//   for (auto item : v.items()) item.qty();   // [table]
//   v.payment().index();                      // a union (std::variant member)
//   v.stock().at("bolt");                     // a sorted vector: key lookup
//   v.customer().unpack();                    // the UserProfile value
//
//   auto checked = psio::validated_view<Order>(psio::flatbuf{}, bytes);
//   if (checked) checked->customer().name();  // psio::flatbuf_trusted_view<Order>
//
// The surface is the one every psio view has (psio/view.hpp): a named
// reader per reflected member, `get<N>()`, lists with size / [] / iteration,
// optionals, variants, `unpack()`, comparison with values.
//
// Two kinds, as for PSSZ (view.hpp "Safe and trusted views"):
//
//   psio::flatbuf_view<T>          the SAFE view. It may be constructed over
//                                  any bytes: every offset, vtable entry and
//                                  length it reads is compared with the
//                                  buffer first, and what does not fit reads
//                                  as absent (a default, an empty string, an
//                                  empty list). It never reads outside the
//                                  bytes it was given.
//   psio::flatbuf_trusted_view<T>  the TRUSTED view, of bytes that passed
//                                  `validate_flatbuf<T>` (psio::validated_view
//                                  returns it) or that the caller vouches for
//                                  (`psio::trusted{bytes}`). It compares
//                                  nothing: a field read is the vtable lookup
//                                  the official accessors do. It is one
//                                  pointer; it does not carry the buffer's
//                                  bounds and so does not convert to the safe
//                                  view.
//
// `unpack()` is a trusted decode in both kinds, as everywhere in PSIO.
//
// A FlatBuffers value is not a contiguous run of bytes (a table is found
// through its vtable, an absent scalar has no bytes at all), so these are
// specializations of `psio::view` for the flatbuf tag that hold a cursor.
// Named readers, iterators and comparisons are the frame's.
//
// Writing — `psio::flatbuf_mutable_view<T>` over `std::span<char>`:
//
//   psio::flatbuf_mutable_view<Order> m{bytes};
//   m.customer().age().set(38);     // false: the field is not in the buffer
//   m.position().x().set(1.5f);     // a struct member: always there
//   m.items()[2].qty().set(7);
//
// Only values of fixed size that are already stored are written: a scalar
// field of a table, a member of a struct, an element of a vector of scalars
// or structs. Nothing moves, no offset, length, vtable or union type is
// touched, so the buffer stays a valid encoding — of the modified value.
// A scalar that was not written (it equalled its default, or the optional
// was empty) has no bytes to overwrite: `set` returns false, exactly as the
// official `mutate_` accessors do. There is no size-changing edit: repack.
// Navigation is the safe view's, so writes stay inside the buffer whatever
// the bytes are. (One caveat for hostile input: validation does not prove
// that objects do not overlap, so in a buffer crafted that way a write can
// change what another field reads. Buffers written by psio or by the
// official builder never overlap objects.)

#include <psio/buffer.hpp>
#include <psio/flatbuf.hpp>
#include <psio/view.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace psio {

   namespace view_layout {
      // The flatbuf views are the specializations in this header; the trait
      // exists so that `psio::flatbuf_view<T>` and
      // `psio::flatbuf_trusted_view<T>` name them.
      template <>
      struct traits<::psio::flatbuf>
      {
         static constexpr bool trusted_views = true;
      };
   }  // namespace view_layout

   namespace detail::flatbuf_impl {

      // The format tag of a union member's view: the union of member I of
      // the table Def describes (its two slots are part of that table).
      template <typename Def, std::size_t I>
      struct in
      {
      };

      template <storage Store>
      inline constexpr bool safe_v = !trusted_store_v<Store>;

      // ── The buffer a view may read ────────────────────────────────────
      //
      // The safe view carries the bounds of the buffer and compares before
      // it reads; the trusted view carries nothing.

      template <storage Store>
      struct frame;
      template <>
      struct frame<storage::trusted_borrow>
      {
      };
      template <>
      struct frame<storage::const_borrow>
      {
         const std::uint8_t* begin = nullptr;
         const std::uint8_t* end   = nullptr;
      };
      using safe_frame    = frame<storage::const_borrow>;
      using trusted_frame = frame<storage::trusted_borrow>;

      // Bytes readable at `p`, a pointer into the buffer.
      [[gnu::always_inline]] inline std::size_t room(const safe_frame&   f,
                                                     const std::uint8_t* p) noexcept
      {
         return static_cast<std::size_t>(f.end - p);
      }

      // What the offset at `cell` (4 readable bytes) points to, with at
      // least 4 readable bytes there; nullptr when it does not fit.
      [[gnu::always_inline]] inline const std::uint8_t* follow_in(
         const trusted_frame&, const std::uint8_t* cell) noexcept
      {
         return follow(cell);
      }
      [[gnu::always_inline]] inline const std::uint8_t* follow_in(
         const safe_frame& f, const std::uint8_t* cell) noexcept
      {
         const std::size_t o    = load<std::uint32_t>(cell);
         const std::size_t left = room(f, cell);
         if (o == 0 || o > left || left - o < 4)
            return nullptr;
         return cell + o;
      }

      // ── Cursors ───────────────────────────────────────────────────────

      template <storage Store>
      struct table_ref;

      template <>
      struct table_ref<storage::trusted_borrow>
      {
         const std::uint8_t*                 tbl = nullptr;
         [[no_unique_address]] trusted_frame f{};

         // The cell of the field at vtable position `vt`, or nullptr.
         [[gnu::always_inline]] const std::uint8_t* cell(std::uint16_t vt,
                                                         std::size_t /*need*/) const noexcept
         {
            if (tbl == nullptr) [[unlikely]]
               return nullptr;
            return field_cell(tbl, vt);
         }
         [[nodiscard]] std::size_t object_size() const noexcept
         {
            return tbl ? load<std::uint16_t>(tbl - load<std::int32_t>(tbl) + 2) : 0;
         }
      };

      template <>
      struct table_ref<storage::const_borrow>
      {
         const std::uint8_t* tbl    = nullptr;
         const std::uint8_t* vtable = nullptr;
         safe_frame          f{};
         std::uint16_t       vsize = 0;  // 0: no table

         // The cell of the field at vtable position `vt` with `need`
         // readable bytes, or nullptr.
         [[gnu::always_inline]] const std::uint8_t* cell(std::uint16_t vt,
                                                         std::size_t   need) const noexcept
         {
            if (static_cast<std::size_t>(vt) + 2 > vsize)
               return nullptr;
            const std::size_t off = load<std::uint16_t>(vtable + vt);
            if (off == 0 || off + need > room(f, tbl))
               return nullptr;
            return tbl + off;
         }
         [[nodiscard]] std::size_t object_size() const noexcept
         {
            if (vsize < 4)
               return 0;
            return std::min<std::size_t>(load<std::uint16_t>(vtable + 2), room(f, tbl));
         }
      };

      // The table at `tbl`: nullptr, or 4 readable bytes.
      [[gnu::always_inline]] inline table_ref<storage::trusted_borrow> table_at(
         const trusted_frame&, const std::uint8_t* tbl) noexcept
      {
         return {tbl, {}};
      }
      inline table_ref<storage::const_borrow> table_at(const safe_frame&   f,
                                                       const std::uint8_t* tbl) noexcept
      {
         if (tbl == nullptr)
            return {};
         const std::int64_t pos  = tbl - f.begin;
         const std::int64_t size = f.end - f.begin;
         const std::int64_t vpos = pos - load<std::int32_t>(tbl);
         if (vpos < 0 || vpos > size - 4)
            return {};
         const std::uint16_t vsize = load<std::uint16_t>(f.begin + vpos);
         if (vsize < 4 || vsize > size - vpos)
            return {};
         return {tbl, f.begin + vpos, f, vsize};
      }

      template <storage Store>
      [[gnu::always_inline]] inline table_ref<Store> root_table(std::span<const char> bytes) noexcept
      {
         const auto* base = reinterpret_cast<const std::uint8_t*>(bytes.data());
         if constexpr (safe_v<Store>)
         {
            if (bytes.size() < 4)
               return {};
            const safe_frame f{base, base + bytes.size()};
            return table_at(f, follow_in(f, base));
         }
         else
         {
            if (bytes.empty())
               return {};
            return {follow(base), {}};
         }
      }

      // A struct: its bytes, or nullptr.
      struct struct_ref
      {
         const std::uint8_t* p = nullptr;
      };

      // The elements of a vector (or of a fixed array of a struct).
      template <storage Store>
      struct vec_ref
      {
         const std::uint8_t*                 data  = nullptr;
         std::uint32_t                       count = 0;
         [[no_unique_address]] frame<Store> f{};
      };

      // The vector at `vec` (nullptr, or 4 readable bytes) of elements of
      // `elem_size` bytes.
      template <storage Store>
      [[gnu::always_inline]] inline vec_ref<Store> vector_at(const frame<Store>& f,
                                                             const std::uint8_t* vec,
                                                             std::size_t         elem_size) noexcept
      {
         if constexpr (safe_v<Store>)
         {
            if (vec == nullptr)
               return {};
            const std::uint32_t count = load<std::uint32_t>(vec);
            if (count > (room(f, vec) - 4) / elem_size)
               return {};
            return {vec + 4, count, f};
         }
         else
            return {vec + 4, load<std::uint32_t>(vec), f};
      }

      template <storage Store>
      [[gnu::always_inline]] inline view<std::string, ::psio::flatbuf, Store> string_in(
         const frame<Store>& f, const std::uint8_t* s) noexcept
      {
         using V = view<std::string, ::psio::flatbuf, Store>;
         if constexpr (safe_v<Store>)
         {
            if (s == nullptr)
               return V{};
            const std::size_t len = load<std::uint32_t>(s);
            if (len > room(f, s) - 4)
               return V{};
            return V{std::span<const char>{reinterpret_cast<const char*>(s + 4), len}};
         }
         else
            return V{trusted{std::span<const char>{reinterpret_cast<const char*>(s + 4),
                                                   load<std::uint32_t>(s)}}};
      }

      // ── Reading ───────────────────────────────────────────────────────

      template <typename F, storage Store>
      using value_view = view<F, ::psio::flatbuf, Store>;

      // What reading a value of type F yields: the value for a scalar, a
      // view otherwise.
      template <typename F, storage Store>
      using read_t = std::conditional_t<Scalar<F>, F, value_view<F, Store>>;

      // The value whose inline cell — a table field, a vector element — is
      // at `cell` (its inline bytes are readable).
      template <typename F, storage Store>
      [[gnu::always_inline]] inline read_t<F, Store> read_cell(const frame<Store>& f,
                                                               const std::uint8_t* cell) noexcept
      {
         if constexpr (Scalar<F>)
            return load_scalar<F>(cell);
         else if constexpr (is_struct_v<F>)
            return value_view<F, Store>{struct_ref{cell}};
         else
         {
            const std::uint8_t* const target = follow_in(f, cell);
            if constexpr (std::is_same_v<F, std::string>)
               return string_in<Store>(f, target);
            else if constexpr (is_vector_v<F> || is_array_v<F> || is_set_v<F>)
               return value_view<F, Store>{
                  vector_at<Store>(f, target, inline_size<typename F::value_type>())};
            else if constexpr (is_map_v<F>)
               return value_view<F, Store>{vector_at<Store>(f, target, 4)};
            else
               return value_view<F, Store>{table_at(f, target)};
         }
      }

      // Member I of the table Def describes.
      template <typename Def, std::size_t I, storage Store>
      [[gnu::always_inline]] inline auto read_member(const table_ref<Store>& t) noexcept
      {
         using F                    = typename Def::template member<I>;
         constexpr std::uint16_t vt = vt_of(Def::slot(I));
         if constexpr (is_variant_v<F>)
            return view<F, in<Def, I>, Store>{Def::template deprecated<I> ? table_ref<Store>{} : t};
         else
         {
            const std::uint8_t* const cell =
               Def::template deprecated<I> ? nullptr : t.cell(vt, inline_size<F>());
            if constexpr (is_optional_v<F>)
               return value_view<F, Store>{cell, t.f};
            else if constexpr (Scalar<F>)
               return cell ? load_scalar<F>(cell) : Def::template default_of<I>();
            else
               return cell ? read_cell<F, Store>(t.f, cell) : read_t<F, Store>{};
         }
      }

      // Member I of struct T at `p` (which may be nullptr: an absent struct
      // reads as a default-constructed T).
      template <Struct T, std::size_t I, storage Store>
      [[gnu::always_inline]] inline auto read_struct_member(const std::uint8_t* p) noexcept
      {
         using L                   = struct_layout<T>;
         using F                   = typename L::template member<I>;
         constexpr std::size_t off = L::offset(I);
         if constexpr (Scalar<F>)
            return p ? load_scalar<F>(p + off) : member_default<T, I>();
         else if constexpr (is_array_v<F>)
            return value_view<F, Store>{
               vec_ref<Store>{p ? p + off : nullptr,
                              p ? static_cast<std::uint32_t>(array_traits<F>::count) : 0u,
                              {}}};
         else
            return value_view<F, Store>{struct_ref{p ? p + off : nullptr}};
      }

      // What reflect<T>::proxy is given.
      template <typename T, storage Store>
      class table_projection
      {
       protected:
         table_ref<Store> t_;

       public:
         using ref_type = table_ref<Store>;
         table_projection() = default;
         explicit table_projection(const table_ref<Store>& t) noexcept : t_(t) {}

         template <std::size_t I, auto MemberPtr>
         [[nodiscard]] auto get() const noexcept
         {
            return read_member<record_def<T>, I, Store>(t_);
         }
      };
      template <typename T, storage Store>
      class struct_projection
      {
       protected:
         struct_ref t_;

       public:
         using ref_type = struct_ref;
         struct_projection() = default;
         explicit struct_projection(const struct_ref& s) noexcept : t_(s) {}

         template <std::size_t I, auto MemberPtr>
         [[nodiscard]] auto get() const noexcept
         {
            return read_struct_member<T, I, Store>(t_.p);
         }
      };
      template <typename T, storage Store>
      using record_projection =
         std::conditional_t<is_struct_v<T>, struct_projection<T, Store>, table_projection<T, Store>>;

      template <typename K>
      using key_arg = std::conditional_t<std::is_same_v<K, std::string>, std::string_view, K>;

   }  // namespace detail::flatbuf_impl

   // ── Record: a table, or a struct ────────────────────────────────────────
   template <typename T, storage Store>
      requires detail::flatbuf_impl::Record<T>
   class view<T, flatbuf, Store>
       : public detail::named_base_t<T, detail::flatbuf_impl::record_projection<T, Store>>
   {
      using projection = detail::flatbuf_impl::record_projection<T, Store>;
      using base       = detail::named_base_t<T, projection>;
      using ref_type   = typename projection::ref_type;

    public:
      using element_type                    = T;
      using format_type                     = flatbuf;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);

      view() = default;
      // The root table of a buffer (not size-prefixed): a span for the safe
      // view, psio::trusted for the trusted view.
      explicit view(detail::view_source<Store> buffer) noexcept
         requires detail::flatbuf_impl::Table<T>
          : base(projection{detail::flatbuf_impl::root_table<Store>(
               detail::view_source_bytes(buffer))})
      {
      }
      explicit view(const ref_type& r) noexcept : base(projection{r}) {}

      // The record's inline bytes: a table's fields, a struct.
      [[nodiscard]] std::span<const char> _psio_data() const noexcept
      {
         if constexpr (detail::flatbuf_impl::is_struct_v<T>)
         {
            const auto* p = this->projection::t_.p;
            return {reinterpret_cast<const char*>(p),
                    p ? detail::flatbuf_impl::struct_layout<T>::size : std::size_t{0}};
         }
         else
            return {reinterpret_cast<const char*>(this->projection::t_.tbl),
                    this->projection::t_.object_size()};
      }
      [[nodiscard]] const ref_type& _psio_ref() const noexcept { return this->projection::t_; }

      // Member N: its value for a scalar, its view otherwise.
      template <std::size_t N>
      [[nodiscard]] auto get() const noexcept
      {
         if constexpr (detail::flatbuf_impl::is_struct_v<T>)
            return detail::flatbuf_impl::read_struct_member<T, N, Store>(this->projection::t_.p);
         else
            return detail::flatbuf_impl::read_member<detail::flatbuf_impl::record_def<T>, N, Store>(
               this->projection::t_);
      }

      [[nodiscard]] T unpack() const
      {
         T out{};
         if constexpr (detail::flatbuf_impl::is_struct_v<T>)
         {
            if (const auto* p = this->projection::t_.p)
               detail::flatbuf_impl::read_fixed<T>(p, out);
         }
         else
         {
            if (const auto* tbl = this->projection::t_.tbl)
               detail::flatbuf_impl::decode_table<detail::flatbuf_impl::record_def<T>>(tbl, out);
         }
         return out;
      }
      // A struct is a value of fixed size: the view is its value.
      operator T() const
         requires detail::flatbuf_impl::is_struct_v<T>
      {
         return unpack();
      }
   };

   // ── Vectors ─────────────────────────────────────────────────────────────
   namespace detail::flatbuf_impl {

      template <typename E, storage Store>
      class list_view_base
      {
       protected:
         vec_ref<Store> v_;

       public:
         using value_type = E;

         list_view_base() = default;
         explicit list_view_base(const vec_ref<Store>& v) noexcept : v_(v) {}

         [[nodiscard]] std::size_t size() const noexcept { return v_.count; }
         [[nodiscard]] bool        empty() const noexcept { return v_.count == 0; }

         // Element i. Past the end: a default-constructed result from the
         // safe view, undefined for the trusted view.
         [[nodiscard]] read_t<E, Store> operator[](std::size_t i) const noexcept
         {
            if constexpr (safe_v<Store>)
            {
               if (i >= v_.count)
                  return read_t<E, Store>{};
            }
            return read_cell<E, Store>(v_.f, v_.data + i * inline_size<E>());
         }
         // Element i, or a default-constructed result past the end.
         [[nodiscard]] read_t<E, Store> at(std::size_t i) const noexcept
         {
            return i < v_.count ? read_cell<E, Store>(v_.f, v_.data + i * inline_size<E>())
                                : read_t<E, Store>{};
         }
         // Precondition: !empty().
         [[nodiscard]] read_t<E, Store> front() const noexcept { return (*this)[0]; }
         [[nodiscard]] read_t<E, Store> back() const noexcept { return (*this)[v_.count - 1]; }

         // The elements' inline bytes.
         [[nodiscard]] std::span<const char> _psio_data() const noexcept
         {
            return {reinterpret_cast<const char*>(v_.data),
                    std::size_t{v_.count} * inline_size<E>()};
         }
         [[nodiscard]] const vec_ref<Store>& _psio_ref() const noexcept { return v_; }

         // A vector of bytes is its bytes.
         operator std::span<const char>() const noexcept
            requires ::psio::detail::view_byte_v<E>
         {
            return _psio_data();
         }
         [[nodiscard]] const char* data() const noexcept
            requires ::psio::detail::view_byte_v<E>
         {
            return reinterpret_cast<const char*>(v_.data);
         }

       protected:
         template <typename Out>
         void unpack_into(Out& out, std::size_t n) const
         {
            if constexpr (Scalar<E> && !std::is_same_v<E, bool>)
            {
               if (n)
                  std::memcpy(static_cast<void*>(out.data()), v_.data, n * sizeof(E));
            }
            else
               for (std::size_t i = 0; i < n; ++i)
               {
                  if constexpr (Scalar<E>)
                     out[i] = (*this)[i];
                  else
                     out[i] = (*this)[i].unpack();
               }
         }
      };

   }  // namespace detail::flatbuf_impl

#define PSIO_FLATBUF_VIEW_ITERATION_                                                         \
   using iterator         = detail::view_iterator<view>;                                    \
   using const_iterator   = iterator;                                                       \
   using reverse_iterator = std::reverse_iterator<iterator>;                                \
   [[nodiscard]] iterator         begin() const noexcept { return {*this, 0}; }             \
   [[nodiscard]] iterator         end() const noexcept { return {*this, this->size()}; }    \
   [[nodiscard]] reverse_iterator rbegin() const noexcept { return reverse_iterator{end()}; } \
   [[nodiscard]] reverse_iterator rend() const noexcept { return reverse_iterator{begin()}; }

   template <typename E, typename A, storage Store>
   class view<std::vector<E, A>, flatbuf, Store>
       : public detail::flatbuf_impl::list_view_base<E, Store>
   {
      using base = detail::flatbuf_impl::list_view_base<E, Store>;

    public:
      using element_type                    = std::vector<E, A>;
      using format_type                     = flatbuf;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);
      using base::base;
      view() = default;

      PSIO_FLATBUF_VIEW_ITERATION_

      [[nodiscard]] element_type unpack() const
      {
         element_type out;
         out.resize(this->size());
         this->unpack_into(out, out.size());
         return out;
      }
   };

   // A fixed array of a struct, or a vector of exactly N in a table.
   template <typename E, std::size_t N, storage Store>
   class view<std::array<E, N>, flatbuf, Store>
       : public detail::flatbuf_impl::list_view_base<E, Store>
   {
      using base = detail::flatbuf_impl::list_view_base<E, Store>;

    public:
      using element_type                    = std::array<E, N>;
      using format_type                     = flatbuf;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);
      using base::base;
      view() = default;

      PSIO_FLATBUF_VIEW_ITERATION_

      [[nodiscard]] element_type unpack() const
      {
         element_type out{};
         this->unpack_into(out, std::min<std::size_t>(this->size(), N));
         return out;
      }
   };

   // ── std::set<K>: a sorted vector, with key lookup ───────────────────────
   template <typename K, typename A, storage Store>
   class view<std::set<K, std::less<K>, A>, flatbuf, Store>
       : public detail::flatbuf_impl::list_view_base<K, Store>
   {
      using base    = detail::flatbuf_impl::list_view_base<K, Store>;
      using key_arg = detail::flatbuf_impl::key_arg<K>;

    public:
      using element_type                    = std::set<K, std::less<K>, A>;
      using format_type                     = flatbuf;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);
      using base::base;
      view() = default;

      PSIO_FLATBUF_VIEW_ITERATION_

      // Key i (i < size()).
      [[nodiscard]] key_arg key(std::size_t i) const noexcept { return key_arg((*this)[i]); }

      // Index of the first key that is not less than `k`; size() if none.
      [[nodiscard]] std::size_t lower_bound(const key_arg& k) const noexcept
      {
         std::size_t lo = 0, hi = this->size();
         while (lo < hi)
         {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (key(mid) < k)
               lo = mid + 1;
            else
               hi = mid;
         }
         return lo;
      }
      // Index of `k`, or size().
      [[nodiscard]] std::size_t find(const key_arg& k) const noexcept
      {
         const std::size_t i = lower_bound(k);
         return i < this->size() && key(i) == k ? i : this->size();
      }
      [[nodiscard]] bool contains(const key_arg& k) const noexcept
      {
         return find(k) != this->size();
      }

      [[nodiscard]] element_type unpack() const
      {
         element_type out;
         for (std::size_t i = 0; i < this->size(); ++i)
            out.insert(out.end(), K(key(i)));
         return out;
      }
   };

   // ── std::map<K, V>: a vector of { key; value } tables sorted by key ─────
   template <typename K, typename V, typename A, storage Store>
   class view<std::map<K, V, std::less<K>, A>, flatbuf, Store>
   {
      using entry_def = detail::flatbuf_impl::entry_def<K, V>;
      using key_arg   = detail::flatbuf_impl::key_arg<K>;

      detail::flatbuf_impl::vec_ref<Store> v_;

      [[nodiscard]] detail::flatbuf_impl::table_ref<Store> entry(std::size_t i) const noexcept
      {
         if constexpr (detail::flatbuf_impl::safe_v<Store>)
         {
            if (i >= v_.count)
               return {};
         }
         return detail::flatbuf_impl::table_at(
            v_.f, detail::flatbuf_impl::follow_in(v_.f, v_.data + i * 4));
      }

    public:
      using element_type                    = std::map<K, V, std::less<K>, A>;
      using format_type                     = flatbuf;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);
      using key_type    = decltype(detail::flatbuf_impl::read_member<entry_def, 0, Store>(
         std::declval<const detail::flatbuf_impl::table_ref<Store>&>()));
      using mapped_type = decltype(detail::flatbuf_impl::read_member<entry_def, 1, Store>(
         std::declval<const detail::flatbuf_impl::table_ref<Store>&>()));
      using value_type  = std::pair<key_type, mapped_type>;

      view() = default;
      explicit view(const detail::flatbuf_impl::vec_ref<Store>& v) noexcept : v_(v) {}

      [[nodiscard]] std::size_t size() const noexcept { return v_.count; }
      [[nodiscard]] bool        empty() const noexcept { return v_.count == 0; }
      [[nodiscard]] std::span<const char> _psio_data() const noexcept
      {
         return {reinterpret_cast<const char*>(v_.data), std::size_t{v_.count} * 4};
      }

      // Key / mapped value / both of entry i (i < size()).
      [[nodiscard]] key_type key(std::size_t i) const noexcept
      {
         return detail::flatbuf_impl::read_member<entry_def, 0, Store>(entry(i));
      }
      [[nodiscard]] mapped_type value(std::size_t i) const noexcept
      {
         return detail::flatbuf_impl::read_member<entry_def, 1, Store>(entry(i));
      }
      [[nodiscard]] value_type operator[](std::size_t i) const noexcept
      {
         if constexpr (detail::flatbuf_impl::safe_v<Store>)
         {
            if (i >= v_.count)
               return value_type{};
         }
         const auto e = entry(i);
         return {detail::flatbuf_impl::read_member<entry_def, 0, Store>(e),
                 detail::flatbuf_impl::read_member<entry_def, 1, Store>(e)};
      }

      PSIO_FLATBUF_VIEW_ITERATION_

      // Index of the first entry whose key is not less than `k`.
      [[nodiscard]] std::size_t lower_bound(const key_arg& k) const noexcept
      {
         std::size_t lo = 0, hi = size();
         while (lo < hi)
         {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (key_arg(key(mid)) < k)
               lo = mid + 1;
            else
               hi = mid;
         }
         return lo;
      }
      // Index of the entry with key `k`, or size().
      [[nodiscard]] std::size_t find(const key_arg& k) const noexcept
      {
         const std::size_t i = lower_bound(k);
         return i < size() && key_arg(key(i)) == k ? i : size();
      }
      [[nodiscard]] bool contains(const key_arg& k) const noexcept { return find(k) != size(); }
      // The mapped value of `k`, if there is one.
      [[nodiscard]] std::optional<mapped_type> at(const key_arg& k) const noexcept
      {
         const std::size_t i = find(k);
         if (i == size())
            return std::nullopt;
         return value(i);
      }
      // The mapped value of `k` (unpacked), or `fallback`.
      template <typename U>
      [[nodiscard]] V value_or(const key_arg& k, U&& fallback) const
      {
         const std::size_t i = find(k);
         if (i == size())
            return static_cast<V>(std::forward<U>(fallback));
         if constexpr (detail::flatbuf_impl::Scalar<V>)
            return value(i);
         else
            return value(i).unpack();
      }

      [[nodiscard]] element_type unpack() const
      {
         element_type out;
         for (std::size_t i = 0; i < size(); ++i)
         {
            std::pair<K, V> e{};
            if (const auto t = entry(i); t.tbl)
               detail::flatbuf_impl::decode_table<entry_def>(t.tbl, e);
            out.insert(out.end(), std::move(e));
         }
         return out;
      }
   };

#undef PSIO_FLATBUF_VIEW_ITERATION_

   // ── std::optional<X>: the field; absent is "no value" ───────────────────
   template <typename X, storage Store>
   class view<std::optional<X>, flatbuf, Store>
   {
      const std::uint8_t*                                       cell_ = nullptr;
      [[no_unique_address]] detail::flatbuf_impl::frame<Store> f_{};

      using payload = detail::flatbuf_impl::read_t<X, Store>;

    public:
      using element_type                    = std::optional<X>;
      using format_type                     = flatbuf;
      using value_type                      = X;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);

      view() = default;
      view(const std::uint8_t* cell, const detail::flatbuf_impl::frame<Store>& f) noexcept
          : cell_(cell), f_(f)
      {
      }

      [[nodiscard]] std::span<const char> _psio_data() const noexcept
      {
         return {reinterpret_cast<const char*>(cell_),
                 cell_ ? detail::flatbuf_impl::inline_size<X>() : std::size_t{0}};
      }

      [[nodiscard]] bool     has_value() const noexcept { return cell_ != nullptr; }
      [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

      // Precondition: has_value().
      [[nodiscard]] payload operator*() const noexcept
      {
         return detail::flatbuf_impl::read_cell<X, Store>(f_, cell_);
      }
      [[nodiscard]] payload get() const noexcept { return **this; }
      [[nodiscard]] detail::arrow_proxy<payload> operator->() const noexcept
         requires (!detail::flatbuf_impl::Scalar<X>)
      {
         return {**this};
      }
      [[nodiscard]] payload value() const
      {
         if (!has_value())
            detail::view_absent("psio::view<std::optional<T>>::value(): no value");
         return **this;
      }
      template <typename U>
      [[nodiscard]] X value_or(U&& fallback) const
      {
         if (!has_value())
            return static_cast<X>(std::forward<U>(fallback));
         if constexpr (detail::flatbuf_impl::Scalar<X>)
            return **this;
         else
            return (**this).unpack();
      }
      [[nodiscard]] element_type unpack() const
      {
         if (!has_value())
            return std::nullopt;
         if constexpr (detail::flatbuf_impl::Scalar<X>)
            return element_type{**this};
         else
            return element_type{(**this).unpack()};
      }
   };

   // ── Union (std::variant member) ─────────────────────────────────────────
   template <typename... Ts, typename Def, std::size_t I, storage Store>
   class view<std::variant<Ts...>, detail::flatbuf_impl::in<Def, I>, Store>
   {
      using V  = std::variant<Ts...>;
      using VT = detail::flatbuf_impl::variant_traits<V>;

      detail::flatbuf_impl::table_ref<Store> t_;

      template <typename Alt>
      using alt_read_t = std::conditional_t<std::is_same_v<Alt, std::monostate>, std::monostate,
                                            view<Alt, flatbuf, Store>>;
      template <std::size_t J>
      using alt_t = alt_read_t<std::variant_alternative_t<J, V>>;

      // The union type byte: 0 is NONE, k the k-th table of the union.
      [[nodiscard]] std::size_t wire_type() const noexcept
      {
         const std::uint8_t* const c =
            t_.cell(detail::flatbuf_impl::vt_of(Def::slot(I)), 1);
         return c ? *c : 0;
      }
      static constexpr std::size_t first_type = VT::has_none ? 0 : 1;

      template <std::size_t J>
      [[nodiscard]] alt_t<J> alt() const noexcept
      {
         using Alt = std::variant_alternative_t<J, V>;
         if constexpr (std::is_same_v<Alt, std::monostate>)
            return {};
         else
         {
            if (wire_type() != J + first_type)
               return alt_t<J>{};
            const std::uint8_t* const cell =
               t_.cell(detail::flatbuf_impl::vt_of(Def::slot(I) + 1), 4);
            return cell ? detail::flatbuf_impl::read_cell<Alt, Store>(t_.f, cell) : alt_t<J>{};
         }
      }

    public:
      using element_type                    = V;
      using format_type                     = detail::flatbuf_impl::in<Def, I>;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);

      view() = default;
      explicit view(const detail::flatbuf_impl::table_ref<Store>& t) noexcept : t_(t) {}

      [[nodiscard]] std::span<const char> _psio_data() const noexcept
      {
         return {reinterpret_cast<const char*>(t_.tbl), t_.object_size()};
      }

      // Which alternative is set. A union that is NONE (or absent) in a
      // variant without std::monostate reads as alternative 0, whose view
      // reads as a default-constructed value.
      [[nodiscard]] std::size_t index() const noexcept
      {
         const std::size_t w = wire_type();
         return w >= first_type && w - first_type < sizeof...(Ts) ? w - first_type : 0;
      }

      // Calls fn with the active alternative: std::monostate for NONE, its
      // view otherwise.
      template <typename F>
      auto visit(F&& fn) const
      {
         using R                 = std::common_type_t<std::invoke_result_t<F&, alt_read_t<Ts>>...>;
         const std::size_t which = index();
         return [&]<std::size_t... Js>(std::index_sequence<Js...>) -> R {
            if constexpr (std::is_void_v<R>)
               (void)((which == Js ? (fn(alt<Js>()), true) : false) || ...);
            else
            {
               R result{};
               (void)((which == Js ? (result = fn(alt<Js>()), true) : false) || ...);
               return result;
            }
         }(std::index_sequence_for<Ts...>{});
      }

      // Alternative J if it is the active one.
      template <std::size_t J>
      [[nodiscard]] std::optional<alt_t<J>> get_if() const noexcept
      {
         if (index() != J)
            return std::nullopt;
         return alt<J>();
      }
      template <typename Alt>
         requires ((std::size_t{std::is_same_v<Alt, Ts>} + ...) == 1)
      [[nodiscard]] auto get_if() const noexcept
      {
         constexpr std::size_t J = [] {
            std::size_t i = 0, found = 0;
            ((std::is_same_v<Alt, Ts> ? void(found = i++) : void(++i)), ...);
            return found;
         }();
         return this->template get_if<J>();
      }
      template <typename Alt>
         requires ((std::size_t{std::is_same_v<Alt, Ts>} + ...) == 1)
      [[nodiscard]] bool holds_alternative() const noexcept
      {
         return this->template get_if<Alt>().has_value();
      }

      [[nodiscard]] element_type unpack() const
      {
         element_type out;
         if (wire_type() == 0)
            return out;
         const std::size_t which = index();
         [&]<std::size_t... Js>(std::index_sequence<Js...>) {
            (void)((which == Js
                       ? ([&] {
                             if constexpr (!std::is_same_v<std::variant_alternative_t<Js, V>,
                                                           std::monostate>)
                                out.template emplace<Js>(alt<Js>().unpack());
                          }(),
                          true)
                       : false) ||
                   ...);
         }(std::index_sequence_for<Ts...>{});
         return out;
      }
   };

   template <typename Alt, typename... Ts, typename Def, std::size_t I, storage Store>
   [[nodiscard]] auto get_if(
      const view<std::variant<Ts...>, detail::flatbuf_impl::in<Def, I>, Store>& v) noexcept
   {
      return v.template get_if<Alt>();
   }
   template <typename Alt, typename... Ts, typename Def, std::size_t I, storage Store>
   [[nodiscard]] bool holds_alternative(
      const view<std::variant<Ts...>, detail::flatbuf_impl::in<Def, I>, Store>& v) noexcept
   {
      return v.template holds_alternative<Alt>();
   }

   // ══════════════════════════════════════════════════════════════════════
   // Writing in place (see the head of this file)
   // ══════════════════════════════════════════════════════════════════════

   namespace detail::flatbuf_impl {

      // A scalar stored in the buffer: read, or overwrite. `cell` is null
      // for a field the buffer does not hold.
      template <Scalar S>
      class scalar_ref
      {
         std::uint8_t* cell_    = nullptr;
         S             default_ = {};

       public:
         scalar_ref() = default;
         scalar_ref(std::uint8_t* cell, S def) noexcept : cell_(cell), default_(def) {}

         // Whether the buffer holds the field (and `set` can write it).
         [[nodiscard]] bool has_value() const noexcept { return cell_ != nullptr; }
         [[nodiscard]] S    get() const noexcept
         {
            return cell_ ? load_scalar<S>(cell_) : default_;
         }
         operator S() const noexcept { return get(); }
         // False when the buffer does not hold the field: nothing is written.
         bool set(S v) const noexcept
         {
            if (cell_ == nullptr)
               return false;
            store_scalar<S>(cell_, v);
            return true;
         }
      };

      [[gnu::always_inline]] inline std::uint8_t* writable(const std::uint8_t* p) noexcept
      {
         return const_cast<std::uint8_t*>(p);
      }

      template <typename E>
      class mutable_list;

      // What writing through a value of type F at `cell` is: a reference
      // for a scalar, a mutable view for a struct / table / vector of those,
      // the read view for anything else.
      template <typename F>
      inline constexpr bool list_writable_v = false;
      template <typename E, typename A>
      inline constexpr bool list_writable_v<std::vector<E, A>> =
         (Scalar<E> || Record<E>) && !std::is_same_v<E, std::string>;
      template <typename E, std::size_t N>
      inline constexpr bool list_writable_v<std::array<E, N>> = Scalar<E> || Record<E>;

      template <typename F>
      [[gnu::always_inline]] inline auto write_cell(const safe_frame&, const std::uint8_t* cell,
                                                    F def = {}) noexcept
         requires Scalar<F>
      {
         return scalar_ref<F>{writable(cell), def};
      }
      template <typename F>
      [[gnu::always_inline]] inline auto write_cell(const safe_frame&   f,
                                                    const std::uint8_t* cell) noexcept
         requires (!Scalar<F>)
      {
         if constexpr (is_struct_v<F>)
            return ::psio::mutable_view<F, ::psio::flatbuf>{struct_ref{cell}};
         else if constexpr (list_writable_v<F>)
            return mutable_list<typename F::value_type>{
               cell ? vector_at<storage::const_borrow>(
                         f, follow_in(f, cell), inline_size<typename F::value_type>())
                    : vec_ref<storage::const_borrow>{}};
         else if constexpr (Table<F>)
            return ::psio::mutable_view<F, ::psio::flatbuf>{
               cell ? table_at(f, follow_in(f, cell)) : table_ref<storage::const_borrow>{}};
         else
            return cell ? read_cell<F, storage::const_borrow>(f, cell)
                        : read_t<F, storage::const_borrow>{};
      }

      // The elements of a vector of scalars, structs or tables.
      template <typename E>
      class mutable_list
      {
         vec_ref<storage::const_borrow> v_;

       public:
         mutable_list() = default;
         explicit mutable_list(const vec_ref<storage::const_borrow>& v) noexcept : v_(v) {}

         [[nodiscard]] std::size_t size() const noexcept { return v_.count; }
         [[nodiscard]] bool        empty() const noexcept { return v_.count == 0; }
         // Element i; past the end, a reference / view of nothing.
         [[nodiscard]] auto operator[](std::size_t i) const noexcept
         {
            return write_cell<E>(v_.f, i < v_.count ? v_.data + i * inline_size<E>() : nullptr);
         }
         [[nodiscard]] auto at(std::size_t i) const noexcept { return (*this)[i]; }
         // The read view of the same vector.
         [[nodiscard]] view<std::vector<E>, ::psio::flatbuf, storage::const_borrow> as_view()
            const noexcept
         {
            return view<std::vector<E>, ::psio::flatbuf, storage::const_borrow>{v_};
         }
      };

      template <typename T>
      class mutable_table_projection
      {
       protected:
         table_ref<storage::const_borrow> t_;  // over mutable bytes

       public:
         using ref_type = table_ref<storage::const_borrow>;
         mutable_table_projection() = default;
         explicit mutable_table_projection(const ref_type& t) noexcept : t_(t) {}

         template <std::size_t I>
         [[nodiscard]] auto _psio_member() const noexcept
         {
            using Def = record_def<T>;
            using F   = typename Def::template member<I>;
            if constexpr (is_variant_v<F>)
               return read_member<Def, I, storage::const_borrow>(t_);
            else
            {
               const std::uint8_t* const cell =
                  Def::template deprecated<I> ? nullptr
                                              : t_.cell(vt_of(Def::slot(I)), inline_size<F>());
               if constexpr (Scalar<F>)
                  return write_cell<F>(t_.f, cell, Def::template default_of<I>());
               else if constexpr (is_optional_v<F>)
               {
                  using X = typename F::value_type;
                  if constexpr (Scalar<X> || is_struct_v<X> || list_writable_v<X> || Table<X>)
                     return write_cell<X>(t_.f, cell);
                  else
                     return read_member<Def, I, storage::const_borrow>(t_);
               }
               else
                  return write_cell<F>(t_.f, cell);
            }
         }
         template <std::size_t I, auto MemberPtr>
         [[nodiscard]] auto get() const noexcept
         {
            return _psio_member<I>();
         }
      };

      template <typename T>
      class mutable_struct_projection
      {
       protected:
         struct_ref t_;  // over mutable bytes

       public:
         using ref_type = struct_ref;
         mutable_struct_projection() = default;
         explicit mutable_struct_projection(const ref_type& s) noexcept : t_(s) {}

         template <std::size_t I>
         [[nodiscard]] auto _psio_member() const noexcept
         {
            using L                         = struct_layout<T>;
            using F                         = typename L::template member<I>;
            const std::uint8_t* const cell = t_.p ? t_.p + L::offset(I) : nullptr;
            if constexpr (Scalar<F>)
               return scalar_ref<F>{writable(cell), member_default<T, I>()};
            else if constexpr (is_array_v<F>)
               return mutable_list<typename F::value_type>{vec_ref<storage::const_borrow>{
                  cell, cell ? static_cast<std::uint32_t>(array_traits<F>::count) : 0u, {}}};
            else
               return ::psio::mutable_view<F, ::psio::flatbuf>{struct_ref{cell}};
         }
         template <std::size_t I, auto MemberPtr>
         [[nodiscard]] auto get() const noexcept
         {
            return _psio_member<I>();
         }
      };

      template <typename T>
      using mutable_projection =
         std::conditional_t<is_struct_v<T>, mutable_struct_projection<T>, mutable_table_projection<T>>;

   }  // namespace detail::flatbuf_impl

   template <>
   inline constexpr bool detail::helpers::mutable_view_format_v<::psio::flatbuf> = true;

   // In-place writes to the fixed-size values a buffer holds. Navigation is
   // by the same names as the read view: a scalar member is a reference
   // with get() / set() / has_value(), a struct or table member is a mutable
   // view of it, a vector of scalars / structs / tables is a list of those,
   // anything else reads as in the safe read view.
   template <typename T>
      requires detail::flatbuf_impl::Record<T>
   class mutable_view<T, flatbuf>
       : public detail::named_base_t<T, detail::flatbuf_impl::mutable_projection<T>>
   {
      using projection = detail::flatbuf_impl::mutable_projection<T>;
      using base       = detail::named_base_t<T, projection>;
      using ref_type   = typename projection::ref_type;

    public:
      using element_type = T;
      using format_type  = flatbuf;

      mutable_view() = default;
      // The root table of a buffer (not size-prefixed).
      explicit mutable_view(std::span<char> buffer) noexcept
         requires detail::flatbuf_impl::Table<T>
          : base(projection{detail::flatbuf_impl::root_table<storage::const_borrow>(
               std::span<const char>{buffer.data(), buffer.size()})})
      {
      }
      explicit mutable_view(std::vector<char>& buffer) noexcept
         requires detail::flatbuf_impl::Table<T>
          : mutable_view(std::span<char>{buffer})
      {
      }
      explicit mutable_view(const ref_type& r) noexcept : base(projection{r}) {}

      template <std::size_t N>
      [[nodiscard]] auto get() const noexcept
      {
         return this->projection::template _psio_member<N>();
      }
      // The (safe) read view of the same record.
      [[nodiscard]] view<T, flatbuf, storage::const_borrow> as_view() const noexcept
      {
         return view<T, flatbuf, storage::const_borrow>{this->projection::t_};
      }
      [[nodiscard]] T unpack() const { return as_view().unpack(); }

      // A struct is written whole.
      bool set(const T& v) const noexcept
         requires detail::flatbuf_impl::is_struct_v<T>
      {
         auto* p = detail::flatbuf_impl::writable(this->projection::t_.p);
         if (p == nullptr)
            return false;
         std::memset(p, 0, detail::flatbuf_impl::struct_layout<T>::size);
         detail::flatbuf_impl::write_fixed<T>(p, v);
         return true;
      }
      [[nodiscard]] bool has_value() const noexcept
         requires detail::flatbuf_impl::is_struct_v<T>
      {
         return this->projection::t_.p != nullptr;
      }
   };

   template <typename T>
   [[nodiscard]] view<T, flatbuf, storage::const_borrow> as_view(
      const mutable_view<T, flatbuf>& m) noexcept
   {
      return m.as_view();
   }

}  // namespace psio
