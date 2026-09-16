#pragma once

// WView<T> and WOwned<T> — WASM Canonical ABI type projections from PSIO_REFLECT.
//
// Given a reflected C++ type T, WView<T> replaces owning types with non-owning
// views (string → string_view, vector<U> → span<WView<U>>), and WOwned<T>
// replaces them with owning wrappers that free cabi_realloc'd memory.
//
// Usage:
//   struct Transfer {
//       std::string from;
//       std::string to;
//       uint64_t amount;
//       std::vector<std::string> memo_lines;
//   };
//   PSIO_REFLECT(Transfer, from, to, amount, memo_lines)
//
//   // WView<Transfer> ≈ { string_view from, to; uint64_t amount; span<string_view> memo_lines; }
//   // WOwned<Transfer> ≈ same layout but destructor frees all allocations

#include <psio/wit_abi.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace psio {

   // ── Forward declarations ──────────────────────────────────────────────────

   template <typename T>
   struct WViewImpl;

   template <typename T>
   struct WOwnedImpl;

   // ── Type classification traits ────────────────────────────────────────────

   namespace detail {

      // ── is_canonical_simple: no indirections (strings, vectors, optionals) ──

      template <typename T, typename Enable = void>
      struct is_canonical_simple_impl : std::false_type {};

      // Scalars are simple
      template <typename T>
      struct is_canonical_simple_impl<T, std::enable_if_t<is_scalar_v<T>>> : std::true_type {};

      // Strings are NOT simple (contain pointer)
      // Vectors are NOT simple (contain pointer)
      // Optionals are NOT simple (contain discriminant + optional pointer)
      // (default false_type catches these)

      // Reflected struct: simple if all fields are simple
      template <typename T>
      struct is_canonical_simple_impl<T, std::enable_if_t<
         !is_scalar_v<T> && !is_std_string_ct<T>::value &&
         !is_std_vector_ct<T>::value && !is_std_optional_ct<T>::value &&
         Reflected<T>>>
      {
         static constexpr bool compute() {
            bool result = true;
            apply_members(
               (detail::data_members_t<T>*)nullptr,
               [&](auto... ptrs) {
                  ((void)(result = result && is_canonical_simple_impl<
                     std::remove_cvref_t<typename MemberPtrType<decltype(ptrs)>::ValueType>
                  >::value), ...);
               }
            );
            return result;
         }
         static constexpr bool value = compute();
      };

   } // namespace detail

   // ── Compile-time simplicity check ─────────────────────────────────────────

   template <typename T>
   inline constexpr bool is_canonical_simple = detail::is_canonical_simple_impl<std::remove_cvref_t<T>>::value;

   // ── WView field type mapping ──────────────────────────────────────────────

   namespace detail {

      template <typename T, typename Enable = void>
      struct wview_field_map;

      // Scalars: pass through
      template <typename T>
      struct wview_field_map<T, std::enable_if_t<is_scalar_v<T>>> {
         using type = T;
      };

      // std::string → std::string_view
      template <>
      struct wview_field_map<std::string, void> {
         using type = std::string_view;
      };

      // std::vector<scalar> → std::span<const scalar>
      template <typename U>
      struct wview_field_map<std::vector<U>, std::enable_if_t<is_scalar_v<U>>> {
         using type = std::span<const U>;
      };

      // std::vector<string> → std::span<const std::string_view>
      template <>
      struct wview_field_map<std::vector<std::string>, void> {
         using type = std::span<const std::string_view>;
      };

      // std::vector<reflected struct> → std::span<const WViewImpl<U>>
      template <typename U>
      struct wview_field_map<std::vector<U>, std::enable_if_t<
         !is_scalar_v<U> && !is_std_string_ct<U>::value && Reflected<U>>> {
         using type = std::span<const WViewImpl<U>>;
      };

      // std::optional<T> → const mapped_type*
      template <typename U>
      struct wview_field_map<std::optional<U>, void> {
         using type = const typename wview_field_map<U>::type*;
      };

      // Reflected struct → WViewImpl<U>
      template <typename U>
      struct wview_field_map<U, std::enable_if_t<
         !is_scalar_v<U> &&
         !is_std_string_ct<U>::value &&
         !is_std_vector_ct<U>::value &&
         !is_std_optional_ct<U>::value &&
         Reflected<U>>> {
         using type = WViewImpl<U>;
      };

      template <typename T>
      using wview_t = typename wview_field_map<std::remove_cvref_t<T>>::type;

      // ── is_wviewable: zero-alloc WView possible? ──

      template <typename T, typename Enable = void>
      struct is_wviewable_impl : std::true_type {};  // scalars

      template <>
      struct is_wviewable_impl<std::string, void> : std::true_type {};

      template <typename U>
      struct is_wviewable_impl<std::vector<U>, std::enable_if_t<is_scalar_v<U>>>
         : std::true_type {};

      // vector<string> or vector<compound> → NOT viewable (need descriptor alloc)
      template <typename U>
      struct is_wviewable_impl<std::vector<U>, std::enable_if_t<!is_scalar_v<U>>>
         : std::false_type {};

      template <typename U>
      struct is_wviewable_impl<std::optional<U>, void> : is_wviewable_impl<U> {};

      // Reflected struct: viewable if all fields viewable
      template <typename T>
      struct is_wviewable_impl<T, std::enable_if_t<
         !is_scalar_v<T> && !is_std_string_ct<T>::value &&
         !is_std_vector_ct<T>::value && !is_std_optional_ct<T>::value &&
         Reflected<T>>>
      {
         static constexpr bool compute() {
            bool result = true;
            apply_members(
               (detail::data_members_t<T>*)nullptr,
               [&](auto... ptrs) {
                  ((void)(result = result && is_wviewable_impl<
                     std::remove_cvref_t<typename MemberPtrType<decltype(ptrs)>::ValueType>
                  >::value), ...);
               }
            );
            return result;
         }
         static constexpr bool value = compute();
      };

      // ── Build tuple of WView field types ──

      template <typename T>
      struct wview_tuple_builder {
         template <auto... M>
         static auto build(MemberList<M...>*) {
            return std::tuple<wview_t<typename MemberPtrType<decltype(M)>::ValueType>...>{};
         }
         using type = decltype(build((detail::data_members_t<T>*)nullptr));
      };

   } // namespace detail

   // ── Compile-time viewability ──────────────────────────────────────────────

   template <typename T>
   inline constexpr bool is_wviewable = detail::is_wviewable_impl<std::remove_cvref_t<T>>::value;

   // ── WView proxy object ───────────────────────────────────────────────────
   //
   // Satisfies the PSIO_REFLECT proxy protocol: get<I, MemberPtr>() returns
   // the mapped view type for each field.

   template <typename T>
   class wview_proxy_obj {
      using tuple_type = typename detail::wview_tuple_builder<T>::type;
      tuple_type* fields_;

   public:
      explicit wview_proxy_obj(tuple_type* f) : fields_(f) {}

      template <int I, auto MemberPtr>
      decltype(auto) get() {
         using field_type = std::remove_cvref_t<typename MemberPtrType<decltype(MemberPtr)>::ValueType>;
         if constexpr (!detail::is_scalar_v<field_type> &&
                       !detail::is_std_string_ct<field_type>::value &&
                       !detail::is_std_vector_ct<field_type>::value &&
                       !detail::is_std_optional_ct<field_type>::value &&
                       Reflected<field_type>) {
            // Nested reflected struct: return proxy with named accessors
            return std::get<I>(*fields_).proxy();
         } else {
            // Leaf or non-struct field: return reference to mapped value
            return (std::get<I>(*fields_));
         }
      }

      template <int I, auto MemberPtr>
      decltype(auto) get() const {
         using field_type = std::remove_cvref_t<typename MemberPtrType<decltype(MemberPtr)>::ValueType>;
         if constexpr (!detail::is_scalar_v<field_type> &&
                       !detail::is_std_string_ct<field_type>::value &&
                       !detail::is_std_vector_ct<field_type>::value &&
                       !detail::is_std_optional_ct<field_type>::value &&
                       Reflected<field_type>) {
            return std::get<I>(*fields_).proxy();
         } else {
            return (std::get<I>(*fields_));
         }
      }
   };

   // ── WViewImpl<T> ─────────────────────────────────────────────────────────

   template <typename T>
   struct WViewImpl {
      using tuple_type = typename detail::wview_tuple_builder<T>::type;
      tuple_type fields;

      // ── Index-based accessors ──

      template <size_t I>
      auto& get() { return std::get<I>(fields); }

      template <size_t I>
      const auto& get() const { return std::get<I>(fields); }

      // ── Named proxy accessors ──

      using proxy_obj_t = wview_proxy_obj<T>;
      using proxy_t = typename reflect<T>::template proxy<proxy_obj_t>;

      proxy_t proxy() {
         return proxy_t{proxy_obj_t{&fields}};
      }

      // ── Construction / destruction ──

      WViewImpl() = default;

      // Construct from RichType (borrows data, may alloc descriptor arrays)
      explicit WViewImpl(const T& rich) {
         apply_members(
            (detail::data_members_t<T>*)nullptr,
            [&](auto... ptrs) {
               [&]<size_t... Is>(std::index_sequence<Is...>) {
                  (init_field(std::get<Is>(fields), rich.*ptrs), ...);
               }(std::index_sequence_for<decltype(ptrs)...>{});
            }
         );
      }

      ~WViewImpl() {
         for (auto* p : owned_bufs_)
            ::operator delete(p);
      }

      WViewImpl(WViewImpl&& o) noexcept : fields(std::move(o.fields)), owned_bufs_(std::move(o.owned_bufs_)) {
         o.owned_bufs_.clear();
      }
      WViewImpl& operator=(WViewImpl&& o) noexcept {
         if (this != &o) {
            for (auto* p : owned_bufs_) ::operator delete(p);
            fields = std::move(o.fields);
            owned_bufs_ = std::move(o.owned_bufs_);
            o.owned_bufs_.clear();
         }
         return *this;
      }
      WViewImpl(const WViewImpl&) = delete;
      WViewImpl& operator=(const WViewImpl&) = delete;

      T promote() const {
         T result;
         apply_members(
            (detail::data_members_t<T>*)nullptr,
            [&](auto... ptrs) {
               [&]<size_t... Is>(std::index_sequence<Is...>) {
                  (promote_field(result.*ptrs, std::get<Is>(fields)), ...);
               }(std::index_sequence_for<decltype(ptrs)...>{});
            }
         );
         return result;
      }

      // Implicit rvalue conversion to T
      explicit operator T() && { return promote(); }

   private:
      std::vector<void*> owned_bufs_;

      // ── Per-type init ──

      // Scalar
      template <typename V>
      static void init_field(V& dst, const V& src)
         requires(detail::is_scalar_v<V>)
      { dst = src; }

      // string → string_view
      static void init_field(std::string_view& dst, const std::string& src)
      { dst = src; }

      // vector<scalar> → span
      template <typename U>
      static void init_field(std::span<const U>& dst, const std::vector<U>& src)
         requires(detail::is_scalar_v<U>)
      { dst = std::span<const U>(src.data(), src.size()); }

      // vector<string> → span<string_view>
      void init_field(std::span<const std::string_view>& dst,
                      const std::vector<std::string>& src) {
         if (src.empty()) { dst = {}; return; }
         auto* buf = new std::string_view[src.size()];
         owned_bufs_.push_back(buf);
         for (size_t i = 0; i < src.size(); i++)
            buf[i] = src[i];
         dst = std::span<const std::string_view>(buf, src.size());
      }

      // vector<reflected> → span<WViewImpl<U>>
      template <typename U>
      void init_field(std::span<const WViewImpl<U>>& dst, const std::vector<U>& src)
         requires(Reflected<U>)
      {
         if (src.empty()) { dst = {}; return; }
         auto* buf = static_cast<WViewImpl<U>*>(::operator new(sizeof(WViewImpl<U>) * src.size()));
         owned_bufs_.push_back(buf);
         for (size_t i = 0; i < src.size(); i++)
            new (&buf[i]) WViewImpl<U>(src[i]);
         dst = std::span<const WViewImpl<U>>(buf, src.size());
      }

      // optional<T> → const view_type*
      template <typename U, typename ViewField>
      void init_field(const ViewField*& dst, const std::optional<U>& src) {
         if (!src.has_value()) { dst = nullptr; return; }
         auto* buf = new ViewField;
         owned_bufs_.push_back(buf);
         init_field(*buf, *src);
         dst = buf;
      }

      // Reflected struct → WViewImpl<U>
      template <typename U>
      static void init_field(WViewImpl<U>& dst, const U& src)
         requires(Reflected<U>)
      { dst = WViewImpl<U>(src); }

      // ── Per-type promote ──

      // Scalar
      template <typename V>
      static void promote_field(V& dst, const V& src)
         requires(detail::is_scalar_v<V>)
      { dst = src; }

      // string_view → string
      static void promote_field(std::string& dst, std::string_view src)
      { dst = std::string(src); }

      // span → vector<scalar>
      template <typename U>
      static void promote_field(std::vector<U>& dst, std::span<const U> src)
         requires(detail::is_scalar_v<U>)
      { dst.assign(src.begin(), src.end()); }

      // span<string_view> → vector<string>
      static void promote_field(std::vector<std::string>& dst,
                                std::span<const std::string_view> src) {
         dst.clear();
         dst.reserve(src.size());
         for (auto sv : src) dst.emplace_back(sv);
      }

      // span<WViewImpl<U>> → vector<U>
      template <typename U>
      static void promote_field(std::vector<U>& dst, std::span<const WViewImpl<U>> src)
         requires(Reflected<U>)
      {
         dst.clear();
         dst.reserve(src.size());
         for (auto& cv : src) dst.push_back(cv.promote());
      }

      // const view_type* → optional<T>
      template <typename U, typename ViewField>
      static void promote_field(std::optional<U>& dst, const ViewField* src) {
         if (!src) { dst.reset(); return; }
         U val;
         promote_field(val, *src);
         dst = std::move(val);
      }

      // WViewImpl<U> → U
      template <typename U>
      static void promote_field(U& dst, const WViewImpl<U>& src)
         requires(Reflected<U>)
      { dst = src.promote(); }
   };

   // ── WOwnedImpl<T> ────────────────────────────────────────────────────────

   template <typename T>
   struct WOwnedImpl {
      WViewImpl<T> view;

      struct alloc_entry { void* ptr; size_t size; };
      std::vector<alloc_entry> allocs;

      using free_fn_t = void(*)(void*, size_t);
      free_fn_t free_fn = nullptr;

      WOwnedImpl() = default;

      explicit WOwnedImpl(WViewImpl<T>&& v, free_fn_t fn = nullptr)
         : view(std::move(v)), free_fn(fn) {}

      ~WOwnedImpl() {
         if (free_fn)
            for (auto& a : allocs)
               free_fn(a.ptr, a.size);
      }

      WOwnedImpl(WOwnedImpl&& o) noexcept
         : view(std::move(o.view)), allocs(std::move(o.allocs)), free_fn(o.free_fn)
      { o.free_fn = nullptr; }

      WOwnedImpl& operator=(WOwnedImpl&& o) noexcept {
         if (this != &o) {
            this->~WOwnedImpl();
            new (this) WOwnedImpl(std::move(o));
         }
         return *this;
      }
      WOwnedImpl(const WOwnedImpl&) = delete;
      WOwnedImpl& operator=(const WOwnedImpl&) = delete;

      template <size_t I> auto& get() { return view.template get<I>(); }
      template <size_t I> const auto& get() const { return view.template get<I>(); }

      auto proxy() { return view.proxy(); }

      void track(void* ptr, size_t size) { allocs.push_back({ptr, size}); }

      T promote() const { return view.promote(); }

      // Implicit rvalue conversion to T
      explicit operator T() && { return promote(); }
   };

   // ── Type-collapsing aliases ───────────────────────────────────────────────
   //
   // Simple types (scalars, flat structs with no indirections) collapse to T.
   // Complex types use the full WViewImpl/WOwnedImpl wrapper.

   template <typename T>
   using WView = std::conditional_t<is_canonical_simple<T>, T, WViewImpl<T>>;

   template <typename T>
   using WOwned = std::conditional_t<is_canonical_simple<T>, T, WOwnedImpl<T>>;

} // namespace psio
