#pragma once
//
// psio/graphql/schema.hpp — the GraphQL type system derived from reflection.
//
// Every C++ type that can appear in a schema has ONE constexpr descriptor
// (`type_desc`): a scalar, an enum, an object, a union or an input object.
// Descriptors point at each other (a field's type, a union's members), so a
// schema is a constant graph with no run-time construction: a guest that
// answers one query touches only the descriptors that query names.
// `schema_types<Roots...>` walks the graph at compile time into the list of
// named types (introspection, SDL, `Unknown type` diagnostics) and rejects
// two C++ types that map to one GraphQL name.
//
// The mapping (docs/graphql.md has the table):
//
//   reflected record        Object: reflected fields, then reflected const
//                           methods; a method with parameters is a field with
//                           arguments named by the reflected parameter names
//   std::optional<T>,
//   pointer, result<T>      nullable; everything else is non-null
//   vector/array/span/
//   any range               List
//   std::variant<records>   Union
//   PSIO_REFLECT_ENUM enum  Enum (an enum without names is its integer)
//   std::tuple              List when all elements have one type, otherwise
//                           an Object with fields _0, _1, ...
//   leaf types              scalars, written exactly as psio::ijson writes
//                           them (json.hpp)
//   view<T,...>, buffer<T>  T: encoded data is transparent to the schema
//
// Customisation, with annotations (annotate.hpp) on a type or a member:
//
//   PSIO_TYPE_ATTRS(T, psio::graphql::name_as{"Account"})
//   PSIO_FIELD_ATTRS(T, secret, psio::graphql::hidden)
//   PSIO_FIELD_ATTRS(T, id,     psio::graphql::id)
//   PSIO_FIELD_ATTRS(T, search, psio::graphql::cost{50} | psio::graphql::doc{"..."})
//   PSIO_FIELD_ATTRS(T, old,    psio::graphql::deprecated{"use `fresh`"})
//
// (inside PSIO_REFLECT: `attr(field, graphql::hidden)` for a data member).
// A type that cannot carry an annotation (a std::variant, a class template)
// is named by specialising `psio::graphql::type_name<T>`.

