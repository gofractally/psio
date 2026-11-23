#pragma once
//
// psio/capnp_view.hpp — views of a Cap'n Proto message, and editing one.
//
//   psio::capnp_view<Order> v{bytes};        // bytes: a validated message
//   v.id();                                  // std::uint64_t
//   v.customer().name();                     // view of Text: std::string_view
//   for (auto item : v.items()) item.qty();  // List(struct)
//   v.payment().index();                     // a union (std::variant member)
//   v.customer().unpack();                   // the UserProfile value
//
// The surface is the one every psio view has (psio/view.hpp): a named
// reader per reflected member, `get<N>()`, lists with size / [] / iteration,
// optionals, variants, `unpack()`, comparison with values.
//
// Where the bytes come from. The frame's shape views hold a span of the
// value's bytes; a Cap'n Proto value is not a contiguous run of bytes (a
// struct is two sections found through a pointer, a Bool is one bit, a
// scalar is XORed with its default), so the views here are specializations
// of `psio::view` for the capnp tag that hold a cursor instead — the data
// section, the section sizes, and the message for far pointers. Named
// readers, iterators and comparisons are the frame's.
//
// A view reads a VALIDATED message (`psio::validate_capnp<T>`, or one
// `psio::to_capnp` produced). Validation is what makes navigation safe:
// every pointer a T can reach has been checked. On bytes that were never
// validated a view is the caller's risk, as everywhere in PSIO.
//
// A view reads any struct size: a member beyond the sections the writer
// wrote (an older schema) reads as its default, exactly as the official
// reader does.
//
// Writing:
//   psio::capnp_mutable_view<Order> m{bytes};   // std::span<char>: scalars in place
//   m.customer().age().set(38);
//   psio::capnp_document<Order> doc{std::move(bytes)};  // size-changing edits
//   doc.edit().customer().name().set("Bob");    // appends the new Text, repoints
//   doc.compact();                              // re-encodes, dropping the old bytes
// See "Editing" below for why append-and-repoint is sound for this format.

