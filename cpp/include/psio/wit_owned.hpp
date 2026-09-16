#pragma once

// psio/wit_owned.hpp — Owning counterpart to view<T, wit>.
//
// owned<T, wit> mirrors view<T, wit> with two differences:
//   1. Destructors free the canonical-ABI allocations (RAII cascade)
//   2. Non-const accessors return mutable types
//
// The type mapping parallels view:
//
//       Schema type          view accessor returns    owned accessor returns (mut / const)
//       ──────────────       ───────────────────────  ──────────────────────────────────────
//       bool / arithmetic    T (by value)             wit_mut_ref<T> / T
//       std::string          std::string_view         owned<string, wit>& / string_view
//       std::vector<E>       vec_view<E, wit>         owned<vector<E>, wit>& / vec_view
//       Reflected record     view<Sub, wit>           owned<Sub, wit>& / view<Sub, wit>
//
// Allocation:
//   __wasm__:  cabi_realloc (canonical ABI allocator)
//   host:      malloc/free
//
// Usage:
//   // Receive a canonical-ABI param (thunk wraps the host's allocation):
//   void process(owned<std::string, wit> name) {
//       std::cout << name.view();   // borrow
//       stored = std::move(name);   // take ownership (zero-copy)
//   }
//
//   // Build a return value:
//   owned<std::string, wit> greet(std::string_view who) {
//       owned<std::string, wit> result(6 + who.size());
//       memcpy(result.data(), "hello ", 6);
//       memcpy(result.data() + 6, who.data(), who.size());
//       return result;
//   }

#include <psio/wit_abi.hpp>
namespace psio { struct wit; }

#include <cstddef>
#include <cstring>
#include <string_view>
#include <span>
#include <type_traits>
#include <utility>

#ifdef __wasm__
extern "C" void* cabi_realloc(void* old_ptr, std::size_t old_size,
                              std::size_t align, std::size_t new_size);
#else
#include <cstdlib>
#endif

namespace psio {

// Forward declaration — specialized per type category below.
template <typename T, typename Fmt>
class owned;
namespace detail {
   template <typename T> struct wit_owned_type { static constexpr bool value = false; };
   template <typename T> struct wit_owned_type<owned<T, wit>> {
      static constexpr bool value = true;
      using type = T;
   };
}


// ═══════════════════════════════════════════════════════════════════════
// owned<std::string, wit> — owning canonical-ABI string buffer
// ═══════════════════════════════════════════════════════════════════════

template <>
class owned<std::string, wit> {
   char*       _ptr = nullptr;
   std::size_t _len = 0;

public:
   owned() = default;

   explicit owned(std::size_t n) {
      if (n == 0) return;
      _ptr = static_cast<char*>(alloc_bytes(1, n));
      _len = n;
   }

   explicit owned(std::string_view v) {
      if (v.empty()) return;
      _ptr = static_cast<char*>(alloc_bytes(1, v.size()));
      std::memcpy(_ptr, v.data(), v.size());
      _len = v.size();
   }

   owned(const owned&)            = delete;
   owned& operator=(const owned&) = delete;

   owned(owned&& o) noexcept : _ptr(o._ptr), _len(o._len) {
      o._ptr = nullptr;
      o._len = 0;
   }
   owned& operator=(owned&& o) noexcept {
      if (this != &o) {
         free_bytes(_ptr, _len);
         _ptr   = o._ptr;
         _len   = o._len;
         o._ptr = nullptr;
         o._len = 0;
      }
      return *this;
   }

   ~owned() { free_bytes(_ptr, _len); }

   // ── Size / capacity ────────────────────────────────────────────────
   std::size_t size() const noexcept { return _len; }
   bool        empty() const noexcept { return _len == 0; }

   // ── Element access ───────────────────────────────────────────────
   char*       data() noexcept { return _ptr; }
   const char* data() const noexcept { return _ptr; }
   char&       operator[](std::size_t i) noexcept { return _ptr[i]; }
   const char& operator[](std::size_t i) const noexcept { return _ptr[i]; }
   char&       front() noexcept { return _ptr[0]; }
   const char& front() const noexcept { return _ptr[0]; }
   char&       back() noexcept { return _ptr[_len - 1]; }
   const char& back() const noexcept { return _ptr[_len - 1]; }

