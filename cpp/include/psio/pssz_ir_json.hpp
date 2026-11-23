#pragma once
//
// psio/pssz_ir_json.hpp — JSON / I-JSON text for a type known only by its
// Schema IR, and the conversions a schema-only consumer needs:
//
//   json_to_pssz(schema, root, psio::ijson{}, text, bytes)   text  -> PSSZ
//   pssz_to_json(schema, root, psio::ijson{}, bytes, text)   PSSZ  -> text
//   validate_pssz(schema, root, bytes)                       check PSSZ
//
// `root` is an AnyType; a named type is `schema_types::Type{"Name"}` or just
// "Name". The PSSZ side is the automatic profile of pssz_ir.hpp (the bytes
// of the typed psio::pssz codec); the text is the text of the typed
// psio::json / psio::ijson codec for the type the IR describes (json.hpp):
//
//   record            object, members by name; unknown keys are skipped, a
//                     missing member must be an Option; a repeated key is
//                     read again and the last one wins
//   tuple             array of exactly its arity
//   variant           ijson {"Name": value} (Name: the member's ijsonName
//                     attribute, else its name); json [index, value]
//   option            null or the value
//   list / array      array (exact count for an array, at most N bounded)
//   set               array of its elements, in any order; a repeated
//                     element is an error
//   map               object {"k": v, ...} when the key type's text is a
//                     string by type ("string" / "utf8", or a custom
//                     presentation with a registered handler), else an array
//                     of [k, v] pairs; any order, a repeated key is an error
//                     (detail/pssz_ir_json_assoc.hpp)
//   string adapters   JSON string (ijson: no noncharacters). "utf8" is
//                     UTF-8 by type; a raw "string" whose bytes are not
//                     UTF-8 is valid PSSZ but has no text (an error)
//   box               the inner type's text
//   Nested            the inner value's text (its dynamic value is the inner
//                     value; the bytes are its canonical encoding). A member
//                     lengthBound on it counts encoded bytes and is checked
//                     where the bytes exist: json_to_pssz / pssz_to_json
//   hex adapter       ijson: one hex string (UPPERCASE on output);
//                     json: array of numbers
//   Int               number; a 64-bit integer under ijson is a bare number
//                     while |v| <= 2^53 - 1, a decimal string beyond (either
//                     form is read, bare only in that range); a member's
//                     jsonInt / ijsonInt attribute forces one form and
//                     reaches the integer through option, list and array
//   bool, Float       true / false; shortest round-trip decimal
//
//   Int 128 / 256     decimal: a string under ijson, a bare number under
//                     json. Read from a decimal string, the legacy "0x"
//                     string, or a bare number (ijson: |v| <= 2^53 - 1)
//   Enum              a listed value is its name, any other its number; read
//                     from the name, the number (or numeric string), or
//                     "name:number" (which must agree)
//
// No JSON form (an error, not a guess): the bitvector / bitlist adapters.
// The typed codec has none to agree with. A non-finite float has no text.
//
// Member and alternative names are resolved by comparing the incoming key
// with the next expected member first (writers emit declaration order) and,
// on a mismatch, by lookup. A `JsonNames` built once for a schema makes that
// lookup a hash probe for every decode that is handed it; without one the
// fallback is a scan of the member list.
//
// The reader is a type walk over a JsonCursor (json_cursor.hpp): it holds no
// text positions and does no tokenizing, and runs on the same syntax layers
// as the typed readers (the hand-written lexer or simdjson, chosen by
// `json_parser`; the default takes the fastest for the document). Everything is checked and status-returning: no exceptions, no
// aborts, nesting and schema recursion limited by kMaxValidationDepth.

