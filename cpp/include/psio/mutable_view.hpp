#pragma once
//
// psio/mutable_view.hpp — in-place mutation of a PSSZ frame.
//
// `psio::mutable_view<T, Fmt>` is a handle onto caller-owned, mutable,
// already validated PSSZ bytes. It navigates exactly like the read view
// (`view<T, Fmt, Store>`, psio/view.hpp) and adds one operation, `set`,
// which overwrites a fixed-size value with another value of the same type.
//
// Why only that operation (docs/pssz-spec.md §17 has the full argument):
// a PSSZ value has exactly one encoding, with no padding and no stored
// lengths. The size of every variable-size value is derived from the
// offsets around it and trailing absent optionals are pruned, so changing
// the size or the presence of anything would move bytes and rewrite
// offsets and headers elsewhere in the frame. A fixed-size value is
// different: its bytes are owned by it alone, no offset, header, selector,
// count or pruning decision reads them, and `set` writes the canonical
// encoding of the new value over the old one. The frame is therefore the
// canonical encoding of the modified value, exactly as if it had been
// decoded, modified and encoded again.
//
// What can be written: a value for which `pssz_impl::is_fixed_v` holds —
// bool, integers, floats, enums, durations / time points, uint128 / int128
// / uint256, bitvector<N>, std::array of those, and a DWNC record whose
// members are all fixed-size (overwritten as a whole or member by member).
// Where it can be written: anywhere the read view can reach it — a record
// member (extensible or DWNC), a tuple or pair member, an array or vector
// element, the payload of a present optional, the active alternative of a
// variant, the mapped value of a map entry, and the inner value of a
// psio::nested<T> (a write there leaves the nested bytes the canonical
// encoding of the modified inner value, by the same argument).
//
// What is rejected at compile time, with a static_assert naming the reason:
//   - `set` on anything variable-size: strings, vectors, optionals
//     (presence cannot change), variants (the alternative cannot change),
//     extensible records, bitlists, adapter-encoded values;
//   - a view of a type whose in-place mutation is blocked
//     (`detail::mutation_blocked_v`), or `set` of a value that contains one;
//   - a view of a set, and any write access to the key of a map entry: the
//     elements / keys are in strictly ascending order of their key bytes,
//     which an overwrite could break (a map's keys are readable, and its
//     mapped values are views);
//   - a view of a psio::nested<T, nested_validation::deferred>: no reader
//     of the frame has checked that its bytes are a T;
//   - navigation into a record member carrying an `as<Tag>` override or an
//     annotation that constrains its content (sorted_spec, element_spec,
//     unique_keys_spec, any spec with a validator);
//   - any format other than PSSZ.
//
// Like the read view, a mutable view assumes validated input and does not
// re-validate. It never allocates and never throws.
//
//    std::vector<char> bytes = psio::encode(psio::pssz{}, account);
//    psio::mutable_view<Account, psio::pssz> mv{bytes};
//    bool written = mv.get<1>().set(std::uint64_t{42});   // member 1

