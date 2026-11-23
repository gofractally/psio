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
// Access model (std::string_view's relationship to std::string):
//   - scalar (arithmetic,     → `.get()` returns T by value; the view also
//     enum, time, uint256)      converts to T implicitly.
//   - std::string             → `.view_()` returns std::string_view; the view
//                               converts to it implicitly.
//   - std::optional<T>        → `.has_value()`, `operator bool`, `operator*`,
//                               `operator->`, `.get()`, `.value()` (the
//                               payload's view), `.value_or(x)` (the value).
//   - std::variant<Ts...>     → `.index()`, `.visit(fn)`, `.get_if<I>()`,
//                               `.get_if<Alt>()`, `.holds_alternative<Alt>()`;
//                               `fn` receives the active alternative's view.
//   - std::vector<T> /        → `.size()`, `.empty()`, `[i]`, `.at(i)`,
//     std::array<T, N>          `.front()`, `.back()`, random-access
//                               `begin()` / `end()`, `rbegin()` / `rend()`.
//                               A view of a byte list (std::vector<char>)
//                               converts to std::span<const char>.
//   - reflected record        → one accessor per reflected member, by its
//                               name: `v.sender()`, `v.header().term()`
//                               (the member's view, or its value for an
//                               arithmetic member); `get<N>()` is the same
//                               by index.
//
// Every view has `unpack()`: the owned C++ value (`element_type`) decoded
// from the bytes — see "View to value" below — and compares with `==`
// (and `<=>` where the value type has it) against a value or another view.
//
// All composite accessors return **sub-views into the same backing
// span** — no heap allocation, no intermediate decode, no payload copy.
// Lifetime: the view is valid while the original span is valid.
//
// A view reads bytes that have been validated (psio::validate_F, or
// produced by to_F in this process): navigation and `unpack()` are trusted
// operations. On bytes that were never validated they are undefined
// behaviour, exactly like `from_F<T>(psio::trusted{bytes})`.
//
// To add a format's view support, specialize `view_layout::traits<Fmt>`
// in that format's companion header (e.g. `ssz_view.hpp`).

#include <psio/adapter.hpp>
#include <psio/buffer.hpp>
#include <psio/error.hpp>
#include <psio/format.hpp>
#include <psio/reflect.hpp>
#include <psio/shapes.hpp>
#include <psio/storage.hpp>
#include <psio/wire_type.hpp>

