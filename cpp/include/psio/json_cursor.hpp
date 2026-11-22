#pragma once
//
// psio/json_cursor.hpp — the JSON SYNTAX layer behind a parser-agnostic
// cursor.
//
// JSON text handling is two layers:
//
//   1. Syntax (this header). A cursor owns everything about the text:
//      whitespace, punctuation, string escapes and UTF-8, the RFC 8259
//      number grammar, nesting depth, skipping a value. It hands out events.
//   2. Types (json.hpp for a C++ type, pssz_ir_json.hpp for the Schema IR). A
//      walk that checks the cursor's events against the type being read:
//      member names, arity, integer ranges and forms, hex digits, variant
//      names, length bounds. It never looks at the text itself.
//
// A cursor is forward-only and single-pass: values are consumed in document
// order, exactly once, and a string view is valid only until the next call.
// That is the contract of a streaming parser such as simdjson's on-demand
// API, so an implementation over one can replace `json_text_cursor` without
// touching a type walk. The walks need no random access and no second pass:
// a record's members are stored as they arrive (a repeated key is read again
// and the last one wins), and a variant's selector precedes its value in
// both text forms.
//
// The cursor concept (all functions return false after recording an error;
// the first error is kept):
//
//   bool kind(json_kind&)             type of the next value, not consumed
//   bool begin_object()               '{'; one nesting level deeper
//   bool next_field(bool& has, std::string_view& key)
//                                     the next member's key (unescaped), or
//                                     has = false once the object is closed
//   bool begin_array()                '['
//   bool next_element(bool& has)      positions at the next element, or
//                                     has = false once the array is closed
//   bool string(std::string_view& text, bool& escaped)
//                                     a string value, unescaped; `escaped`
//                                     tells whether the text between the
//                                     quotes differed from the value
//   bool number(std::string_view& token)
//                                     a number as its RFC 8259 token text
//   bool boolean(bool&)   bool null()
//   bool skip()                       one value of any shape
//   bool finish()                     nothing but whitespace remains
//   bool fail(std::string_view what)  a type-layer error at this position
//   codec_status status(std::string_view format) const
//
// Optional fast paths. A cursor may also offer either of these; a type walk
// uses them when present (`requires`) and the required events otherwise.
// Neither changes what is accepted.
//
//   bool scalar(std::string_view& token)
//                                     for a value that starts like a number:
//                                     its characters up to the next
//                                     whitespace or structural character,
//                                     NOT checked against the number
//                                     grammar. For a caller that parses the
//                                     token itself and thereby validates it
//                                     (an integer is `0 | -?[1-9][0-9]*`,
//                                     stricter than the grammar).
//   bool real(double& value)          a number as its correctly rounded
//                                     double: the RFC 8259 grammar, an
//                                     error when out of range
//
// Three cursors implement it: `json_text_cursor` (here) over the hand-written
// lexer of detail/json_syntax.hpp, and, when PSIO_HAVE_SIMDJSON,
// `simdjson_index_cursor` (simdjson's stage 1 plus a walk over its token
// index) and `simdjson_cursor` (simdjson's on-demand API), both in
// json_simdjson.hpp.

