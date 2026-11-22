#pragma once
//
// psio/json_simdjson.hpp — simdjson as the JSON syntax layer: two JsonCursor
// implementations. Included by json.hpp when PSIO_HAVE_SIMDJSON is 1.
//
//   simdjson_index_cursor   simdjson's stage 1 (one SIMD pass that indexes
//                           every token and validates the document's UTF-8
//                           and its strings' control characters) followed by
//                           PSIO's own walk over the token index. The
//                           fastest reader for any document of 64 bytes or
//                           more; what json_parser::automatic uses.
//   simdjson_cursor         simdjson's on-demand API (ondemand::document,
//                           object / array iterators). Selectable with
//                           json_parser::simdjson; slower than the index
//                           walk on every measured shape, kept as the
//                           reference use of simdjson's public reader.
//
// In both, simdjson contributes what it is fastest at and everything the
// type layer depends on is decided by PSIO on the raw text:
//
//   * a number is handed over as its token text (never through a 64-bit
//     accessor), checked against the RFC 8259 grammar here and parsed
//     exactly by the type layer. That covers the whole u64 range, bare big
//     integers in strict json, the "bare only up to 2^53 - 1" rule of ijson
//     and the rejection of `-0`, leading zeros, fractions and exponents for
//     integers. Only a `double` target uses simdjson's number parser, which
//     is exact and accepts the same grammar;
//   * a string without escapes is a view of the input (no copy); one with
//     escapes is unescaped by simdjson. Either way the cursor checks that
//     nothing but whitespace follows the closing quote inside the token
//     (on-demand itself would let `"a"x` through), reports whether the
//     string was written with escapes, and applies the I-JSON noncharacter
//     rule;
//   * true / false / null are compared as whole tokens;
//   * a skipped value is walked and checked like a read one (on-demand's
//     own skip only counts brackets);
//   * nesting is limited to kMaxValidationDepth;
//   * a UTF-8 byte order mark is rejected (simdjson would skip it).
//
// simdjson is used through its error-code API only: no exceptions.
//
// Scope. The cursors take documents whose root is an object or an array.
// A root scalar goes to json_text_cursor (see use_simdjson). The simdjson
// state is one per thread; a read that starts while another is in progress
// on the same thread (a text adapter decoding JSON from inside a decode)
// also goes to json_text_cursor.
//
// Padding. simdjson reads up to json_padding bytes past the document. A
// `padded_json` is parsed in place; a plain span is copied once into a
// padded per-thread buffer (documents over 1 MiB: a buffer freed at the end
// of the read).