#include <cstddef>
#include <array>
#include <cstdint>
#include <compare>
#include <concepts>
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

   // `view` reads, and it borrows: it is a span of someone else's bytes
   // (std::string_view to buffer's std::string), valid while they are.
   // storage::const_borrow is therefore its only storage kind. Every
   // specialization instantiates this, so naming another kind is an error
   // with a pointer to the type that has that meaning, not a borrowing
   // read-only view with a misleading tag:
   //   mut_borrow → psio::mutable_view<T, Fmt>           (the view that writes)
   //   owning     → psio::buffer<T, Fmt>                 (F_buffer<T>)
   //   shared     → psio::buffer<T, Fmt, storage::shared> (shared_F<T>)
   // A buffer keeps the bytes alive; psio::as_view(buffer) reads them.
   namespace detail {
      template <storage Store>
      struct read_view_store
      {
         static_assert(Store != storage::mut_borrow,
                       "psio::view<T, Fmt, storage::mut_borrow>: psio::view is the read-only "
                       "view; the view that writes is psio::mutable_view<T, Fmt>");
         static_assert(Store != storage::owning,
                       "psio::view<T, Fmt, storage::owning>: a psio::view borrows its bytes "
                       "and never owns them; the type that owns encoded bytes is "
                       "psio::buffer<T, Fmt> (F_buffer<T>), read through psio::as_view(buffer)");
         static_assert(Store != storage::shared,
                       "psio::view<T, Fmt, storage::shared>: a psio::view borrows its bytes "
                       "and never shares ownership of them; the type that does is "
                       "psio::buffer<T, Fmt, storage::shared> (shared_F<T>), read through "
                       "psio::as_view(buffer)");
      };
   }  // namespace detail

   // ── View to value ─────────────────────────────────────────────────────
   //
   // `v.unpack()` is the owned value of what a view designates: T for a
   // view<T, Fmt>. It is the one spelling for every view — record, list,
   // string, optional, variant, tuple, nested, scalar — and it is correct
   // wherever the view came from: a member of a record with a
   // maxDynamicData cap or a bounded list is not laid out like a standalone
   // value of its type (its offsets are narrower), which the view's format
   // knows and `psio::from_F<T>(psio::trusted{psio::bytes(v)})` does not.
   //
   // `psio::value_of(x)` is the same for generic code that is handed either
   // a view or a value (a record view returns its arithmetic members by
   // value): the unpacked value of a view, x itself otherwise.
   //
   // Both are trusted operations: a view is over validated bytes.
   //
   // A format provides it as
   //   template <typename T>
   //   static T view_layout::traits<Fmt>::unpack(std::span<const char>);
   namespace detail {
      template <typename V>
      inline constexpr bool is_view_v = false;
      template <typename T, typename Fmt, storage Store>
      inline constexpr bool is_view_v<view<T, Fmt, Store>> = true;

      template <typename T, typename Fmt, storage Store>
      std::true_type  view_base_test(const view<T, Fmt, Store>*);
      std::false_type view_base_test(...);

      // A read view: a psio::view (or a class derived from one) that can
      // be unpacked.
      template <typename V>
      concept read_view =
         decltype(view_base_test(static_cast<const V*>(nullptr)))::value &&
         requires(const V& v) {
            typename V::element_type;
            { v._psio_data() } -> std::convertible_to<std::span<const char>>;
            v.unpack();
         };

      // What a comparison reads from a view: the value of a scalar, the
      // characters of a string, the unpacked value of anything else.
      template <typename V>
      [[nodiscard]] decltype(auto) view_read(const V& v)
      {
         if constexpr (requires { v.view_(); })
            return v.view_();
         else if constexpr (requires { { v.get() } -> std::same_as<typename V::element_type>; })
            return v.get();
         else
            return v.unpack();
      }

      [[noreturn]] inline void view_absent(const char* what)
      {
         ::psio::codec_abort(codec_error{what, 0, "view"});
      }

      // `operator->` of a view that hands out sub-views by value.
      template <typename V>
      struct arrow_proxy
      {
         V              value;
         [[nodiscard]] const V* operator->() const noexcept { return &value; }
      };

      // Random access over a view with size() and operator[] whose
      // elements are views (returned by value). The iterator holds a copy
      // of the list view, so it does not dangle when the list view was a
      // temporary (`for (auto x : record.items())`, `record.items().begin()`).
      template <typename V>
      class view_iterator
      {
         V           owner_{};
         std::size_t index_ = 0;

       public:
         using iterator_concept  = std::random_access_iterator_tag;
         using iterator_category = std::input_iterator_tag;
         using value_type        = decltype(std::declval<const V&>()[std::size_t{}]);
         using difference_type   = std::ptrdiff_t;
         using pointer           = arrow_proxy<value_type>;
         using reference         = value_type;

         view_iterator() = default;
         view_iterator(const V& owner, std::size_t index) noexcept : owner_(owner), index_(index)
         {
         }

         [[nodiscard]] reference operator*() const noexcept { return owner_[index_]; }
         [[nodiscard]] pointer   operator->() const noexcept { return {owner_[index_]}; }
         [[nodiscard]] reference operator[](difference_type n) const noexcept
         {
            return owner_[static_cast<std::size_t>(static_cast<difference_type>(index_) + n)];
         }
         view_iterator& operator++() noexcept { ++index_; return *this; }
         view_iterator  operator++(int) noexcept { auto copy = *this; ++index_; return copy; }
         view_iterator& operator--() noexcept { --index_; return *this; }
         view_iterator  operator--(int) noexcept { auto copy = *this; --index_; return copy; }
         view_iterator& operator+=(difference_type n) noexcept
         {
            index_ = static_cast<std::size_t>(static_cast<difference_type>(index_) + n);
            return *this;
         }
         view_iterator& operator-=(difference_type n) noexcept { return *this += -n; }
         friend view_iterator operator+(view_iterator it, difference_type n) noexcept { return it += n; }
         friend view_iterator operator+(difference_type n, view_iterator it) noexcept { return it += n; }
         friend view_iterator operator-(view_iterator it, difference_type n) noexcept { return it -= n; }
         friend difference_type operator-(const view_iterator& a, const view_iterator& b) noexcept
         {
            return static_cast<difference_type>(a.index_) - static_cast<difference_type>(b.index_);
         }
         friend bool operator==(const view_iterator& a, const view_iterator& b) noexcept
         {
            return a.index_ == b.index_;
         }
         friend std::strong_ordering operator<=>(const view_iterator& a,
                                                 const view_iterator& b) noexcept
         {
            return a.index_ <=> b.index_;
         }
         [[nodiscard]] std::size_t index() const noexcept { return index_; }
      };
   }  // namespace detail

   // The members every read view has. `unpack()`: see "View to value".