#include <psio/buffer.hpp>
#include <psio/capnp.hpp>
#include <psio/view.hpp>

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
      // The capnp views are the specializations in this header; the trait
      // exists so that `psio::capnp_view<T>` names them.
      template <>
      struct traits<::psio::capnp>
      {
      };
   }  // namespace view_layout

   namespace detail::capnp_impl {

      // The format tag of a union member's view: the union of member I of
      // record T (its slots are part of T's layout).
      template <typename T, std::size_t I>
      struct in
      {
      };

      template <typename F, storage Store>
      using pointer_view = view<F, ::psio::capnp, Store>;

      // What reading a value of type F yields: the value for a scalar, a
      // view otherwise.
      template <typename F, storage Store>
      using read_t = std::conditional_t<Scalar<F>, F, pointer_view<F, Store>>;

      // The value behind the pointer at `slot` (which may be nullptr: the
      // writer's struct has no such slot).
      template <Pointer P, storage Store>
      [[gnu::always_inline]] inline pointer_view<P, Store> read_pointer(
         const std::uint8_t* msg, const std::uint8_t* slot) noexcept
      {
         static constexpr std::uint64_t null_word = 0;
         if (slot == nullptr) [[unlikely]]
            slot = reinterpret_cast<const std::uint8_t*>(&null_word);
         if constexpr (std::is_same_v<P, std::string>)
         {
            const auto text = text_at(msg, slot);
            return pointer_view<P, Store>{std::span<const char>{text.data(), text.size()}};
         }
         else if constexpr (is_vector_v<P> || is_array_v<P>)
            return pointer_view<P, Store>{list_at(msg, slot)};
         else if constexpr (is_optional_v<P>)
            return pointer_view<P, Store>{msg, slot};
         else
            return pointer_view<P, Store>{struct_at(msg, slot)};
      }

      template <typename Alt, storage Store>
      using alt_read_t =
         std::conditional_t<std::is_same_v<Alt, std::monostate>, std::monostate, read_t<Alt, Store>>;

      template <typename Alt, storage Store>
      [[gnu::always_inline]] inline alt_read_t<Alt, Store> read_alt(const struct_ref& s,
                                                                    slot loc) noexcept
      {
         if constexpr (std::is_same_v<Alt, std::monostate>)
            return {};
         else if constexpr (Scalar<Alt>)
            return read_scalar<Alt>(s, loc, Alt{});
         else
            return read_pointer<Alt, Store>(s.msg, pointer_slot(s, loc));
      }

      template <typename V>
      inline constexpr bool is_byte_v =
         std::is_same_v<V, char> || std::is_same_v<V, signed char> ||
         std::is_same_v<V, unsigned char> || std::is_same_v<V, std::byte>;

      // Member I of record T.
      template <Record T, std::size_t I, storage Store>
      [[gnu::always_inline]] inline auto read_member(const struct_ref& s) noexcept
      {
         using L = layout<T>;
         using F = typename L::template member<I>;
         constexpr slot loc = L::field(I);
         if constexpr (is_union_v<F>)
            return view<F, in<T, I>, Store>{s};
         else if constexpr (Scalar<F>)
            return read_scalar<F>(s, loc, member_default<T, I>());
         else
            return read_pointer<F, Store>(s.msg, pointer_slot(s, loc));
      }

      // What reflect<T>::proxy is given: the struct, and member I.
      template <typename T, storage Store>
      class record_projection
      {
       protected:
         struct_ref s_;

       public:
         record_projection() = default;
         explicit record_projection(const struct_ref& s) noexcept : s_(s) {}

         template <std::size_t I, auto MemberPtr>
         [[nodiscard]] auto get() const noexcept
         {
            return read_member<T, I, Store>(s_);
         }
      };

   }  // namespace detail::capnp_impl

   // ── Record ──────────────────────────────────────────────────────────────
   template <typename T, storage Store>
      requires detail::capnp_impl::Record<T>
   class view<T, capnp, Store>
       : public detail::named_base_t<T, detail::capnp_impl::record_projection<T, Store>>
   {
      using projection = detail::capnp_impl::record_projection<T, Store>;
      using base       = detail::named_base_t<T, projection>;

    public:
      using element_type                    = T;
      using format_type                     = capnp;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);

      view() = default;
      // The root struct of a validated, framed message.
      explicit view(std::span<const char> message) noexcept
          : base(projection{message.empty() ? detail::capnp_impl::struct_ref{}
                                            : detail::capnp_impl::root_of(message)})
      {
      }
      explicit view(const detail::capnp_impl::struct_ref& s) noexcept : base(projection{s}) {}

      // The struct's own bytes: its data section, then its pointer section.
      [[nodiscard]] std::span<const char> _psio_data() const noexcept
      {
         const auto& s = this->projection::s_;
         return {reinterpret_cast<const char*>(s.data),
                 std::size_t{s.data_bytes} + std::size_t{s.ptr_count} * 8};
      }
      [[nodiscard]] const detail::capnp_impl::struct_ref& _psio_struct() const noexcept
      {
         return this->projection::s_;
      }

      // Member N: its value for a scalar, its view otherwise.
      template <std::size_t N>
      [[nodiscard]] auto get() const noexcept
      {
         return detail::capnp_impl::read_member<T, N, Store>(this->projection::s_);
      }

      [[nodiscard]] T unpack() const
      {
         T out{};
         detail::capnp_impl::decode_struct(this->projection::s_, out);
         return out;
      }
   };

   // ── List ────────────────────────────────────────────────────────────────
   namespace detail::capnp_impl {
      template <typename Seq, typename E, storage Store>
      class list_view_base
      {
       protected:
         list_ref l_;

       public:
         using value_type = E;

         list_view_base() = default;
         explicit list_view_base(const list_ref& l) noexcept : l_(l) {}

         [[nodiscard]] std::size_t size() const noexcept { return l_.count; }
         [[nodiscard]] bool        empty() const noexcept { return l_.count == 0; }

         [[nodiscard]] read_t<E, Store> operator[](std::size_t i) const noexcept
         {
            if constexpr (std::is_same_v<E, bool>)
               return ((l_.data[i >> 3] >> (i & 7)) & 1) != 0;
            else if constexpr (Scalar<E>)
            {
               E v;
               std::memcpy(static_cast<void*>(&v), l_.data + i * sizeof(E), sizeof(E));
               return v;
            }
            else if constexpr (Record<E>)
               return read_t<E, Store>{struct_ref{l_.data + i * std::size_t{l_.step}, l_.msg,
                                                  l_.elem_data_bytes, l_.elem_ptr_count}};
            else
               return read_pointer<E, Store>(l_.msg, l_.data + i * 8);
         }
         // Element i, or a default-constructed result past the end.
         [[nodiscard]] read_t<E, Store> at(std::size_t i) const noexcept
         {
            return i < l_.count ? (*this)[i] : read_t<E, Store>{};
         }
         // Precondition: !empty().
         [[nodiscard]] read_t<E, Store> front() const noexcept { return (*this)[0]; }
         [[nodiscard]] read_t<E, Store> back() const noexcept { return (*this)[l_.count - 1]; }

         [[nodiscard]] std::span<const char> _psio_data() const noexcept
         {
            const std::size_t bytes =
               std::is_same_v<E, bool> ? (std::size_t{l_.count} + 7) / 8
                                       : std::size_t{l_.count} * l_.step;
            return {reinterpret_cast<const char*>(l_.data), bytes};
         }
         [[nodiscard]] const list_ref& _psio_list() const noexcept { return l_; }

         // A list of bytes (Data) is its bytes.
         operator std::span<const char>() const noexcept
            requires is_byte_v<E>
         {
            return _psio_data();
         }
         [[nodiscard]] const char* data() const noexcept
            requires is_byte_v<E>
         {
            return reinterpret_cast<const char*>(l_.data);
         }
      };
   }  // namespace detail::capnp_impl

   template <typename E, typename A, storage Store>
   class view<std::vector<E, A>, capnp, Store>
       : public detail::capnp_impl::list_view_base<std::vector<E, A>, E, Store>
   {
      using base = detail::capnp_impl::list_view_base<std::vector<E, A>, E, Store>;

    public:
      using element_type                    = std::vector<E, A>;
      using format_type                     = capnp;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);
      using base::base;
      view() = default;

      using iterator         = detail::view_iterator<view>;
      using const_iterator   = iterator;
      using reverse_iterator = std::reverse_iterator<iterator>;
      [[nodiscard]] iterator         begin() const noexcept { return {*this, 0}; }
      [[nodiscard]] iterator         end() const noexcept { return {*this, this->size()}; }
      [[nodiscard]] reverse_iterator rbegin() const noexcept { return reverse_iterator{end()}; }
      [[nodiscard]] reverse_iterator rend() const noexcept { return reverse_iterator{begin()}; }

      [[nodiscard]] element_type unpack() const
      {
         element_type out;
         out.resize(this->l_.count);
         if constexpr (std::is_same_v<E, bool>)
         {
            for (std::size_t i = 0; i < out.size(); ++i)
               out[i] = (*this)[i];
         }
         else
            detail::capnp_impl::decode_list<E>(this->l_, out.data(), out.size());
         return out;
      }
   };

   template <typename E, std::size_t N, storage Store>
   class view<std::array<E, N>, capnp, Store>
       : public detail::capnp_impl::list_view_base<std::array<E, N>, E, Store>
   {
      using base = detail::capnp_impl::list_view_base<std::array<E, N>, E, Store>;

    public:
      using element_type                    = std::array<E, N>;
      using format_type                     = capnp;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);
      using base::base;
      view() = default;

      using iterator         = detail::view_iterator<view>;
      using const_iterator   = iterator;
      using reverse_iterator = std::reverse_iterator<iterator>;
      [[nodiscard]] iterator         begin() const noexcept { return {*this, 0}; }
      [[nodiscard]] iterator         end() const noexcept { return {*this, this->size()}; }
      [[nodiscard]] reverse_iterator rbegin() const noexcept { return reverse_iterator{end()}; }
      [[nodiscard]] reverse_iterator rend() const noexcept { return reverse_iterator{begin()}; }

      [[nodiscard]] element_type unpack() const
      {
         element_type out{};
         detail::capnp_impl::decode_list<E>(this->l_, out.data(),
                                            std::min<std::size_t>(this->l_.count, N));
         return out;
      }
   };

   // ── Optional pointer: the pointer; null is "no value" ───────────────────
   template <typename P, storage Store>
      requires detail::capnp_impl::Pointer<P>
   class view<std::optional<P>, capnp, Store>
   {
      const std::uint8_t* msg_  = nullptr;
      const std::uint8_t* slot_ = nullptr;

      using payload = detail::capnp_impl::pointer_view<P, Store>;

    public:
      using element_type                    = std::optional<P>;
      using format_type                     = capnp;
      using value_type                      = P;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);

      view() = default;
      view(const std::uint8_t* msg, const std::uint8_t* slot) noexcept : msg_(msg), slot_(slot) {}

      [[nodiscard]] std::span<const char> _psio_data() const noexcept
      {
         return {reinterpret_cast<const char*>(slot_), slot_ ? std::size_t{8} : std::size_t{0}};
      }

      [[nodiscard]] bool has_value() const noexcept
      {
         return slot_ != nullptr && detail::capnp_impl::load64(slot_) != 0;
      }
      [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

      // Precondition: has_value().
      [[nodiscard]] payload operator*() const noexcept
      {
         return detail::capnp_impl::read_pointer<P, Store>(msg_, slot_);
      }
      [[nodiscard]] payload get() const noexcept { return **this; }
      [[nodiscard]] detail::arrow_proxy<payload> operator->() const noexcept { return {**this}; }
      [[nodiscard]] payload value() const
      {
         if (!has_value())
            detail::view_absent("psio::view<std::optional<T>>::value(): no value");
         return **this;
      }
      template <typename U>
      [[nodiscard]] P value_or(U&& fallback) const
      {
         if (has_value())
            return (**this).unpack();
         return static_cast<P>(std::forward<U>(fallback));
      }
      [[nodiscard]] element_type unpack() const
      {
         element_type out;
         if (has_value())
            out.emplace((**this).unpack());
         return out;
      }
   };

   // ── Optional scalar: union { none :Void; some :S } ──────────────────────
   template <typename S, typename T, std::size_t I, storage Store>
   class view<std::optional<S>, detail::capnp_impl::in<T, I>, Store>
   {
      detail::capnp_impl::struct_ref s_;
      using L = detail::capnp_impl::layout<T>;

    public:
      using element_type                    = std::optional<S>;
      using format_type                     = detail::capnp_impl::in<T, I>;
      using value_type                      = S;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);

      view() = default;
      explicit view(const detail::capnp_impl::struct_ref& s) noexcept : s_(s) {}

      [[nodiscard]] std::span<const char> _psio_data() const noexcept
      {
         return {reinterpret_cast<const char*>(s_.data), s_.data_bytes};
      }
      [[nodiscard]] bool has_value() const noexcept
      {
         return detail::capnp_impl::read_scalar<std::uint16_t>(s_, L::field(I), 0) != 0;
      }
      [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
      // Precondition: has_value().
      [[nodiscard]] S operator*() const noexcept
      {
         return detail::capnp_impl::read_scalar<S>(s_, L::alt(I, 1), S{});
      }
      [[nodiscard]] S get() const noexcept { return **this; }
      [[nodiscard]] S value() const
      {
         if (!has_value())
            detail::view_absent("psio::view<std::optional<T>>::value(): no value");
         return **this;
      }
      template <typename U>
      [[nodiscard]] S value_or(U&& fallback) const
      {
         return has_value() ? **this : static_cast<S>(std::forward<U>(fallback));
      }
      [[nodiscard]] element_type unpack() const
      {
         return has_value() ? element_type{**this} : element_type{};
      }
   };

   // ── Union (std::variant member) ─────────────────────────────────────────
   template <typename... Ts, typename T, std::size_t I, storage Store>
   class view<std::variant<Ts...>, detail::capnp_impl::in<T, I>, Store>
   {
      detail::capnp_impl::struct_ref s_;
      using L = detail::capnp_impl::layout<T>;
      using V = std::variant<Ts...>;

      template <std::size_t J>
      using alt_t = detail::capnp_impl::alt_read_t<std::variant_alternative_t<J, V>, Store>;

      template <std::size_t J>
      [[nodiscard]] alt_t<J> alt() const noexcept
      {
         return detail::capnp_impl::read_alt<std::variant_alternative_t<J, V>, Store>(
            s_, L::alt(I, J));
      }

    public:
      using element_type                    = V;
      using format_type                     = detail::capnp_impl::in<T, I>;
      static constexpr storage storage_kind = Store;
      static_assert(sizeof(detail::read_view_store<Store>) > 0);

      view() = default;
      explicit view(const detail::capnp_impl::struct_ref& s) noexcept : s_(s) {}

      [[nodiscard]] std::span<const char> _psio_data() const noexcept
      {
         return {reinterpret_cast<const char*>(s_.data),
                 std::size_t{s_.data_bytes} + std::size_t{s_.ptr_count} * 8};
      }

      // The discriminant: which alternative is set.
      [[nodiscard]] std::size_t index() const noexcept
      {
         return detail::capnp_impl::read_scalar<std::uint16_t>(s_, L::field(I), 0);
      }

      // Calls fn with the active alternative: its value for a scalar,
      // std::monostate for Void, its view otherwise.
      template <typename F>
      auto visit(F&& fn) const
      {
         using R = std::common_type_t<
            std::invoke_result_t<F&, detail::capnp_impl::alt_read_t<Ts, Store>>...>;
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
         element_type        out;
         const std::uint16_t which = static_cast<std::uint16_t>(index());
         [&]<std::size_t... Js>(std::index_sequence<Js...>) {
            (void)((which == Js ? (detail::capnp_impl::decode_alt(s_, L::alt(I, Js),
                                                                  out.template emplace<Js>()),
                                   true)
                                : false) ||
                   ...);
         }(std::index_sequence_for<Ts...>{});
         return out;
      }
   };

   template <typename Alt, typename... Ts, typename T, std::size_t I, storage Store>
   [[nodiscard]] auto get_if(
      const view<std::variant<Ts...>, detail::capnp_impl::in<T, I>, Store>& v) noexcept
   {
      return v.template get_if<Alt>();
   }
   template <typename Alt, typename... Ts, typename T, std::size_t I, storage Store>
   [[nodiscard]] bool holds_alternative(
      const view<std::variant<Ts...>, detail::capnp_impl::in<T, I>, Store>& v) noexcept
   {
      return v.template holds_alternative<Alt>();
   }

   // ══════════════════════════════════════════════════════════════════════
   // Editing
   //
   // 1. In place — `psio::capnp_mutable_view<T>` over `std::span<char>`.
   //    A scalar of the data section is bits no pointer, count or tag is
   //    derived from, so overwriting it leaves a valid message that is the
   //    encoding of the modified value. A scalar the writer's struct has no
   //    room for (an older schema) cannot be written: `set` returns false.
   //
   // 2. Changing a size — `psio::capnp_document<T>`, which owns the bytes.
   //    Cap'n Proto pointers are relative offsets and objects may sit
   //    anywhere in a segment, so a pointer member is replaced by appending
   //    the new object to the end of the segment and rewriting the one
   //    pointer word; nothing else in the message refers to the old object
   //    or to the position of the new one. This is what the official
   //    builders do when a field is set twice, and the result is a valid
   //    message for every reader. It is NOT the splice-and-patch scheme the
   //    first generation used for fracpack: no existing byte moves.
   //    The cost is that the replaced object stays in the message as
   //    unreachable bytes (so do not rely on an edit to erase a secret);
   //    `compact()` re-encodes the value and drops them.
   //    Limits: the message must be a single segment (what psio writes; a
   //    multi-segment message is compacted on construction) and the struct
   //    must have the slot (false otherwise — `compact()` first, which
   //    re-encodes every struct at this schema's size).
   // ══════════════════════════════════════════════════════════════════════

   namespace detail::capnp_impl {

      // A scalar in a struct of the message: read, or overwrite in place.
      template <Scalar S>
      class scalar_ref
      {
         std::uint8_t* data_       = nullptr;
         std::uint32_t data_bytes_ = 0;
         slot          loc_{};
         S             default_{};

       public:
         scalar_ref() = default;
         scalar_ref(std::uint8_t* data, std::uint32_t data_bytes, slot loc, S def) noexcept
             : data_(data), data_bytes_(data_bytes), loc_(loc), default_(def)
         {
         }
         [[nodiscard]] S get() const noexcept
         {
            return read_scalar<S>(struct_ref{data_, nullptr, data_bytes_, 0}, loc_, default_);
         }
         operator S() const noexcept { return get(); }
         // False when the struct the writer wrote ends before this member.
         bool set(S v) const noexcept
         {
            const std::uint32_t byte = loc_.byte();
            if constexpr (std::is_same_v<S, bool>)
            {
               if (byte >= data_bytes_)
                  return false;
               const auto mask = static_cast<std::uint8_t>(1u << loc_.bit());
               if (v != default_)
                  data_[byte] |= mask;
               else
                  data_[byte] &= static_cast<std::uint8_t>(~mask);
            }
            else
            {
               if (byte + sizeof(S) > data_bytes_)
                  return false;
               const bits_t<S> wire = static_cast<bits_t<S>>(to_bits(v) ^ to_bits(default_));
               std::memcpy(data_ + byte, &wire, sizeof(S));
            }
            return true;
         }
      };

      template <typename T>
      class mutable_projection
      {
       protected:
         struct_ref s_;  // over mutable bytes

       public:
         mutable_projection() = default;
         explicit mutable_projection(const struct_ref& s) noexcept : s_(s) {}

         template <std::size_t I>
         [[nodiscard]] auto _psio_member() const noexcept
         {
            using L = layout<T>;
            using F = typename L::template member<I>;
            if constexpr (Scalar<F>)
               return scalar_ref<F>{const_cast<std::uint8_t*>(s_.data), s_.data_bytes, L::field(I),
                                    member_default<T, I>()};
            else if constexpr (Record<F>)
               return ::psio::mutable_view<F, ::psio::capnp>{
                  struct_at(s_.msg, pointer_slot_or_null(s_, L::field(I)))};
            else
               return read_member<T, I, storage::const_borrow>(s_);
         }
         template <std::size_t I, auto MemberPtr>
         [[nodiscard]] auto get() const noexcept
         {
            return _psio_member<I>();
         }

       private:
         static const std::uint8_t* pointer_slot_or_null(const struct_ref& s, slot loc) noexcept
         {
            static constexpr std::uint64_t null_word = 0;
            const auto*                    p         = pointer_slot(s, loc);
            return p ? p : reinterpret_cast<const std::uint8_t*>(&null_word);
         }
      };
   }  // namespace detail::capnp_impl

   template <>
   inline constexpr bool detail::helpers::mutable_view_format_v<::psio::capnp> = true;

   // In-place writes to the scalars of a validated message. Navigation is
   // by the same names as the read view: a scalar member is a reference
   // with get() / set(), a record member is a mutable view of it, anything
   // else reads as in the read view.
   template <typename T>
      requires detail::capnp_impl::Record<T>
   class mutable_view<T, capnp>
       : public detail::named_base_t<T, detail::capnp_impl::mutable_projection<T>>
   {
      using projection = detail::capnp_impl::mutable_projection<T>;
      using base       = detail::named_base_t<T, projection>;

    public:
      using element_type = T;
      using format_type  = capnp;

      mutable_view() = default;
      explicit mutable_view(std::span<char> message) noexcept
          : base(projection{
               detail::capnp_impl::root_of(std::span<const char>{message.data(), message.size()})})
      {
      }
      explicit mutable_view(std::vector<char>& message) noexcept
          : mutable_view(std::span<char>{message})
      {
      }
      explicit mutable_view(const detail::capnp_impl::struct_ref& s) noexcept : base(projection{s})
      {
      }

      template <std::size_t N>
      [[nodiscard]] auto get() const noexcept
      {
         return this->projection::template _psio_member<N>();
      }
      // The read view of the same struct.
      [[nodiscard]] view<T, capnp, storage::const_borrow> as_view() const noexcept
      {
         return view<T, capnp, storage::const_borrow>{this->projection::s_};
      }
      [[nodiscard]] T unpack() const { return as_view().unpack(); }
   };

   template <typename T>
   [[nodiscard]] view<T, capnp, storage::const_borrow> as_view(
      const mutable_view<T, capnp>& m) noexcept
   {
      return m.as_view();
   }

   // ── capnp_document: a message that can be edited ────────────────────────
   template <typename T>
      requires detail::capnp_impl::Record<T>
   class capnp_document;

   namespace detail::capnp_impl {

      // A position in a document that survives reallocation: byte offsets.
      struct struct_pos
      {
         std::size_t   data       = 0;  // offset of the data section in the bytes; 0 = null
         std::uint32_t data_bytes = 0;
         std::uint16_t ptr_count  = 0;
      };

      template <typename Root, typename F>
      class editor;

      // The editor of a pointer member (Text, list, record, optional
      // pointer): `set(value)` appends the new object and repoints.
      template <typename Root, Pointer P>
      class pointer_editor
      {
         capnp_document<Root>* doc_  = nullptr;
         std::size_t           slot_ = 0;  // byte offset of the pointer word; 0 = no slot

       public:
         pointer_editor() = default;
         pointer_editor(capnp_document<Root>* doc, std::size_t slot) noexcept
             : doc_(doc), slot_(slot)
         {
         }
         // The current value's view.
         [[nodiscard]] pointer_view<P, storage::const_borrow> view() const noexcept
         {
            const auto* msg = reinterpret_cast<const std::uint8_t*>(doc_->bytes().data());
            return read_pointer<P, storage::const_borrow>(msg, slot_ ? msg + slot_ : nullptr);
         }
         // Replaces the value. False when the writer's struct has no such
         // slot: compact() the document first.
         template <typename U>
            requires std::constructible_from<P, U&&>
         bool set(U&& value) const
         {
            if (slot_ == 0)
               return false;
            const P v(std::forward<U>(value));
            return doc_->replace_pointer(slot_, v);
         }
         // Members of a record behind this pointer (the record must be
         // present; a null record has no members to edit — set() it).
         [[nodiscard]] auto operator->() const noexcept
            requires Record<P>
         {
            return arrow_proxy<editor<Root, P>>{editor<Root, P>{doc_, doc_->struct_at_slot(slot_)}};
         }
      };

      template <typename Root, typename T>
      class edit_projection
      {
       protected:
         capnp_document<Root>* doc_ = nullptr;
         struct_pos            pos_{};

       public:
         edit_projection() = default;
         edit_projection(capnp_document<Root>* doc, struct_pos pos) noexcept : doc_(doc), pos_(pos)
         {
         }
         template <std::size_t I>
         [[nodiscard]] auto _psio_member() const noexcept
         {
            using L = layout<T>;
            using F = typename L::template member<I>;
            auto* base = reinterpret_cast<std::uint8_t*>(doc_->mutable_bytes().data());
            if constexpr (Scalar<F>)
               return scalar_ref<F>{base + pos_.data, pos_.data_bytes, L::field(I),
                                    member_default<T, I>()};
            else if constexpr (is_union_v<F>)
               return read_member<T, I, storage::const_borrow>(
                  struct_ref{base + pos_.data, base, pos_.data_bytes, pos_.ptr_count});
            else
            {
               constexpr slot    loc  = L::field(I);
               const std::size_t slot = loc.offset < pos_.ptr_count
                                           ? pos_.data + pos_.data_bytes + std::size_t{loc.offset} * 8
                                           : 0;
               if constexpr (Record<F>)
                  return editor<Root, F>{doc_, slot ? doc_->struct_at_slot(slot) : struct_pos{},
                                         slot};
               else
                  return pointer_editor<Root, F>{doc_, slot};
            }
         }
         template <std::size_t I, auto MemberPtr>
         [[nodiscard]] auto get() const noexcept
         {
            return _psio_member<I>();
         }
      };

      // The editor of a record: named members (scalars are references,
      // records are editors, other pointers are pointer_editors), and
      // `set(value)` to replace the record as a whole.
      template <typename Root, typename T>
      class editor : public named_base_t<T, edit_projection<Root, T>>
      {
         using projection = edit_projection<Root, T>;
         using base       = named_base_t<T, projection>;
         std::size_t slot_ = 0;  // the pointer to this record; 0 for the root

       public:
         editor() = default;
         editor(capnp_document<Root>* doc, struct_pos pos, std::size_t slot = 0) noexcept
             : base(projection{doc, pos}), slot_(slot)
         {
         }
         template <std::size_t N>
         [[nodiscard]] auto get() const noexcept
         {
            return this->projection::template _psio_member<N>();
         }
         // Replaces the whole record (not available for the root: assign
         // the document).
         bool set(const T& value) const
         {
            return slot_ != 0 && this->projection::doc_->replace_pointer(slot_, value);
         }
      };
   }  // namespace detail::capnp_impl

   template <typename T>
      requires detail::capnp_impl::Record<T>
   class capnp_document
   {
      std::vector<char> bytes_;

      template <typename, detail::capnp_impl::Pointer>
      friend class detail::capnp_impl::pointer_editor;
      template <typename, typename>
      friend class detail::capnp_impl::edit_projection;
      template <typename, typename>
      friend class detail::capnp_impl::editor;

      // A builder that appends to bytes_ instead of starting a message.
      struct appender
      {
         static constexpr bool counts_only = false;
         std::vector<char>&    out;
         std::uint32_t         alloc(std::uint32_t n)
         {
            const std::size_t at = (out.size() - 8) / 8;
            if (at + n > (std::size_t{1} << 29))
               detail::capnp_impl::encode_too_large();
            out.resize(out.size() + std::size_t{n} * 8);
            return static_cast<std::uint32_t>(at);
         }
         std::uint8_t* bytes(std::uint32_t word) noexcept
         {
            return reinterpret_cast<std::uint8_t*>(out.data()) + 8 + std::size_t{word} * 8;
         }
         void put(std::uint32_t word, std::uint64_t v) noexcept { std::memcpy(bytes(word), &v, 8); }
      };

      template <detail::capnp_impl::Pointer P>
      bool replace_pointer(std::size_t slot, const P& value)
      {
         const std::uint32_t at = static_cast<std::uint32_t>((slot - 8) / 8);
         appender            b{bytes_};
         b.put(at, 0);
         detail::capnp_impl::encode_pointer(b, at, value);
         const std::uint32_t words = static_cast<std::uint32_t>((bytes_.size() - 8) / 8);
         std::memcpy(bytes_.data() + 4, &words, 4);
         return true;
      }
      detail::capnp_impl::struct_pos struct_at_slot(std::size_t slot) const noexcept
      {
         const auto* msg = reinterpret_cast<const std::uint8_t*>(bytes_.data());
         const auto  s   = detail::capnp_impl::struct_at(msg, msg + slot);
         if (s.data == nullptr)
            return {};
         return {static_cast<std::size_t>(s.data - msg), s.data_bytes, s.ptr_count};
      }
      std::span<char> mutable_bytes() noexcept { return bytes_; }

    public:
      using element_type = T;

      capnp_document() : bytes_(::psio::encode(capnp{}, T{})) {}
      explicit capnp_document(const T& value) : bytes_(::psio::encode(capnp{}, value)) {}
      // Takes a VALIDATED message. A multi-segment message is re-encoded
      // as one segment.
      explicit capnp_document(std::vector<char> message) : bytes_(std::move(message))
      {
         if (bytes_.size() < 8 || detail::capnp_impl::load32(
                                     reinterpret_cast<const std::uint8_t*>(bytes_.data())) != 0)
            compact();
      }
      capnp_document& operator=(const T& value)
      {
         detail::capnp_impl::encode_into(value, bytes_);
         return *this;
      }

      [[nodiscard]] std::span<const char> bytes() const noexcept { return bytes_; }
      [[nodiscard]] std::vector<char>     release() && noexcept { return std::move(bytes_); }

      [[nodiscard]] view<T, capnp, storage::const_borrow> view() const noexcept
      {
         return ::psio::view<T, capnp, storage::const_borrow>{bytes()};
      }
      [[nodiscard]] T unpack() const { return view().unpack(); }

      // Named access for writing; see "Editing" above.
      [[nodiscard]] detail::capnp_impl::editor<T, T> edit() noexcept
      {
         const auto* msg = reinterpret_cast<const std::uint8_t*>(bytes_.data());
         const auto  s   = detail::capnp_impl::root_of(bytes());
         return {this, s.data ? detail::capnp_impl::struct_pos{static_cast<std::size_t>(s.data - msg),
                                                               s.data_bytes, s.ptr_count}
                              : detail::capnp_impl::struct_pos{}};
      }

      // Re-encodes the value: one segment, every struct at this schema's
      // size, no unreachable bytes.
      void compact()
      {
         T value{};
         if (bytes_.size() >= 8)
            detail::capnp_impl::decode_struct(detail::capnp_impl::root_of(bytes()), value);
         detail::capnp_impl::encode_into(value, bytes_);
      }
   };

}  // namespace psio