#include <psio/json.hpp>
#include <psio/json_cursor.hpp>
#include <psio/name_lookup.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir.hpp>

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace psio::pssz_ir
{
namespace json_detail
{
using namespace schema_types;
inline std::string_view variant_name(const Member &member);
struct map_key_text;
} // namespace json_detail

// ── Custom types ───────────────────────────────────────────────────────────
//
// pssz_ir_custom.hpp defines the registry (CustomHandlers) and the rules:
// a custom node is presented and checked by the handler registered for its
// id; with no handler it is its wire type (the wire type's text, structure
// only), or an error under `require_all`.
//
// In text, a handler with `to_text` / `from_text` reads and writes the value
// as ONE JSON string; a handler with only `check` leaves the wire type's
// text. Either way `check` runs on every value read (after the text was
// accepted) and written (before any text is returned), and its status is
// passed through unchanged. Text a handler's `from_text` refuses is
// "json: text is not a value of the custom type".
namespace detail
{
// The handler CustomHandlers::add<T>() registers.
template <typename T> struct typed_custom_handler
{
   static CustomHandlers::Handler make()
   {
      static_assert(::psio::schema_custom_v<T>,
                    "psio: CustomHandlers::add<T>() takes a custom type: one with a "
                    "text_category adapter, an invariant, or both");
      CustomHandlers::Handler handler;
      if constexpr (::psio::detail::text_adapter_is_custom<T>())
      {
         using A = ::psio::adapter<T, ::psio::text_category>;
         handler.to_text = [](std::span<const char> pssz, std::string &text) -> codec_status
         {
            T value{};
            bool unknown = false;
            if (auto status = ::psio::decode_pssz_checked(::psio::pssz{}, pssz, value, unknown);
                !status.ok())
               return status;
            A::encode(value, text);
            return codec_ok();
         };
         handler.from_text = [](std::string_view text, std::vector<char> &pssz) -> codec_status
         {
            const std::span<const char> in{text.data(), text.size()};
            if (auto status = A::validate(in); !status.ok())
               return status;
            // The bytes of the value the text denotes. Whether that value
            // satisfies T's invariant is `check`'s to say, which the codecs
            // run on these bytes next.
            const T value = A::decode(in);
            return ::psio::detail::pssz_impl::encode_ignoring_invariants(::psio::pssz{}, value,
                                                                         pssz);
         };
      }
      if constexpr (::psio::has_invariant_v<T>)
         handler.check = [](std::span<const char> pssz) -> codec_status
         { return ::psio::validate<T>(::psio::pssz{}, pssz); };
      return handler;
   }
};
} // namespace detail

// Name -> member index for every record and variant of a schema: the
// run-time counterpart of the tables psio::reflect<T> gets at compile time.
// Build it once per schema (it walks `schema.types`, and `root` when the
// root is not a named type) and pass it to any number of json_to_value /
// json_to_pssz calls; lookups are const and thread-safe. It views the
// schema's strings: the schema must outlive it and must not be modified.
class JsonNames
{
   using entry = std::pair<const std::vector<schema_types::Member> *, runtime_name_index>;
   std::vector<entry> tables_;  // sorted by member-list address
   // Custom node -> its handler, resolved once here (null: no handler).
   using binding = std::pair<const schema_types::Custom *, const CustomHandlers::Handler *>;
   std::vector<binding> bindings_;  // sorted by node address
   const CustomHandlers *handlers_ = nullptr;

   void add(const std::vector<schema_types::Member> &members, bool variant)
   {
      std::vector<std::string_view> names;
      names.reserve(members.size());
      for (const auto &member : members)
         names.push_back(variant ? json_detail::variant_name(member) : std::string_view{member.name});
      tables_.emplace_back(&members, runtime_name_index(std::move(names)));
   }
   void walk(const schema_types::Box<schema_types::AnyType> &box)
   {
      if (box.get())
         walk(*box);
   }
   void walk(const schema_types::AnyType &type)
   {
      using namespace schema_types;
      std::visit(
          [&](const auto &t)
          {
             using T = std::decay_t<decltype(t)>;
             if constexpr (std::is_same_v<T, Object> || std::is_same_v<T, Struct> ||
                           std::is_same_v<T, schema_types::Variant>)
             {
                add(t.members, std::is_same_v<T, schema_types::Variant>);
                for (const auto &member : t.members)
                   walk(member.type);
             }
             else if constexpr (std::is_same_v<T, Tuple>)
             {
                for (const auto &member : t.members)
                   walk(member);
             }
             else if constexpr (requires { t.type.get(); })
             {
                if constexpr (std::is_same_v<T, schema_types::Custom>)
                   if (handlers_)
                      bindings_.emplace_back(&t, handlers_->find(t.id));
                walk(t.type);
             }
          },
          type.value);
   }
   void finish()
   {
      std::sort(tables_.begin(), tables_.end(),
                [](const entry &a, const entry &b) { return std::less<>{}(a.first, b.first); });
      std::sort(bindings_.begin(), bindings_.end(),
                [](const binding &a, const binding &b) { return std::less<>{}(a.first, b.first); });
   }

 public:
   JsonNames() = default;
   explicit JsonNames(const schema_types::Schema &schema)
   {
      for (const auto &named : schema.types)
         walk(named.type);
      finish();
   }
   JsonNames(const schema_types::Schema &schema, const schema_types::AnyType &root)
   {
      for (const auto &named : schema.types)
         walk(named.type);
      walk(root);
      finish();
   }
   // As above, and the handler of every Custom node of the schema is looked
   // up here, once: a decode / encode handed this object finds it by the
   // node's address. `handlers` must outlive this object and must not
   // change while it is in use.
   JsonNames(const schema_types::Schema &schema, const CustomHandlers &handlers)
       : handlers_(&handlers)
   {
      for (const auto &named : schema.types)
         walk(named.type);
      finish();
   }
   JsonNames(const schema_types::Schema &schema, const schema_types::AnyType &root,
             const CustomHandlers &handlers)
       : handlers_(&handlers)
   {
      for (const auto &named : schema.types)
         walk(named.type);
      walk(root);
      finish();
   }
   const CustomHandlers *handlers() const noexcept { return handlers_; }
   // The handler bound to `custom`; `known` is false when the node was not
   // part of the walk (the caller then looks the id up).
   const CustomHandlers::Handler *handler(const schema_types::Custom &custom,
                                          bool &known) const noexcept
   {
      auto at = std::lower_bound(bindings_.begin(), bindings_.end(), &custom,
                                 [](const binding &a, const schema_types::Custom *b)
                                 { return std::less<>{}(a.first, b); });
      known = at != bindings_.end() && at->first == &custom;
      return known ? at->second : nullptr;
   }

   // The index of `members`, or null when it was not part of the walk.
   const runtime_name_index *find(const std::vector<schema_types::Member> &members) const noexcept
   {
      auto at = std::lower_bound(tables_.begin(), tables_.end(), &members,
                                 [](const entry &a, const std::vector<schema_types::Member> *b)
                                 { return std::less<>{}(a.first, b); });
      return at != tables_.end() && at->first == &members ? &at->second : nullptr;
   }
};

namespace json_detail
{

// The text form of a record node (schema_builder.hpp, an argument list).
enum class tuple_text : std::uint8_t
{
   none,        // a record: an object
   named,       // an object keyed by the member names; a reader also takes the array
   positional,  // the array
};
inline tuple_text tuple_form(const std::vector<Attribute> &attributes) noexcept
{
   const auto *attribute = psio::pssz_ir::detail::find_attribute(attributes, "tuple");
   if (!attribute || !attribute->value)
      return tuple_text::none;
   return *attribute->value == "positional" ? tuple_text::positional : tuple_text::named;
}

enum class int_form : std::uint8_t
{
   natural,
   number,
   string,
};

inline constexpr unsigned __int128 safe_integer = (static_cast<unsigned __int128>(1) << 53) - 1;

inline int_form member_form(const std::vector<Attribute> &attributes, bool ijson)
{
   if (auto a = detail::find_attribute(attributes, ijson ? "ijsonInt" : "jsonInt"); a && a->value)
      return *a->value == "string" ? int_form::string : int_form::number;
   return int_form::natural;
}

inline std::string_view variant_name(const Member &member)
{
   if (auto a = detail::find_attribute(member.attributes, "ijsonName"); a && a->value)
      return *a->value;
   return member.name;
}

// `type` with named aliases followed and the Custom nodes that are not part
// of the binary form ("box", text presentations) looked through: what
// decides whether a missing member reads as an empty Option.
inline const AnyType *shape_of(const Schema &schema, const AnyType &type)
{
   const AnyType *at = &type;
   for (std::size_t hops = 0; hops <= 4 * schema.types.size() + 64; ++hops)
   {
      at = at->resolve(schema);
      if (!at)
         return nullptr;
      auto custom = std::get_if<Custom>(&at->value);
      if (!custom || detail::wire_custom_id(custom->id) || !custom->type.get())
         return at;
      at = custom->type.get();
   }
   return nullptr;
}

// Whether a member type is a Nested (through Option): its lengthBound counts
// the bytes of the inner encoding, which a dynamic value does not have.
inline bool nested_member(const Schema &schema, const AnyType &type)
{
   const AnyType *at = &type;
   for (std::size_t hops = 0; hops <= kMaxValidationDepth; ++hops)
   {
      at = shape_of(schema, *at);
      if (!at)
         return false;
      auto option = std::get_if<Option>(&at->value);
      if (!option)
         return std::holds_alternative<Nested>(at->value);
      if (!option->type.get())
         return false;
      at = option->type.get();
   }
   return false;
}

// The handler of a Custom node, and whether a missing one is an error.
struct custom_lookup
{
   const CustomHandlers *handlers = nullptr;
   const JsonNames *names = nullptr;

   const CustomHandlers *registry() const
   {
      return names && names->handlers() ? names->handlers() : handlers;
   }
   const CustomHandlers::Handler *find(const Custom &custom) const
   {
      if (names && names->handlers())
      {
         bool known = false;
         auto bound = names->handler(custom, known);
         if (known)
            return bound;
      }
      auto all = registry();
      return all ? all->find(custom.id) : nullptr;
   }
   bool required() const
   {
      auto all = registry();
      return all && all->require_all;
   }
};

// The byte container under a string / hex adapter.
struct byte_shape
{
   bool valid = false;
   bool exact = false;                  // Array: exactly `limit` bytes
   std::optional<std::uint64_t> limit;  // BoundedList: at most
   bool admits(std::uint64_t n) const { return !limit || (exact ? n == *limit : n <= *limit); }
};
inline byte_shape bytes_of(const Schema &schema, const Custom &custom)
{
   byte_shape out;
   const AnyType *container = custom.type.get() ? custom.type->resolve(schema) : nullptr;
   if (!container)
      return out;
   out.valid = true;
   if (auto list = std::get_if<BoundedList>(&container->value))
      out.limit = list->maxCount;
   else if (auto array = std::get_if<Array>(&container->value))
   {
      out.limit = array->len;
      out.exact = true;
   }
   else if (!std::holds_alternative<List>(container->value))
      out.valid = false;
   return out;
}

// `0 | -?[1-9][0-9]*` within the range of an integer of `bits` bits.
inline bool parse_integer(std::string_view text, bool is_signed, std::uint32_t bits,
                          bool &negative, unsigned __int128 &magnitude)
{
   negative = !text.empty() && text[0] == '-';
   if (negative && !is_signed)
      return false;
   const std::string_view digits = text.substr(negative ? 1 : 0);
   if (digits.empty() || digits.size() > 39)
      return false;
   if (digits[0] == '0' && (digits.size() > 1 || negative))
      return false;
   magnitude = 0;
   constexpr unsigned __int128 all = ~static_cast<unsigned __int128>(0);
   for (char c : digits)
   {
      if (c < '0' || c > '9')
         return false;
      const unsigned d = static_cast<unsigned>(c - '0');
      if (magnitude > (all - d) / 10)
         return false;
      magnitude = magnitude * 10 + d;
   }
   const unsigned __int128 one = 1;
   if (!is_signed)
      return bits >= 128 || magnitude <= (one << bits) - 1;
   const unsigned __int128 half = one << (bits - 1);
   return negative ? magnitude <= half : magnitude <= half - 1;
}

template <typename F> bool parse_float(std::string_view token, F &value)
{
   const char *begin = token.data();
   const char *end = begin + token.size();
   const auto result = std::from_chars(begin, end, value);
   if (result.ec == std::errc::result_out_of_range)
   {
      // Overflow is an error; underflow rounds toward zero.
      const std::string copy(token);
      const double wide = std::strtod(copy.c_str(), nullptr);
      value = static_cast<F>(wide);
      return std::isfinite(wide) && std::isfinite(value);
   }
   if (result.ec != std::errc{} || result.ptr != end)
      return false;
   return std::isfinite(value);
}

inline int hex_value(char c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   return -1;
}

// ── Reading: the type walk over a cursor ───────────────────────────────────
template <JsonCursor C> class Reader
{
   const Schema &schema;
   C &c;
   const bool ijson;
   const JsonNames *names;
   const custom_lookup customs;

   const AnyType *resolve(const AnyType &type) { return type.resolve(schema); }

   // Index of the member `key` names, or members.size(). `expected` is
   // the member after the previous one read: tried first, with one compare.
   template <typename Name>
   std::size_t member_index(const std::vector<Member> &members, std::string_view key,
                            std::size_t expected, Name &&name_of)
   {
      if (expected < members.size() && name_of(members[expected]) == key)
         return expected;
      if (names)
         if (auto table = names->find(members))
            return table->find(key);
      std::size_t index = 0;
      while (index < members.size() && name_of(members[index]) != key)
         ++index;
      return index;
   }

   // A 128 / 256-bit integer: a decimal string (or the legacy "0x" string),
   // or a bare decimal number — any in-range value under json, at most
   // 2^53 - 1 in magnitude under ijson. A forced form accepts that form only.
   bool wide_integer(const Int &type, int_form form, Value &out)
   {
      if (type.bits != 128 && !(type.bits == 256 && !type.isSigned))
         return c.fail("json: this integer width has no JSON form");
      json_kind kind;
      if (!c.kind(kind))
         return false;
      std::string_view text;
      const bool quoted = kind == json_kind::string;
      if (quoted)
      {
         if (form == int_form::number)
            return c.fail("json: expected a number, found a string");
         bool escaped;
         if (!c.string(text, escaped))
            return false;
         if (escaped)
            return c.fail("json: integer string is not a canonical in-range decimal");
      }
      else
      {
         if (form == int_form::string)
            return c.fail("json: expected an integer string");
         if (kind != json_kind::number)
            return c.fail("json: expected an integer");
         if (!c.number(text))
            return false;
      }
      const char *const bad = quoted ? "json: integer string is not a canonical in-range decimal"
                                     : "json: integer is not a canonical in-range decimal";
      const bool bare_limit = !quoted && ijson && form == int_form::natural;
      if (type.bits == 256)
      {
         ::psio::uint256 v{};
         if (!psio::detail::json_impl::parse_wide_text(text, v))
            return c.fail(bad);
         if (bare_limit && !psio::detail::json_impl::wide_is_safe(v))
            return c.fail("ijson: bare integer outside the safe range must be a string");
         Bytes bytes(32);
         for (int i = 0; i < 32; ++i)
            bytes[i] = static_cast<unsigned char>(v.limb[i / 8] >> (8 * (i % 8)));
         out = std::move(bytes);
         return true;
      }
      if (type.isSigned)
      {
         ::psio::int128 v = 0;
         if (!psio::detail::json_impl::parse_wide_text(text, v))
            return c.fail(bad);
         if (bare_limit && !psio::detail::json_impl::wide_is_safe(v))
            return c.fail("ijson: bare integer outside the safe range must be a string");
         out = Signed{v};
      }
      else
      {
         ::psio::uint128 v = 0;
         if (!psio::detail::json_impl::parse_wide_text(text, v))
            return c.fail(bad);
         if (bare_limit && !psio::detail::json_impl::wide_is_safe(v))
            return c.fail("ijson: bare integer outside the safe range must be a string");
         out = Unsigned{v};
      }
      return true;
   }

   // An Enum: the enumerator's name ("active"), the number (2 or the numeric
   // string "2"), or both ("active:2", which must agree). An unknown name is
   // an error; a number that names no enumerator is accepted.
   bool enumeration(const schema_types::Enum &type, int_form form, Value &out)
   {
      const AnyType *under = type.type.get() ? resolve(*type.type) : nullptr;
      const Int *integer_type = under ? std::get_if<Int>(&under->value) : nullptr;
      if (!integer_type || integer_type->bits > 64 || integer_type->bits < 8)
         return c.fail("pssz IR: an enum needs an underlying integer of 8 to 64 bits");
      json_kind kind;
      if (!c.kind(kind))
         return false;
      if (kind != json_kind::string)
         return integer(*integer_type, form, out);
      std::string_view text;
      bool escaped;
      if (!c.string(text, escaped))
         return false;
      bool negative = false;
      unsigned __int128 magnitude = 0;
      auto number = [&](std::string_view digits) {
         return parse_integer(digits, integer_type->isSigned, integer_type->bits, negative,
                              magnitude);
      };
      auto bits_of = [&]() -> std::uint64_t {
         const std::uint64_t low = static_cast<std::uint64_t>(magnitude);
         return negative ? std::uint64_t{0} - low : low;
      };
      if (!text.empty() && (text[0] == '-' || (text[0] >= '0' && text[0] <= '9')))
      {
         if (!number(text))
            return c.fail("json: enum number is not a canonical in-range decimal");
      }
      else
      {
         const std::size_t colon = text.find(':');
         const std::string_view name = text.substr(0, colon);
         const Enumerator *found = nullptr;
         for (const auto &member : type.members)
            if (member.name == name)
            {
               found = &member;
               break;
            }
         if (!found)
            return c.fail("json: unknown enumerator name");
         if (colon != std::string_view::npos)
         {
            if (!number(text.substr(colon + 1)))
               return c.fail("json: enum number is not a canonical in-range decimal");
            if (bits_of() != found->value)
               return c.fail("json: enumerator name and number disagree");
         }
         else
         {
            const auto raw = static_cast<std::int64_t>(found->value);
            negative = integer_type->isSigned && raw < 0;
            magnitude = negative ? static_cast<unsigned __int128>(-(raw + 1)) + 1
                                 : static_cast<unsigned __int128>(found->value);
         }
      }
      if (integer_type->isSigned)
         out = Signed{negative ? -static_cast<__int128>(magnitude)
                               : static_cast<__int128>(magnitude)};
      else
         out = Unsigned{magnitude};
      return true;
   }

   bool integer(const Int &type, int_form form, Value &out)
   {
      if (type.bits > 64)
         return wide_integer(type, form, out);
      if (type.bits != 1 && type.bits != 8 && type.bits != 16 && type.bits != 32 &&
          type.bits != 64)
         return c.fail("json: this integer width has no JSON form");
      const bool wide = ijson && type.bits == 64 && form == int_form::natural;
      json_kind kind;
      if (!c.kind(kind))
         return false;
      bool negative;
      unsigned __int128 magnitude;
      if (kind == json_kind::string)
      {
         if (form != int_form::string && !wide)
            return c.fail("json: expected a number, found a string");
         std::string_view text;
         bool escaped;
         if (!c.string(text, escaped))
            return false;
         if (escaped || !parse_integer(text, type.isSigned, type.bits, negative, magnitude))
            return c.fail("json: integer string is not a canonical in-range decimal");
      }
      else
      {
         if (form == int_form::string)
            return c.fail("json: expected an integer string");
         if (kind != json_kind::number)
            return c.fail("json: expected an integer");
         std::string_view token;
         if (!c.number(token))
            return false;
         if (!parse_integer(token, type.isSigned, type.bits, negative, magnitude))
            return c.fail("json: integer is not a canonical in-range decimal");
         // Natural ijson: a bare number only where JavaScript is exact.
         if (wide && magnitude > safe_integer)
            return c.fail("ijson: bare integer outside the safe range must be a string");
      }
      if (type.isSigned)
         out = Signed{negative ? -static_cast<__int128>(magnitude)
                               : static_cast<__int128>(magnitude)};
      else
         out = Unsigned{magnitude};
      return true;
   }

   template <typename Item>
   bool array_items(std::optional<std::uint64_t> limit, bool exact, Item &&item)
   {
      if (!c.begin_array())
         return false;
      std::uint64_t count = 0;
      while (true)
      {
         bool has;
         if (!c.next_element(has))
            return false;
         if (!has)
            break;
         if (limit && count >= *limit)
            return c.fail(exact ? "json: array has too many elements"
                                : "json: array exceeds its bound");
         ++count;
         if (!item())
            return false;
      }
      if (exact && limit && count != *limit)
         return c.fail("json: array has too few elements");
      return true;
   }

   bool sequence(const Box<AnyType> &element, std::optional<std::uint64_t> limit, bool exact,
                 int_form form, [[maybe_unused]] std::size_t n, Value &out)
   {
      if (!element.get())
         return c.fail("pssz IR: null schema box");
      Sequence items;
      if (!array_items(limit, exact, [&] {
             Value item;
             if (!read(*element, form, 0, item))
                return false;
             items.push_back(std::move(item));
             return true;
          }))
         return false;
      out = std::move(items);
      return true;
   }

   // A Custom id that is not part of the binary form: "box" is the inner
   // type's text; a time id is a built-in presentation (time.hpp); any other
   // id is presented by its registered handler, or, with none, as the inner
   // type's text.
   bool transparent(const Custom &type, int_form form, std::size_t n, Value &out)
   {
      if (!type.type.get())
         return c.fail("pssz IR: null schema box");
      if (type.id != "box")
      {
         if (auto kind = ::psio::parse_time_schema_id(type.id))
            return time(type, *kind, form, out);
         if (::psio::reserved_time_schema_id(type.id))
            return c.fail("pssz IR: unsupported custom adapter");
         auto handler = customs.find(type);
         if (!handler && customs.required())
            return c.fail("pssz IR: no handler registered for a custom type");
         if (handler && handler->from_text)
            return presented(type, *handler, out);
         if (handler && handler->check)
         {
            // The wire type's text; the custom values nested in it are
            // checked as they are read, then this one on its bytes.
            if (!read(*type.type, form, n + 1, out))
               return false;
            std::vector<char> bytes;
            if (!encode_auto(schema, *type.type, out, bytes).ok())
               return c.fail("pssz IR: custom value does not match its inner type");
            return checked(*handler, bytes);
         }
      }
      return read(*type.type, form, n + 1, out);
   }
   // The handler's invariant on the bytes of a value just read.
   bool checked(const CustomHandlers::Handler &handler, const std::vector<char> &bytes)
   {
      if (!handler.check)
         return true;
      auto status = handler.check(std::span<const char>{bytes});
      if (status.ok())
         return true;
      const std::string_view what = status.error().what;
      if (failure.ok())
         failure = std::move(status);
      return c.fail(what.empty() ? std::string_view{"json: invariant violated"} : what);
   }
   // A custom presentation: one JSON string, turned by its handler into the
   // PSSZ bytes of the inner type, which are then read as a value of it.
   bool presented(const Custom &type, const CustomHandlers::Handler &handler, Value &out)
   {
      json_kind kind;
      if (!c.kind(kind))
         return false;
      if (kind != json_kind::string)
         return c.fail("json: expected a string (custom text form)");
      std::string_view text;
      bool escaped;
      if (!c.string(text, escaped))
         return false;
      std::vector<char> bytes;
      if (!handler.from_text || !handler.from_text(text, bytes).ok())
         return c.fail("json: text is not a value of the custom type");
      // The bytes are read as the wire type, which checks the custom values
      // nested in it; then this one.
      detail::Codec<0> codec(schema, customs.registry());
      codec.allow_unknown = true;
      auto status = codec.decode_at(*type.type, std::span<const char>{bytes}, out);
      if (!status.ok() || codec.has_unknown)
      {
         if (codec.check_failed)
         {
            const std::string_view what = status.error().what;
            if (failure.ok())
               failure = std::move(status);
            return c.fail(what.empty() ? std::string_view{"json: invariant violated"} : what);
         }
         return c.fail("json: custom handler produced bytes that are not the inner type");
      }
      return checked(handler, bytes);
   }
   // The built-in presentation of the time ids: a duration is its tick
   // integer; a time point is RFC 3339 text (time.hpp).
   bool time(const Custom &type, const ::psio::time_kind &kind, int_form form, Value &out)
   {
      const AnyType *inner = shape_of(schema, *type.type);
      const Int *ticks = inner ? std::get_if<Int>(&inner->value) : nullptr;
      if (!ticks || (ticks->bits != 8 && ticks->bits != 16 && ticks->bits != 32 &&
                     ticks->bits != 64))
         return c.fail("pssz IR: time value requires an 8/16/32/64-bit tick integer");
      if (!kind.point)
         return integer(*ticks, form, out);
      const int digits = ::psio::time_fraction_digits(kind.num, kind.den);
      if (digits < 0)
         return c.fail("json: a time point of this unit has no text form");
      json_kind json;
      if (!c.kind(json))
         return false;
      if (json != json_kind::string)
         return c.fail("json: expected a string (RFC 3339 time point)");
      std::string_view text;
      bool escaped;
      if (!c.string(text, escaped))
         return false;
      __int128 value;
      if (const char *err =
              ::psio::parse_time_text(text, digits, ticks->bits, ticks->isSigned, value))
         return c.fail(err);
      if (ticks->isSigned)
         out = Signed{value};
      else
         out = Unsigned{static_cast<unsigned __int128>(value)};
      return true;
   }

   bool custom(const Custom &type, int_form form, std::size_t n, Value &out)
   {
      if (type.id == "bool")
      {
         bool value;
         if (!c.boolean(value))
            return false;
         out = value;
         return true;
      }
      if (type.id == "char")
         return integer(Int{8, false}, form, out);
      if (type.id == "bitvector" || type.id == "bitlist")
         return c.fail("json: bit types have no JSON form");
      if (detail::assoc_id(type.id))
         return assoc(type, form, out);
      if (!detail::wire_custom_id(type.id))
         return transparent(type, form, n, out);
      if (!detail::byte_adapter_id(type.id))
         return c.fail("pssz IR: unsupported custom adapter");
      const byte_shape shape = bytes_of(schema, type);
      if (!shape.valid)
         return c.fail("pssz IR: byte adapter requires byte container");
      if (detail::text_adapter_id(type.id))
      {
         json_kind kind;
         if (!c.kind(kind))
            return false;
         if (kind != json_kind::string)
            return c.fail("json: expected a string");
         std::string_view text;
         bool escaped;
         if (!c.string(text, escaped))
            return false;
         if (!shape.admits(text.size()))
            return c.fail("json: string exceeds its bound");
         out = std::string(text);
         return true;
      }
      Bytes bytes;
      if (ijson)
      {
         json_kind kind;
         if (!c.kind(kind))
            return false;
         if (kind != json_kind::string)
            return c.fail("ijson: expected a hex string");
         std::string_view text;
         bool escaped;
         if (!c.string(text, escaped))
            return false;
         if (escaped)
            return c.fail("ijson: non-hex character in hex string");
         for (char digit : text)
            if (hex_value(digit) < 0)
               return c.fail("ijson: non-hex character in hex string");
         if (text.size() % 2)
            return c.fail("ijson: odd-length hex string");
         if (!shape.admits(text.size() / 2))
            return c.fail(shape.exact ? "ijson: hex string has the wrong length"
                                      : "json: array exceeds its bound");
         bytes.reserve(text.size() / 2);
         for (std::size_t i = 0; i < text.size(); i += 2)
            bytes.push_back(
                static_cast<unsigned char>((hex_value(text[i]) << 4) | hex_value(text[i + 1])));
      }
      else if (!array_items(shape.limit, shape.exact, [&] {
                  Value byte;
                  if (!integer(Int{8, false}, form, byte))
                     return false;
                  bytes.push_back(
                      static_cast<unsigned char>(std::get<Unsigned>(byte.value).value));
                  return true;
               }))
         return false;
      out = std::move(bytes);
      return true;
   }

   // Maps and sets (detail/pssz_ir_json_assoc.hpp).
   bool assoc(const Custom &type, int_form form, Value &out);
   bool key_from_text(const map_key_text &key, std::string_view text, Value &out);
   bool key_checked(const AnyType &type, const Value &key);

   bool record(const std::vector<Member> &members, [[maybe_unused]] std::size_t n, Value &out)
   {
      if (!c.begin_object())
         return false;
      // One slot per member, filled as keys arrive; a repeated key
      // overwrites its slot (last one wins).
      std::vector<std::optional<Value>> slots(members.size());
      std::size_t expected = 0;
      while (true)
      {
         bool has;
         std::string_view key;
         if (!c.next_field(has, key))
            return false;
         if (!has)
            break;
         const std::size_t index =
             member_index(members, key, expected,
                          [](const Member &m) -> std::string_view { return m.name; });
         if (index == members.size())
         {
            if (!c.skip())
               return false;
            continue;
         }
         expected = index + 1;
         const Member &member = members[index];
         if (!member.type.get())
            return c.fail("pssz IR: null schema box");
         Value value;
         if (!read(*member.type, member_form(member.attributes, ijson), 0, value))
            return false;
         std::optional<detail::LengthBound> bound;
         if (!detail::parse_length_bound(member.attributes, bound).ok())
            return c.fail("pssz IR: malformed lengthBound attribute");
         if (bound && !nested_member(schema, *member.type) && !admits(*bound, value))
            return c.fail("json: member length_bound violated");
         slots[index] = std::move(value);
      }
      Record values;
      values.reserve(members.size());
      for (std::size_t i = 0; i < members.size(); ++i)
      {
         if (!slots[i])
         {
            // A missing Option member is empty; any other is an error.
            const AnyType *type =
                members[i].type.get() ? shape_of(schema, *members[i].type) : nullptr;
            if (!type || !std::holds_alternative<Option>(type->value))
               return c.fail("json: missing required member");
            slots[i] = Value::none();
         }
         values.emplace_back(members[i].name, std::move(*slots[i]));
      }
      out = std::move(values);
      return true;
   }

   // The members of an argument list, given positionally: exactly one
   // element per member.
   bool record_from_array(const std::vector<Member> &members, Value &out)
   {
      Record values;
      values.reserve(members.size());
      std::size_t index = 0;
      if (!array_items(members.size(), true, [&] {
             const Member &member = members[index++];
             if (!member.type.get())
                return c.fail("pssz IR: null schema box");
             Value value;
             if (!read(*member.type, member_form(member.attributes, ijson), 0, value))
                return false;
             std::optional<detail::LengthBound> bound;
             if (!detail::parse_length_bound(member.attributes, bound).ok())
                return c.fail("pssz IR: malformed lengthBound attribute");
             if (bound && !nested_member(schema, *member.type) && !admits(*bound, value))
                return c.fail("json: member length_bound violated");
             values.emplace_back(member.name, std::move(value));
             return true;
          }))
         return false;
      out = std::move(values);
      return true;
   }

   bool variant(const schema_types::Variant &type, [[maybe_unused]] std::size_t n, Value &out)
   {
      std::size_t index = type.members.size();
      Value payload;
      auto alternative = [&] {
         const Member &member = type.members[index];
         if (!member.type.get())
            return c.fail("pssz IR: null schema box");
         return read(*member.type, int_form::natural, 0, payload);
      };
      bool has;
      if (ijson)
      {
         json_kind kind;
         if (!c.kind(kind))
            return false;
         if (kind != json_kind::object)
            return c.fail("ijson: expected a variant object");
         std::string_view key;
         if (!c.begin_object() || !c.next_field(has, key))
            return false;
         if (!has)
            return c.fail("ijson: variant object must have one key");
         index = member_index(type.members, key, type.members.size(),
                              [](const Member &m) { return variant_name(m); });
         if (index == type.members.size())
            return c.fail("ijson: unknown variant alternative");
         if (!alternative() || !c.next_field(has, key))
            return false;
         if (has)
            return c.fail("ijson: variant object must have one key");
      }
      else
      {
         json_kind kind;
         if (!c.kind(kind))
            return false;
         if (kind != json_kind::array)
            return c.fail("json: expected a [index, value] variant");
         if (!c.begin_array() || !c.next_element(has))
            return false;
         if (!has)
            return c.fail("json: expected a [index, value] variant");
         Value selector;
         if (!integer(Int{32, false}, int_form::natural, selector))
            return false;
         const auto wanted = std::get<Unsigned>(selector.value).value;
         if (wanted >= type.members.size())
            return c.fail("json: variant index out of range");
         index = static_cast<std::size_t>(wanted);
         if (!c.next_element(has))
            return false;
         if (!has)
            return c.fail("json: expected ',' in variant");
         if (!alternative() || !c.next_element(has))
            return false;
         if (has)
            return c.fail("json: expected ']' after variant value");
      }
      out = Value::variant(index, std::move(payload));
      return true;
   }

 public:
   // The status of a violated invariant (a handler's `check`), reported
   // unchanged in place of the cursor's.
   codec_status failure{};

   Reader(const Schema &schema, C &cursor, bool ijson, const JsonNames *names = nullptr,
          const CustomHandlers *handlers = nullptr)
       : schema(schema), c(cursor), ijson(ijson), names(names), customs{handlers, names}
   {
   }

   // The length a member lengthBound constrains, looking through Option.
   static bool admits(const detail::LengthBound &bound, const Value &value)
   {
      if (auto optional = std::get_if<Optional>(&value.value))
         return !optional->some || admits(bound, *optional->some);
      if (auto text = std::get_if<std::string>(&value.value))
         return bound.admits(text->size());
      if (auto bytes = std::get_if<Bytes>(&value.value))
         return bound.admits(bytes->size());
      if (auto items = std::get_if<Sequence>(&value.value))
         return bound.admits(items->size());
      return true;
   }

   bool read(const AnyType &input, int_form form, std::size_t n, Value &out)
   {
      // `n` counts schema steps that consume no text (nested Options); it
      // restarts inside every array / object, whose nesting the cursor
      // limits. Together they bound the recursion for any schema and text.
      if (n > kMaxValidationDepth)
         return c.fail("json: max depth exceeded");
      const AnyType *type = resolve(input);
      if (!type)
         return c.fail("pssz IR: unresolved named type");
      return std::visit(
          [&](const auto &t) -> bool
          {
             using T = std::decay_t<decltype(t)>;
             if constexpr (std::is_same_v<T, Int>)
                return integer(t, form, out);
             else if constexpr (std::is_same_v<T, Float>)
             {
                std::string_view token;
                if (!c.number(token))
                   return false;
                if (t.exp == 8)
                {
                   float value;
                   if (!parse_float(token, value))
                      return c.fail("json: number out of range");
                   out = value;
                }
                else
                {
                   double value;
                   if (!parse_float(token, value))
                      return c.fail("json: number out of range");
                   out = value;
                }
                return true;
             }
             else if constexpr (std::is_same_v<T, Custom>)
                return custom(t, form, n, out);
             else if constexpr (std::is_same_v<T, schema_types::Enum>)
                return enumeration(t, form, out);
             else if constexpr (std::is_same_v<T, Option>)
             {
                json_kind kind;
                if (!c.kind(kind))
                   return false;
                if (kind == json_kind::null)
                {
                   if (!c.null())
                      return false;
                   out = Value::none();
                   return true;
                }
                if (!t.type.get())
                   return c.fail("pssz IR: null schema box");
                Value value;
                if (!read(*t.type, form, n + 1, value))
                   return false;
                out = Value::some(std::move(value));
                return true;
             }
             else if constexpr (std::is_same_v<T, List>)
                return sequence(t.type, std::nullopt, false, form, n, out);
             else if constexpr (std::is_same_v<T, BoundedList>)
                return sequence(t.type, t.maxCount, false, form, n, out);
             else if constexpr (std::is_same_v<T, Array>)
                return sequence(t.type, t.len, true, form, n, out);
             else if constexpr (std::is_same_v<T, Tuple>)
             {
                Sequence items;
                std::size_t index = 0;
                if (!array_items(t.members.size(), true, [&] {
                       const auto &member = t.members[index++];
                       if (!member.get())
                          return c.fail("pssz IR: null schema box");
                       Value item;
                       if (!read(*member, int_form::natural, 0, item))
                          return false;
                       items.push_back(std::move(item));
                       return true;
                    }))
                   return false;
                out = std::move(items);
                return true;
             }
             else if constexpr (std::is_same_v<T, Object> || std::is_same_v<T, Struct>)
             {
                // An argument list (the `tuple` attribute): "positional" is
                // the array; "named" is the object, and a reader takes the
                // positional array as well.
                switch (json_detail::tuple_form(t.attributes))
                {
                   case json_detail::tuple_text::positional:
                      return record_from_array(t.members, out);
                   case json_detail::tuple_text::named:
                   {
                      json_kind kind;
                      if (!c.kind(kind))
                         return false;
                      if (kind == json_kind::array)
                         return record_from_array(t.members, out);
                      return record(t.members, n, out);
                   }
                   case json_detail::tuple_text::none:
                      break;
                }
                return record(t.members, n, out);
             }
             else if constexpr (std::is_same_v<T, schema_types::Variant>)
                return variant(t, n, out);
             else if constexpr (std::is_same_v<T, Nested>)
             {
                // The inner value's own text.
                if (!t.type.get())
                   return c.fail("pssz IR: null schema box");
                return read(*t.type, form, n + 1, out);
             }
             else
                return c.fail("pssz IR: unsupported schema type");
          },
          type->value);
   }
};

// ── Writing ────────────────────────────────────────────────────────────────
class Writer
{
   const Schema &schema;
   psio::detail::json_impl::writer &w;
   const bool ijson;
   const custom_lookup customs;

   void integer(const Int &type, int_form form, bool negative, unsigned __int128 magnitude)
   {
      // 64 bits: a string beyond 2^53 - 1 under ijson. Wider: always a
      // string under ijson, a bare number under json.
      const bool quoted =
          form == int_form::string ||
          (form == int_form::natural && ijson &&
           (type.bits > 64 || (type.bits == 64 && magnitude > safe_integer)));
      char buffer[48];
      char *p = buffer;
      if (quoted)
         *p++ = '"';
      if (negative)
         *p++ = '-';
      p = std::to_chars(p, buffer + sizeof(buffer) - 1, magnitude).ptr;
      if (quoted)
         *p++ = '"';
      w.append(buffer, p);
   }
   bool integer_value(const Int &type, const Value &value, bool &negative,
                      unsigned __int128 &magnitude)
   {
      if (type.isSigned)
      {
         auto v = std::get_if<Signed>(&value.value);
         if (!v)
            return false;
         negative = v->value < 0;
         magnitude = negative ? static_cast<unsigned __int128>(-(v->value + 1)) + 1
                              : static_cast<unsigned __int128>(v->value);
      }
      else
      {
         auto v = std::get_if<Unsigned>(&value.value);
         if (!v)
            return false;
         negative = false;
         magnitude = v->value;
      }
      const unsigned __int128 one = 1;
      const std::uint32_t bits = type.bits > 128 ? 128 : type.bits;
      if (!type.isSigned)
         return bits == 128 || magnitude <= (one << bits) - 1;
      const unsigned __int128 half = one << (bits - 1);
      return negative ? magnitude <= half : magnitude <= half - 1;
   }
   template <typename F> void floating(F value)
   {
      // NaN and the infinities have no JSON text, in either format.
      if (!std::isfinite(value))
         return w.fail(ijson ? "ijson: non-finite float has no JSON text"
                             : "json: non-finite float has no JSON text");
      char buffer[64];
      auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
      w.append(buffer, result.ptr);
   }
   void time(const Custom &type, const ::psio::time_kind &kind, const Value &value, int_form form)
   {
      const AnyType *inner = shape_of(schema, *type.type);
      const Int *ticks = inner ? std::get_if<Int>(&inner->value) : nullptr;
      bool negative;
      unsigned __int128 magnitude;
      if (!ticks || (ticks->bits != 8 && ticks->bits != 16 && ticks->bits != 32 &&
                     ticks->bits != 64))
         return w.fail("pssz IR: time value requires an 8/16/32/64-bit tick integer");
      if (!integer_value(*ticks, value, negative, magnitude))
         return w.fail("pssz IR: integer value required for a time value");
      if (!kind.point)
         return integer(*ticks, form, negative, magnitude);
      const int digits = ::psio::time_fraction_digits(kind.num, kind.den);
      if (digits < 0)
         return w.fail("json: a time point of this unit has no text form");
      char buffer[::psio::max_time_text_size + 2];
      buffer[0] = '"';
      char *end = ::psio::write_time_text(
          negative ? -static_cast<__int128>(magnitude) : static_cast<__int128>(magnitude), digits,
          buffer + 1);
      if (!end)
         return w.fail("json: time point outside years 0000..9999 has no text");
      *end++ = '"';
      w.append(buffer, end);
   }
   void custom(const Custom &type, const Value &value, int_form form)
   {
      if (type.id == "bool")
      {
         auto v = std::get_if<bool>(&value.value);
         if (!v)
            return w.fail("pssz IR: bool value required");
         w.append(*v ? std::string_view{"true"} : std::string_view{"false"});
         return;
      }
      if (type.id == "char")
      {
         auto v = std::get_if<Unsigned>(&value.value);
         if (!v || v->value > 255)
            return w.fail("pssz IR: char byte value required");
         return integer(Int{8, false}, form, false, v->value);
      }
      if (type.id == "bitvector" || type.id == "bitlist")
         return w.fail("json: bit types have no JSON form");
      if (detail::assoc_id(type.id))
         return assoc(type, value, form);
      if (!detail::wire_custom_id(type.id))
      {
         if (!type.type.get())
            return w.fail("pssz IR: null schema box");
         if (type.id != "box")
         {
            if (auto kind = ::psio::parse_time_schema_id(type.id))
               return time(type, *kind, value, form);
            if (::psio::reserved_time_schema_id(type.id))
               return w.fail("pssz IR: unsupported custom adapter");
            auto handler = customs.find(type);
            if (!handler && customs.required())
               return w.fail("pssz IR: no handler registered for a custom type");
            const bool check = handler && handler->check && checks;
            if (handler && (handler->to_text || check))
            {
               // The inner value as PSSZ bytes — which checks the custom
               // values nested in it — then this one's check, then the
               // handler's text as one JSON string.
               std::vector<char> bytes;
               const detail::Codec<0> codec(schema, checks ? customs.registry() : nullptr);
               if (auto status = codec.encode_at(*type.type, value, bytes); !status.ok())
               {
                  if (codec.check_failed)
                     return w.fail(status.error());
                  return w.fail("pssz IR: custom value does not match its inner type");
               }
               if (check)
                  if (auto st = handler->check(std::span<const char>{bytes}); !st.ok())
                     return w.fail(st.error());
               if (handler->to_text)
               {
                  std::string text;
                  if (!handler->to_text(std::span<const char>{bytes}, text).ok())
                     return w.fail("json: custom handler has no text for the value");
                  return psio::detail::json_impl::write_string(w, text, ijson);
               }
               // The wire type's text; everything under it is checked.
               const bool outer = checks;
               checks           = false;
               write(*type.type, value, form, 0);
               checks = outer;
               return;
            }
         }
         // "box", or a custom type with no handler: the inner type.
         return write(*type.type, value, form, 0);
      }
      if (!detail::byte_adapter_id(type.id))
         return w.fail("pssz IR: unsupported custom adapter");
      const byte_shape shape = bytes_of(schema, type);
      if (!shape.valid)
         return w.fail("pssz IR: byte adapter requires byte container");
      if (detail::text_adapter_id(type.id))
      {
         // A raw "string" holding bytes that are not UTF-8 has no JSON
         // text: write_string reports it.
         auto v = std::get_if<std::string>(&value.value);
         if (!v)
            return w.fail("pssz IR: string value required");
         if (!shape.admits(v->size()))
            return w.fail("json: string exceeds its bound");
         return psio::detail::json_impl::write_string(w, *v, ijson);
      }
      auto v = std::get_if<Bytes>(&value.value);
      if (!v)
         return w.fail("pssz IR: bytes value required");
      if (!shape.admits(v->size()))
         return w.fail("json: array exceeds its bound");
      if (ijson)
         return psio::detail::json_impl::write_hex(w, v->data(), v->size());
      w.put('[');
      for (std::size_t i = 0; i < v->size(); ++i)
      {
         if (i)
            w.put(',');
         integer(Int{8, false}, form, false, (*v)[i]);
      }
      w.put(']');
   }
   // Maps and sets (detail/pssz_ir_json_assoc.hpp).
   void assoc(const Custom &type, const Value &value, int_form form);

   void sequence(const Box<AnyType> &element, const Value &value,
                 std::optional<std::uint64_t> limit, bool exact, int_form form, std::size_t n)
   {
      auto items = std::get_if<Sequence>(&value.value);
      if (!items || !element.get())
         return w.fail("pssz IR: sequence value required");
      if (limit && (exact ? items->size() != *limit : items->size() > *limit))
         return w.fail("json: array exceeds its bound");
      w.put('[');
      for (std::size_t i = 0; i < items->size(); ++i)
      {
         if (i)
            w.put(',');
         write(*element, (*items)[i], form, n + 1);
      }
      w.put(']');
   }

 public:
   // Whether the handlers' `check` runs on the values written (false for a
   // value that was just decoded with the same registry).
   bool checks = true;

   Writer(const Schema &schema, psio::detail::json_impl::writer &w, bool ijson,
          const CustomHandlers *handlers = nullptr, const JsonNames *names = nullptr)
       : schema(schema), w(w), ijson(ijson), customs{handlers, names}
   {
   }

   void write(const AnyType &input, const Value &value, int_form form, std::size_t n)
   {
      // The recursion follows the value, which is finite (a decoded value
      // is at most kMaxValidationDepth nodes deep).
      (void)n;
      if (!w.err.empty())
         return;
      const AnyType *type = input.resolve(schema);
      if (!type)
         return w.fail("pssz IR: unresolved named type");
      std::visit(
          [&](const auto &t)
          {
             using T = std::decay_t<decltype(t)>;
             if constexpr (std::is_same_v<T, Int>)
             {
                if (t.bits == 256 && !t.isSigned)
                {
                   auto bytes = std::get_if<Bytes>(&value.value);
                   if (!bytes || bytes->size() != 32)
                      return w.fail("pssz IR: 256-bit integer needs 32 little-endian bytes");
                   ::psio::uint256 v{};
                   for (int i = 0; i < 32; ++i)
                      v.limb[i / 8] |= static_cast<std::uint64_t>((*bytes)[i]) << (8 * (i % 8));
                   const bool quoted = form == int_form::string ||
                                       (form == int_form::natural && ijson);
                   std::string digits;
                   if (quoted)
                      digits.push_back('"');
                   ::psio::ext_int_detail::append_decimal_u256(digits, v);
                   if (quoted)
                      digits.push_back('"');
                   return w.append(std::string_view{digits});
                }
                if (t.bits > 128)
                   return w.fail("json: this integer width has no JSON form");
                bool negative;
                unsigned __int128 magnitude;
                if (!integer_value(t, value, negative, magnitude))
                   return w.fail("pssz IR: integer value does not match the schema");
                integer(t, form, negative, magnitude);
             }
             else if constexpr (std::is_same_v<T, Float>)
             {
                if (auto v = std::get_if<float>(&value.value); v && t.exp == 8)
                   floating(*v);
                else if (auto d = std::get_if<double>(&value.value); d && t.exp == 11)
                   floating(*d);
                else
                   w.fail("pssz IR: float value does not match the schema");
             }
             else if constexpr (std::is_same_v<T, Custom>)
                custom(t, value, form);
             else if constexpr (std::is_same_v<T, schema_types::Enum>)
             {
                const AnyType *under = t.type.get() ? t.type->resolve(schema) : nullptr;
                const Int *integer_type = under ? std::get_if<Int>(&under->value) : nullptr;
                if (!integer_type || integer_type->bits > 64 || integer_type->bits < 8)
                   return w.fail("pssz IR: an enum needs an underlying integer of 8 to 64 bits");
                bool negative;
                unsigned __int128 magnitude;
                if (!integer_value(*integer_type, value, negative, magnitude))
                   return w.fail("pssz IR: integer value does not match the schema");
                const std::uint64_t low = static_cast<std::uint64_t>(magnitude);
                const std::uint64_t raw = negative ? std::uint64_t{0} - low : low;
                // A listed value is its (first) name; any other its number.
                for (const auto &member : t.members)
                   if (member.value == raw)
                      return psio::detail::json_impl::write_string(w, member.name, ijson);
                integer(*integer_type, form, negative, magnitude);
             }
             else if constexpr (std::is_same_v<T, Option>)
             {
                auto v = std::get_if<Optional>(&value.value);
                if (!v || !t.type.get())
                   return w.fail("pssz IR: explicit optional value required");
                if (!v->some)
                   w.append("null");
                else
                   write(*t.type, *v->some, form, n + 1);
             }
             else if constexpr (std::is_same_v<T, List>)
                sequence(t.type, value, std::nullopt, false, form, n);
             else if constexpr (std::is_same_v<T, BoundedList>)
                sequence(t.type, value, t.maxCount, false, form, n);
             else if constexpr (std::is_same_v<T, Array>)
                sequence(t.type, value, t.len, true, form, n);
             else if constexpr (std::is_same_v<T, Tuple>)
             {
                auto items = std::get_if<Sequence>(&value.value);
                if (!items || items->size() != t.members.size())
                   return w.fail("pssz IR: tuple value count mismatch");
                w.put('[');
                for (std::size_t i = 0; i < items->size(); ++i)
                {
                   if (i)
                      w.put(',');
                   if (!t.members[i].get())
                      return w.fail("pssz IR: null schema box");
                   write(*t.members[i], (*items)[i], int_form::natural, n + 1);
                }
                w.put(']');
             }
             else if constexpr (std::is_same_v<T, Object> || std::is_same_v<T, Struct>)
             {
                auto fields = std::get_if<Record>(&value.value);
                if (!fields)
                   return w.fail("pssz IR: ordered record value required");
                const Value absent = Value::none();
                // An argument list without parameter names is the array.
                const bool positional =
                    json_detail::tuple_form(t.attributes) == json_detail::tuple_text::positional;
                w.put(positional ? '[' : '{');
                bool first = true;
                for (const auto &member : t.members)
                {
                   const Value *field = &absent;
                   bool found = false;
                   for (const auto &[name, candidate] : *fields)
                      if (name == member.name)
                      {
                         field = &candidate;
                         found = true;
                      }
                   const AnyType *member_type =
                       member.type.get() ? member.type->resolve(schema) : nullptr;
                   if (!member_type)
                      return w.fail("pssz IR: unresolved named type");
                   const AnyType *member_shape = shape_of(schema, *member_type);
                   if (!found &&
                       (!member_shape || !std::holds_alternative<Option>(member_shape->value)))
                      return w.fail("pssz IR: missing required record value");
                   std::optional<detail::LengthBound> bound;
                   if (!detail::parse_length_bound(member.attributes, bound).ok())
                      return w.fail("pssz IR: malformed lengthBound attribute");
                   if (bound && !nested_member(schema, *member_type) &&
                       !Reader<json_text_cursor>::admits(*bound, *field))
                      return w.fail("json: member length_bound violated");
                   if (!first)
                      w.put(',');
                   first = false;
                   if (!positional)
                   {
                      psio::detail::json_impl::write_string(w, member.name, false);
                      w.put(':');
                   }
                   write(*member_type, *field, member_form(member.attributes, ijson), n + 1);
                }
                w.put(positional ? ']' : '}');
             }
             else if constexpr (std::is_same_v<T, schema_types::Variant>)
             {
                auto v = std::get_if<pssz_ir::Variant>(&value.value);
                if (!v || !v->payload || v->selector >= t.members.size() ||
                    !t.members[v->selector].type.get())
                   return w.fail("pssz IR: invalid explicit variant");
                const Member &member = t.members[v->selector];
                if (ijson)
                {
                   w.put('{');
                   psio::detail::json_impl::write_string(w, variant_name(member), false);
                   w.put(':');
                   write(*member.type, *v->payload, int_form::natural, n + 1);
                   w.put('}');
                }
                else
                {
                   w.put('[');
                   {
                      char digits[24];
                      w.append(digits, std::to_chars(digits, digits + sizeof(digits), v->selector).ptr);
                   }
                   w.put(',');
                   write(*member.type, *v->payload, int_form::natural, n + 1);
                   w.put(']');
                }
             }
             else if constexpr (std::is_same_v<T, Nested>)
             {
                // The inner value's own text.
                if (!t.type.get())
                   return w.fail("pssz IR: null schema box");
                write(*t.type, value, form, n + 1);
             }
             else
                w.fail("pssz IR: unsupported schema type");
          },
          type->value);
   }
};
} // namespace json_detail

// ── The type walk over any cursor ──────────────────────────────────────────
//
// Reads one value of `root` from `cursor` (which must be positioned at it)
// and requires the document to end. Errors are recorded in the cursor.
// `names`, when given, must have been built for `schema`.
template <JsonCursor C>
[[nodiscard]] bool read_json(const schema_types::Schema &schema,
                             const schema_types::AnyType &root, bool ijson, C &cursor,
                             Value &output, const JsonNames *names = nullptr,
                             const CustomHandlers *custom = nullptr,
                             codec_status *invariant = nullptr)
{
   Value value;
   json_detail::Reader<C> reader(schema, cursor, ijson, names, custom);
   if (!reader.read(root, json_detail::int_form::natural, 0, value) || !cursor.finish())
   {
      // A violated invariant is reported with its own status.
      if (invariant && !reader.failure.ok())
         *invariant = std::move(reader.failure);
      return false;
   }
   output = std::move(value);
   return true;
}

// ── Text <-> Value ─────────────────────────────────────────────────────────
template <psio::detail::json_impl::json_family Fmt>
[[nodiscard]] codec_status json_to_value(const schema_types::Schema &schema,
                                         const schema_types::AnyType &root, Fmt,
                                         std::span<const char> text, Value &output,
                                         const JsonNames *names = nullptr,
                                         json_parser parser = json_parser::automatic,
                                         const CustomHandlers *custom = nullptr)
{
   // Schema errors (unsupported shapes, bad attributes) come first.
   detail::Codec<0> codec(schema);
   if (auto status = codec.metadata(root); !status.ok())
      return status;
   // The same choice of syntax layer as the typed readers (json.hpp).
   codec_status invariant;
   auto run = [&](auto &cursor) -> codec_status
   {
      if (read_json(schema, root, Fmt::i_json, cursor, output, names, custom, &invariant))
         return codec_ok();
      if (!invariant.ok())
         return std::move(invariant);
      return cursor.status(Fmt::name);
   };
#if PSIO_HAVE_SIMDJSON
   if (psio::detail::json_impl::use_simdjson(text, parser))
   {
      if (parser == json_parser::simdjson)
      {
         simdjson_cursor cursor(text, Fmt::i_json);
         return run(cursor);
      }
      simdjson_index_cursor cursor(text, Fmt::i_json);
      return run(cursor);
   }
#else
   (void)parser;
#endif
   json_text_cursor cursor(text, Fmt::i_json);
   return run(cursor);
}

// A string literal / C string is its characters, without the terminator.
template <psio::detail::json_impl::json_family Fmt>
[[nodiscard]] codec_status json_to_value(const schema_types::Schema &schema,
                                         const schema_types::AnyType &root, Fmt format,
                                         const char *text, Value &output,
                                         const JsonNames *names = nullptr,
                                         json_parser parser = json_parser::automatic,
                                         const CustomHandlers *custom = nullptr)
{
   const std::string_view view{text};
   return json_to_value(schema, root, format, std::span<const char>{view.data(), view.size()},
                        output, names, parser, custom);
}

// Appends the text of `value`; `out` is unchanged on failure. The handlers'
// checks run on the value's custom types before any text is returned
// (`run_checks = false`: the value was just decoded with the same registry).
template <psio::detail::json_impl::json_family Fmt>
[[nodiscard]] codec_status value_to_json(const schema_types::Schema &schema,
                                         const schema_types::AnyType &root, Fmt,
                                         const Value &value, std::string &out,
                                         const CustomHandlers *custom = nullptr,
                                         const JsonNames *names = nullptr,
                                         bool run_checks = true)
{
   detail::Codec<0> codec(schema);
   if (auto status = codec.metadata(root); !status.ok())
      return status;
   const std::size_t start = out.size();
   psio::detail::json_impl::writer w{out, Fmt::name, false};
   json_detail::Writer writer(schema, w, Fmt::i_json, custom, names);
   writer.checks = run_checks;
   writer.write(root, value, json_detail::int_form::natural, 0);
   w.finish();
   if (w.err.empty())
      return codec_ok();
   out.resize(start);
   if (w.has_invariant)
      return codec_status{w.invariant};
   return codec_fail(w.err, static_cast<std::uint32_t>(start), Fmt::name);
}

// ── What a schema-only consumer calls ──────────────────────────────────────

// JSON / I-JSON text of a `root` value -> its canonical PSSZ bytes (appended
// to `out`, which is unchanged on failure).
template <psio::detail::json_impl::json_family Fmt>
[[nodiscard]] codec_status json_to_pssz(const schema_types::Schema &schema,
                                        const schema_types::AnyType &root, Fmt format,
                                        std::span<const char> text, std::vector<char> &out,
                                        const JsonNames *names = nullptr,
                                        json_parser parser = json_parser::automatic,
                                        const CustomHandlers *custom = nullptr)
{
   Value value;
   if (auto status = json_to_value(schema, root, format, text, value, names, parser, custom);
       !status.ok())
      return status;
   return encode_auto(schema, root, value, out);
}

template <psio::detail::json_impl::json_family Fmt>
[[nodiscard]] codec_status json_to_pssz(const schema_types::Schema &schema,
                                        const schema_types::AnyType &root, Fmt format,
                                        const char *text, std::vector<char> &out,
                                        const JsonNames *names = nullptr,
                                        json_parser parser = json_parser::automatic,
                                        const CustomHandlers *custom = nullptr)
{
   const std::string_view view{text};
   return json_to_pssz(schema, root, format, std::span<const char>{view.data(), view.size()},
                       out, names, parser, custom);
}

// PSSZ bytes of a `root` value -> its JSON / I-JSON text (appended to `out`,
// unchanged on failure). The bytes are fully validated. Unknown appended
// optional fields are accepted, dropped from the text and reported through
// `has_unknown` (a caller that must not lose them rejects when it is set).
template <psio::detail::json_impl::json_family Fmt>
[[nodiscard]] codec_status pssz_to_json(const schema_types::Schema &schema,
                                        const schema_types::AnyType &root, Fmt format,
                                        std::span<const char> bytes, std::string &out,
                                        bool *has_unknown = nullptr,
                                        const CustomHandlers *custom = nullptr,
                                        const JsonNames *names = nullptr)
{
   // The decode runs the handlers' checks; the writer need not repeat them.
   const CustomHandlers *registry = names && names->handlers() ? names->handlers() : custom;
   Value value;
   bool unknown = false;
   if (auto status = decode_auto(schema, root, bytes, value, unknown, registry); !status.ok())
      return status;
   if (auto status = value_to_json(schema, root, format, value, out, custom, names, false);
       !status.ok())
      return status;
   if (has_unknown)
      *has_unknown = unknown;
   return codec_ok();
}

// Validates PSSZ bytes against `root` (the typed `validate` rule: unknown
// appended optional fields are accepted and reported). Without `custom` the
// check is structural: a custom type is its wire type. With it, every value
// of a custom type whose handler has a `check` is checked too, and under
// `require_all` a custom type with no handler is an error.
// The bytes of a Nested node are validated as its inner type, checks
// included; `nested_bytes::opaque` leaves them unexamined (the rule of a
// typed nested<T, nested_validation::deferred>).
[[nodiscard]] inline codec_status validate_pssz(const schema_types::Schema &schema,
                                                const schema_types::AnyType &root,
                                                std::span<const char> bytes,
                                                bool *has_unknown = nullptr,
                                                const CustomHandlers *custom = nullptr,
                                                nested_bytes nested = nested_bytes::typed)
{
   return validate_auto(schema, root, bytes, has_unknown, custom, nested);
}
} // namespace psio::pssz_ir

#include <psio/detail/pssz_ir_json_assoc.hpp>