#include <psio/detail/json_syntax.hpp>
#include <psio/detail/validate_depth.hpp>
#include <psio/error.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace psio
{
   enum class json_kind : std::uint8_t
   {
      object,
      array,
      string,
      number,
      boolean,
      null,
   };

   // Which syntax layer a typed read runs on.
   //
   //   automatic   the fastest measured for the document: with simdjson
   //               built in (PSIO_HAVE_SIMDJSON), simdjson_index_cursor for
   //               a document of at least 64 bytes whose root is an object
   //               or array; the hand-written lexer otherwise (small
   //               documents, root scalars, builds without simdjson)
   //   text        the hand-written lexer (json_text_cursor), always
   //   simdjson    simdjson's on-demand parser (simdjson_cursor) whenever it
   //               can take the document: it is built in and the root is an
   //               object or array; else the lexer
   //   simdjson_index
   //               simdjson's stage 1 (SIMD token index and UTF-8 check)
   //               with PSIO's own walk over the index
   //               (simdjson_index_cursor); same conditions
   //
   // The choice never changes what is accepted or what is read.
   enum class json_parser : std::uint8_t
   {
      automatic,
      text,
      simdjson,
      simdjson_index,
   };

   // Readable bytes simdjson needs after the last byte of a document.
   inline constexpr std::size_t json_padding = 64;

   // JSON text followed by at least `json_padding` readable bytes (their
   // content does not matter). Handing a document over as `padded_json`
   // lets the simdjson layer parse it in place; a plain span is copied once
   // into a padded buffer first.
   struct padded_json
   {
      std::span<const char> text;

      // The caller vouches for the padding.
      static padded_json assume(std::span<const char> text) noexcept { return {text}; }
      // Grows `s`'s capacity (not its size) to cover the padding.
      static padded_json of(std::string& s)
      {
         if (s.capacity() - s.size() < json_padding)
            s.reserve(s.size() + json_padding);
         return {std::span<const char>{s.data(), s.size()}};
      }
   };

   template <typename C>
   concept JsonCursor = requires(C& c, const C& cc, json_kind& kind, bool& flag,
                                 std::string_view& text) {
      { c.kind(kind) } -> std::same_as<bool>;
      { c.begin_object() } -> std::same_as<bool>;
      { c.next_field(flag, text) } -> std::same_as<bool>;
      { c.begin_array() } -> std::same_as<bool>;
      { c.next_element(flag) } -> std::same_as<bool>;
      { c.string(text, flag) } -> std::same_as<bool>;
      { c.number(text) } -> std::same_as<bool>;
      { c.boolean(flag) } -> std::same_as<bool>;
      { c.null() } -> std::same_as<bool>;
      { c.skip() } -> std::same_as<bool>;
      { c.finish() } -> std::same_as<bool>;
      { c.fail(std::string_view{}) } -> std::same_as<bool>;
      { cc.status(std::string_view{}) } -> std::same_as<codec_status>;
   };

   // The hand-written lexer as a cursor. `ijson` applies the
   // I-JSON string rule (no Unicode noncharacters), also inside skipped
   // values. Nesting deeper than kMaxValidationDepth is an error. It never
   // throws or aborts and allocates only for strings that carry escapes.
   class json_text_cursor
   {
      detail::json_impl::reader r_;
      bool                      nonchar_;
      std::size_t               depth_ = 0;
      // Whether the container at each open level has produced an item yet.
      std::array<bool, kMaxValidationDepth + 1> fresh_{};
      std::string                               scratch_;
      std::string                               key_scratch_;

      bool open(char c, std::string_view expected)
      {
         if (!r_.take(c))
            return r_.fail(expected);
         if (depth_ >= kMaxValidationDepth)
            return r_.fail("json: max depth exceeded");
         fresh_[depth_++] = true;
         return true;
      }
      // Advances past the separator before the next item, or closes the
      // container. `more` is false once it is closed.
      bool advance(char close, std::string_view expected, bool& more)
      {
         if (depth_ == 0)
            return r_.fail("json: cursor is not inside a container");
         const bool first = fresh_[depth_ - 1];
         fresh_[depth_ - 1] = false;
         if (r_.take(close))
         {
            --depth_;
            more = false;
            return true;
         }
         if (!first && !r_.take(','))
            return r_.fail(expected);
         more = true;
         return true;
      }

     public:
      json_text_cursor(std::span<const char> text, bool ijson)
          : r_{text.data(), text.data(), text.data() + text.size()}, nonchar_(ijson)
      {
         if (text.empty())
            r_.fail("json: empty input");
      }

      bool fail(std::string_view what) { return r_.fail(what); }
      bool failed() const { return !r_.err.empty(); }

      codec_status status(std::string_view format) const
      {
         if (r_.err.empty())
            return codec_ok();
         return codec_fail(r_.err, static_cast<std::uint32_t>(r_.err_at - r_.begin), format);
      }

      bool kind(json_kind& out)
      {
         if (failed())
            return false;
         r_.ws();
         if (r_.p >= r_.end)
            return r_.fail("json: unexpected end of input");
         switch (*r_.p)
         {
            case '{': out = json_kind::object; return true;
            case '[': out = json_kind::array; return true;
            case '"': out = json_kind::string; return true;
            case 't':
            case 'f': out = json_kind::boolean; return true;
            case 'n': out = json_kind::null; return true;
            default:
               if (*r_.p == '-' || (*r_.p >= '0' && *r_.p <= '9'))
               {
                  out = json_kind::number;
                  return true;
               }
               return r_.fail("json: unexpected character at value start");
         }
      }

      bool begin_object() { return !failed() && open('{', "json: expected '{'"); }
      bool begin_array() { return !failed() && open('[', "json: expected '['"); }

      bool next_field(bool& has, std::string_view& key)
      {
         if (failed() || !advance('}', "json: expected ',' or '}' in object", has))
            return false;
         if (!has)
            return true;
         if (!detail::json_impl::read_key(r_, key_scratch_, key, nonchar_))
            return false;
         if (!r_.take(':'))
            return r_.fail("json: expected ':' after object key");
         return true;
      }

      bool next_element(bool& has)
      {
         return !failed() && advance(']', "json: expected ',' or ']' in array", has);
      }

      bool string(std::string_view& text, bool& escaped)
      {
         if (failed())
            return false;
         r_.ws();
         const char* start = r_.p;
         if (!detail::json_impl::read_string(r_, nullptr, nonchar_, &escaped))
            return false;
         if (!escaped)
         {
            text = std::string_view{start + 1, static_cast<std::size_t>(r_.p - start - 2)};
            return true;
         }
         r_.p = start;
         scratch_.clear();
         if (!detail::json_impl::read_string(r_, &scratch_, nonchar_))
            return false;
         text = scratch_;
         return true;
      }

      bool number(std::string_view& token)
      {
         if (failed())
            return false;
         r_.ws();
         const char* begin = r_.p;
         if (begin >= r_.end || !(*begin == '-' || (*begin >= '0' && *begin <= '9')))
            return r_.fail("json: expected a number");
         if (!detail::json_impl::skip_number(r_))
            return false;
         token = std::string_view{begin, static_cast<std::size_t>(r_.p - begin)};
         return true;
      }

      // Optional fast path: the token's characters, unchecked.
      bool scalar(std::string_view& token)
      {
         if (failed())
            return false;
         r_.ws();
         const char* const begin = r_.p;
         if (begin >= r_.end || !(*begin == '-' || (*begin >= '0' && *begin <= '9')))
            return r_.fail("json: expected a number");
         const char* p = begin + 1;
         while (p < r_.end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' &&
                *p != '\t' && *p != '\r')
            ++p;
         r_.p  = p;
         token = std::string_view{begin, static_cast<std::size_t>(p - begin)};
         return true;
      }

      bool boolean(bool& value)
      {
         if (failed())
            return false;
         r_.ws();
         if (r_.keyword("true"))
         {
            value = true;
            return true;
         }
         if (r_.keyword("false"))
         {
            value = false;
            return true;
         }
         return r_.fail("json: expected true or false");
      }

      bool null()
      {
         if (failed())
            return false;
         r_.ws();
         return r_.keyword("null") || r_.fail("json: malformed keyword");
      }

      bool skip() { return !failed() && detail::json_impl::skip_value(r_, depth_, nonchar_); }

      bool finish()
      {
         if (failed())
            return false;
         r_.ws();
         if (r_.p != r_.end)
            return r_.fail("json: trailing bytes after value");
         return true;
      }
   };
   static_assert(JsonCursor<json_text_cursor>);

}  // namespace psio
