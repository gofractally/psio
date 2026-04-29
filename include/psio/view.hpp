#pragma once
//
// psio/view.hpp — unified `view<T, Fmt, Store>` zero-copy read surface.
//
// Design rule: the view templates in this file are written ONCE per
// shape (Primitive, string, vector, optional, variant, …). Each format
// plugs in its own wire-layout knowledge by specializing
// `view_layout::traits<Fmt>`. Adding a new format never requires
// duplicating view templates — only writing a new traits specialization.
//
// Access model (matches std::string_view's relationship to std::string):
//   - Primitive / arithmetic  → `.get()` returns T by value (memcpy).
//   - std::string             → `.view_()` returns std::string_view.
//   - std::optional<T>        → `.has_value()`, `operator*`, `.get()`.
//                                 Returns view<T, Fmt> for the payload.
//   - std::variant<Ts...>     → `.index()`, `.visit(fn)`. `fn` receives
//                                 a view<Ts_i, Fmt> for the active alt.
//   - std::vector<T>          → `.size()`, `operator[](i)`, iterator.
//                                 Returns view<T, Fmt> per element.
//   - Record                  → per-field accessors (generated per-format;
//                                 lands in a follow-up).
//
// All composite accessors return **sub-views into the same backing
// span** — no heap allocation, no intermediate decode, no payload copy.
// Lifetime: the view is valid while the original span is valid.
//
// To add a format's view support, specialize `view_layout::traits<Fmt>`
// in that format's companion header (e.g. `ssz_view.hpp`).