   // ── Iterators ────────────────────────────────────────────────────
   char*       begin() noexcept { return _ptr; }
   char*       end() noexcept { return _ptr + _len; }
   const char* begin() const noexcept { return _ptr; }
   const char* end() const noexcept { return _ptr + _len; }

   // ── View access ──────────────────────────────────────────────────
   operator std::string_view() const noexcept { return {_ptr, _len}; }
   std::string_view view() const noexcept { return {_ptr, _len}; }

   // ── Mutation — mirrors std::string ────────────────────────────────
   void resize(std::size_t n) {
      if (n == _len) return;
      _ptr = static_cast<char*>(realloc_bytes(_ptr, _len, 1, n));
      _len = n;
   }

   void clear() {
      free_bytes(_ptr, _len);
      _ptr = nullptr;
      _len = 0;
   }

   owned& append(std::string_view s) {
      if (s.empty()) return *this;
      std::size_t old = _len;
      resize(old + s.size());
      std::memcpy(_ptr + old, s.data(), s.size());
      return *this;
   }

   owned& append(const char* s, std::size_t n) {
      return append(std::string_view{s, n});
   }

   owned& append(std::size_t n, char c) {
      std::size_t old = _len;
      resize(old + n);
      std::memset(_ptr + old, c, n);
      return *this;
   }

   owned& operator+=(std::string_view s) { return append(s); }
   owned& operator+=(char c) { return append(1, c); }

   void push_back(char c) { append(1, c); }

   void pop_back() {
      if (_len > 0) resize(_len - 1);
   }

   owned& insert(std::size_t pos, std::string_view s) {
      if (s.empty()) return *this;
      std::size_t old = _len;
      resize(old + s.size());
      std::memmove(_ptr + pos + s.size(), _ptr + pos, old - pos);
      std::memcpy(_ptr + pos, s.data(), s.size());
      return *this;
   }

   owned& erase(std::size_t pos = 0, std::size_t count = std::string_view::npos) {
      if (pos >= _len) return *this;
      if (count > _len - pos) count = _len - pos;
      std::memmove(_ptr + pos, _ptr + pos + count, _len - pos - count);
      resize(_len - count);
      return *this;
   }

   // ── Comparison ───────────────────────────────────────────────────
   bool operator==(std::string_view o) const noexcept { return view() == o; }
   auto operator<=>(std::string_view o) const noexcept { return view() <=> o; }

   // ── Canonical ABI handoff ────────────────────────────────────────
   struct raw { char* ptr; std::size_t len; };
   raw release() noexcept {
      raw r{_ptr, _len};
      _ptr = nullptr;
      _len = 0;
      return r;
   }

   static owned adopt(char* p, std::size_t n) noexcept {
      owned s;
      s._ptr = p;
      s._len = n;
      return s;
   }

private:
#ifdef __wasm__
   static void* alloc_bytes(std::size_t align, std::size_t n) {
      return cabi_realloc(nullptr, 0, align, n);
   }
   static void* realloc_bytes(void* p, std::size_t old, std::size_t align, std::size_t n) {
      return cabi_realloc(p, old, align, n);
   }
   static void free_bytes(char* p, std::size_t n) noexcept {
      if (p) cabi_realloc(p, n, 1, 0);
   }
#else
   static void* alloc_bytes(std::size_t, std::size_t n) { return std::malloc(n); }
   static void* realloc_bytes(void* p, std::size_t, std::size_t, std::size_t n) {
      return std::realloc(p, n);
   }
   static void free_bytes(char* p, std::size_t) noexcept { std::free(p); }
#endif
};

static_assert(sizeof(owned<std::string, wit>) <= sizeof(char*) + sizeof(std::size_t) + 16,
              "owned<string, wit> should be a small type");

// ═══════════════════════════════════════════════════════════════════════
// owned<std::vector<E>, wit> — owning canonical-ABI list buffer
// ═══════════════════════════════════════════════════════════════════════

template <typename E>
class owned<std::vector<E>, wit> {
   static_assert(std::is_trivially_copyable_v<E>,
                 "owned<vector<E>, wit> requires trivially-copyable E; "
                 "records with nested strings need recursive owned field mapping");