#include <psio/adapter.hpp>
#include <psio/annotate.hpp>
#include <psio/detail/nested_fwd.hpp>
#include <psio/ext_int.hpp>
#include <psio/fixed_string.hpp>
#include <psio/json.hpp>
#include <psio/name_lookup.hpp>
#include <psio/reflect.hpp>
#include <psio/time.hpp>
#include <psio/view.hpp>
#include <psio/wrappers.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace psio::graphql
{
   inline constexpr std::uint32_t nil = 0xFFFFFFFFu;

   // ── Annotations ───────────────────────────────────────────────────────

   // `spec | spec` composes the specs below as it does psio's own.
   using ::psio::operator|;

   // The GraphQL name of a type or of a member.
   struct name_as
   {
      using spec_category = static_spec_tag;
      const char* value   = "";
      constexpr bool operator==(const name_as&) const = default;
   };
   // The member is not part of the schema.
   struct hidden_spec
   {
      using spec_category = static_spec_tag;
      constexpr bool operator==(const hidden_spec&) const = default;
   };
   inline constexpr hidden_spec hidden{};
   // The member's scalar is the GraphQL `ID` (written as a string).
   struct id_spec
   {
      using spec_category = static_spec_tag;
      constexpr bool operator==(const id_spec&) const = default;
   };
   inline constexpr id_spec id{};
   // What selecting the member adds to a query's complexity (default 1).
   struct cost
   {
      using spec_category = static_spec_tag;
      std::uint32_t value = 1;
      constexpr bool operator==(const cost&) const = default;
   };
   // Description of a type or member (introspection, SDL).
   struct doc
   {
      using spec_category = static_spec_tag;
      const char* text    = "";
      constexpr bool operator==(const doc&) const = default;
   };
   // The member is deprecated (introspection `isDeprecated`, SDL `@deprecated`).
   // A member annotated with psio::deprecated_spec is deprecated too.
   struct deprecated
   {
      using spec_category = static_spec_tag;
      const char* reason  = "No longer supported";
      constexpr bool operator==(const deprecated&) const = default;
   };

   // The member's own fields are presented as fields of the enclosing object
   // type, in the member's place (output only; the member must be a
   // reflected record):
   //    template <typename T> struct WithBlock {
   //       BlockInfo block; T data;
   //       PSIO_REFLECT(WithBlock, block, attr(data, psio::graphql::flatten))
   //    };
   // gives `{ block {...} <the fields of T> }`. Two fields with one name are
   // a compile error.
   struct flatten_spec
   {
      using spec_category = static_spec_tag;
      constexpr bool operator==(const flatten_spec&) const = default;
   };
   inline constexpr flatten_spec flatten{};
   // Type-level: a reflected record that, as a method PARAMETER, is not an
   // input object but a group of arguments — each member is one argument of
   // the field, named by the member:
   //    struct Paging { std::optional<std::int32_t> first; std::optional<std::string> after; };
   //    PSIO_REFLECT(Paging, first, after)
   //    PSIO_TYPE_ATTRS(Paging, psio::graphql::arguments)
   //    ... rows(Paging page) const;      // rows(first: Int, after: String)
   // A class template cannot be annotated: specialise `argument_group`.
   struct arguments_spec
   {
      using spec_category = static_spec_tag;
      constexpr bool operator==(const arguments_spec&) const = default;
   };
   inline constexpr arguments_spec arguments{};

   // Name of a type that cannot be annotated. Specialise with
   //    static constexpr std::string_view value = "SearchResult";
   // `input_type_name<T>` names T used as an input object (default: the
   // output name with "Input" appended unless it already ends that way).
   template <typename T>
   struct type_name
   {
   };
   template <typename T>
   struct input_type_name
   {
   };

   // True for a parameter type whose members are the field's arguments (see
   // `arguments`). Specialise for a class template:
   //    template <typename K>
   //    inline constexpr bool psio::graphql::argument_group<Range<K>> = true;
   namespace detail
   {
      template <typename T>
      constexpr bool annotated_argument_group() noexcept
      {
         if constexpr (Reflected<T> && !std::is_enum_v<T>)
            return has_spec_v<arguments_spec, std::remove_cvref_t<decltype(annotate<type<T>{}>)>>;
         else
            return false;
      }
   }  // namespace detail
   template <typename T>
   inline constexpr bool argument_group = detail::annotated_argument_group<T>();

   // A method result that is completed by arguments of its own: the field
   // takes the method's arguments followed by those of `arguments` (an
   // argument group), and its value is what `resolve` returns. Specialise
   // for the type an index or a query builder has:
   //    template <typename Row, typename Key>
   //    struct psio::graphql::resolver<TableIndex<Row, Key>> {
   //       struct arguments { key_range<Key> range; page_args page; ... };
   //       static auto resolve(TableIndex<Row, Key>&& index, const arguments& a)
   //       { return psio::graphql::paginate(source_of(index), a.range, a.page); }
   //    };
   // so that `auto rows() const { return table.index(); }` is the field
   // `rows(gt, ge, lt, le, first, last, before, after): RowConnection`.
   // `resolve` may take a third parameter `context&`.
   template <typename R>
   struct resolver
   {
   };

   // ── Descriptors ───────────────────────────────────────────────────────

   // In the order of the introspection enum __TypeKind.
   enum class type_kind : std::uint8_t
   {
      scalar,
      object,
      interface_,
      union_,
      enumeration,
      input_object,
      list,
      non_null,
   };

   // What literal a scalar accepts before its typed check runs.
   enum class scalar_class : std::uint8_t
   {
      none,
      integer,   // Int: an integer literal
      floating,  // Float: an integer or float literal
      string,    // String
      boolean,   // Boolean
      id,        // ID: a string or an integer literal
      custom,    // any scalar literal the type's I-JSON reader accepts
   };

   enum class value_kind : std::uint8_t
   {
      absent,
      null,
      boolean,
      integer,
      floating,
      string,
      enumeration,
      list,
      object,
      variable,
   };

   // One scalar input value: a number's characters, a string's (unescaped)
   // bytes, an enum value's name, or a boolean.
   struct leaf_token
   {
      value_kind       kind = value_kind::absent;
      std::string_view text{};
      bool             flag = false;
   };

   struct type_desc;

   // A type reference: `depth` list wrappers around a named type. Bit i of
   // `nonnull` says level i is non-null; level 0 is the outermost, level
   // `depth` the named type itself. `[Int!]` is {Int, 1, 0b10}.
   struct type_ref
   {
      const type_desc* named   = nullptr;
      std::uint8_t     depth   = 0;
      std::uint8_t     nonnull = 0;

      constexpr bool is_nonnull(unsigned level = 0) const noexcept { return (nonnull >> level) & 1; }
   };
   inline constexpr unsigned max_list_depth = 7;

   // An argument of a field, or a field of an input object.
   struct arg_desc
   {
      std::string_view name;
      type_ref         type;
      std::uint16_t    slot = 0;  // position among the decoded arguments / member index
      std::string_view description{};
   };

   struct field_desc
   {
      std::string_view name;
      type_ref         type;
      const arg_desc*  args      = nullptr;
      std::uint16_t    arg_count = 0;
      std::uint16_t    slot      = 0;  // index of the resolver in the type's handler table
      std::uint32_t    cost      = 1;
      // Arguments named `first` / `last` (Int): the page size that multiplies
      // the cost of the selection below; -1 when the field has none.
      std::int8_t      first_arg  = -1;
      std::int8_t      last_arg   = -1;
      bool             deprecated = false;
      std::string_view description{};
      std::string_view deprecation_reason{};
   };

   struct type_desc
   {
      type_kind        kind   = type_kind::scalar;
      scalar_class     scalar = scalar_class::none;
      // Several C++ types share this name by design (Int, Float, Int64, ...).
      bool             builtin = false;
      // ... and all of them accept the same values (Int64, UInt64, ...), so a
      // value typed by NAME (a variable) can be checked with `check`.
      bool             check_by_name = false;
      std::string_view name{};
      std::string_view description{};
      const field_desc*       fields         = nullptr;  // object
      std::uint16_t           field_count    = 0;
      const arg_desc*         inputs         = nullptr;  // input object
      std::uint16_t           input_count    = 0;
      const std::string_view* values         = nullptr;  // enum
      std::uint16_t           value_count    = 0;
      const type_desc* const* possible       = nullptr;  // union
      std::uint16_t           possible_count = 0;
      // Name -> index of a field / input field / enum value / union member;
      // the count when there is none. A perfect hash (name_lookup.hpp).
      std::size_t (*find)(std::string_view) = nullptr;
      // Scalars: the typed check of one input value.
      bool (*check)(const leaf_token&) = nullptr;

      constexpr bool is_leaf() const noexcept
      {
         return kind == type_kind::scalar || kind == type_kind::enumeration;
      }
      constexpr bool is_composite() const noexcept
      {
         return kind == type_kind::object || kind == type_kind::union_;
      }
      constexpr bool is_input() const noexcept { return is_leaf() || kind == type_kind::input_object; }
   };

   // ── Request context ───────────────────────────────────────────────────
   //
   // A reflected method that takes a `psio::graphql::context&` parameter
   // receives the request's context; that parameter is not a GraphQL
   // argument. `user<T>()` is the object the caller attached to the request
   // (request::user), or nullptr when none of that type was attached.
   namespace detail
   {
      template <typename T>
      inline constexpr char user_tag{};
   }
   class context
   {
     public:
      template <typename T>
      T* user() const noexcept
      {
         return tag_ == &detail::user_tag<T> ? static_cast<T*>(user_) : nullptr;
      }

     protected:
      context()                          = default;
      context(const context&)            = delete;
      context& operator=(const context&) = delete;
      void*       user_ = nullptr;
      const void* tag_  = nullptr;
   };

   // ── A value or a field error ──────────────────────────────────────────
   //
   // A method that can fail returns result<T>: the field is nullable, and an
   // error becomes a field error (message, location, path) with `null` in its
   // place. Any type with has_value() / operator* / error() where error() is
   // convertible to std::string_view is treated the same way.
   struct field_error
   {
      std::string message;
   };
   template <typename T>
   class result
   {
      std::variant<T, field_error> v_;

     public:
      result(T value) : v_(std::in_place_index<0>, std::move(value)) {}
      result(field_error e) : v_(std::in_place_index<1>, std::move(e)) {}
      bool               has_value() const noexcept { return v_.index() == 0; }
      explicit           operator bool() const noexcept { return has_value(); }
      T&                 operator*() noexcept { return *std::get_if<0>(&v_); }
      const T&           operator*() const noexcept { return *std::get_if<0>(&v_); }
      const std::string& error() const noexcept { return std::get_if<1>(&v_)->message; }
   };
   inline field_error fail(std::string message) { return field_error{std::move(message)}; }

   // An opaque Relay cursor: key bytes, written as a hex String.
   struct page_cursor
   {
      std::string_view key{};
   };

   namespace detail
   {
      namespace ji = ::psio::detail::json_impl;

      // ── Sources: a C++ value, or a view / buffer of encoded bytes ────────
      template <typename S>
      struct logical
      {
         using type = S;
      };
      template <typename T, typename F, storage St>
      struct logical<view<T, F, St>>
      {
         using type = T;
      };
      template <typename T, typename F, storage St>
      struct logical<buffer<T, F, St>>
      {
         using type = T;
      };
      // std::reference_wrapper<T> is a T that is not copied (a row source
      // over values in memory yields these).
      template <typename T>
      struct logical<std::reference_wrapper<T>>
      {
         using type = std::remove_cv_t<T>;
      };
      // psio::nested<T> (nested.hpp) is the T its bytes hold, read through
      // the view over them. A deferred one has no mapping: an enclosing
      // reader has not checked that its bytes are a T.
      template <typename T>
      struct logical<nested<T, nested_validation::eager>>
      {
         using type = T;
      };
      template <typename T, typename F, storage St>
      struct logical<view<nested<T, nested_validation::eager>, F, St>>
      {
         using type = T;
      };
      template <typename S>
      inline constexpr bool is_nested_source_v = false;
      template <typename T>
      inline constexpr bool is_nested_source_v<nested<T, nested_validation::eager>> = true;
      template <typename T, typename F, storage St>
      inline constexpr bool
         is_nested_source_v<view<nested<T, nested_validation::eager>, F, St>> = true;
      // A std::pair is the 2-tuple of its members (the entries of a map
      // are pairs): `Tuple_<A>_<B>`, or a List when A and B are one type.
      template <typename A, typename B>
      struct logical<std::pair<A, B>>
      {
         using type = std::tuple<std::remove_const_t<A>, B>;
      };
      template <typename S>
      using logical_t = typename logical<std::remove_cvref_t<S>>::type;
      template <typename S>
      inline constexpr bool is_reference_wrapper_v = false;
      template <typename T>
      inline constexpr bool is_reference_wrapper_v<std::reference_wrapper<T>> = true;

      template <typename S>
      inline constexpr bool is_view_v = false;
      template <typename T, typename F, storage St>
      inline constexpr bool is_view_v<view<T, F, St>> = true;
      template <typename S>
      inline constexpr bool is_buffer_v = false;
      template <typename T, typename F, storage St>
      inline constexpr bool is_buffer_v<buffer<T, F, St>> = true;

      // ── Shapes ───────────────────────────────────────────────────────────
      template <typename T>
      inline constexpr bool is_string_v =
         std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view> ||
         ji::is_utf8_string<T>::value;
      template <typename T>
      inline constexpr bool is_bytes_v = ji::byte_seq<T>::value || ji::is_bounded_bytes<T>::value;
      template <typename T>
      inline constexpr bool is_text_adapter_v = ji::dispatch_adapter_v<ijson, T>;

      // A transparent type (transparent.hpp) over a leaf is a leaf: a custom
      // scalar with the type's own name and its member's text.
      template <typename T>
      constexpr bool leaf_shaped() noexcept
      {
         if constexpr (is_text_adapter_v<T> || std::is_arithmetic_v<T> || std::is_enum_v<T> ||
                       ji::is_wide_int_v<T> || TimeValue<T> || is_string_v<T> ||
                       is_bytes_v<T> || std::is_same_v<T, page_cursor>)
            return true;
         else if constexpr (is_transparent_v<T>)
            return leaf_shaped<transparent_inner_t<T>>();
         else
            return false;
      }
      template <typename T>
      inline constexpr bool is_leaf_v = leaf_shaped<T>();

      template <typename T>
      struct nullable_inner
      {
      };
      template <typename T>
      struct nullable_inner<std::optional<T>>
      {
         using type = T;
      };
      template <typename T, typename D>
      struct nullable_inner<std::unique_ptr<T, D>>
      {
         using type = T;
      };
      template <typename T>
      struct nullable_inner<std::shared_ptr<T>>
      {
         using type = T;
      };
      template <typename T>
         requires(!std::is_same_v<std::remove_cv_t<T>, char> && !std::is_function_v<T>)
      struct nullable_inner<T*>
      {
         using type = std::remove_cv_t<T>;
      };
      template <typename T>
      concept Nullable = requires { typename nullable_inner<T>::type; };

      template <typename T>
      concept ResultLike = !Nullable<T> && requires(const T& r) {
         { r.has_value() } -> std::convertible_to<bool>;
         *r;
         { r.error() } -> std::convertible_to<std::string_view>;
      };

      template <typename T>
      inline constexpr bool is_tuple_v = false;
      template <typename... Ts>
      inline constexpr bool is_tuple_v<std::tuple<Ts...>> = true;
      template <typename T>
      inline constexpr bool is_homogeneous_tuple_v = false;
      template <typename T0, typename... Ts>
      inline constexpr bool is_homogeneous_tuple_v<std::tuple<T0, Ts...>> =
         (std::is_same_v<T0, Ts> && ...);

      template <typename T>
      concept RangeLike = requires(T& r) {
         r.begin();
         r.end();
      };

      enum class shape : std::uint8_t
      {
         leaf,
         nullable,
         result,
         list,
         tuple_list,
         object,
         union_,
         unsupported,
      };

      template <typename T>
      inline constexpr bool is_object_v =
         (Reflected<T> && !std::is_enum_v<T>) ||
         (is_tuple_v<T> && !is_homogeneous_tuple_v<T> && !std::is_same_v<T, std::tuple<>>);

      // Shape of a logical type (a source with view<> / buffer<> removed).
      template <typename T>
      constexpr shape shape_of() noexcept
      {
         if constexpr (is_leaf_v<T>)
            return shape::leaf;
         else if constexpr (Nullable<T>)
            return shape::nullable;
         else if constexpr (ResultLike<T>)
            return shape::result;
         else if constexpr (ji::is_std_variant<T>::value)
            return shape::union_;
         else if constexpr (is_homogeneous_tuple_v<T>)
            return shape::tuple_list;
         else if constexpr (is_object_v<T>)
            return shape::object;
         else if constexpr (RangeLike<T>)
            return shape::list;
         else
            return shape::unsupported;
      }

      // Element source of a list source S (a range of values, or a view of
      // a vector / array, whose iterator yields sub-views).
      template <typename S>
      using element_source_t =
         std::remove_cvref_t<decltype(*std::declval<std::remove_cvref_t<S>&>().begin())>;

      // ── Names in static storage ──────────────────────────────────────────
      struct name_buf
      {
         char        d[192]{};
         std::size_t n = 0;
         // (A name longer than the buffer indexes past it: a compile error,
         // as every name is built in a constant expression.)
         constexpr name_buf& add(std::string_view s)
         {
            for (char c : s)
               d[n++] = c;
            return *this;
         }
         constexpr name_buf& add(std::uintmax_t v)
         {
            char        tmp[24]{};
            std::size_t k = 0;
            do
               tmp[k++] = static_cast<char>('0' + v % 10);
            while (v /= 10);
            while (k)
               d[n++] = tmp[--k];
            return *this;
         }
      };
      template <typename Maker>
      struct stored_name
      {
         static constexpr name_buf buf  = Maker::make();
         static constexpr auto     text = [] {
            std::array<char, buf.n + 1> a{};
            for (std::size_t i = 0; i < buf.n; ++i)
               a[i] = buf.d[i];
            return a;
         }();
         static constexpr std::string_view value{text.data(), buf.n};
      };

      template <typename T>
      concept HasTypeName = requires { std::string_view{type_name<T>::value}; };
      template <typename T>
      concept HasInputTypeName = requires { std::string_view{input_type_name<T>::value}; };

      template <typename Spec, auto X>
      inline constexpr bool has_ann_v =
         has_spec_v<Spec, std::remove_cvref_t<decltype(annotate<X>)>>;

      template <typename T>
      constexpr std::string_view named_type_name() noexcept;

      // Name of T inside a composed name (tuple objects, unions).
      template <typename T>
      constexpr void compose_name(name_buf& out)
      {
         using L = logical_t<T>;
         if constexpr (Nullable<L>)
         {
            out.add("Optional");
            compose_name<typename nullable_inner<L>::type>(out);
         }
         else if constexpr (shape_of<L>() == shape::list || shape_of<L>() == shape::tuple_list)
         {
            if constexpr (is_tuple_v<L>)
               compose_name<std::tuple_element_t<0, L>>(out);
            else
               compose_name<element_source_t<L>>(out);
            out.add("List");
         }
         else
            out.add(named_type_name<L>());
      }
      template <typename T>
      struct tuple_namer;
      template <typename... Ts>
      struct tuple_namer<std::tuple<Ts...>>
      {
         static constexpr name_buf make()
         {
            name_buf out;
            out.add("Tuple");
            ((out.add("_"), compose_name<Ts>(out)), ...);
            return out;
         }
      };
      template <typename T>
      struct union_namer;
      template <typename T0, typename... Ts>
      struct union_namer<std::variant<T0, Ts...>>
      {
         static constexpr name_buf make()
         {
            name_buf out;
            compose_name<T0>(out);
            ((out.add("_or_"), compose_name<Ts>(out)), ...);
            return out;
         }
      };
      // The scalar of a time value. Its name carries the kind, the unit and,
      // when it is not a signed 64-bit count, the tick integer — the names
      // Rust's async-graphql scalars have (rust/psio graphql_scalars.rs),
      // where two types cannot share one scalar name:
      //
      //   time point          DateTimeSeconds, DateTimeMilliseconds,
      //                       DateTimeMicroseconds, DateTimeNanoseconds,
      //                       DateTime_<num>_<den>         RFC 3339 UTC text
      //   duration            Seconds, ..., Duration_<num>_<den>     tick count
      //   monotonic point     MonotonicSeconds, ..., Monotonic_<num>_<den>
      //   tick integer        suffix _i32, _u32, _u64, ... (none for int64)
      template <typename T>
      struct duration_namer
      {
         static constexpr name_buf make()
         {
            using Tr = time_traits<T>;
            name_buf out;
            const char* const kind =
               Tr::is_monotonic ? "Monotonic" : Tr::is_point ? "DateTime" : "";
            const char* const ratio_kind =
               Tr::is_monotonic ? "Monotonic" : Tr::is_point ? "DateTime" : "Duration";
            if (Tr::num == 1 && Tr::den == 1)
               out.add(kind).add("Seconds");
            else if (Tr::num == 1 && Tr::den == 1000)
               out.add(kind).add("Milliseconds");
            else if (Tr::num == 1 && Tr::den == 1000000)
               out.add(kind).add("Microseconds");
            else if (Tr::num == 1 && Tr::den == 1000000000)
               out.add(kind).add("Nanoseconds");
            else
               out.add(ratio_kind)
                  .add("_")
                  .add(static_cast<std::uintmax_t>(Tr::num))
                  .add("_")
                  .add(static_cast<std::uintmax_t>(Tr::den));
            using Rep = typename Tr::rep;
            if constexpr (!(std::is_signed_v<Rep> && sizeof(Rep) == 8))
               out.add(std::is_signed_v<Rep> ? "_i" : "_u")
                  .add(static_cast<std::uintmax_t>(sizeof(Rep) * 8));
            return out;
         }
      };

      // An instantiation of a reflected class template whose arguments are
      // all types is named `<Template>_<Arg>_<Arg>...` (WithBlock<Account> is
      // `WithBlock_Account`), each argument by its GraphQL name as in a
      // tuple's or a union's name. A template with non-type arguments cannot
      // be taken apart: name it with type_name<T>.
      template <typename T>
      constexpr bool composable() noexcept
      {
         using L = logical_t<T>;
         if constexpr (Nullable<L>)
            return composable<typename nullable_inner<L>::type>();
         else
            return shape_of<L>() != shape::unsupported;
      }
      template <typename T>
      struct template_args
      {
         static constexpr bool is_instance = false;
      };
      template <template <typename...> class C, typename... Ts>
      struct template_args<C<Ts...>>
      {
         static constexpr bool is_instance = sizeof...(Ts) != 0;
         static constexpr bool nameable    = (composable<Ts>() && ...);
         static constexpr void add(name_buf& out) { ((out.add("_"), compose_name<Ts>(out)), ...); }
      };
      template <typename T>
      struct template_namer
      {
         static constexpr name_buf make()
         {
            static_assert(template_args<T>::nameable,
                          "psio::graphql: a template argument of this reflected class template "
                          "has no GraphQL name, so the instantiation has none: specialise "
                          "psio::graphql::type_name<T>");
            name_buf out;
            out.add(reflect<T>::name);
            template_args<T>::add(out);
            return out;
         }
      };

      // The annotated or reflected name of a record / enum / adapter type.
      template <typename T>
      constexpr std::string_view declared_name() noexcept
      {
         if constexpr (HasTypeName<T>)
            return std::string_view{type_name<T>::value};
         else if constexpr (has_ann_v<name_as, type<T>{}>)
            return std::string_view{std::get<name_as>(annotate<type<T>{}>).value};
         else if constexpr (is_text_adapter_v<T> && schema_custom_v<T>)
            return schema_custom_id<T>();
         else if constexpr (Reflected<T> && template_args<T>::is_instance)
            return stored_name<template_namer<T>>::value;
         else if constexpr (Reflected<T>)
            return reflect<T>::name;
         else if constexpr (ReflectedEnum<T>)
            return reflect_enum<T>::name;
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio::graphql: this type has no GraphQL name; specialise "
                          "psio::graphql::type_name<T>");
            return {};
         }
      }

      template <typename T>
      struct input_namer
      {
         static constexpr name_buf make()
         {
            name_buf               out;
            const std::string_view base = declared_name<T>();
            out.add(base);
            if (!base.ends_with("Input"))
               out.add("Input");
            return out;
         }
      };
      template <typename T>
      constexpr std::string_view input_object_name() noexcept
      {
         if constexpr (HasInputTypeName<T>)
            return std::string_view{input_type_name<T>::value};
         else
            return stored_name<input_namer<T>>::value;
      }

      template <typename T>
      constexpr std::string_view doc_of_type() noexcept
      {
         if constexpr (has_ann_v<doc, type<T>{}>)
            return std::string_view{std::get<doc>(annotate<type<T>{}>).text};
         else
            return {};
      }

      // ── Leaves ───────────────────────────────────────────────────────────
      struct leaf_meta
      {
         std::string_view name;
         scalar_class     cls     = scalar_class::custom;
         bool             builtin = false;
         bool             is_enum = false;
         bool             by_name = false;  // type_desc::check_by_name
      };
      template <typename T>
      constexpr leaf_meta leaf_meta_of() noexcept
      {
         if constexpr (std::is_same_v<T, page_cursor>)
            return {"String", scalar_class::string, true};
         else if constexpr (is_text_adapter_v<T> || is_transparent_v<T>)
            return {declared_name<T>(), scalar_class::custom, false};
         else if constexpr (std::is_same_v<T, bool>)
            return {"Boolean", scalar_class::boolean, true, false, true};
         else if constexpr (ReflectedEnum<T>)
            return {declared_name<T>(), scalar_class::none, false, true};
         else if constexpr (std::is_enum_v<T>)
            return leaf_meta_of<std::underlying_type_t<T>>();
         else if constexpr (std::is_same_v<T, uint128>)
            return {"UInt128", scalar_class::custom, true, false, true};
         else if constexpr (std::is_same_v<T, int128>)
            return {"Int128", scalar_class::custom, true, false, true};
         else if constexpr (std::is_same_v<T, uint256>)
            return {"UInt256", scalar_class::custom, true, false, true};
         else if constexpr (std::is_integral_v<T>)
         {
            if constexpr (sizeof(T) <= 4)
               return {"Int", scalar_class::integer, true};
            else if constexpr (std::is_signed_v<T>)
               return {"Int64", scalar_class::custom, true, false, true};
            else
               return {"UInt64", scalar_class::custom, true, false, true};
         }
         else if constexpr (std::is_floating_point_v<T>)
            return {"Float", scalar_class::floating, true};
         else if constexpr (TimePointValue<T>)
            return {stored_name<duration_namer<T>>::value, scalar_class::custom, true};
         else if constexpr (TickTextValue<T>)
            return {stored_name<duration_namer<T>>::value, scalar_class::custom, true};
         else if constexpr (is_string_v<T>)
            return {"String", scalar_class::string, true};
         else
         {
            static_assert(is_bytes_v<T>, "psio::graphql: not a leaf type");
            return {"Bytes", scalar_class::custom, true};
         }
      }

      // A cursor over ONE scalar token for psio's typed I-JSON reader
      // (json_cursor.hpp): input scalars are read by the reader that reads
      // them from JSON, so one rule covers 64-bit integers, hex, time and
      // text adapters in both directions.
      struct leaf_cursor
      {
         leaf_token       t;
         std::string_view err{};

         bool fail(std::string_view what)
         {
            if (err.empty())
               err = what;
            return false;
         }
         bool kind(json_kind& out)
         {
            switch (t.kind)
            {
               case value_kind::boolean: out = json_kind::boolean; return true;
               case value_kind::integer:
               case value_kind::floating: out = json_kind::number; return true;
               case value_kind::string:
               case value_kind::enumeration: out = json_kind::string; return true;
               case value_kind::null: out = json_kind::null; return true;
               default: return fail("graphql: expected a scalar");
            }
         }
         bool begin_object() { return fail("graphql: expected a scalar"); }
         bool next_field(bool&, std::string_view&) { return fail("graphql: expected a scalar"); }
         bool begin_array() { return fail("graphql: expected a scalar"); }
         bool next_element(bool&) { return fail("graphql: expected a scalar"); }
         bool string(std::string_view& text, bool& escaped)
         {
            if (t.kind != value_kind::string && t.kind != value_kind::enumeration)
               return fail("graphql: expected a string");
            text    = t.text;
            escaped = false;
            return true;
         }
         bool number(std::string_view& text)
         {
            if (t.kind != value_kind::integer && t.kind != value_kind::floating)
               return fail("graphql: expected a number");
            text = t.text;
            return true;
         }
         bool boolean(bool& v)
         {
            if (t.kind != value_kind::boolean)
               return fail("graphql: expected a boolean");
            v = t.flag;
            return true;
         }
         bool null() { return t.kind == value_kind::null || fail("graphql: expected null"); }
         bool skip() { return true; }
         bool finish() { return err.empty(); }
         codec_status status(std::string_view format) const
         {
            return err.empty() ? codec_ok() : codec_fail(err, 0, format);
         }
      };

      // The C++ type an input leaf is read as.
      template <typename T>
      using leaf_storage_t = std::conditional_t<std::is_same_v<T, std::string_view>, std::string, T>;

      template <typename T>
      bool read_leaf(const leaf_token& t, T* out)
      {
         leaf_cursor                  c{t};
         ji::typed_reader<leaf_cursor> r{c};
         return ji::read_value<ijson, ji::int_mode::natural, T>(r, out) && c.err.empty();
      }
      template <typename T>
      bool check_leaf(const leaf_token& t)
      {
         return read_leaf<leaf_storage_t<T>>(t, nullptr);
      }

      template <typename Holder>
      std::size_t find_name(std::string_view name) noexcept
      {
         if constexpr (std::tuple_size_v<std::remove_cvref_t<decltype(Holder::_names_array_)>> == 0)
            return 0;
         else
            return ::psio::detail::name_table_for<Holder>.find(name);
      }

      template <typename E>
      struct enum_names
      {
         static constexpr const auto& _names_array_ = reflect_enum<E>::names;
      };

      template <typename T>
      struct leaf_type
      {
         static constexpr type_desc value = [] {
            constexpr leaf_meta m = leaf_meta_of<T>();
            type_desc           d;
            d.name          = m.name;
            d.builtin       = m.builtin;
            d.check_by_name = m.by_name;
            if constexpr (m.is_enum)
            {
               d.kind        = type_kind::enumeration;
               d.values      = reflect_enum<T>::names.data();
               d.value_count = static_cast<std::uint16_t>(reflect_enum<T>::count);
               d.find        = &find_name<enum_names<T>>;
               d.description = doc_of_type<T>();
            }
            else
            {
               d.kind   = type_kind::scalar;
               d.scalar = m.cls;
               if constexpr (!std::is_same_v<T, page_cursor>)
                  d.check = &check_leaf<T>;
               if constexpr (is_text_adapter_v<T>)
                  d.description = doc_of_type<T>();
            }
            return d;
         }();
      };
      // `ID`: the scalar of a member annotated graphql::id.
      inline constexpr type_desc id_type{.kind    = type_kind::scalar,
                                         .scalar  = scalar_class::id,
                                         .builtin = true,
                                         .name    = "ID"};

      // ── Objects ──────────────────────────────────────────────────────────
      //
      // object_def<T> lists the slots of an object type: the reflected data
      // members, then the reflected methods (a tuple: its elements).

      struct slot_meta
      {
         std::string_view name{};
         bool             hidden     = false;
         bool             is_id      = false;
         bool             deprecated = false;
         bool             flatten    = false;
         std::uint32_t    cost       = 1;
         std::string_view description{};
         std::string_view reason{};
      };

      template <auto Ptr>
      constexpr void apply_member_annotations(slot_meta& m)
      {
         using A = std::remove_cvref_t<decltype(annotate<Ptr>)>;
         if constexpr (has_spec_v<name_as, A>)
            m.name = std::string_view{std::get<name_as>(annotate<Ptr>).value};
         if constexpr (has_spec_v<hidden_spec, A>)
            m.hidden = true;
         if constexpr (has_spec_v<id_spec, A>)
            m.is_id = true;
         if constexpr (has_spec_v<flatten_spec, A>)
            m.flatten = true;
         if constexpr (has_spec_v<::psio::graphql::cost, A>)
            m.cost = std::get<::psio::graphql::cost>(annotate<Ptr>).value;
         if constexpr (has_spec_v<doc, A>)
            m.description = std::string_view{std::get<doc>(annotate<Ptr>).text};
         if constexpr (has_spec_v<deprecated_spec, A>)
         {
            m.deprecated = true;
            m.reason     = std::string_view{std::get<deprecated_spec>(annotate<Ptr>).version};
            if (m.reason.empty())
               m.reason = "No longer supported";
         }
         if constexpr (has_spec_v<::psio::graphql::deprecated, A>)
         {
            m.deprecated = true;
            m.reason =
               std::string_view{std::get<::psio::graphql::deprecated>(annotate<Ptr>).reason};
         }
      }

      template <typename A>
      inline constexpr bool is_context_param_v = std::is_same_v<std::remove_cvref_t<A>, context>;

      template <typename T>
      struct object_def
      {
         static_assert(!is_transparent_v<T>,
                       "psio::graphql: a transparent type over a record, a list or a variant "
                       "has no GraphQL mapping: expose its member, or give the type a text "
                       "adapter (a custom scalar)");
         using R = reflect<T>;
         static constexpr std::size_t field_slots = R::member_count;
         static constexpr std::size_t slot_count  = R::member_count + R::method_count;

         template <std::size_t I>
         static constexpr bool is_method = I >= field_slots;

         template <std::size_t I>
         static constexpr auto pointer_impl() noexcept
         {
            if constexpr (I < field_slots)
               return R::template member_pointer<I>;
            else
               return R::template method_pointer<I - field_slots>;
         }
         template <std::size_t I>
         static constexpr auto pointer = pointer_impl<I>();

         template <std::size_t I>
         static constexpr slot_meta meta() noexcept
         {
            slot_meta m;
            if constexpr (I < field_slots)
               m.name = R::template member_name<I>;
            else
               m.name = R::template method_name<I - field_slots>;
            apply_member_annotations<pointer<I>>(m);
            return m;
         }

         // Slot I is a member annotated graphql::flatten (and not hidden):
         // its fields stand in its place.
         template <std::size_t I>
         static constexpr bool is_flat = I < field_slots && meta<I>().flatten && !meta<I>().hidden;

         // Field slot I of a value or of a view.
         template <std::size_t I, typename Src>
         static decltype(auto) get(Src& src)
         {
            if constexpr (is_view_v<std::remove_cv_t<Src>>)
               return src.template get<I>();
            else
               return (src.*(R::template member_pointer<I>));
         }
      };

      template <typename... Ts>
      struct object_def<std::tuple<Ts...>>
      {
         using T = std::tuple<Ts...>;
         static constexpr std::size_t field_slots = sizeof...(Ts);
         static constexpr std::size_t slot_count  = sizeof...(Ts);
         template <std::size_t I>
         static constexpr bool is_method = false;
         template <std::size_t I>
         static constexpr bool is_flat = false;

         struct namer
         {
            template <std::size_t I>
            struct at
            {
               static constexpr name_buf make() { return name_buf{}.add("_").add(I); }
            };
         };
         template <std::size_t I>
         static constexpr slot_meta meta() noexcept
         {
            slot_meta m;
            m.name = stored_name<typename namer::template at<I>>::value;
            return m;
         }
         template <std::size_t I, typename Src>
         static decltype(auto) get(Src& src)
         {
            if constexpr (is_view_v<std::remove_cv_t<Src>>)
               return src.template get<I>();
            else
               return (std::get<I>(src));
         }
      };

      // Field type of slot I: the member's type, or the method's result.
      template <typename T, std::size_t I>
      struct slot_types
      {
         using result = std::tuple_element_t<I, T>;
         using args   = std::tuple<>;
         static constexpr bool is_const  = true;
         static constexpr bool is_static = false;
      };
      template <typename T, std::size_t I>
         requires(Reflected<T> && I < reflect<T>::member_count)
      struct slot_types<T, I>
      {
         using result = typename reflect<T>::template member_type<I>;
         using args   = std::tuple<>;
         static constexpr bool is_const  = true;
         static constexpr bool is_static = false;
      };
      template <typename T, std::size_t I>
         requires(Reflected<T> && I >= reflect<T>::member_count)
      struct slot_types<T, I>
      {
         static constexpr std::size_t J = I - reflect<T>::member_count;
         using result                   = typename reflect<T>::template method_result<J>;
         using args                     = typename reflect<T>::template method_args<J>;
         static constexpr bool is_const  = reflect<T>::template method_is_const<J>;
         static constexpr bool is_static = reflect<T>::template method_is_static<J>;
      };

      // A method result completed by arguments of its own (resolver<R>).
      template <typename R>
      concept Deferred = requires { typename resolver<std::remove_cvref_t<R>>::arguments; };
      template <typename R>
      using deferred_args_t = typename resolver<std::remove_cvref_t<R>>::arguments;
      template <typename R>
      decltype(auto) resolve_deferred(context& c, R&& value, const deferred_args_t<R>& args)
      {
         using Res = resolver<std::remove_cvref_t<R>>;
         if constexpr (requires { Res::resolve(std::forward<R>(value), args, c); })
            return Res::resolve(std::forward<R>(value), args, c);
         else
            return Res::resolve(std::forward<R>(value), args);
      }
      // What the field of slot I yields: the member, the method's result,
      // or what the result's resolver makes of it.
      template <typename T, std::size_t I>
      struct slot_value
      {
         using type = typename slot_types<T, I>::result;
         static constexpr bool deferred = false;
      };
      template <typename T, std::size_t I>
         requires(Reflected<T> && I >= reflect<T>::member_count &&
                  Deferred<typename slot_types<T, I>::result>)
      struct slot_value<T, I>
      {
         using raw  = typename slot_types<T, I>::result;
         using type = decltype(resolve_deferred(std::declval<context&>(), std::declval<raw>(),
                                                std::declval<const deferred_args_t<raw>&>()));
         static constexpr bool deferred = true;
      };

      // A flattened member's record type.
      template <typename T, std::size_t I>
      struct flat_member
      {
         using type = typename slot_types<T, I>::result;
         static_assert(Reflected<type> && !std::is_enum_v<type> && shape_of<type>() == shape::object,
                       "psio::graphql::flatten annotates a member that is a reflected record "
                       "(held by value)");
      };
      // Resolver slots: one per member or method, and for a flattened
      // member one per slot of its record. A field's slot is its position
      // in this numbering.
      template <typename T, std::size_t I>
      constexpr std::size_t slot_width() noexcept;
      template <typename T>
      constexpr std::size_t flat_slot_count() noexcept
      {
         return []<std::size_t... Is>(std::index_sequence<Is...>) {
            return (slot_width<T, Is>() + ... + std::size_t{0});
         }(std::make_index_sequence<object_def<T>::slot_count>{});
      }
      template <typename T, std::size_t I>
      constexpr std::size_t slot_width() noexcept
      {
         if constexpr (object_def<T>::template is_flat<I>)
            return flat_slot_count<typename flat_member<T, I>::type>();
         else
            return 1;
      }
      template <typename T, std::size_t I>
      constexpr std::size_t slot_base() noexcept
      {
         return []<std::size_t... Is>(std::index_sequence<Is...>) {
            return (slot_width<T, Is>() + ... + std::size_t{0});
         }(std::make_index_sequence<I>{});
      }

      // ── Type references, at the type level ───────────────────────────────
      //
      // A reference is {descriptor address, list depth, non-null bits}. The
      // wrappers and the named type are computed from the C++ type alone;
      // the address is taken only inside the initializer of the descriptor
      // that holds the reference. (A shared `ref_of<T>()` function would be
      // re-entered, half instantiated, by recursive types.)
      struct wrap_info
      {
         std::uint8_t depth   = 0;
         std::uint8_t nonnull = 1;
      };
      constexpr wrap_info wrap_nullable(wrap_info w) noexcept
      {
         return {w.depth, static_cast<std::uint8_t>(w.nonnull & ~1u)};
      }
      constexpr wrap_info wrap_list(wrap_info w) noexcept
      {
         return {static_cast<std::uint8_t>(w.depth + 1),
                 static_cast<std::uint8_t>((w.nonnull << 1) | 1)};
      }

      template <typename T>
      struct out_wrap;
      template <typename T, shape S = shape_of<logical_t<T>>()>
      struct out_wrap_impl
      {
         static_assert(S == shape::leaf || S == shape::union_ || S == shape::object,
                       "psio::graphql: this type has no GraphQL mapping (reflect it with "
                       "PSIO_REFLECT, or hide the member with psio::graphql::hidden)");
         using named = logical_t<T>;
         static constexpr wrap_info info{};
      };
      template <typename T>
      struct out_wrap_impl<T, shape::nullable>
      {
         using inner = out_wrap<typename nullable_inner<logical_t<T>>::type>;
         static_assert(inner::info.nonnull & 1,
                       "psio::graphql: an optional of an optional has no GraphQL type");
         using named = typename inner::named;
         static constexpr wrap_info info = wrap_nullable(inner::info);
      };
      template <typename T>
      struct out_wrap_impl<T, shape::result>
      {
         using inner = out_wrap<decltype(*std::declval<const logical_t<T>&>())>;
         using named = typename inner::named;
         static constexpr wrap_info info = wrap_nullable(inner::info);
      };
      template <typename T>
      struct out_wrap_impl<T, shape::tuple_list>
      {
         using inner = out_wrap<std::tuple_element_t<0, logical_t<T>>>;
         static_assert(inner::info.depth < max_list_depth, "psio::graphql: lists nest at most 7 deep");
         using named = typename inner::named;
         static constexpr wrap_info info = wrap_list(inner::info);
      };
      template <typename T>
      struct out_wrap_impl<T, shape::list>
      {
         using inner = out_wrap<element_source_t<logical_t<T>>>;
         static_assert(inner::info.depth < max_list_depth, "psio::graphql: lists nest at most 7 deep");
         using named = typename inner::named;
         static constexpr wrap_info info = wrap_list(inner::info);
      };
      template <typename T>
      struct out_wrap : out_wrap_impl<std::remove_cvref_t<T>>
      {
      };

      template <typename T>
      inline constexpr bool is_std_vector_v = ji::is_std_vector<T>::value;

      template <typename T>
      struct in_wrap
      {
         static_assert(is_leaf_v<T> || (Reflected<T> && !std::is_enum_v<T>),
                       "psio::graphql: an argument must be a scalar, an enum, std::optional, "
                       "std::vector or a reflected record of those");
         static_assert(!std::is_same_v<T, std::string_view> && !std::is_same_v<T, page_cursor>,
                       "psio::graphql: take a string argument as std::string");
         using named = T;
         static constexpr wrap_info info{};
      };
      template <typename T>
      struct in_wrap<std::optional<T>>
      {
         static_assert(in_wrap<T>::info.nonnull & 1,
                       "psio::graphql: an optional of an optional has no GraphQL type");
         using named = typename in_wrap<T>::named;
         static constexpr wrap_info info = wrap_nullable(in_wrap<T>::info);
      };
      template <typename T, typename A>
         requires(!is_leaf_v<std::vector<T, A>>)  // a byte vector is the scalar Bytes
      struct in_wrap<std::vector<T, A>>
      {
         static_assert(in_wrap<T>::info.depth < max_list_depth, "psio::graphql: lists nest at most 7 deep");
         using named = typename in_wrap<T>::named;
         static constexpr wrap_info info = wrap_list(in_wrap<T>::info);
      };

      // An enum without PSIO_REFLECT_ENUM names is its integer.
      template <typename L>
      using leaf_norm_t = typename std::conditional_t<
         std::is_enum_v<L> && !ReflectedEnum<L> && !is_text_adapter_v<L>,
         std::underlying_type<L>, std::type_identity<L>>::type;

      template <typename T>
      struct leaf_type;
      template <typename T, bool Mutation>
      struct object_type;
      template <typename V>
      struct union_type;
      template <typename T>
      struct input_type;

      // The descriptor holder of a named type: `out_holder_t<L>::value`.
      template <typename L>
      using out_holder_t = std::conditional_t<
         shape_of<L>() == shape::leaf, leaf_type<leaf_norm_t<L>>,
         std::conditional_t<shape_of<L>() == shape::union_, union_type<L>, object_type<L, false>>>;
      template <typename L>
      using in_holder_t =
         std::conditional_t<is_leaf_v<L>, leaf_type<leaf_norm_t<L>>, input_type<L>>;