#include <psio/buffer.hpp>
#include <psio/shapes.hpp>
#include <psio/storage.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
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

   // Forward declaration of the unified view template. Partial
   // specializations for each shape appear below.  The default for
   // Store lives on the canonical forward decl in `format.hpp`; this
   // re-declaration must NOT repeat it (template-default-redefinition
   // is a hard error).
   template <typename T, typename Fmt, storage Store>
   class view;

   // ── view_layout traits ────────────────────────────────────────────────
   //
   // Per-format wire-layout policy used by the composite view
   // specializations. A format opts in by specializing this trait and
   // providing any of the member functions the composite views call.
   // Formats that don't participate simply never instantiate the
   // relevant view.
   //
   // Required members (when the format supports the shape):
   //
   //   template <typename T>
   //   static std::size_t vector_count(std::span<const char>) noexcept;
   //
   //   template <typename T>
   //   static std::span<const char>
   //   vector_element_span(std::span<const char>, std::size_t i) noexcept;
   //
   //   template <typename T>
   //   static bool optional_has_value(std::span<const char>) noexcept;
   //
   //   template <typename T>
   //   static std::span<const char>
   //   optional_payload_span(std::span<const char>) noexcept;
   //
   //   template <typename... Ts>
   //   static std::size_t variant_index(std::span<const char>) noexcept;
   //
   //   template <typename... Ts>
   //   static std::span<const char>
   //   variant_payload_span(std::span<const char>) noexcept;

   namespace view_layout {

      template <typename Fmt>
      struct traits;  // specialized per format

      template <typename Fmt, typename = void>
      struct has_vector_support : std::false_type {};

      template <typename Fmt>
      struct has_vector_support<
         Fmt,
         std::void_t<decltype(traits<Fmt>::template vector_count<int>(
            std::span<const char>{}))>> : std::true_type {};

      template <typename Fmt, typename = void>
      struct has_optional_support : std::false_type {};

      template <typename Fmt>
      struct has_optional_support<
         Fmt,
         std::void_t<decltype(traits<Fmt>::template optional_has_value<int>(
            std::span<const char>{}))>> : std::true_type {};

      template <typename Fmt, typename = void>
      struct has_variant_support : std::false_type {};

      template <typename Fmt>
      struct has_variant_support<
         Fmt,
         std::void_t<decltype(traits<Fmt>::template variant_index<int>(
            std::span<const char>{}))>> : std::true_type {};

      // has_record_support<Fmt, T> — does Fmt's traits provide
      // record_field_span<T, 0>?  Per-(Fmt, T) because some formats
      // only support record view for shapes meeting certain criteria
      // (e.g., ssz needs all-fixed-fields).  Per-T detection gives
      // SFINAE-friendly fallback for unsupported (Fmt, T) pairs.
      template <typename Fmt, typename T, typename = void>
      struct has_record_support : std::false_type {};

      // Detects either signature:
      //   record_field_span<T, N>(span)              [legacy / no root]
      //   record_field_span<T, N>(span, span root)   [pointer-following]
      template <typename Fmt, typename T>
      struct has_record_support<
         Fmt, T,
         std::void_t<decltype(traits<Fmt>::template record_field_span<T, 0>(
            std::span<const char>{}, std::span<const char>{}))>>
         : std::true_type {};

   }  // namespace view_layout

   // ── Primitive view ────────────────────────────────────────────────────
   //
   // Every format in v3 stores arithmetic scalars as raw little-endian
   // bytes (ssz, pssz, frac, bin, borsh, bincode, wit). Zig-zag-varint
   // formats (avro) need their own specialization; this primary takes
   // the common path.
   template <Primitive T, typename Fmt, storage Store>
   class view<T, Fmt, Store>
   {
      std::span<const char> data_;

     public:
      using element_type                    = T;
      using format_type                     = Fmt;
      static constexpr storage storage_kind = Store;

      view() = default;
      explicit view(std::span<const char> s) noexcept : data_(s) {}
      // Indirect-friendly ctor: the parent vector / record view passes
      // a root span for formats that follow offsets (flatbuf, capnp).
      // Primitives never indirect, so the root is discarded.
      view(std::span<const char> s, std::span<const char> /*r*/) noexcept
         : data_(s)
      {
      }

      [[nodiscard]] std::span<const char> _psio_data() const noexcept
      {
         return data_;
      }

      [[nodiscard]] T get() const noexcept
      {
         T out{};
         if (data_.size() >= sizeof(T))
            std::memcpy(&out, data_.data(), sizeof(T));
         return out;
      }
   };

   // ── Byte-sequence / string view ───────────────────────────────────────
   //
   // Parent views strip any format-specific framing (e.g. bin's u32
   // length prefix) before constructing a view<std::string, Fmt>, so
   // the span is always payload-only raw UTF-8.
   template <typename Fmt, storage Store>
   class view<std::string, Fmt, Store>
   {
      std::span<const char> data_;

     public:
      using element_type                    = std::string;
      using format_type                     = Fmt;
      static constexpr storage storage_kind = Store;

      view() = default;
      explicit view(std::span<const char> s) noexcept : data_(s) {}
      // Indirect-friendly ctor: same rationale as the primitive view —
      // strings are leaf payloads, so the root span is discarded.
      view(std::span<const char> s, std::span<const char> /*r*/) noexcept
         : data_(s)
      {
      }

      [[nodiscard]] std::span<const char> _psio_data() const noexcept
      {
         return data_;
      }

      [[nodiscard]] std::string_view view_() const noexcept
      {
         return std::string_view{data_.data(), data_.size()};
      }
   };

   // ── view<std::vector<T>, Fmt, Store> ──────────────────────────────────
   //
   // Delegates to view_layout::traits<Fmt>::vector_count<T> and
   // vector_element_span<T> for per-format wire parsing. Every accessor
   // returns a sub-view into the same backing span.
   template <typename T, typename Fmt, storage Store>
      requires view_layout::has_vector_support<Fmt>::value
   class view<std::vector<T>, Fmt, Store>
   {
      std::span<const char> data_;
      std::span<const char> root_;

     public:
      using element_type                    = std::vector<T>;
      using format_type                     = Fmt;
      using value_type                      = T;
      static constexpr storage storage_kind = Store;

      view() = default;
      explicit view(std::span<const char> s) noexcept : data_(s), root_(s) {}
      view(std::span<const char> s, std::span<const char> r) noexcept
         : data_(s), root_(r)
      {
      }

      [[nodiscard]] std::span<const char> _psio_data() const noexcept
      {
         return data_;
      }
      [[nodiscard]] std::span<const char> _psio_root() const noexcept
      {
         return root_;
      }

      [[nodiscard]] std::size_t size() const noexcept
      {
         return view_layout::traits<Fmt>::template vector_count<T>(data_);
      }

      [[nodiscard]] bool empty() const noexcept { return size() == 0; }

      [[nodiscard]] view<T, Fmt, Store> operator[](std::size_t i) const noexcept
      {
         return view<T, Fmt, Store>{
            view_layout::traits<Fmt>::template vector_element_span<T>(data_, i),
            root_};
      }

      [[nodiscard]] view<T, Fmt, Store> at(std::size_t i) const noexcept
      {
         return i < size() ? (*this)[i] : view<T, Fmt, Store>{};
      }

      class iterator
      {
         const view* owner_ = nullptr;
         std::size_t i_     = 0;

        public:
         using iterator_category = std::forward_iterator_tag;
         using value_type        = view<T, Fmt, Store>;
         using difference_type   = std::ptrdiff_t;
         using pointer           = void;
         using reference         = value_type;

         iterator() = default;
         iterator(const view& o, std::size_t i) noexcept : owner_(&o), i_(i) {}

         [[nodiscard]] value_type operator*() const noexcept
         {
            return (*owner_)[i_];
         }
         iterator& operator++() noexcept
         {
            ++i_;
            return *this;
         }
         iterator operator++(int) noexcept
         {
            auto tmp = *this;
            ++i_;
            return tmp;
         }
         bool operator==(const iterator& other) const noexcept = default;
      };

      [[nodiscard]] iterator begin() const noexcept { return {*this, 0}; }
      [[nodiscard]] iterator end() const noexcept { return {*this, size()}; }
   };

   // ── view<std::optional<T>, Fmt, Store> ────────────────────────────────
   template <typename T, typename Fmt, storage Store>
      requires view_layout::has_optional_support<Fmt>::value
   class view<std::optional<T>, Fmt, Store>
   {
      std::span<const char> data_;

     public:
      using element_type                    = std::optional<T>;
      using format_type                     = Fmt;
      using value_type                      = T;
      static constexpr storage storage_kind = Store;

      view() = default;
      explicit view(std::span<const char> s) noexcept : data_(s) {}

      [[nodiscard]] std::span<const char> _psio_data() const noexcept
      {
         return data_;
      }

      [[nodiscard]] bool has_value() const noexcept
      {
         return view_layout::traits<Fmt>::template optional_has_value<T>(data_);
      }

      [[nodiscard]] explicit operator bool() const noexcept
      {
         return has_value();
      }

      // Precondition: has_value(). Returns a view over the payload bytes.
      [[nodiscard]] view<T, Fmt, Store> operator*() const noexcept
      {
         return view<T, Fmt, Store>{
            view_layout::traits<Fmt>::template optional_payload_span<T>(data_)};
      }

      [[nodiscard]] view<T, Fmt, Store> get() const noexcept { return **this; }
   };

   // ── view<std::variant<Ts...>, Fmt, Store> ─────────────────────────────
   template <typename... Ts, typename Fmt, storage Store>
      requires view_layout::has_variant_support<Fmt>::value
   class view<std::variant<Ts...>, Fmt, Store>
   {
      std::span<const char> data_;

     public:
      using element_type                    = std::variant<Ts...>;
      using format_type                     = Fmt;
      static constexpr storage storage_kind = Store;

      view() = default;
      explicit view(std::span<const char> s) noexcept : data_(s) {}

      [[nodiscard]] std::span<const char> _psio_data() const noexcept
      {
         return data_;
      }

      [[nodiscard]] std::size_t index() const noexcept
      {
         return view_layout::traits<Fmt>::template variant_index<Ts...>(data_);
      }

      // visit(fn) — dispatches on the active index, calling `fn` with
      // a view<Ts_i, Fmt, Store> for the active alternative.
      template <typename F>
      auto visit(F&& fn) const
      {
         auto payload =
            view_layout::traits<Fmt>::template variant_payload_span<Ts...>(
               data_);
         return visit_impl(std::forward<F>(fn), payload,
                            std::index_sequence_for<Ts...>{});
      }

     private:
      template <typename F, std::size_t... Is>
      auto visit_impl(F&& fn, std::span<const char> payload,
                       std::index_sequence<Is...>) const
      {
         using R = std::common_type_t<decltype(std::forward<F>(fn)(
            std::declval<view<Ts, Fmt, Store>>()))...>;

         const std::size_t idx = index();
         if constexpr (std::is_void_v<R>)
         {
            ((idx == Is
                 ? (std::forward<F>(fn)(view<Ts, Fmt, Store>{payload}), true)
                 : false) ||
             ...);
         }
         else
         {
            R    result{};
            bool matched = false;
            ((idx == Is
                 ? (result = std::forward<F>(fn)(
                       view<Ts, Fmt, Store>{payload}),
                    matched = true)
                 : false) ||
             ...);
            (void)matched;
            return result;
         }
      }
   };

   // ── view<T, Fmt, Store> for Reflected struct T ────────────────────────
   //
   // A Reflected struct view exposes per-field accessors via `get<N>()`.
   // The format's traits provide `record_field_span<T, N>(span)`, which
   // returns the byte range that holds field N — for fixed-layout formats
   // this is a compile-time offset/size; for formats with extensibility
   // headers or offset tables it does the necessary runtime walk.
   //
   // `get<N>()` returns:
   //   - the field value directly if the field is a Primitive (memcpy)
   //   - a sub-view<FieldT, Fmt, Store> for everything else (string,
   //     vector, optional, variant, nested record).  The sub-view shares
   //     the parent's backing span — no allocation, no decode.
   //
   // Selection: only enabled when traits<Fmt> defines record_field_span<T, 0>.
   // Formats that don't (or shapes the format can't handle, e.g., ssz on
   // a record with a variable field but no offset-table support) fall
   // through to whatever caller mechanism handles the unsupported case.
   template <typename T, typename Fmt, storage Store>
      requires Reflected<T>
            && view_layout::has_record_support<Fmt, T>::value
   class view<T, Fmt, Store>
   {
      // data_ — the bytes of THIS record (a slice of the encoded buffer).
      // root_ — the absolute encoded buffer, used by formats that follow
      //         pointers between record fields and heap regions
      //         (canonical-ABI / wit).  Defaults to data_ for formats
      //         that don't need pointer-following (ssz, pssz).
      std::span<const char> data_;
      std::span<const char> root_;

     public:
      using element_type                    = T;
      using format_type                     = Fmt;
      static constexpr storage storage_kind = Store;

      view() = default;
      explicit view(std::span<const char> s) noexcept : data_(s), root_(s) {}
      view(std::span<const char> s, std::span<const char> r) noexcept
         : data_(s), root_(r)
      {
      }

      [[nodiscard]] std::span<const char> _psio_data() const noexcept
      {
         return data_;
      }
      [[nodiscard]] std::span<const char> _psio_root() const noexcept
      {
         return root_;
      }

      template <std::size_t N>
      [[nodiscard]] auto get() const noexcept
      {
         using FieldT = std::remove_cvref_t<
            typename ::psio::reflect<T>::template member_type<N>>;
         auto field_span =
            view_layout::traits<Fmt>::template record_field_span<T, N>(
               data_, root_);
         if constexpr (std::is_arithmetic_v<FieldT>
                       || std::is_same_v<FieldT, bool>)
         {
            FieldT out{};
            if (field_span.size() >= sizeof(FieldT))
               std::memcpy(&out, field_span.data(), sizeof(FieldT));
            return out;
         }
         else
         {
            return view<FieldT, Fmt, Store>{field_span, root_};
         }
      }
   };

   // ── Free-function storage API for views ───────────────────────────────

   template <typename T, typename Fmt, storage S>
      requires requires(const view<T, Fmt, S>& v) { v._psio_data(); }
   [[nodiscard]] std::span<const char> bytes(const view<T, Fmt, S>& v) noexcept
   {
      return v._psio_data();
   }

   template <typename T, typename Fmt, storage S>
      requires requires(const view<T, Fmt, S>& v) { v._psio_data(); }
   [[nodiscard]] std::size_t size(const view<T, Fmt, S>& v) noexcept
   {
      return v._psio_data().size();
   }

   template <typename T, typename Fmt, storage S>
   [[nodiscard]] constexpr Fmt format_of(const view<T, Fmt, S>&) noexcept
   {
      return {};
   }

   // ── Construct a view from a buffer ────────────────────────────────────
   template <typename T, typename Fmt, storage S>
   [[nodiscard]] view<T, Fmt, storage::const_borrow>
   as_view(const buffer<T, Fmt, S>& b) noexcept
   {
      return view<T, Fmt, storage::const_borrow>{bytes(b)};
   }

}  // namespace psio