   E*          _ptr = nullptr;
   std::size_t _len = 0;

public:
   owned() = default;

   explicit owned(std::size_t n) {
      if (n == 0) return;
      _ptr = static_cast<E*>(alloc_bytes(alignof(E), n * sizeof(E)));
      _len = n;
   }

   explicit owned(std::span<const E> src) {
      if (src.empty()) return;
      _ptr = static_cast<E*>(alloc_bytes(alignof(E), src.size() * sizeof(E)));
      std::memcpy(_ptr, src.data(), src.size() * sizeof(E));
      _len = src.size();
   }

   owned(const owned&)            = delete;
   owned& operator=(const owned&) = delete;

   owned(owned&& o) noexcept : _ptr(o._ptr), _len(o._len) {
      o._ptr = nullptr;
      o._len = 0;
   }
   owned& operator=(owned&& o) noexcept {
      if (this != &o) {
         free_bytes(_ptr, _len);
         _ptr   = o._ptr;
         _len   = o._len;
         o._ptr = nullptr;
         o._len = 0;
      }
      return *this;
   }

   ~owned() { free_bytes(_ptr, _len); }

   // Mutable access
   E*          data() noexcept { return _ptr; }
   E&          operator[](std::size_t i) noexcept { return _ptr[i]; }
   std::size_t size() const noexcept { return _len; }
   bool        empty() const noexcept { return _len == 0; }

   E* begin() noexcept { return _ptr; }
   E* end() noexcept { return _ptr + _len; }

   // View access (const)
   const E*    data() const noexcept { return _ptr; }
   const E&    operator[](std::size_t i) const noexcept { return _ptr[i]; }
   const E*    begin() const noexcept { return _ptr; }
   const E*    end() const noexcept { return _ptr + _len; }

   operator std::span<const E>() const noexcept { return {_ptr, _len}; }
   std::span<const E> view() const noexcept { return {_ptr, _len}; }

   // Canonical ABI handoff
   struct raw { E* ptr; std::size_t len; };
   raw release() noexcept {
      raw r{_ptr, _len};
      _ptr = nullptr;
      _len = 0;
      return r;
   }

   static owned adopt(E* p, std::size_t n) noexcept {
      owned l;
      l._ptr = p;
      l._len = n;
      return l;
   }

private:
#ifdef __wasm__
   static void* alloc_bytes(std::size_t align, std::size_t n) {
      return cabi_realloc(nullptr, 0, align, n);
   }
   static void free_bytes(E* p, std::size_t n) noexcept {
      if (p) cabi_realloc(p, n * sizeof(E), alignof(E), 0);
   }
#else
   static void* alloc_bytes(std::size_t, std::size_t n) { return std::malloc(n); }
   static void  free_bytes(E* p, std::size_t) noexcept { std::free(p); }
#endif
};

// ═══════════════════════════════════════════════════════════════════════
// Type mapping: schema type T → its owned equivalent for a given Fmt
// ═══════════════════════════════════════════════════════════════════════
//
// Scalars pass through unchanged; strings/vectors/records get wrapped
// in owned<T, Fmt>. This drives the field tuple inside owned<Record>.
//
// Record ownership is composed from the same buffer wrappers and indexed reflection.

namespace detail_owned {
   template <typename T, std::size_t... I>
   auto field_tuple(std::index_sequence<I...>)
      -> std::tuple<typename reflect<T>::template member_type<I>...>;
   template <typename T>
   using field_tuple_t = decltype(field_tuple<T>(std::make_index_sequence<reflect<T>::member_count>{}));


   template <typename T, typename Fmt, typename = void>
   struct owned_field_type { using type = T; };

   template <typename Fmt>
   struct owned_field_type<std::string, Fmt> {
      using type = owned<std::string, Fmt>;
   };

   template <typename E, typename Fmt>
   struct owned_field_type<std::vector<E>, Fmt> {
      using type = owned<std::vector<E>, Fmt>;
   };

   template <typename T, typename Fmt>
   struct owned_field_type<T, Fmt, std::enable_if_t<
      Reflected<T> && !std::is_arithmetic_v<T> && !std::is_enum_v<T>
      && !detail::is_std_string_ct<T>::value
      && !detail::is_std_vector_ct<T>::value>> {
      using type = owned<T, Fmt>;
   };