#include <psio/json_cursor.hpp>

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace psio
{
   namespace detail::json_impl
   {
      struct simdjson_thread_state
      {
         simdjson::ondemand::parser parser;
         // Stage 1 on its own, for simdjson_index_cursor.
         std::unique_ptr<simdjson::internal::dom_parser_implementation> stage1;
         std::string unescaped;            // strings and keys that carry escapes
         std::string copy;                 // padded copy of a small document
         bool        busy = false;         // a cursor is live on this thread
      };
      inline simdjson_thread_state& simdjson_state() noexcept
      {
         thread_local simdjson_thread_state state;
         return state;
      }

      // Documents shorter than this stay on the hand-written lexer under
      // json_parser::automatic. A simdjson read has a fixed cost (the padded
      // copy, stage 1 set-up: about 20 ns) that its faster scan repays from
      // roughly 64 bytes on; a 28-byte record reads in 35 ns on the lexer
      // and 48 ns through simdjson, a 92-byte one in 94 ns and 70 ns
      // (arm64; cpp/benchmarks/bench_json_text.cpp, docs/json.md).
#ifndef PSIO_JSON_SIMDJSON_MIN_SIZE
#define PSIO_JSON_SIMDJSON_MIN_SIZE 64
#endif

      // Whether `text` goes to simdjson under `parser`.
      inline bool use_simdjson(std::span<const char> text, json_parser parser) noexcept
      {
         if (parser == json_parser::text)
            return false;
         if (parser == json_parser::automatic && text.size() < PSIO_JSON_SIMDJSON_MIN_SIZE)
            return false;
         const char*       p   = text.data();
         const char* const end = p + text.size();
         while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            ++p;
         if (p >= end || (*p != '{' && *p != '['))
            return false;
         return !simdjson_state().busy;
      }

      // `text` holds a backslash. Reads whole words: `text` must be followed
      // by at least seven readable bytes (it lies in a padded document).
      inline bool has_backslash(std::string_view text) noexcept
      {
         constexpr std::uint64_t ones  = 0x0101010101010101ull;
         constexpr std::uint64_t highs = 0x8080808080808080ull;
         const char*             p     = text.data();
         std::size_t             n     = text.size();
         while (true)
         {
            std::uint64_t word;
            std::memcpy(&word, p, 8);
            if constexpr (std::endian::native == std::endian::little)
            {
               if (n < 8)
                  word |= ~std::uint64_t{0} << (8 * n);  // bytes past the end: no match
            }
            else if (n < 8)
               word |= ~std::uint64_t{0} >> (8 * n);
            const std::uint64_t x = word ^ (ones * '\\');
            if ((x - ones) & ~x & highs)
               return true;
            if (n <= 8)
               return false;
            p += 8;
            n -= 8;
         }
      }

      constexpr bool json_ws(char c) noexcept
      {
         return c == ' ' || c == '\t' || c == '\n' || c == '\r';
      }

      // Valid UTF-8 `text` holds a Unicode noncharacter.
      inline bool has_noncharacter(std::string_view text) noexcept
      {
         const auto*       p   = reinterpret_cast<const unsigned char*>(text.data());
         const auto* const end = p + text.size();
         while (p < end)
         {
            if (end - p >= 8)
            {
               std::uint64_t word;
               std::memcpy(&word, p, 8);
               if ((word & 0x8080808080808080ull) == 0)
               {
                  p += 8;
                  continue;
               }
            }
            if (*p < 0x80)
            {
               ++p;
               continue;
            }
            std::uint32_t  cp = 0;
            const unsigned n  = utf8_multibyte(p, end, cp);
            if (n == 0)
               return true;  // not reached for validated text
            if (is_noncharacter(cp))
               return true;
            p += n;
         }
         return false;
      }
   }  // namespace detail::json_impl

   // A document's bytes with the padding simdjson needs: the caller's own
   // buffer (padded_json) or one padded copy.
   class simdjson_input
   {
      std::unique_ptr<char[]> large_;  // padded copy of a large document
     public:
      const char* data = nullptr;
      std::size_t size = 0;

      simdjson_input(std::span<const char> text, detail::json_impl::simdjson_thread_state& state)
          : size(text.size())
      {
         char* copy;
         if (size > (std::size_t{1} << 20))
         {
            large_.reset(new char[size + json_padding]);
            copy = large_.get();
         }
         else
         {
            if (state.copy.size() < size + json_padding)
               state.copy.resize(size + json_padding);
            copy = state.copy.data();
         }
         std::memcpy(copy, text.data(), size);
         std::memset(copy + size, 0, json_padding);
         data = copy;
      }
      simdjson_input(padded_json text, detail::json_impl::simdjson_thread_state&)
          : data(text.text.data()), size(text.text.size())
      {
      }
   };

   // ── simdjson stage 1 + PSIO's own token walk ───────────────────────────
   //
   // Stage 1 is where simdjson's speed is: one SIMD pass that validates the
   // document's UTF-8, rejects raw control characters in strings and
   // unclosed strings, and writes the offset of every token (structural
   // character or first character of a scalar) into an index. This cursor
   // runs that pass and then walks the index itself: the next token is one
   // array load away, with no whitespace scanning and no iterator objects.
   // All grammar above the token level is checked here, as in the
   // hand-written lexer: separators, colons, closers, depth, the number
   // grammar, literals, escapes (unescaped by simdjson's parse_string).
   //
   // A string token runs from its opening quote to the next token. Stage 1
   // starts a new token at any non-whitespace character after a closing
   // quote, so the closing quote is the last non-whitespace byte before the
   // next token; it is found backwards, without scanning the string.
   class simdjson_index_cursor
   {
      detail::json_impl::simdjson_thread_state& state_;
      simdjson_input                            in_;
      const char*                               buf_ = nullptr;
      const std::uint32_t*                      idx_ = nullptr;  // token offsets
      std::uint32_t                             n_   = 0;        // token count
      std::uint32_t                             i_   = 0;        // next token
      bool                                      nonchar_;
      bool                                      root_open_ = false;
      std::size_t                               depth_     = 0;
      std::string_view                          err_{};
      std::uint32_t                             err_at_ = 0;
      // Whether the container at each open level has produced an item yet.
      std::array<bool, kMaxValidationDepth + 1> fresh_;

      bool fail_at(std::string_view what, std::size_t at) noexcept
      {
         if (err_.empty())
         {
            err_    = what;
            err_at_ = static_cast<std::uint32_t>(at <= in_.size ? at : in_.size);
         }
         return false;
      }
      // Offset of token i; the index ends with sentinels equal to size.
      std::size_t at(std::uint32_t i) const noexcept { return idx_[i]; }
      // First byte of the next token, or 0 at the end of the document.
      char peek() const noexcept { return i_ < n_ ? buf_[idx_[i_]] : '\0'; }

      void run() noexcept
      {
         state_.busy = true;
         buf_        = in_.data;
         if (in_.size >= 3 && std::memcmp(buf_, "\xEF\xBB\xBF", 3) == 0)
         {
            fail_at("json: unexpected character at value start", 0);
            return;
         }
         auto& stage1 = state_.stage1;
         simdjson::error_code code = simdjson::SUCCESS;
         const std::size_t capacity = std::max<std::size_t>(in_.size, 4096);
         if (!stage1)
            code = simdjson::get_active_implementation()->create_dom_parser_implementation(
               capacity, 2, stage1);
         else if (stage1->capacity() < in_.size)
            code = stage1->allocate(capacity, 2);
         if (code == simdjson::SUCCESS)
            code = stage1->stage1(reinterpret_cast<const std::uint8_t*>(buf_), in_.size,
                                  simdjson::stage1_mode::regular);
         if (code != simdjson::SUCCESS)
         {
            fail_at(simdjson::error_message(code), 0);
            return;
         }
         idx_ = stage1->structural_indexes.get();
         n_   = stage1->n_structural_indexes;
      }

      // The current scalar token without the whitespace after it.
      std::string_view token() const noexcept
      {
         const std::size_t b = at(i_);
         std::size_t       e = at(i_ + 1);
         while (e > b && detail::json_impl::json_ws(buf_[e - 1]))
            --e;
         return {buf_ + b, e - b};
      }

      // The string token at i_: `text` is its value. Does not advance.
      bool read_string(std::string_view& text, bool& escaped)
      {
         const std::string_view raw = token();
         if (raw.size() < 2 || raw.back() != '"')
            return fail_at("json: unterminated string", at(i_));
         const std::string_view body = raw.substr(1, raw.size() - 2);
         escaped                     = detail::json_impl::has_backslash(body);
         if (!escaped)
            text = body;
         else
         {
            // The unescaped text is never longer than the escaped one;
            // simdjson writes in blocks, hence the padding.
            if (state_.unescaped.size() < body.size() + json_padding)
               state_.unescaped.resize(body.size() + json_padding);
            auto* const dst = reinterpret_cast<std::uint8_t*>(state_.unescaped.data());
            const std::uint8_t* const end = state_.stage1->parse_string(
               reinterpret_cast<const std::uint8_t*>(body.data()), dst, false);
            if (!end)
               return fail_at("json: invalid escape in string", at(i_));
            // parse_string stops at the first unescaped quote: it must be
            // this token's closing quote.
            text = std::string_view{state_.unescaped.data(),
                                    static_cast<std::size_t>(end - dst)};
         }
         if (nonchar_ && detail::json_impl::has_noncharacter(text))
            return fail_at("ijson: Unicode noncharacter in string", at(i_));
         return true;
      }

      bool open(char c, std::string_view expected)
      {
         if (failed())
            return false;
         if (peek() != c || (depth_ == 0 && root_open_))
            return fail(expected);
         if (depth_ >= kMaxValidationDepth)
            return fail("json: max depth exceeded");
         fresh_[depth_++] = true;
         root_open_       = true;
         ++i_;
         return true;
      }
      // Past the separator before the next item, or out of the container.
      bool advance(char close, std::string_view expected, bool& more)
      {
         if (failed())
            return false;
         if (depth_ == 0)
            return fail("json: cursor is not inside a container");
         const bool first   = fresh_[depth_ - 1];
         fresh_[depth_ - 1] = false;
         const char c       = peek();
         if (c == close)
         {
            ++i_;
            --depth_;
            more = false;
            return true;
         }
         if (!first)
         {
            if (c != ',')
               return fail(expected);
            ++i_;
         }
         more = true;
         return true;
      }

     public:
      template <typename Text>
      simdjson_index_cursor(Text text, bool ijson)
          : state_(detail::json_impl::simdjson_state()), in_(text, state_), nonchar_(ijson)
      {
         run();
      }
      simdjson_index_cursor(const simdjson_index_cursor&)            = delete;
      simdjson_index_cursor& operator=(const simdjson_index_cursor&) = delete;
      ~simdjson_index_cursor() { state_.busy = false; }

      bool fail(std::string_view what) { return fail_at(what, i_ < n_ ? at(i_) : in_.size); }
      bool failed() const { return !err_.empty(); }

      codec_status status(std::string_view format) const
      {
         if (err_.empty())
            return codec_ok();
         return codec_fail(err_, err_at_, format);
      }

      bool kind(json_kind& out)
      {
         if (failed())
            return false;
         switch (const char c = (depth_ == 0 && root_open_) ? '\0' : peek())
         {
            case '{': out = json_kind::object; return true;
            case '[': out = json_kind::array; return true;
            case '"': out = json_kind::string; return true;
            case 't':
            case 'f': out = json_kind::boolean; return true;
            case 'n': out = json_kind::null; return true;
            default:
               if (c == '-' || (c >= '0' && c <= '9'))
               {
                  out = json_kind::number;
                  return true;
               }
               return fail("json: unexpected character at value start");
         }
      }

      bool begin_object() { return open('{', "json: expected '{'"); }
      bool begin_array() { return open('[', "json: expected '['"); }

      bool next_field(bool& has, std::string_view& key)
      {
         if (!advance('}', "json: expected ',' or '}' in object", has))
            return false;
         if (!has)
            return true;
         if (peek() != '"')
            return fail("json: expected a string");
         bool escaped = false;
         if (!read_string(key, escaped))
            return false;
         ++i_;
         if (peek() != ':')
            return fail("json: expected ':' after object key");
         ++i_;
         return true;
      }

      bool next_element(bool& has)
      {
         return advance(']', "json: expected ',' or ']' in array", has);
      }

      bool string(std::string_view& text, bool& escaped)
      {
         if (failed())
            return false;
         if (depth_ == 0 || peek() != '"')
            return fail("json: expected a string");
         if (!read_string(text, escaped))
            return false;
         ++i_;
         return true;
      }

      // Optional fast path: the token's characters, unchecked.
      bool scalar(std::string_view& text)
      {
         if (failed())
            return false;
         const char c = depth_ == 0 ? '\0' : peek();
         if (!(c == '-' || (c >= '0' && c <= '9')))
            return fail("json: expected a number");
         text = token();
         ++i_;
         return true;
      }

      // Optional fast path: simdjson's number parser (exact, RFC 8259
      // grammar, checks what follows the number, out of range is an error).
      bool real(double& value)
      {
         if (failed())
            return false;
         const char c = depth_ == 0 ? '\0' : peek();
         if (!(c == '-' || (c >= '0' && c <= '9')))
            return fail("json: expected a number");
         if (simdjson::SIMDJSON_BUILTIN_IMPLEMENTATION::numberparsing::parse_double(
                reinterpret_cast<const std::uint8_t*>(buf_ + at(i_)))
                .get(value) != simdjson::SUCCESS)
            return fail("json: malformed or out-of-range number");
         ++i_;
         return true;
      }

      bool number(std::string_view& text)
      {
         std::string_view raw;
         if (!scalar(raw))
            return false;
         detail::json_impl::reader r{raw.data(), raw.data(), raw.data() + raw.size()};
         if (!detail::json_impl::skip_number(r) || r.p != r.end)
         {
            --i_;
            return fail(r.err.empty() ? std::string_view{"json: malformed number"} : r.err);
         }
         text = raw;
         return true;
      }

      bool boolean(bool& value)
      {
         if (failed())
            return false;
         if (depth_ > 0 && i_ < n_)
         {
            // The usual case first: the literal runs right up to the next
            // token.
            const char* const p    = buf_ + at(i_);
            const std::size_t size = at(i_ + 1) - at(i_);
            if (size == 4 && std::memcmp(p, "true", 4) == 0)
            {
               value = true;
               ++i_;
               return true;
            }
            if (size == 5 && std::memcmp(p, "false", 5) == 0)
            {
               value = false;
               ++i_;
               return true;
            }
            const std::string_view text = token();
            if (text == "true" || text == "false")
            {
               value = text.size() == 4;
               ++i_;
               return true;
            }
         }
         return fail("json: expected true or false");
      }

      bool null()
      {
         if (failed())
            return false;
         if (depth_ > 0 && i_ < n_ && token() == "null")
         {
            ++i_;
            return true;
         }
         return fail("json: malformed keyword");
      }

      // Walks the value: a skipped value is checked like a read one.
      bool skip()
      {
         json_kind k;
         if (!kind(k))
            return false;
         std::string_view text;
         bool             flag = false;
         switch (k)
         {
            case json_kind::object:
               if (!begin_object())
                  return false;
               while (true)
               {
                  if (!next_field(flag, text))
                     return false;
                  if (!flag)
                     return true;
                  if (!skip())
                     return false;
               }
            case json_kind::array:
               if (!begin_array())
                  return false;
               while (true)
               {
                  if (!next_element(flag))
                     return false;
                  if (!flag)
                     return true;
                  if (!skip())
                     return false;
               }
            case json_kind::string: return string(text, flag);
            case json_kind::number: return number(text);
            case json_kind::boolean: return boolean(flag);
            case json_kind::null: return null();
         }
         return fail("json: unexpected character at value start");
      }

      bool finish()
      {
         if (failed())
            return false;
         if (depth_ != 0 || !root_open_)
            return fail("json: unexpected end of input");
         if (i_ != n_)
            return fail("json: trailing bytes after value");
         return true;
      }
   };
   static_assert(JsonCursor<simdjson_index_cursor>);

   // ── simdjson on-demand ─────────────────────────────────────────────────
   class simdjson_cursor
   {
      struct frame
      {
         simdjson::ondemand::object_iterator object;
         simdjson::ondemand::array_iterator  array;
         bool                                started;
      };

      detail::json_impl::simdjson_thread_state& state_;
      simdjson::ondemand::document              doc_;
      simdjson::ondemand::value                 cur_;        // the value the cursor is at
      const char*                               tok_     = nullptr;  // its first byte
      const char*                               tok_end_ = nullptr;  // start of the next token
      const char*                               begin_   = nullptr;
      const char*                               end_     = nullptr;
      bool                                      nonchar_;
      bool                                      root_open_ = false;  // the root was entered
      std::size_t                               depth_     = 0;
      std::string_view                          err_{};
      std::uint32_t                             err_at_ = 0;
      simdjson_input                            in_;
      std::array<frame, kMaxValidationDepth>    frames_;

      bool fail_at(std::string_view what, const char* at) noexcept
      {
         if (err_.empty())
         {
            err_    = what;
            err_at_ = at >= begin_ && at <= end_ ? static_cast<std::uint32_t>(at - begin_) : 0;
         }
         return false;
      }
      bool fail(simdjson::error_code code) noexcept
      {
         const char* at = nullptr;
         if (doc_.current_location().get(at) != simdjson::SUCCESS)
            at = end_;
         return fail_at(simdjson::error_message(code), at);
      }

      void open(const char* data, std::size_t size, std::size_t capacity) noexcept
      {
         state_.busy = true;
         begin_      = data;
         end_        = data + size;
         // simdjson skips a UTF-8 byte order mark; JSON text has none.
         if (size >= 3 && std::memcmp(data, "\xEF\xBB\xBF", 3) == 0)
         {
            fail_at("json: unexpected character at value start", data);
            return;
         }
         if (auto code = state_.parser.iterate(data, size, capacity).get(doc_);
             code != simdjson::SUCCESS)
            fail_at(simdjson::error_message(code), data);
      }

      // Positions the cursor at `cur_`.
      void at_value() noexcept
      {
         const std::string_view token = cur_.raw_json_token();
         tok_                         = token.data();
         tok_end_                     = token.data() + token.size();
      }

      // The current scalar's token without the whitespace after it.
      std::string_view scalar() const noexcept
      {
         const char* e = tok_end_;
         while (e > tok_ && detail::json_impl::json_ws(e[-1]))
            --e;
         return {tok_, static_cast<std::size_t>(e - tok_)};
      }

      // The closing quote of the string whose first character is `s`.
      // `escaped` is set when a backslash comes first. The input is padded
      // and stage 1 has paired every quote, so the scan cannot run away.
      static const char* string_end(const char* s, bool& escaped) noexcept
      {
         escaped = false;
         while (true)
         {
            std::uint64_t word;
            std::memcpy(&word, s, 8);
            constexpr std::uint64_t ones  = 0x0101010101010101ull;
            constexpr std::uint64_t highs = 0x8080808080808080ull;
            const std::uint64_t     quote = word ^ (ones * '"');
            const std::uint64_t     slash = word ^ (ones * '\\');
            if ((((quote - ones) & ~quote) | ((slash - ones) & ~slash)) & highs)
               break;
            s += 8;
         }
         while (true)
         {
            if (*s == '"')
               return s;
            if (*s == '\\')
            {
               escaped = true;
               s += 2;
            }
            else
               ++s;
         }
      }

      bool enter() noexcept
      {
         if (depth_ >= kMaxValidationDepth)
            return fail_at("json: max depth exceeded", tok_ ? tok_ : begin_);
         frames_[depth_].started = false;
         ++depth_;
         root_open_ = true;
         return true;
      }
      // The root may be entered once; below it there is always a value.
      bool at_root() const noexcept { return depth_ == 0 && !root_open_; }
      char first() const noexcept
      {
         if (depth_ > 0)
            return *tok_;
         if (root_open_)
            return '\0';  // the root value was already read
         const char* p = begin_;
         while (p < end_ && detail::json_impl::json_ws(*p))
            ++p;
         return p < end_ ? *p : '\0';
      }
      const char* here() const noexcept { return depth_ > 0 ? tok_ : begin_; }

     public:
      // A plain span is copied into a padded buffer; a padded_json is
      // parsed in place.
      template <typename Text>
      simdjson_cursor(Text text, bool ijson)
          : state_(detail::json_impl::simdjson_state()), nonchar_(ijson), in_(text, state_)
      {
         open(in_.data, in_.size, in_.size + json_padding);
      }
      simdjson_cursor(const simdjson_cursor&)            = delete;
      simdjson_cursor& operator=(const simdjson_cursor&) = delete;
      ~simdjson_cursor() { state_.busy = false; }

      bool fail(std::string_view what) { return fail_at(what, here()); }
      bool failed() const { return !err_.empty(); }

      codec_status status(std::string_view format) const
      {
         if (err_.empty())
            return codec_ok();
         return codec_fail(err_, err_at_, format);
      }

      bool kind(json_kind& out)
      {
         if (failed())
            return false;
         switch (const char c = first())
         {
            case '{': out = json_kind::object; return true;
            case '[': out = json_kind::array; return true;
            case '"': out = json_kind::string; return true;
            case 't':
            case 'f': out = json_kind::boolean; return true;
            case 'n': out = json_kind::null; return true;
            default:
               if (c == '-' || (c >= '0' && c <= '9'))
               {
                  out = json_kind::number;
                  return true;
               }
               return fail("json: unexpected character at value start");
         }
      }

      bool begin_object()
      {
         if (failed())
            return false;
         if (first() != '{')
            return fail("json: expected '{'");
         if (depth_ >= kMaxValidationDepth)
            return fail("json: max depth exceeded");
         simdjson::ondemand::object object;
         if (auto code = at_root() ? doc_.get_object().get(object) : cur_.get_object().get(object);
             code != simdjson::SUCCESS)
            return fail(code);
         if (auto code = object.begin().get(frames_[depth_].object); code != simdjson::SUCCESS)
            return fail(code);
         return enter();
      }

      bool begin_array()
      {
         if (failed())
            return false;
         if (first() != '[')
            return fail("json: expected '['");
         if (depth_ >= kMaxValidationDepth)
            return fail("json: max depth exceeded");
         simdjson::ondemand::array array;
         if (auto code = at_root() ? doc_.get_array().get(array) : cur_.get_array().get(array);
             code != simdjson::SUCCESS)
            return fail(code);
         if (auto code = array.begin().get(frames_[depth_].array); code != simdjson::SUCCESS)
            return fail(code);
         return enter();
      }

      bool next_field(bool& has, std::string_view& key)
      {
         if (failed())
            return false;
         if (depth_ == 0)
            return fail("json: cursor is not inside a container");
         frame& f = frames_[depth_ - 1];
         if (f.started)
            ++f.object;
         f.started = true;
         if (!(f.object != f.object))
         {
            --depth_;
            has = false;
            return true;
         }
         simdjson::ondemand::field field;
         if (auto code = (*f.object).get(field); code != simdjson::SUCCESS)
            return fail(code);
         const char* const start   = field.key().raw();
         bool              escaped = false;
         const char* const quote   = string_end(start, escaped);
         if (!escaped)
            key = std::string_view{start, static_cast<std::size_t>(quote - start)};
         else if (auto code = field.unescaped_key().get(key); code != simdjson::SUCCESS)
            return fail_at("json: invalid escape in string", start);
         // Only whitespace may sit between the key's closing quote and ':'.
         const char* after = quote + 1;
         while (detail::json_impl::json_ws(*after))
            ++after;
         if (*after != ':')
            return fail_at("json: expected ':' after object key", after);
         if (nonchar_ && detail::json_impl::has_noncharacter(key))
            return fail_at("ijson: Unicode noncharacter in string", start);
         cur_ = field.value();
         at_value();
         has = true;
         return true;
      }

      bool next_element(bool& has)
      {
         if (failed())
            return false;
         if (depth_ == 0)
            return fail("json: cursor is not inside a container");
         frame& f = frames_[depth_ - 1];
         if (f.started)
            ++f.array;
         f.started = true;
         if (!(f.array != f.array))
         {
            --depth_;
            has = false;
            return true;
         }
         if (auto code = (*f.array).get(cur_); code != simdjson::SUCCESS)
            return fail(code);
         at_value();
         has = true;
         return true;
      }

      bool string(std::string_view& text, bool& escaped)
      {
         if (failed())
            return false;
         if (first() != '"')
            return fail("json: expected a string");
         const char* const start = tok_ + 1;
         const char* const quote = string_end(start, escaped);
         // Up to the next structural character only whitespace may follow.
         for (const char* after = quote + 1; after < tok_end_; ++after)
            if (!detail::json_impl::json_ws(*after))
               return fail_at("json: unexpected character after string", after);
         if (!escaped)
            text = std::string_view{start, static_cast<std::size_t>(quote - start)};
         else if (auto code = cur_.get_string().get(text); code != simdjson::SUCCESS)
            return fail_at("json: invalid escape in string", start);
         if (nonchar_ && detail::json_impl::has_noncharacter(text))
            return fail("ijson: Unicode noncharacter in string");
         return true;
      }

      bool number(std::string_view& token)
      {
         if (failed())
            return false;
         const char c = first();
         if (!(c == '-' || (c >= '0' && c <= '9')))
            return fail("json: expected a number");
         const std::string_view     text = scalar();
         detail::json_impl::reader r{text.data(), text.data(), text.data() + text.size()};
         if (!detail::json_impl::skip_number(r))
            return fail(r.err);
         if (r.p != r.end)
            return fail_at("json: malformed number", r.p);
         token = text;
         return true;
      }

      // Optional fast path: the token's characters, unchecked.
      bool scalar(std::string_view& token)
      {
         if (failed())
            return false;
         const char c = first();
         if (!(c == '-' || (c >= '0' && c <= '9')))
            return fail("json: expected a number");
         token = scalar();
         return true;
      }

      // Optional fast path: simdjson's number parser (exact, RFC 8259
      // grammar, out of range is an error).
      bool real(double& value)
      {
         if (failed())
            return false;
         const char c = first();
         if (!(c == '-' || (c >= '0' && c <= '9')))
            return fail("json: expected a number");
         if (cur_.get_double().get(value) != simdjson::SUCCESS)
            return fail("json: malformed or out-of-range number");
         return true;
      }

      bool boolean(bool& value)
      {
         if (failed())
            return false;
         if (depth_ > 0)
         {
            const std::string_view text = scalar();
            if (text == "true")
            {
               value = true;
               return true;
            }
            if (text == "false")
            {
               value = false;
               return true;
            }
         }
         return fail("json: expected true or false");
      }

      bool null()
      {
         if (failed())
            return false;
         if (depth_ > 0 && scalar() == "null")
            return true;
         return fail("json: malformed keyword");
      }

      // Walks the value: a skipped value is checked like a read one.
      bool skip()
      {
         json_kind k;
         if (!kind(k))
            return false;
         std::string_view text;
         bool             flag = false;
         switch (k)
         {
            case json_kind::object:
               if (!begin_object())
                  return false;
               while (true)
               {
                  if (!next_field(flag, text))
                     return false;
                  if (!flag)
                     return true;
                  if (!skip())
                     return false;
               }
            case json_kind::array:
               if (!begin_array())
                  return false;
               while (true)
               {
                  if (!next_element(flag))
                     return false;
                  if (!flag)
                     return true;
                  if (!skip())
                     return false;
               }
            case json_kind::string: return string(text, flag);
            case json_kind::number: return number(text);
            case json_kind::boolean: return boolean(flag);
            case json_kind::null: return null();
         }
         return fail("json: unexpected character at value start");
      }

      bool finish()
      {
         if (failed())
            return false;
         if (depth_ != 0 || !root_open_)
            return fail("json: unexpected end of input");
         if (!doc_.at_end())
            return fail("json: trailing bytes after value");
         return true;
      }
   };
   static_assert(JsonCursor<simdjson_cursor>);

}  // namespace psio