#define PSIO_VIEW_UNPACK_                                                                  \
   [[nodiscard]] element_type unpack() const                                               \
   {                                                                                       \
      return view_layout::traits<format_type>::template unpack<element_type>(_psio_data()); \
   }

   // ── view<const T, Fmt, Store> ─────────────────────────────────────────
   //
   // A view is read-only whether or not T is spelled const: view<const T,
   // Fmt> is view<T, Fmt> under another name (like std::span<const T>, the
   // const says the bytes are not written, which is true of every view). It
   // derives from view<T, Fmt, Store>, adds nothing, and converts implicitly
   // both ways, so either spelling can be passed where the other is
   // expected. Sub-views (members, elements) are the plain view<Child, …>.
   //
   // pjson's typed view (psio/pjson_typed.hpp) is a separate template
   // specialization with its own surface and is not covered.
   template <typename T, typename Fmt, storage Store>
      requires (!std::is_same_v<Fmt, pjson_format>)
   class view<const T, Fmt, Store> : public view<T, Fmt, Store>
   {
      using base = view<T, Fmt, Store>;

    public:
      using base::base;
      view() = default;
      view(const base& v) noexcept : base(v) {}
   };

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

      template <typename Fmt, typename Parent, typename = void>
      struct nested_format { using type = Fmt; };
      template <typename Fmt, typename Parent>
      struct nested_format<Fmt, Parent, std::void_t<typename traits<Fmt>::template nested_format<Parent>>>
      { using type = typename traits<Fmt>::template nested_format<Parent>; };
      template <typename Fmt, typename Parent>
      using nested_format_t = typename nested_format<Fmt, Parent>::type;

      // The format of the view of member N of record T: the format's own
      // `member_format<T, N>` when it has one (a format whose layout of a
      // member depends on which member it is), else nested_format_t.
      template <typename Fmt, typename T, std::size_t N, typename = void>
      struct member_format { using type = nested_format_t<Fmt, T>; };
      template <typename Fmt, typename T, std::size_t N>
      struct member_format<Fmt, T, N,
                           std::void_t<typename traits<Fmt>::template member_format<T, N>>>
      { using type = typename traits<Fmt>::template member_format<T, N>; };
      template <typename Fmt, typename T, std::size_t N>
      using member_format_t = typename member_format<Fmt, T, N>::type;

      template <typename Fmt, typename = void>
      struct has_vector_support : std::false_type {};

      template <typename Fmt>
      struct has_vector_support<
         Fmt,
         std::void_t<decltype(traits<Fmt>::template vector_count<int>(
            std::span<const char>{}))>> : std::true_type {};

      template <typename Fmt, typename = void>
      struct has_array_support : std::false_type {};
      template <typename Fmt>
      struct has_array_support<Fmt, std::void_t<decltype(
         traits<Fmt>::template array_element_span<int, 1>(std::span<const char>{}, 0))>>
         : std::true_type {};

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
   // (Every arithmetic type: `char`, `long` and the like are not among the
   // fixed-width names Primitive lists, and a std::vector<char> view hands
   // out views of them.)
   template <typename T, typename Fmt, storage Store>
      requires (Primitive<T> || std::is_arithmetic_v<T>)
   class view<T, Fmt, Store>
   {
      std::span<const char> data_;

     public:
      using element_type                    = T;
      using format_type                     = Fmt;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);

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
      [[nodiscard]] T unpack() const noexcept { return get(); }
      // A scalar view is its value.
      operator T() const noexcept { return get(); }
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
      static_assert(sizeof(detail::read_view_store<Store>) > 0);

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
      // The characters, borrowed. (An owned std::string is unpack().)
      operator std::string_view() const noexcept { return view_(); }
      [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
      [[nodiscard]] bool        empty() const noexcept { return data_.empty(); }
      [[nodiscard]] const char* data() const noexcept { return data_.data(); }
      [[nodiscard]] std::string unpack() const { return std::string{view_()}; }
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
      using child_format = view_layout::nested_format_t<Fmt, std::vector<T>>;
      std::span<const char> data_;
      std::span<const char> root_;

     public:
      using element_type                    = std::vector<T>;
      using format_type                     = Fmt;
      using value_type                      = T;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);

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

      [[nodiscard]] view<T, child_format, Store> operator[](std::size_t i) const noexcept
      {
         return view<T, child_format, Store>{
            view_layout::traits<Fmt>::template vector_element_span<T>(data_, i),
            root_};
      }

      [[nodiscard]] view<T, child_format, Store> at(std::size_t i) const noexcept
      {
         return i < size() ? (*this)[i] : view<T, child_format, Store>{};
      }

      // Precondition: !empty().
      [[nodiscard]] view<T, child_format, Store> front() const noexcept { return (*this)[0]; }
      [[nodiscard]] view<T, child_format, Store> back() const noexcept
      {
         return (*this)[size() - 1];
      }

      using iterator         = detail::view_iterator<view>;
      using const_iterator   = iterator;
      using reverse_iterator = std::reverse_iterator<iterator>;

      [[nodiscard]] iterator begin() const noexcept { return {*this, 0}; }
      [[nodiscard]] iterator end() const noexcept { return {*this, size()}; }
      [[nodiscard]] reverse_iterator rbegin() const noexcept { return reverse_iterator{end()}; }
      [[nodiscard]] reverse_iterator rend() const noexcept { return reverse_iterator{begin()}; }

      // A byte list is its bytes.
      operator std::span<const char>() const noexcept
         requires std::same_as<T, char>
      {
         return data_;
      }
      [[nodiscard]] const char* data() const noexcept
         requires std::same_as<T, char>
      {
         return data_.data();
      }

      PSIO_VIEW_UNPACK_
   };

   // A fixed-count array has its own logical container width. Its element
   // views still use Fmt, so automatic PSSZ selects the element's own width.
   template <typename T, std::size_t N, typename Fmt, storage Store>
      requires view_layout::has_array_support<Fmt>::value
   class view<std::array<T, N>, Fmt, Store>
   {
      using child_format = view_layout::nested_format_t<Fmt, std::array<T, N>>;
      std::span<const char> data_;
      std::span<const char> root_;
     public:
      using element_type = std::array<T, N>;
      using format_type = Fmt;
      using value_type = T;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);
      view() = default;
      explicit view(std::span<const char> bytes) noexcept : data_(bytes), root_(bytes) {}
      view(std::span<const char> bytes, std::span<const char> root) noexcept : data_(bytes), root_(root) {}
      [[nodiscard]] std::span<const char> _psio_data() const noexcept { return data_; }
      [[nodiscard]] std::span<const char> _psio_root() const noexcept { return root_; }
      [[nodiscard]] constexpr std::size_t size() const noexcept { return N; }
      [[nodiscard]] constexpr bool empty() const noexcept { return N == 0; }
      [[nodiscard]] view<T, child_format, Store> operator[](std::size_t index) const noexcept
      {
         return view<T, child_format, Store>{
            view_layout::traits<Fmt>::template array_element_span<T, N>(data_, index), root_};
      }
      [[nodiscard]] view<T, child_format, Store> at(std::size_t index) const noexcept
      {
         return index < N ? (*this)[index] : view<T, child_format, Store>{};
      }
      // Precondition: N != 0.
      [[nodiscard]] view<T, child_format, Store> front() const noexcept { return (*this)[0]; }
      [[nodiscard]] view<T, child_format, Store> back() const noexcept { return (*this)[N - 1]; }

      using iterator         = detail::view_iterator<view>;
      using const_iterator   = iterator;
      using reverse_iterator = std::reverse_iterator<iterator>;
      [[nodiscard]] iterator begin() const noexcept { return {*this, 0}; }
      [[nodiscard]] iterator end() const noexcept { return {*this, N}; }
      [[nodiscard]] reverse_iterator rbegin() const noexcept { return reverse_iterator{end()}; }
      [[nodiscard]] reverse_iterator rend() const noexcept { return reverse_iterator{begin()}; }

      PSIO_VIEW_UNPACK_
   };

   // ── view<std::optional<T>, Fmt, Store> ────────────────────────────────
   template <typename T, typename Fmt, storage Store>
      requires view_layout::has_optional_support<Fmt>::value
   class view<std::optional<T>, Fmt, Store>
   {
      using child_format = view_layout::nested_format_t<Fmt, std::optional<T>>;
      std::span<const char> data_;
      std::span<const char> root_;

     public:
      using element_type                    = std::optional<T>;
      using format_type                     = Fmt;
      using value_type                      = T;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);

      view() = default;
      explicit view(std::span<const char> s) noexcept : data_(s), root_(s) {}
      view(std::span<const char> s, std::span<const char> root) noexcept : data_(s), root_(root) {}

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
      [[nodiscard]] view<T, child_format, Store> operator*() const noexcept
      {
         return view<T, child_format, Store>{
            view_layout::traits<Fmt>::template optional_payload_span<T>(data_), root_};
      }

      [[nodiscard]] view<T, child_format, Store> get() const noexcept { return **this; }

      // `opt->member()`: the payload's view. Precondition: has_value().
      [[nodiscard]] detail::arrow_proxy<view<T, child_format, Store>> operator->() const noexcept
      {
         return {**this};
      }

      // The payload's view, checked: an absent optional fails through
      // psio::codec_abort (an exception where they are enabled), as
      // std::optional::value() throws.
      [[nodiscard]] view<T, child_format, Store> value() const
      {
         if (!has_value())
            detail::view_absent("psio::view<std::optional<T>>::value(): no value");
         return **this;
      }

      // The payload's value, or `fallback` converted to T.
      template <typename U>
      [[nodiscard]] T value_or(U&& fallback) const
      {
         if (has_value())
            return (**this).unpack();
         return static_cast<T>(std::forward<U>(fallback));
      }

      PSIO_VIEW_UNPACK_
   };

   // ── view<std::variant<Ts...>, Fmt, Store> ─────────────────────────────
   template <typename... Ts, typename Fmt, storage Store>
      requires view_layout::has_variant_support<Fmt>::value
   class view<std::variant<Ts...>, Fmt, Store>
   {
      using child_format = view_layout::nested_format_t<Fmt, std::variant<Ts...>>;
      std::span<const char> data_;
      std::span<const char> root_;

     public:
      using element_type                    = std::variant<Ts...>;
      using format_type                     = Fmt;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);

      view() = default;
      explicit view(std::span<const char> s) noexcept : data_(s), root_(s) {}
      view(std::span<const char> s, std::span<const char> root) noexcept : data_(s), root_(root) {}

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

      // The view of alternative I if it is the active one.
      template <std::size_t I>
      [[nodiscard]] std::optional<
         view<std::variant_alternative_t<I, std::variant<Ts...>>, child_format, Store>>
      get_if() const noexcept
      {
         using V = view<std::variant_alternative_t<I, std::variant<Ts...>>, child_format, Store>;
         if (index() != I)
            return std::nullopt;
         return V{view_layout::traits<Fmt>::template variant_payload_span<Ts...>(data_), root_};
      }
      // By type: Alt must be exactly one of the alternatives.
      template <typename Alt>
         requires ((std::size_t{std::is_same_v<Alt, Ts>} + ...) == 1)
      [[nodiscard]] auto get_if() const noexcept
      {
         constexpr std::size_t I = [] {
            std::size_t i = 0, found = 0;
            ((std::is_same_v<Alt, Ts> ? void(found = i++) : void(++i)), ...);
            return found;
         }();
         return this->template get_if<I>();
      }
      template <typename Alt>
         requires ((std::size_t{std::is_same_v<Alt, Ts>} + ...) == 1)
      [[nodiscard]] bool holds_alternative() const noexcept
      {
         return this->template get_if<Alt>().has_value();
      }

      PSIO_VIEW_UNPACK_

     private:
      template <typename F, std::size_t... Is>
      auto visit_impl(F&& fn, std::span<const char> payload,
                       std::index_sequence<Is...>) const
      {
         using R = std::common_type_t<decltype(std::forward<F>(fn)(
            std::declval<view<Ts, child_format, Store>>()))...>;

         const std::size_t idx = index();
         if constexpr (std::is_void_v<R>)
         {
            (void)((idx == Is
                       ? (std::forward<F>(fn)(view<Ts, child_format, Store>{payload, root_}), true)
                       : false) ||
                   ...);
         }
         else
         {
            R    result{};
            bool matched = false;
            ((idx == Is
                 ? (result = std::forward<F>(fn)(
                       view<Ts, child_format, Store>{payload, root_}),
                    matched = true)
                 : false) ||
             ...);
            (void)matched;
            return result;
         }
      }
   };

   // ── view<T, Fmt, Store> for adapter-encoded T ─────────────────────────
   //
   // When T has an adapter registered under Fmt's preferred presentation
   // category, the encoded bytes belong to the adapter — not to Fmt's
   // shape walker. The Record-view, vector-view, etc. cannot interpret
   // those bytes; only the adapter can. This specialization is the
   // "opaque payload" view: it exposes the raw bytes for zero-copy use
   // and a `decode()` accessor that hands back an owned T via
   // `adapter::decode`.
   //
   // Activates whenever `format_should_dispatch_adapter_v<Fmt, T>` is
   // true. The Record-view specialization adds the inverse constraint
   // so a reflected struct with an adapter routes here, not through
   // the field walker.
   //
   // For adapters that wish to publish a richer field-level surface
   // (e.g. a length-prefixed structured payload), they can return a
   // bespoke view from `decode()` — the contract here is just "the
   // adapter owns the bytes, this view gives you an honest interface
   // to them".
   template <typename T, typename Fmt, storage Store>
      requires (::psio::format_should_dispatch_adapter_v<Fmt,
                   std::remove_cvref_t<T>>)
   class view<T, Fmt, Store>
   {
      std::span<const char> data_;

     public:
      using element_type                    = T;
      using format_type                     = Fmt;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);

      view() = default;
      explicit view(std::span<const char> s) noexcept : data_(s) {}
      // Indirect-friendly ctor — the parent (record / vector / …) view
      // passes a root span used by formats that follow offsets. The
      // adapter view is opaque, so the root is discarded.
      view(std::span<const char> s, std::span<const char> /*root*/) noexcept
         : data_(s)
      {
      }

      [[nodiscard]] std::span<const char> _psio_data() const noexcept
      {
         return data_;
      }

      // Raw bytes of the adapter payload — zero-copy.
      [[nodiscard]] std::span<const char> bytes() const noexcept
      {
         return data_;
      }

      // Reconstruct an owned T by calling the adapter's decode. Pays
      // an allocation / copy for variable-length adapters; for fixed-
      // width adapters this is typically a memcpy.
      [[nodiscard]] T decode() const
      {
         using A = ::psio::adapter<std::remove_cvref_t<T>,
                                   ::psio::preferred_category_t<Fmt>>;
         return A::decode(data_);
      }

      // Convenience alias matching the std::optional / std::vector
      // surface — `.get()` on those returns a view; here we already
      // are the view, so `.get()` returns the decoded T. Callers who
      // want a sub-view should use `.bytes()` and construct one
      // themselves with the appropriate adapter-specific viewer.
      [[nodiscard]] T get() const { return decode(); }
      [[nodiscard]] T unpack() const { return decode(); }
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
   // Empty records need no field layout: their view only borrows the bytes.
   // Nonempty records require traits<Fmt>::record_field_span<T, 0>.
   // Formats that don't (or shapes the format can't handle, e.g., ssz on
   // a record with a variable field but no offset-table support) fall
   // through to whatever caller mechanism handles the unsupported case.
   //
   // Adapter exclusion: when T has an adapter under Fmt's category, the
   // bytes aren't a record layout — they're the adapter's payload. The
   // adapter-view specialization above wins instead.
   //
   // Named accessors. The view derives from `reflect<T>::proxy<...>`, the
   // class PSIO_REFLECT generates with one member function per reflected
   // member: `v.name()` is `v.get<I>()` for the member called `name`, so a
   // chain of records reads like the value (`block.header().producer()`).
   //
   // Names the view itself uses win over a reflected member of the same
   // name: `get`, `unpack`, `element_type`, `format_type`, `storage_kind`,
   // `view`, and everything starting with `psio_` / `_psio_`. A record that
   // has such a member still views; that member is read with `get<N>()`
   // (N = *psio::reflect<T>::index_of("unpack")). Reflected METHODS are
   // members of the proxy too; calling one on a view does not compile (a
   // view projects fields, it does not dispatch calls).
   namespace detail {
      // What reflect<T>::proxy<...> is given: the bytes, and member I.
      template <typename T, typename Fmt, storage Store>
      class record_view_projection
      {
       protected:
         // data_ — the bytes of THIS record (a slice of the encoded buffer).
         // root_ — the absolute encoded buffer, used by formats that follow
         //         pointers between record fields and heap regions
         //         (canonical-ABI / wit).  Defaults to data_ for formats
         //         that don't need pointer-following (ssz, pssz).
         std::span<const char> data_;
         std::span<const char> root_;

       public:
         record_view_projection() = default;
         record_view_projection(std::span<const char> s, std::span<const char> r) noexcept
            : data_(s), root_(r)
         {
         }

         template <std::size_t N>
         [[nodiscard]] auto _psio_member() const noexcept
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
               return view<FieldT, view_layout::member_format_t<Fmt, T, N>, Store>{field_span, root_};
            }
         }

         // The projection reflect<T>::proxy calls: member I, named by its
         // member pointer.
         template <std::size_t I, auto MemberPtr>
         [[nodiscard]] auto get() const noexcept
         {
            return this->template _psio_member<I>();
         }
      };

      // reflect<T>::proxy<P> where the reflection has one (PSIO_REFLECT);
      // P itself for a hand-written reflection without a proxy (get<N>() only).
      template <typename T, typename P, typename = void>
      struct named_base
      {
         using type = P;
      };
      template <typename T, typename P>
      struct named_base<T, P, std::void_t<typename ::psio::reflect<T>::template proxy<P>>>
      {
         using type = typename ::psio::reflect<T>::template proxy<P>;
      };
      template <typename T, typename P>
      using named_base_t = typename named_base<T, P>::type;
   }  // namespace detail

   template <typename T, typename Fmt, storage Store>
      requires Reflected<T>
            && (!std::is_enum_v<T>)  // a reflected enum is a scalar, not a record
            && (reflect<T>::member_count == 0
                || view_layout::has_record_support<Fmt, T>::value)
            && (!::psio::format_should_dispatch_adapter_v<Fmt, T>)
   class view<T, Fmt, Store>
       : public detail::named_base_t<T, detail::record_view_projection<T, Fmt, Store>>
   {
      using projection = detail::record_view_projection<T, Fmt, Store>;
      using base       = detail::named_base_t<T, projection>;

     public:
      using element_type                    = T;
      using format_type                     = Fmt;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);

      view() = default;
      explicit view(std::span<const char> s) noexcept : base(projection{s, s}) {}
      view(std::span<const char> s, std::span<const char> r) noexcept : base(projection{s, r}) {}

      [[nodiscard]] std::span<const char> _psio_data() const noexcept
      {
         return this->projection::data_;
      }
      [[nodiscard]] std::span<const char> _psio_root() const noexcept
      {
         return this->projection::root_;
      }

      // Member N: its value for an arithmetic member, its view otherwise.
      // The named accessor of the member returns the same.
      template <std::size_t N>
      [[nodiscard]] auto get() const noexcept
      {
         return this->projection::template _psio_member<N>();
      }

      PSIO_VIEW_UNPACK_
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

   // ── value_of ──────────────────────────────────────────────────────────
   //
   // One spelling for "the value of this", whatever a view accessor
   // returned: `v.unpack()` for a view, the argument itself for a value (an
   // arithmetic record member is returned by value). Trusted, like unpack().
   template <typename X>
   [[nodiscard]] constexpr decltype(auto) value_of(X&& x)
   {
      if constexpr (detail::read_view<std::remove_cvref_t<X>>)
         return x.unpack();
      else if constexpr (requires { x.psio_mutable_view_tag(); })
         return as_view(x).unpack();
      else
         return static_cast<std::remove_cvref_t<X>>(std::forward<X>(x));
   }

   // ── Comparison ────────────────────────────────────────────────────────
   //
   // A view compares as the value it designates: with a value of its type
   // (or anything that type compares with), and with another view. Scalars
   // and strings are compared in place; anything else through unpack(),
   // which allocates what the value type allocates. `<=>` exists where the
   // value type has it.
   template <detail::read_view V, typename U>
      requires (!detail::read_view<U> &&
                requires(const V& v, const U& u) {
                   { detail::view_read(v) == u } -> std::convertible_to<bool>;
                })
   [[nodiscard]] bool operator==(const V& v, const U& u)
   {
      return detail::view_read(v) == u;
   }
   template <detail::read_view A, detail::read_view B>
      requires requires(const A& a, const B& b) {
         { detail::view_read(a) == detail::view_read(b) } -> std::convertible_to<bool>;
      }
   [[nodiscard]] bool operator==(const A& a, const B& b)
   {
      return detail::view_read(a) == detail::view_read(b);
   }
   template <detail::read_view V, typename U>
      requires (!detail::read_view<U> &&
                requires(const V& v, const U& u) { detail::view_read(v) <=> u; })
   [[nodiscard]] auto operator<=>(const V& v, const U& u)
   {
      return detail::view_read(v) <=> u;
   }
   template <detail::read_view A, detail::read_view B>
      requires requires(const A& a, const B& b) {
         detail::view_read(a) <=> detail::view_read(b);
      }
   [[nodiscard]] auto operator<=>(const A& a, const B& b)
   {
      return detail::view_read(a) <=> detail::view_read(b);
   }

   // get_if<Alt>(variant_view) / get_if<I>(variant_view), found by ADL.
   template <typename Alt, typename... Ts, typename Fmt, storage Store>
   [[nodiscard]] auto get_if(const view<std::variant<Ts...>, Fmt, Store>& v) noexcept
   {
      return v.template get_if<Alt>();
   }
   template <std::size_t I, typename... Ts, typename Fmt, storage Store>
   [[nodiscard]] auto get_if(const view<std::variant<Ts...>, Fmt, Store>& v) noexcept
   {
      return v.template get_if<I>();
   }
   template <typename Alt, typename... Ts, typename Fmt, storage Store>
   [[nodiscard]] bool holds_alternative(const view<std::variant<Ts...>, Fmt, Store>& v) noexcept
   {
      return v.template holds_alternative<Alt>();
   }

   // A view is encoded as the value it designates (wire_type.hpp).
   template <typename T, typename Fmt, storage Store>
   struct wire_type<view<T, Fmt, Store>>
   {
      using type = std::remove_const_t<T>;
   };

   // ── Construct a view from a buffer ────────────────────────────────────
   template <typename T, typename Fmt, storage S>
   [[nodiscard]] view<T, Fmt, storage::const_borrow>
   as_view(const buffer<T, Fmt, S>& b) noexcept
   {
      return view<T, Fmt, storage::const_borrow>{bytes(b)};
   }

}  // namespace psio