#include <psio/buffer.hpp>
#include <psio/detail/assoc.hpp>
#include <psio/detail/nested_fwd.hpp>
#include <psio/invariant.hpp>
#include <psio/pssz_view.hpp>
#include <psio/storage.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace psio {

   template <typename T, typename Fmt>
   class mutable_view;
   template <typename K, typename V, typename Fmt>
   class mutable_map_entry;

   // The formats that have the `F_mutable_view<T>` helper (psio/helpers.hpp).
   template <typename Fmt>
      requires detail::is_pssz_format_v<Fmt>
   inline constexpr bool detail::helpers::mutable_view_format_v<Fmt> = true;

   namespace detail {

      // ── The extension point for types that must not be mutated in place ──
      //
      // Specialize to `true` for a type whose bytes carry a constraint that
      // a size-preserving write of one of its parts could break — a semantic
      // invariant spanning several members, for example. `mutable_view`
      // consults it for every type on the navigation path (the root, each
      // record / container / optional / variant passed through, and the
      // leaf) and for every type nested inside the value given to `set`:
      // such a type has no mutable view at all, so nothing beneath it can
      // be written. The read view is unaffected.
      //
      //    template <>
      //    inline constexpr bool psio::detail::mutation_blocked_v<Sorted> = true;
      //
      // The specialization must be visible before the first mutable view of
      // the type (or of a type containing it) is named.
      //
      // The same trait is consulted for the annotation specs of a record
      // member, because a spec can constrain the member's content just as a
      // type can: a member carrying a blocked spec is not addressable. The
      // library's own content-constraining specs are blocked below, and so
      // is every spec that has a `validate(std::span<const char>)` member.
      //
      // A type with an invariant (invariant.hpp) is blocked without being
      // told: the invariant is exactly such a constraint, and a leaf write
      // beneath the type would not run it.
      template <typename T>
      inline constexpr bool mutation_blocked_v = ::psio::has_invariant_v<T>;

      template <>
      inline constexpr bool mutation_blocked_v<::psio::sorted_spec> = true;
      template <>
      inline constexpr bool mutation_blocked_v<::psio::unique_keys_spec> = true;
      template <typename Child>
      inline constexpr bool mutation_blocked_v<::psio::element_spec<Child>> = true;

      namespace mutable_view_impl {

         namespace pz = ::psio::detail::pssz_impl;

         // `part` is a span the read-side layout computed inside `whole`;
         // return the same range with write access. A range outside `whole`
         // (the read view's static stand-in for a pruned optional, or the
         // empty span of an absent position) yields an empty span, so no
         // write can ever land outside the caller's bytes.
         inline std::span<char> rebase(std::span<char>       whole,
                                       std::span<const char> part) noexcept
         {
            const char* const first = whole.data();
            if (first == nullptr || part.data() == nullptr)
               return {};
            const std::less_equal<const char*> ordered;
            if (!ordered(first, part.data()) ||
                !ordered(part.data() + part.size(), first + whole.size()))
               return {};
            return whole.subspan(static_cast<std::size_t>(part.data() - first), part.size());
         }

         // Whether any annotation spec in `Annotations` (a std::tuple of
         // specs) constrains the content of the member it is attached to.
         template <typename Annotations>
         inline constexpr bool blocked_annotations_v = false;
         template <typename... Specs>
         inline constexpr bool blocked_annotations_v<std::tuple<Specs...>> =
            ((mutation_blocked_v<Specs> || vstrict::has_span_validate_v<Specs>) || ... || false);

         // Whether the annotations of a record member of type `Member` keep
         // it from being navigated into. A map / set member carries the
         // order and uniqueness of its keys as annotations (sorted_spec,
         // unique_keys_spec): those say what the type itself is, and the
         // view of the type upholds them — a map's keys are read-only, a
         // set has no mutable view.
         template <typename Member, typename Spec>
         inline constexpr bool blocked_spec_v =
            (mutation_blocked_v<Spec> || vstrict::has_span_validate_v<Spec>) &&
            !(::psio::detail::is_assoc_v<Member> &&
              (std::is_same_v<Spec, ::psio::sorted_spec> ||
               std::is_same_v<Spec, ::psio::unique_keys_spec>));
         template <typename Member, typename Annotations>
         inline constexpr bool blocked_member_v = false;
         template <typename Member, typename... Specs>
         inline constexpr bool blocked_member_v<Member, std::tuple<Specs...>> =
            (blocked_spec_v<Member, Specs> || ... || false);

         // Whether a fixed-size value, or anything nested in it (a nested
         // type, or a record member through its annotations), is blocked.
         template <typename T>
         consteval bool contains_blocked()
         {
            if constexpr (mutation_blocked_v<T>)
               return true;
            else if constexpr (pz::is_psio_box_v<T>)
               return contains_blocked<typename pz::is_psio_box<T>::elem>();
            else if constexpr (pz::is_std_array_v<T>)
               return contains_blocked<typename T::value_type>();
            else if constexpr (::psio::is_transparent_v<T>)
               return contains_blocked<::psio::transparent_inner_t<T>>();
            else if constexpr (pz::Record<T>)
               return []<std::size_t... Is>(std::index_sequence<Is...>) {
                  return ((blocked_annotations_v<std::remove_cvref_t<
                              typename pz::record_member_annotations<T, Is>::value_t>> ||
                           contains_blocked<std::remove_cvref_t<
                              typename pz::record_traits<T>::template member_type<Is>>>()) ||
                          ... || false);
               }(std::make_index_sequence<pz::record_traits<T>::member_count>{});
            else
               return false;
         }

         // Whether a fixed-size C++ value has a valid encoding. Almost every
         // one does; the exceptions are a null Box and a bitvector<N> whose
         // unused high bits were set through its raw storage (the validator
         // requires them to be zero). A bool is always written as 0 or 1.
         template <typename T>
         constexpr bool encodable(const T& value) noexcept
         {
            if constexpr (pz::is_psio_box_v<T>)
               return value.value && encodable(*value.value);
            else if constexpr (pz::is_bitvector_v<T>)
            {
               if constexpr (T::size_value % 8 != 0)
                  return (value.data()[T::bytes_value - 1] >> (T::size_value % 8)) == 0;
               else
                  return true;
            }
            else if constexpr (pz::is_std_array_v<T>)
            {
               for (const auto& element : value)
                  if (!encodable(element))
                     return false;
               return true;
            }
            else if constexpr (pz::Record<T>)
               return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                  return (encodable(pz::record_member<Is>(value)) && ... && true);
               }(std::make_index_sequence<pz::record_traits<T>::member_count>{});
            else
               return true;
         }

         template <typename Fmt>
         inline constexpr bool is_auto_v = std::is_same_v<Fmt, ::psio::pssz>;

         template <typename Fmt>
         inline constexpr std::size_t explicit_width_v = 4;
         template <std::size_t W>
         inline constexpr std::size_t explicit_width_v<::psio::pssz_<W>> = W;

      }  // namespace mutable_view_impl
   }     // namespace detail

   // ── mutable_view<T, Fmt> ──────────────────────────────────────────────
   //
   // One template for every shape; the members available depend on T, as
   // they do across the read view's specializations:
   //
   //   record, tuple   get<N>()                       → mutable_view of member N
   //   pair            get<N>(), first(), second()    → mutable_view of the member
   //   vector / array  size(), empty(), [i], at(i),
   //                   begin(), end()                 → mutable_view of an element
   //   optional        has_value(), operator bool,
   //                   operator*, get()               → mutable_view of the payload
   //   variant         index(), visit(fn)             → fn(mutable_view of the alternative)
   //   map             size(), empty(), [i], at(i),
   //                   begin(), end()                 → mutable_map_entry: key() is read-only,
   //                                                    value() a mutable_view
   //   nested<T>       get(), operator*               → mutable_view<T, pssz> of the inner value
   //   scalar          get()                          → the value
   //   fixed-size T    set(value)                     → overwrite, see below
   //
   // Reads of anything else go through `psio::as_view(mv)`, the read view
   // over the same bytes.
   template <typename T, typename Fmt>
   class mutable_view
   {
      static_assert(!std::is_const_v<T>,
                    "psio::mutable_view<const T, Fmt>: a mutable view of a const type is a "
                    "contradiction; the read-only view is psio::view<T, Fmt>");
      static_assert(detail::is_pssz_format_v<Fmt>,
                    "psio::mutable_view: in-place mutation is defined for PSSZ formats only");
      static_assert(!detail::mutation_blocked_v<T>,
                    "psio::mutable_view: in-place mutation is blocked for this type "
                    "(psio::detail::mutation_blocked_v)");

      template <typename Child>
      using child_format = view_layout::nested_format_t<Fmt, Child>;
      using layout       = view_layout::traits<Fmt>;
      using read_view    = view<T, Fmt, storage::const_borrow>;

      static constexpr bool adapter_v = ::psio::format_should_dispatch_adapter_v<Fmt, T>;
      static constexpr bool record_v  = detail::pssz_impl::Record<T> && !adapter_v;
      static constexpr bool vector_v  = detail::pssz_impl::is_std_vector_v<T> && !adapter_v;
      static constexpr bool array_v   = detail::pssz_impl::is_std_array_v<T> && !adapter_v;
      static constexpr bool optional_v = detail::pssz_impl::is_std_optional_v<T> && !adapter_v;
      static constexpr bool variant_v  = detail::pssz_impl::is_std_variant_v<T> && !adapter_v;
      static constexpr bool fixed_v    = detail::pssz_impl::is_fixed_v<T> && !adapter_v;
      static constexpr bool pair_v     = detail::is_std_pair_v<T> && record_v;
      static constexpr bool map_v      = detail::is_map_v<T> && !adapter_v;
      static constexpr bool set_v      = detail::is_set_v<T> && !adapter_v;
      static constexpr bool nested_v   = detail::is_nested_v<T>;
      static constexpr bool deferred_v = [] {
         if constexpr (detail::is_nested_v<T>)
            return !detail::is_nested<T>::descends;
         else
            return false;
      }();

      static_assert(!set_v,
                    "psio::mutable_view: a set has no mutable view: its elements are in strictly "
                    "ascending order of their key bytes, which an in-place overwrite could break "
                    "(decode, modify and encode the set)");
      static_assert(!deferred_v,
                    "psio::mutable_view: a nested value with deferred validation has no mutable "
                    "view: no reader of the frame has checked that its bytes are the inner type");

      std::span<char> data_;

      [[nodiscard]] std::span<const char> const_data() const noexcept
      {
         return std::span<const char>{data_.data(), data_.size()};
      }

      template <typename Child>
      [[nodiscard]] mutable_view<Child, child_format<T>> child(
         std::span<const char> part) const noexcept
      {
         return mutable_view<Child, child_format<T>>{
            detail::mutable_view_impl::rebase(data_, part)};
      }

     public:
      using element_type                    = T;
      using format_type                     = Fmt;
      static constexpr storage storage_kind = storage::mut_borrow;

      // Whether `set` is available: T is fixed-size and nothing in it is
      // blocked. This is the whole compile-time rule for a write.
      static constexpr bool assignable = [] {
         if constexpr (fixed_v)
            return !detail::mutable_view_impl::contains_blocked<T>();
         else
            return false;
      }();

      mutable_view() = default;
      // `bytes` is the complete encoding of a T: the whole frame for a root
      // view. A std::vector<char>& converts implicitly.
      explicit mutable_view(std::span<char> bytes) noexcept : data_(bytes) {}

      [[nodiscard]] std::span<char> _psio_data() const noexcept { return data_; }

      // ── Fixed-size value: overwrite in place ───────────────────────────
      //
      // Writes the encoding of `value` over the value this view designates
      // and returns true. Returns false, leaving every byte unchanged, when
      // there is nothing of the right size to overwrite (the payload of an
      // absent optional, an element index out of range) or when `value` has
      // no valid encoding (see `encodable` above). A zero-size value has no
      // bytes: there is nothing to write and the result is true.
      [[nodiscard]] bool set(const T& value) const noexcept
      {
         static_assert(!optional_v,
                       "psio::mutable_view::set: an optional's presence cannot change in place; "
                       "overwrite a present fixed-size payload through operator*");
         static_assert(!variant_v,
                       "psio::mutable_view::set: a variant's alternative cannot change in place; "
                       "overwrite a fixed-size active alternative through visit");
         static_assert(fixed_v,
                       "psio::mutable_view::set: only a fixed-size value can be overwritten in "
                       "place (decode, modify and encode a variable-size value)");
         static_assert(!fixed_v || assignable,
                       "psio::mutable_view::set: the value contains a type whose in-place "
                       "mutation is blocked (psio::detail::mutation_blocked_v)");
         if constexpr (assignable)
         {
            constexpr std::size_t size = detail::pssz_impl::fixed_size_of<T>();
            if (data_.size() != size || !detail::mutable_view_impl::encodable(value))
               return false;
            ::psio::fast_buf_stream sink(data_.data(), size);
            detail::pssz_impl::encode_value<
               detail::mutable_view_impl::explicit_width_v<Fmt>,
               detail::mutable_view_impl::is_auto_v<Fmt>>(value, sink);
            return true;
         }
         else
            return false;
      }

      // ── Scalar: read by value, as the read view does ───────────────────
      [[nodiscard]] auto get() const noexcept
         requires (!optional_v && !nested_v && requires(const read_view& v) { v.get(); })
      {
         return read_view{const_data()}.get();
      }

      // ── Record: member N ───────────────────────────────────────────────
      //
      // Unlike the read view, which returns arithmetic members by value,
      // every member is returned as a view: a value could not be assigned.
      template <std::size_t N>
         requires record_v
      [[nodiscard]] auto get() const noexcept
      {
         static_assert(N < detail::pssz_impl::record_traits<T>::member_count,
                       "psio::mutable_view: record member index out of range");
         static_assert(!::psio::has_as_override_v<
                          typename detail::pssz_impl::record_member_annotations<T, N>::value_t>,
                       "psio::mutable_view: a member with an as<Tag> override is not "
                       "addressable in place");
         using Member = std::remove_cvref_t<
            typename detail::pssz_impl::record_traits<T>::template member_type<N>>;
         static_assert(!detail::mutable_view_impl::blocked_member_v<
                          Member,
                          std::remove_cvref_t<typename detail::pssz_impl::record_member_annotations<
                             T, N>::value_t>>,
                       "psio::mutable_view: a member whose annotations constrain its content is "
                       "not addressable in place (psio::detail::mutation_blocked_v)");
         return child<Member>(layout::template record_field_span<T, N>(const_data()));
      }

      // ── Pair: the record of its two members, also by name ──────────────
      [[nodiscard]] auto first() const noexcept
         requires pair_v
      {
         return this->template get<0>();
      }
      [[nodiscard]] auto second() const noexcept
         requires pair_v
      {
         return this->template get<1>();
      }

      // ── Nested value: the inner value, in place ────────────────────────
      //
      // The bytes are the automatic-profile encoding of the inner value
      // whatever the enclosing profile is. An overwrite beneath leaves them
      // the canonical encoding of the modified inner value, and their count
      // unchanged, so the enclosing frame stays canonical too.
      [[nodiscard]] auto get() const noexcept
         requires nested_v
      {
         return mutable_view<typename detail::is_nested<T>::inner, ::psio::pssz>{data_};
      }
      [[nodiscard]] auto operator*() const noexcept
         requires nested_v
      {
         return get();
      }

      // ── Vector / array: elements. Map: entries ─────────────────────────
      //
      // A map is the list of its entries in ascending key order. An entry
      // is handed out as a mutable_map_entry: its key can be read, never
      // written (the order depends on it); its mapped value is a view like
      // any other.
      [[nodiscard]] std::size_t size() const noexcept
         requires (vector_v || array_v || map_v)
      {
         if constexpr (array_v)
            return std::tuple_size_v<T>;
         else if constexpr (map_v)
            return layout::template vector_count<detail::assoc_entry_t<T>>(const_data());
         else
            return layout::template vector_count<typename T::value_type>(const_data());
      }

      [[nodiscard]] bool empty() const noexcept
         requires (vector_v || array_v || map_v)
      {
         return size() == 0;
      }

      // Precondition: i < size(). `at` checks and returns an empty view
      // (whose `set` returns false) instead.
      [[nodiscard]] auto operator[](std::size_t i) const noexcept
         requires (vector_v || array_v || map_v)
      {
         if constexpr (map_v)
         {
            using List = detail::assoc_list_t<T>;
            using Entry = detail::assoc_entry_t<T>;
            using EntryFormat = view_layout::nested_format_t<Fmt, List>;
            return mutable_map_entry<detail::assoc_key_t<T>, typename T::mapped_type,
                                     EntryFormat>{mutable_view<Entry, EntryFormat>{
               detail::mutable_view_impl::rebase(
                  data_, layout::template vector_element_span<Entry>(const_data(), i))}};
         }
         else
            return element(i);
      }

     private:
      [[nodiscard]] auto element(std::size_t i) const noexcept
      {
         using Element = typename T::value_type;
         if constexpr (array_v)
            return mutable_view<Element, Fmt>{detail::mutable_view_impl::rebase(
               data_, layout::template array_element_span<Element, std::tuple_size_v<T>>(
                         const_data(), i))};
         else
            return child<Element>(layout::template vector_element_span<Element>(const_data(), i));
      }

     public:
      [[nodiscard]] auto at(std::size_t i) const noexcept
         requires (vector_v || array_v || map_v)
      {
         return i < size() ? (*this)[i] : decltype((*this)[i]){};
      }

      class iterator
      {
         const mutable_view* owner_ = nullptr;
         std::size_t         index_ = 0;

        public:
         using iterator_category = std::forward_iterator_tag;
         using value_type        = decltype(std::declval<const mutable_view&>()[0]);
         using difference_type   = std::ptrdiff_t;
         using pointer           = void;
         using reference         = value_type;

         iterator() = default;
         iterator(const mutable_view& owner, std::size_t index) noexcept
            : owner_(&owner), index_(index)
         {
         }

         [[nodiscard]] value_type operator*() const noexcept { return (*owner_)[index_]; }
         iterator&                operator++() noexcept
         {
            ++index_;
            return *this;
         }
         iterator operator++(int) noexcept
         {
            auto copy = *this;
            ++index_;
            return copy;
         }
         friend bool operator==(const iterator&, const iterator&) = default;
      };

      [[nodiscard]] auto begin() const noexcept
         requires (vector_v || array_v || map_v)
      {
         return iterator{*this, 0};
      }
      [[nodiscard]] auto end() const noexcept
         requires (vector_v || array_v || map_v)
      {
         return iterator{*this, size()};
      }

      // ── Optional: presence is read-only, the payload is a view ─────────
      [[nodiscard]] bool has_value() const noexcept
         requires optional_v
      {
         return layout::template optional_has_value<typename T::value_type>(const_data());
      }

      [[nodiscard]] explicit operator bool() const noexcept
         requires optional_v
      {
         return has_value();
      }

      // Precondition: has_value(). For an absent optional the result is an
      // empty view; a `set` through it returns false.
      [[nodiscard]] auto operator*() const noexcept
         requires optional_v
      {
         using Payload = typename T::value_type;
         return child<Payload>(has_value()
                                  ? layout::template optional_payload_span<Payload>(const_data())
                                  : std::span<const char>{});
      }

      [[nodiscard]] auto get() const noexcept
         requires optional_v
      {
         return **this;
      }

      // ── Variant: the alternative is read-only, its payload is a view ───
      [[nodiscard]] std::size_t index() const noexcept
         requires variant_v
      {
         return view_layout::detail_pssz::variant_index(const_data());
      }

      // Calls `fn` with a mutable_view of the active alternative.
      template <typename F>
         requires variant_v
      auto visit(F&& fn) const
      {
         return visit_impl(std::forward<F>(fn),
                           std::make_index_sequence<std::variant_size_v<T>>{});
      }

     private:
      template <typename F, std::size_t... Is>
      auto visit_impl(F&& fn, std::index_sequence<Is...>) const
      {
         using R = std::common_type_t<decltype(std::forward<F>(fn)(std::declval<
            mutable_view<std::variant_alternative_t<Is, T>, child_format<T>>>()))...>;
         const std::size_t active  = index();
         const auto        payload = view_layout::detail_pssz::variant_payload_span(const_data());
         if constexpr (std::is_void_v<R>)
            (void)((active == Is
                       ? (std::forward<F>(fn)(child<std::variant_alternative_t<Is, T>>(payload)),
                          true)
                       : false) ||
                   ...);
         else
         {
            R result{};
            (void)((active == Is
                       ? (result = std::forward<F>(fn)(
                             child<std::variant_alternative_t<Is, T>>(payload)),
                          true)
                       : false) ||
                   ...);
            return result;
         }
      }
   };

   // Box<T> is transparent on the wire, and so is its view.
   template <typename T, typename Fmt>
   class mutable_view<schema_types::Box<T>, Fmt> : public mutable_view<T, Fmt>
   {
    public:
      using mutable_view<T, Fmt>::mutable_view;
   };

   // A transparent type (transparent.hpp) is its member on the wire, and so
   // is its view: navigation and `set` are the member's. A transparent type
   // with an invariant is blocked like any type with one.
   template <typename T, typename Fmt>
      requires (::psio::is_transparent_v<T> &&
                !::psio::format_should_dispatch_adapter_v<Fmt, T>)
   class mutable_view<T, Fmt> : public mutable_view<::psio::transparent_inner_t<T>, Fmt>
   {
      static_assert(!detail::mutation_blocked_v<T>,
                    "psio::mutable_view: in-place mutation is blocked for this type "
                    "(psio::detail::mutation_blocked_v)");

    public:
      using mutable_view<::psio::transparent_inner_t<T>, Fmt>::mutable_view;
      mutable_view() = default;
      mutable_view(mutable_view<::psio::transparent_inner_t<T>, Fmt> inner) noexcept
         : mutable_view<::psio::transparent_inner_t<T>, Fmt>(inner)
      {
      }
   };

   // ── One entry of a map ────────────────────────────────────────────────
   //
   // The entry (K, V) at one position of a mutable_view of a map. The key
   // decides the entry's position, so it is read-only: `key()` is what the
   // read view of the entry gives for it. `value()` is the mutable_view of
   // the mapped value.
   template <typename K, typename V, typename Fmt>
   class mutable_map_entry
   {
      mutable_view<std::tuple<K, V>, Fmt> entry_;

     public:
      using key_type    = K;
      using mapped_type = V;

      mutable_map_entry() = default;
      explicit mutable_map_entry(mutable_view<std::tuple<K, V>, Fmt> entry) noexcept
         : entry_(entry)
      {
      }

      [[nodiscard]] auto key() const noexcept
      {
         const auto data = entry_._psio_data();
         return view<std::tuple<K, V>, Fmt, storage::const_borrow>{
            std::span<const char>{data.data(), data.size()}}
            .template get<0>();
      }
      [[nodiscard]] auto value() const noexcept { return entry_.template get<1>(); }
   };

   // ── Free-function storage API, as for buffer and view ─────────────────

   template <typename T, typename Fmt>
   [[nodiscard]] std::span<const char> bytes(const mutable_view<T, Fmt>& v) noexcept
   {
      const auto data = v._psio_data();
      return std::span<const char>{data.data(), data.size()};
   }

   template <typename T, typename Fmt>
   [[nodiscard]] std::size_t size(const mutable_view<T, Fmt>& v) noexcept
   {
      return v._psio_data().size();
   }

   template <typename T, typename Fmt>
   [[nodiscard]] constexpr Fmt format_of(const mutable_view<T, Fmt>&) noexcept
   {
      return {};
   }

   // The read view over the same bytes.
   template <typename T, typename Fmt>
   [[nodiscard]] view<T, Fmt, storage::const_borrow> as_view(
      const mutable_view<T, Fmt>& v) noexcept
   {
      return view<T, Fmt, storage::const_borrow>{bytes(v)};
   }

   // A mutable view of a buffer that owns or mutably borrows its bytes.
   template <typename T, typename Fmt, storage S>
      requires requires(buffer<T, Fmt, S>& b) { mutable_bytes(b); }
   [[nodiscard]] mutable_view<T, Fmt> as_mutable_view(buffer<T, Fmt, S>& b) noexcept
   {
      return mutable_view<T, Fmt>{mutable_bytes(b)};
   }

   // Construction checks the complete frame, as `validated_pssz_view` does;
   // navigation and `set` then rely on it.
   template <typename T, typename Fmt>
      requires detail::is_pssz_format_v<Fmt>
   [[nodiscard]] codec_status validated_pssz_mutable_view(
      Fmt fmt, std::span<char> bytes, std::optional<mutable_view<T, Fmt>>& result,
      bool& has_unknown)
   {
      auto status = validate_pssz_compatible<T>(
         fmt, std::span<const char>{bytes.data(), bytes.size()}, has_unknown);
      if (status.ok())
         result.emplace(bytes);
      return status;
   }

}  // namespace psio