// The reference to T as an output / input type. An expression, not a
// function: see the note above.
#define PSIO_GRAPHQL_OUT_REF(...)                                                              \
   ::psio::graphql::type_ref                                                                   \
   {                                                                                           \
      &::psio::graphql::detail::out_holder_t<                                                  \
         typename ::psio::graphql::detail::out_wrap<__VA_ARGS__>::named>::value,               \
         ::psio::graphql::detail::out_wrap<__VA_ARGS__>::info.depth,                           \
         ::psio::graphql::detail::out_wrap<__VA_ARGS__>::info.nonnull                          \
   }
#define PSIO_GRAPHQL_IN_REF(...)                                                               \
   ::psio::graphql::type_ref                                                                   \
   {                                                                                           \
      &::psio::graphql::detail::in_holder_t<                                                   \
         typename ::psio::graphql::detail::in_wrap<__VA_ARGS__>::named>::value,                \
         ::psio::graphql::detail::in_wrap<__VA_ARGS__>::info.depth,                            \
         ::psio::graphql::detail::in_wrap<__VA_ARGS__>::info.nonnull                           \
   }

      // ── Arguments ────────────────────────────────────────────────────────
      //
      // A method parameter is one argument, named by the reflected parameter
      // name — except the request context (no argument) and an argument
      // group (graphql::arguments), whose members are arguments each, named
      // by the member; a group inside a group is taken apart the same way.
      template <typename A>
      constexpr std::size_t arg_width() noexcept
      {
         if constexpr (is_context_param_v<A>)
            return 0;
         else if constexpr (argument_group<A>)
         {
            static_assert(std::is_default_constructible_v<A>,
                          "psio::graphql: an argument group must be default-constructible");
            return []<std::size_t... Is>(std::index_sequence<Is...>) {
               return ((object_def<A>::template meta<Is>().hidden
                           ? std::size_t{0}
                           : arg_width<typename reflect<A>::template member_type<Is>>()) +
                       ... + std::size_t{0});
            }(std::make_index_sequence<reflect<A>::member_count>{});
         }
         else
            return 1;
      }
      template <typename A, std::size_t N>
      constexpr void emit_args(std::array<arg_desc, N>& out, std::size_t& n, std::string_view name,
                               std::string_view description = {})
      {
         if constexpr (is_context_param_v<A>)
            return;
         else if constexpr (argument_group<A>)
         {
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (([&] {
                   constexpr slot_meta m = object_def<A>::template meta<Is>();
                   if constexpr (!m.hidden)
                      emit_args<typename reflect<A>::template member_type<Is>>(out, n, m.name,
                                                                               m.description);
                }()),
                ...);
            }(std::make_index_sequence<reflect<A>::member_count>{});
         }
         else
         {
            out[n] = arg_desc{name, PSIO_GRAPHQL_IN_REF(A), static_cast<std::uint16_t>(n), description};
            ++n;
         }
      }

      // The arguments of method slot I: those of its parameters, then those
      // of its result's resolver (resolver<R>::arguments).
      template <typename T, std::size_t I>
      struct slot_args
      {
         using Args = typename slot_types<T, I>::args;
         static constexpr std::size_t arity = std::tuple_size_v<Args>;

         // Position of parameter K's first argument.
         template <std::size_t K>
         static constexpr std::size_t arg_index = []<std::size_t... Ks>(std::index_sequence<Ks...>) {
            return (arg_width<std::remove_cvref_t<std::tuple_element_t<Ks, Args>>>() + ... +
                    std::size_t{0});
         }(std::make_index_sequence<K>{});

         // Arguments of the parameters; position of the resolver's.
         static constexpr std::size_t own_count = arg_index<arity>;
         static constexpr std::size_t count     = [] {
            if constexpr (slot_value<T, I>::deferred)
               return own_count +
                      arg_width<deferred_args_t<typename slot_types<T, I>::result>>();
            else
               return own_count;
         }();

         static constexpr auto value = [] {
            std::array<arg_desc, count> out{};
            std::size_t                 n = 0;
            if constexpr (arity != 0)
            {
               constexpr auto names =
                  reflect<T>::template method_param_names<I - reflect<T>::member_count>;
               static_assert(names.size() == arity,
                             "psio::graphql: a reflected method that is a GraphQL field must "
                             "name every parameter: method(name, param1, param2, ...)");
               [&]<std::size_t... Ks>(std::index_sequence<Ks...>) {
                  (emit_args<std::remove_cvref_t<std::tuple_element_t<Ks, Args>>>(out, n, names[Ks]),
                   ...);
               }(std::make_index_sequence<arity>{});
            }
            if constexpr (slot_value<T, I>::deferred)
            {
               static_assert(argument_group<deferred_args_t<typename slot_types<T, I>::result>>,
                             "psio::graphql: resolver<R>::arguments must be an argument group "
                             "(psio::graphql::arguments)");
               emit_args<deferred_args_t<typename slot_types<T, I>::result>>(out, n, {});
            }
            return out;
         }();

         static constexpr bool names_distinct = [] {
            for (std::size_t i = 0; i < count; ++i)
               for (std::size_t j = i + 1; j < count; ++j)
                  if (value[i].name == value[j].name)
                     return false;
            return true;
         }();

         // Index of the page-size argument (of type Int) named `name`, or -1.
         static constexpr std::int8_t page_arg(std::string_view name) noexcept
         {
            for (std::size_t i = 0; i < count && i < 127; ++i)
               if (value[i].name == name && value[i].type.depth == 0 &&
                   value[i].type.named->name == "Int")
                  return static_cast<std::int8_t>(i);
            return -1;
         }
      };

      // A slot is part of the object type when it is not hidden and, for a
      // method, when it can be called: const (or static), or any method of
      // the mutation root. A const method that returns void has no value and
      // is left out.
      template <typename T, bool Mutation, std::size_t I>
      constexpr bool slot_visible() noexcept
      {
         if (object_def<T>::template meta<I>().hidden)
            return false;
         if constexpr (object_def<T>::template is_method<I>)
         {
            using S = slot_types<T, I>;
            if constexpr (std::is_void_v<typename S::result>)
               return Mutation;
            else
               return Mutation || S::is_const || S::is_static;
         }
         else
            return true;
      }

      template <typename T, bool Mutation, std::size_t I>
      constexpr field_desc make_field() noexcept
      {
         constexpr slot_meta m = object_def<T>::template meta<I>();
         using S               = slot_types<T, I>;
         field_desc f;
         f.name               = m.name;
         f.slot               = static_cast<std::uint16_t>(slot_base<T, I>());
         f.cost               = m.cost;
         f.deprecated         = m.deprecated;
         f.description        = m.description;
         f.deprecation_reason = m.reason;
         using V = typename slot_value<T, I>::type;
         if constexpr (std::is_void_v<V>)
            f.type = type_ref{&leaf_type<bool>::value, 0, 1};
         else
            f.type = PSIO_GRAPHQL_OUT_REF(V);
         if constexpr (m.is_id)
         {
            static_assert(!std::is_void_v<V> && out_wrap<V>::info.depth == 0 &&
                             shape_of<typename out_wrap<V>::named>() == shape::leaf,
                          "psio::graphql::id annotates a scalar member");
            f.type.named = &id_type;
         }
         if constexpr (object_def<T>::template is_method<I>)
         {
            static_assert(slot_args<T, I>::names_distinct,
                          "psio::graphql: two arguments of a field have the same name (a "
                          "parameter and a member of an argument group, or members of two "
                          "groups)");
            f.args      = slot_args<T, I>::value.data();
            f.arg_count = static_cast<std::uint16_t>(slot_args<T, I>::count);
            f.first_arg = slot_args<T, I>::page_arg("first");
            f.last_arg  = slot_args<T, I>::page_arg("last");
         }
         return f;
      }

      // The field of an object type without members or methods (see
      // object_fields::placeholder).
      inline constexpr std::string_view placeholder_name = "_";

      template <typename T, bool Mutation>
      struct object_fields
      {
         static constexpr std::size_t slot_count = object_def<T>::slot_count;

         // GraphQL has no object type without fields. A record with no
         // members and no methods (a marker: `struct TLSInfo {}`) gets the
         // one field `_: Boolean`, which is always true — what servers with
         // SDL-first schemas write for an empty type.
         static constexpr bool placeholder = slot_count == 0;

         // Fields slot I contributes.
         template <std::size_t I>
         static constexpr std::size_t fields_of() noexcept
         {
            if constexpr (!slot_visible<T, Mutation, I>())
               return 0;
            else if constexpr (object_def<T>::template is_flat<I>)
               return object_fields<typename flat_member<T, I>::type, false>::real_count;
            else
               return 1;
         }
         static constexpr std::size_t real_count = []<std::size_t... Is>(std::index_sequence<Is...>) {
            return (fields_of<Is>() + ... + std::size_t{0});
         }(std::make_index_sequence<slot_count>{});
         static constexpr std::size_t count = real_count + (placeholder ? 1 : 0);

         static constexpr auto value = [] {
            std::array<field_desc, count> out{};
            std::size_t                   n = 0;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (([&] {
                   if constexpr (!slot_visible<T, Mutation, Is>())
                      return;
                   else if constexpr (object_def<T>::template is_flat<Is>)
                   {
                      using M = typename flat_member<T, Is>::type;
                      for (std::size_t k = 0; k < object_fields<M, false>::real_count; ++k)
                      {
                         out[n] = object_fields<M, false>::value[k];
                         out[n].slot =
                            static_cast<std::uint16_t>(out[n].slot + slot_base<T, Is>());
                         ++n;
                      }
                   }
                   else
                      out[n++] = make_field<T, Mutation, Is>();
                }()),
                ...);
            }(std::make_index_sequence<slot_count>{});
            if constexpr (placeholder)
               out[n++] = field_desc{.name = placeholder_name,
                                     .type = type_ref{&leaf_type<bool>::value, 0, 0},
                                     .slot = 0};
            return out;
         }();

         static constexpr auto _names_array_ = [] {
            std::array<std::string_view, count> out{};
            for (std::size_t i = 0; i < count; ++i)
               out[i] = value[i].name;
            return out;
         }();

         static constexpr bool names_distinct = [] {
            for (std::size_t i = 0; i < count; ++i)
               for (std::size_t j = i + 1; j < count; ++j)
                  if (_names_array_[i] == _names_array_[j])
                     return false;
            return true;
         }();
      };

      template <typename T>
      constexpr std::string_view object_name() noexcept
      {
         if constexpr (HasTypeName<T>)
            return std::string_view{type_name<T>::value};
         else if constexpr (is_tuple_v<T>)
            return stored_name<tuple_namer<T>>::value;
         else
            return declared_name<T>();
      }

      template <typename T, bool Mutation>
      struct object_type
      {
         static constexpr type_desc value = [] {
            static_assert(object_fields<T, Mutation>::count != 0,
                          "psio::graphql: an object type needs at least one visible field");
            static_assert(object_fields<T, Mutation>::names_distinct,
                          "psio::graphql: two members of an object type have the same "
                          "GraphQL name");
            type_desc d;
            d.kind        = type_kind::object;
            d.name        = object_name<T>();
            d.fields      = object_fields<T, Mutation>::value.data();
            d.field_count = static_cast<std::uint16_t>(object_fields<T, Mutation>::count);
            d.find        = &find_name<object_fields<T, Mutation>>;
            if constexpr (!is_tuple_v<T>)
               d.description = doc_of_type<T>();
            return d;
         }();
      };

      // ── Unions ───────────────────────────────────────────────────────────
      template <typename V>
      struct union_members;
      template <typename... Ts>
      struct union_members<std::variant<Ts...>>
      {
         static_assert((is_object_v<Ts> && ...),
                       "psio::graphql: every alternative of a std::variant (a GraphQL union) "
                       "must be a reflected record");
         static constexpr std::array<const type_desc*, sizeof...(Ts)> value{
            &object_type<Ts, false>::value...};
         static constexpr std::array<std::string_view, sizeof...(Ts)> _names_array_{
            object_name<Ts>()...};
      };
      template <typename V>
      struct union_type
      {
         static constexpr type_desc value = [] {
            type_desc d;
            d.kind = type_kind::union_;
            d.name = named_type_name<V>();
            d.possible       = union_members<V>::value.data();
            d.possible_count = static_cast<std::uint16_t>(union_members<V>::value.size());
            d.find           = &find_name<union_members<V>>;
            return d;
         }();
      };

      // ── Input objects ────────────────────────────────────────────────────
      template <typename T>
      struct input_fields
      {
         using R = reflect<T>;
         static constexpr std::size_t count = []<std::size_t... Is>(std::index_sequence<Is...>) {
            return (std::size_t{!object_def<T>::template meta<Is>().hidden} + ... + 0);
         }(std::make_index_sequence<R::member_count>{});

         static constexpr auto value = [] {
            std::array<arg_desc, count> out{};
            std::size_t                 n = 0;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (([&] {
                   constexpr slot_meta m = object_def<T>::template meta<Is>();
                   if constexpr (!m.hidden)
                      out[n++] = arg_desc{m.name,
                                          PSIO_GRAPHQL_IN_REF(typename R::template member_type<Is>),
                                          static_cast<std::uint16_t>(Is), m.description};
                }()),
                ...);
            }(std::make_index_sequence<R::member_count>{});
            return out;
         }();
         static constexpr auto _names_array_ = [] {
            std::array<std::string_view, count> out{};
            for (std::size_t i = 0; i < count; ++i)
               out[i] = value[i].name;
            return out;
         }();
      };
      template <typename T>
      struct input_type
      {
         static constexpr type_desc value = [] {
            static_assert(std::is_default_constructible_v<T>,
                          "psio::graphql: an input object type must be default-constructible");
            static_assert(input_fields<T>::count != 0,
                          "psio::graphql: an input object type needs at least one field");
            type_desc d;
            d.kind        = type_kind::input_object;
            d.name        = input_object_name<T>();
            d.inputs      = input_fields<T>::value.data();
            d.input_count = static_cast<std::uint16_t>(input_fields<T>::count);
            d.find        = &find_name<input_fields<T>>;
            d.description = doc_of_type<T>();
            return d;
         }();
      };

      // The name alone, without touching a descriptor (names are composed
      // while descriptors that refer to each other are still being built).
      template <typename T>
      constexpr std::string_view named_type_name() noexcept
      {
         if constexpr (shape_of<T>() == shape::leaf)
         {
            if constexpr (std::is_enum_v<T> && !ReflectedEnum<T> && !is_text_adapter_v<T>)
               return leaf_meta_of<std::underlying_type_t<T>>().name;
            else
               return leaf_meta_of<T>().name;
         }
         else if constexpr (shape_of<T>() == shape::union_)
         {
            if constexpr (HasTypeName<T>)
               return std::string_view{type_name<T>::value};
            else
               return stored_name<union_namer<T>>::value;
         }
         else
         {
            static_assert(shape_of<T>() == shape::object,
                          "psio::graphql: this type has no GraphQL mapping");
            return object_name<T>();
         }
      }

      // ── The named types of a schema ──────────────────────────────────────
      inline constexpr std::size_t max_schema_types = 1024;
      struct type_table
      {
         std::array<const type_desc*, max_schema_types> t{};
         std::size_t                                    n        = 0;
         bool                                           conflict = false;
         bool                                           overflow = false;

         constexpr void add(const type_desc* d) noexcept
         {
            if (!d)
               return;
            for (std::size_t i = 0; i < n; ++i)
               if (t[i] == d)
                  return;
            if (n == max_schema_types)
            {
               overflow = true;
               return;
            }
            t[n++] = d;
         }
      };
      constexpr bool same_ref(const type_ref& a, const type_ref& b) noexcept
      {
         return a.depth == b.depth && a.nonnull == b.nonnull && a.named->name == b.named->name;
      }
      // Two descriptors with one name describe the same GraphQL type: the
      // same record reached through different sources (connection<view<T>>
      // and connection<buffer<T>>, say). Compared one level deep, by name;
      // the types they refer to are compared when their own names meet.
      constexpr bool equivalent(const type_desc& a, const type_desc& b) noexcept
      {
         if (a.kind != b.kind || a.field_count != b.field_count || a.input_count != b.input_count ||
             a.value_count != b.value_count || a.possible_count != b.possible_count)
            return false;
         if (a.kind == type_kind::scalar)
            return a.builtin && b.builtin;
         for (std::size_t i = 0; i < a.field_count; ++i)
         {
            const field_desc& x = a.fields[i];
            const field_desc& y = b.fields[i];
            if (x.name != y.name || x.slot != y.slot || !same_ref(x.type, y.type) ||
                x.arg_count != y.arg_count)
               return false;
            for (std::size_t k = 0; k < x.arg_count; ++k)
               if (x.args[k].name != y.args[k].name || !same_ref(x.args[k].type, y.args[k].type))
                  return false;
         }
         for (std::size_t i = 0; i < a.input_count; ++i)
            if (a.inputs[i].name != b.inputs[i].name || a.inputs[i].slot != b.inputs[i].slot ||
                !same_ref(a.inputs[i].type, b.inputs[i].type))
               return false;
         for (std::size_t i = 0; i < a.value_count; ++i)
            if (a.values[i] != b.values[i])
               return false;
         for (std::size_t i = 0; i < a.possible_count; ++i)
            if (a.possible[i]->name != b.possible[i]->name)
               return false;
         return a.kind != type_kind::enumeration && a.kind != type_kind::input_object;
      }

      // Breadth-first from `roots`, then from `extra` (the built-in scalars
      // and the introspection types, so that a schema lists its own types
      // first); then one pass over the names: a name is listed once, and two
      // descriptors with one name that are not `equivalent` are a conflict.
      constexpr type_table collect_types(std::initializer_list<const type_desc*> roots,
                                         std::initializer_list<const type_desc*> extra = {}) noexcept
      {
         type_table  all;
         std::size_t done = 0;
         auto        walk = [&] {
            for (; done < all.n; ++done)
            {
               const type_desc* d = all.t[done];
               for (std::size_t f = 0; f < d->field_count; ++f)
               {
                  all.add(d->fields[f].type.named);
                  for (std::size_t a = 0; a < d->fields[f].arg_count; ++a)
                     all.add(d->fields[f].args[a].type.named);
               }
               for (std::size_t f = 0; f < d->input_count; ++f)
                  all.add(d->inputs[f].type.named);
               for (std::size_t p = 0; p < d->possible_count; ++p)
                  all.add(d->possible[p]);
            }
         };
         for (const type_desc* r : roots)
            all.add(r);
         walk();
         for (const type_desc* r : extra)
            all.add(r);
         walk();
         type_table out;
         out.overflow = all.overflow;
         for (std::size_t i = 0; i < all.n; ++i)
         {
            bool repeated = false;
            for (std::size_t j = 0; j < out.n && !repeated; ++j)
               if (out.t[j]->name == all.t[i]->name)
               {
                  repeated = true;
                  if (!equivalent(*out.t[j], *all.t[i]))
                     out.conflict = true;
               }
            if (!repeated)
               out.t[out.n++] = all.t[i];
         }
         return out;
      }
   }  // namespace detail

   // A name built from the GraphQL name of `T`, for a type_name
   // specialisation of a class template:
   //    template <typename T>
   //    struct psio::graphql::type_name<WithBlock<T>> {
   //       static constexpr std::string_view value =
   //          psio::graphql::affixed_name<"", T, "WithBlock">;   // AccountWithBlock
   //    };
   namespace detail
   {
      template <fixed_string Prefix, typename T, fixed_string Suffix>
      struct affix_namer
      {
         static constexpr name_buf make()
         {
            return name_buf{}
               .add(Prefix.view())
               .add(named_type_name<typename out_wrap<T>::named>())
               .add(Suffix.view());
         }
      };
   }  // namespace detail
   template <fixed_string Prefix, typename T, fixed_string Suffix>
   inline constexpr std::string_view affixed_name =
      detail::stored_name<detail::affix_namer<Prefix, T, Suffix>>::value;

   struct exec;
   struct plan_field;

   // A schema: the root types, every named type reachable from them, and the
   // introspection hooks (null when the schema was built without it).
   struct schema_desc
   {
      const type_desc*        query      = nullptr;
      const type_desc*        mutation   = nullptr;
      const type_desc* const* types      = nullptr;
      std::size_t             type_count = 0;
      std::size_t (*find_type)(std::string_view) = nullptr;
      // `__schema` and `__type` of the query root.
      const field_desc* meta_fields = nullptr;
      bool (*resolve_meta)(exec&, const plan_field&, std::size_t which) = nullptr;

      const type_desc* type_named(std::string_view name) const noexcept
      {
         const std::size_t i = find_type(name);
         return i < type_count ? types[i] : nullptr;
      }
   };

}  // namespace psio::graphql