   template <typename T, typename Fmt>
   using owned_field_t = typename owned_field_type<std::remove_cvref_t<T>, Fmt>::type;

} // namespace detail_owned

// ═══════════════════════════════════════════════════════════════════════
// owned_proxy<T, Fmt> — proxy object for owned<Record, Fmt>
//
// Non-const get<I>() returns a mutable reference to the owned field.
// Const get<I>() borrows buffer fields and nested owned records.
// ═══════════════════════════════════════════════════════════════════════

template <typename T, typename Fmt>
class owned_proxy {
   // Build the field storage tuple by mapping each reflected field type
   // through owned_field_type.
   template <typename Tuple, typename F, std::size_t... Is>
   static auto make_fields_tuple_type(std::index_sequence<Is...>)
      -> std::tuple<detail_owned::owned_field_t<std::tuple_element_t<Is, Tuple>, F>...>;

   using fields_tuple_t = decltype(
      make_fields_tuple_type<detail_owned::field_tuple_t<T>, Fmt>(
         std::make_index_sequence<std::tuple_size_v<detail_owned::field_tuple_t<T>>>{}));

   fields_tuple_t fields_;

public:
   owned_proxy() = default;

   // Construct from individual owned fields (used by canonical lift)
   explicit owned_proxy(fields_tuple_t&& f) : fields_(std::move(f)) {}

   // Mutable access — returns reference to owned field
   template <int I, auto>
   auto& get() { return std::get<I>(fields_); }

   // Const access — returns view equivalent
   template <int I, auto>
   decltype(auto) get() const {
      const auto& f = std::get<I>(fields_);
      using F = std::remove_cvref_t<decltype(f)>;

      if constexpr (std::is_same_v<F, owned<std::string, Fmt>>)
         return std::string_view{f};
      else if constexpr (std::is_arithmetic_v<F> || std::is_enum_v<F>)
         return f;
      else if constexpr (detail::wit_owned_type<F>::value && requires { f.view(); })
         return f.view();
      else
         return (f);
   }

   fields_tuple_t&       fields() { return fields_; }
   const fields_tuple_t& fields() const { return fields_; }
};

// ═══════════════════════════════════════════════════════════════════════
// owned<T, wit> — owning record for Reflected types
//
// Inherits from reflect<T>::proxy<owned_proxy<T, wit>> to get the same
// named field accessors as view<T, wit>. Destructors cascade through
// the owned fields — each owned<string> frees its buffer, each
// owned<vector<E>> frees its array, etc.
// ═══════════════════════════════════════════════════════════════════════

template <typename T>
class owned<T, wit> : public reflect<T>::template proxy<owned_proxy<T, wit>> {
   using base = typename reflect<T>::template proxy<owned_proxy<T, wit>>;

public:
   owned() : base(owned_proxy<T, wit>{}) {}

   explicit owned(owned_proxy<T, wit>&& proxy) : base(std::move(proxy)) {}

   // Move
   owned(owned&&) = default;
   owned& operator=(owned&&) = default;

   // No copy
   owned(const owned&)            = delete;
   owned& operator=(const owned&) = delete;

   // Destructor — cascades through owned fields automatically (RAII)
   ~owned() = default;

   // Access the underlying proxy (for canonical dispatch integration)
   owned_proxy<T, wit>&       proxy() { return this->psio_get_proxy(); }
   const owned_proxy<T, wit>& proxy() const { return this->psio_get_proxy(); }
};



} // namespace psio

// ═══════════════════════════════════════════════════════════════════════
// Top-level wit:: namespace — clean aliases for canonical-ABI owned types
//
//   wit::string                   owning canonical string
//   wit::vector<E>                owning canonical list
//   wit::val<T>                   owning canonical record (reflected T)
// ═══════════════════════════════════════════════════════════════════════

namespace wit {

using string = psio::owned<std::string, psio::wit>;

template <typename E>
using vector = psio::owned<std::vector<E>, psio::wit>;

template <typename T>
using val = psio::owned<T, psio::wit>;

}
