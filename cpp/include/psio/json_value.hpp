#pragma once
//
// psio/json_value.hpp — psio::json_value: any JSON value, without a schema.
//
// A small owning tree for the places where the shape of a document is not
// known when the program is compiled: a configuration passed through, the
// arguments of an action shown to a person, the `variables` of a GraphQL
// request, a member of a reflected record that holds "any JSON here".
//
//    psio::json_value v = psio::from_ijson<psio::json_value>(R"({"a":1.50,"b":[true,null]})");
//    v.find("a")->as_number()->text        // "1.50": a number keeps its text
//    v.set("c", "x");                      // {"a":1.50,"b":[true,null],"c":"x"}
//    psio::to_ijson(v);                    // the same text, compact
//
//   null      std::monostate
//   boolean   bool
//   number    psio::json_numeral: the RFC 8259 token as it was written.
//             Nothing is rounded or renormalised: 1.50, 1e3 and
//             18446744073709551616 are written back as they were read;
//             as_int64 / as_uint64 / as_double convert on request
//   string    std::string (UTF-8)
//   array     std::vector<json_value>
//   object    std::vector<std::pair<std::string, json_value>>: the members
//             in the order they were written. A reader keeps the LAST value
//             of a repeated key, at the position of the first; a writer
//             writes the last value of a key that a hand-built object holds
//             twice. So an object read and written again has each key once
//
// It has no exceptions, no RTTI and no dependency beyond the standard
// library: it is usable in a wasm guest. Reading and writing it is
// psio/json.hpp's: `psio::json_value` is a value like any other to
// psio::json and psio::ijson (from_ijson, to_ijson, decode_json_checked,
// encode_json_checked, a member of a reflected record, an element of a
// std::vector, ...). The two formats differ for it in one thing only: under
// ijson a string holding a Unicode noncharacter is not written or read.
// Numbers are written as their text in both — a json_value does not know
// that a number is a 64-bit integer, so it cannot give it the ijson string
// form.
//
// Binary. In PSSZ (and the other binary formats) a json_value is a UTF-8
// string holding its compact psio::json text: the bytes of a `utf8_string`
// member whose content is that text. The Schema IR node is
// `Custom{ Custom{List<u8>, "utf8"}, "json" }`: on the wire a UTF-8
// string; the id "json" (reserved for this) tells a schema-driven codec
// that the value's text form is the JSON value itself, inline, not a string
// holding it (docs/json.md "Any JSON value").

#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace psio
{
   // A JSON number: the token, kept as written.
   struct json_numeral
   {
      std::string text = "0";

      json_numeral() = default;
      // The token itself ("1.50", "-0", "1e3"). A text that is not an RFC
      // 8259 number is not written: the writer reports it.
      explicit json_numeral(std::string token) : text(std::move(token)) {}
      template <std::integral I>
         requires(!std::same_as<I, bool>)
      json_numeral(I v) : text(std::to_string(v))
      {
      }
      // The shortest text that reads back as `v`. A non-finite value has
      // no JSON text (the writer reports it).
      json_numeral(double v)
      {
         char       buffer[64];
         const auto end = std::to_chars(buffer, buffer + sizeof buffer, v).ptr;
         text.assign(buffer, end);
      }

      // The value when the token is an integer in range, written without
      // fraction or exponent ("7", "-12"; not "7.0", not "1e3").
      std::optional<std::int64_t> as_int64() const noexcept { return integer<std::int64_t>(); }
      std::optional<std::uint64_t> as_uint64() const noexcept { return integer<std::uint64_t>(); }
      // The nearest double (any token).
      double as_double() const noexcept { return std::strtod(text.c_str(), nullptr); }

      friend bool operator==(const json_numeral&, const json_numeral&) = default;

     private:
      template <typename I>
      std::optional<I> integer() const noexcept
      {
         I          v{};
         const auto end = text.data() + text.size();
         const auto res = std::from_chars(text.data(), end, v);
         if (res.ec != std::errc{} || res.ptr != end)
            return std::nullopt;
         return v;
      }
   };

   class json_value
   {
     public:
      using array   = std::vector<json_value>;
      using member  = std::pair<std::string, json_value>;
      using object  = std::vector<member>;
      using storage = std::variant<std::monostate, bool, json_numeral, std::string, array, object>;

      storage value;

      json_value() = default;  // null
      json_value(std::nullptr_t) {}
      template <std::same_as<bool> B>
      json_value(B b) : value(b)
      {
      }
      template <std::integral I>
         requires(!std::same_as<I, bool>)
      json_value(I v) : value(json_numeral{v})
      {
      }
      json_value(double v) : value(json_numeral{v}) {}
      json_value(json_numeral n) : value(std::move(n)) {}
      json_value(std::string s) : value(std::move(s)) {}
      json_value(std::string_view s) : value(std::string{s}) {}
      json_value(const char* s) : value(std::string{s}) {}
      json_value(array a) : value(std::move(a)) {}
      json_value(object o) : value(std::move(o)) {}

      bool is_null() const noexcept { return std::holds_alternative<std::monostate>(value); }
      bool is_bool() const noexcept { return std::holds_alternative<bool>(value); }
      bool is_number() const noexcept { return std::holds_alternative<json_numeral>(value); }
      bool is_string() const noexcept { return std::holds_alternative<std::string>(value); }
      bool is_array() const noexcept { return std::holds_alternative<array>(value); }
      bool is_object() const noexcept { return std::holds_alternative<object>(value); }

      // The value as one kind, or null when it is another.
      const bool*        as_bool() const noexcept { return std::get_if<bool>(&value); }
      const json_numeral* as_number() const noexcept { return std::get_if<json_numeral>(&value); }
      const std::string* as_string() const noexcept { return std::get_if<std::string>(&value); }
      const array*       as_array() const noexcept { return std::get_if<array>(&value); }
      const object*      as_object() const noexcept { return std::get_if<object>(&value); }
      bool*              as_bool() noexcept { return std::get_if<bool>(&value); }
      json_numeral*       as_number() noexcept { return std::get_if<json_numeral>(&value); }
      std::string*       as_string() noexcept { return std::get_if<std::string>(&value); }
      array*             as_array() noexcept { return std::get_if<array>(&value); }
      object*            as_object() noexcept { return std::get_if<object>(&value); }

      // The member `key` of an object (its last value, when a hand-built
      // object holds the key twice); null when there is none or this is not
      // an object.
      const json_value* find(std::string_view key) const noexcept
      {
         const json_value* found = nullptr;
         if (const object* members = as_object())
            for (const member& m : *members)
               if (m.first == key)
                  found = &m.second;
         return found;
      }
      json_value* find(std::string_view key) noexcept
      {
         return const_cast<json_value*>(std::as_const(*this).find(key));
      }
      // Sets the member `key`: replaced where it is, appended when new. A
      // value that is not an object becomes one (empty) first.
      json_value& set(std::string_view key, json_value v)
      {
         if (!is_object())
            value = object{};
         if (json_value* existing = find(key))
         {
            *existing = std::move(v);
            return *existing;
         }
         return std::get_if<object>(&value)->emplace_back(std::string{key}, std::move(v)).second;
      }
      // Removes the member `key` (every occurrence); how many were removed.
      std::size_t erase(std::string_view key)
      {
         object* members = as_object();
         if (!members)
            return 0;
         return std::erase_if(*members, [&](const member& m) { return m.first == key; });
      }
   };

   inline bool operator==(const json_value& a, const json_value& b)
   {
      return a.value == b.value;
   }
}  // namespace psio
