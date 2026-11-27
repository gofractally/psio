#pragma once
//
// psio/pjson_json.hpp — JSON text → pjson bytes via simdjson on-demand.
//
// True streaming pipeline:
//   * No DOM tape — on-demand walks the structural bitmap directly.
//   * No payload buffer — bytes flow straight into the output buffer
//     as on-demand emits them.
//   * No double round-trip on numbers — `value::raw_json_token()` gives
//     the original digit string; `pjson_number::from_string` parses it
//     directly to (mantissa, scale).
//
// The output is a single `std::vector<std::uint8_t>` that grows as the
// encoder walks the JSON. Reserved up front to a multiple of the JSON
// text size (pjson is typically < text size for non-trivial docs);
// realloc is rare in practice.
//
// Per-container scratch is a stack-resident array of u32 slot records
// kept while iterating; spills to heap above its small-buffer
// threshold for huge containers.

#include <psio/pjson.hpp>

#if defined(PSIO_HAVE_SIMDJSON) && PSIO_HAVE_SIMDJSON

#include <simdjson.h>

#include <charconv>
#include <cmath>
#include <cstring>

#include <stdexcept>
#include <string_view>

namespace psio {

   // Internal output buffer type. Skips vector's per-byte zero-init
   // on resize/insert. We always overwrite every byte we allocate,
   // so the default-construction zero-pass is pure waste.
   using pjson_out_buffer =
       std::vector<std::uint8_t,
                   pjson_detail::uninit_alloc<std::uint8_t>>;

   namespace pjson_detail {

      inline std::size_t reserve_bytes(pjson_out_buffer& out,
                                       std::size_t       n)
      {
         std::size_t p = out.size();
         out.resize(p + n);
         return p;
      }

      // Forward decl.
      inline void od_encode_value(pjson_out_buffer& out,
                                  simdjson::ondemand::value  v);

      // Per-field record kept on the stack while iterating a
      // container; emitted to the slot/hash table at container close.
      struct slot_rec
      {
         std::uint32_t voff;     // offset within value_data
         std::uint8_t  hash;     // objects only (zero for arrays)
         std::uint8_t  key_size; // objects only (zero for arrays)
      };

      // Single-pass container encode: stream child bytes directly into
      // `out`, recording per-field slot data in a stack-resident SBO.
      // At container close, memmove the value_data right by the header
      // size and write the header in place. One bitmap walk per
      // container — no count + reset double pass.

      // Tail-indexed array encoder.
      //
      // Layout: [tag][value_data][slot[N]][count u16]
      //
      // True forward streaming write: tag, then children, then index
      // appended at the tail. No memmove, no resize-then-shift.
      inline void od_encode_array(pjson_out_buffer&  out,
                                  simdjson::ondemand::array&  a)
      {
         constexpr std::size_t SBO = 32;
         slot_rec              sbo[SBO];
         std::vector<slot_rec> heap;
         slot_rec*             records = sbo;
         std::size_t           cap     = SBO;
         std::size_t           N       = 0;

         out.push_back(static_cast<std::uint8_t>(t_array << 4));
         std::size_t width_byte_pos = out.size();
         out.push_back(0);  // placeholder; backfilled at end
         std::size_t value_data_start = out.size();

         for (auto el : a)
         {
            simdjson::ondemand::value v;
            if (el.get(v) != simdjson::SUCCESS)
               throw std::runtime_error("pjson_json: array element");
            if (N == cap)
            {
               heap.assign(records, records + N);
               heap.reserve(cap * 2);
               heap.resize(N);
               records = heap.data();
               cap     = heap.capacity();
            }
            records[N].voff =
                static_cast<std::uint32_t>(out.size() - value_data_start);
            records[N].hash     = 0;
            records[N].key_size = 0;
            ++N;
            od_encode_value(out, v);
         }

         // Pick slot_w from value_data size, backfill width byte, then
         // append slot[N] (adaptive width) + count u16.
         std::uint32_t vd_size =
            static_cast<std::uint32_t>(out.size() - value_data_start);
         std::uint8_t slot_w_code = width_code_for(vd_size);
         std::size_t  slot_w      = width_bytes(slot_w_code);
         out[width_byte_pos] = slot_w_code;
         std::size_t slot_pos = reserve_bytes(out, slot_w * N);
         for (std::size_t i = 0; i < N; ++i)
            write_width(out.data() + slot_pos + i * slot_w,
                        slot_w_code, records[i].voff);
         std::size_t count_pos = reserve_bytes(out, 2);
         out[count_pos]     = static_cast<std::uint8_t>(N & 0xFF);
         out[count_pos + 1] = static_cast<std::uint8_t>((N >> 8) & 0xFF);
      }

      // Tail-indexed object encoder.
      //
      // Layout: [tag][value_data][hash[N]][slot[N]][count u16]
      inline void od_encode_object(pjson_out_buffer&   out,
                                   simdjson::ondemand::object&  o)
      {
         constexpr std::size_t SBO = 32;
         slot_rec              sbo[SBO];
         std::vector<slot_rec> heap;
         slot_rec*             records = sbo;
         std::size_t           cap     = SBO;
         std::size_t           N       = 0;

         out.push_back(static_cast<std::uint8_t>(t_object << 4));
         std::size_t width_byte_pos = out.size();
         out.push_back(0);  // placeholder; backfilled at end
         std::size_t value_data_start = out.size();

         for (auto field : o)
         {
            // escaped_key() returns the key bytes between the quotes
            // as a string_view directly into the JSON buffer — zero
            // copy, zero unescape pass.
            std::string_view k = field.escaped_key();
            // A member name is stored as raw text (there is no flag to say
            // otherwise): the rare name that carries an escape is unescaped
            // here, in the lazy transcode too.
            std::vector<std::uint8_t> key_text;
            if (std::memchr(k.data(), '\\', k.size()) != nullptr)
            {
               if (!append_unescaped(key_text, reinterpret_cast<const std::uint8_t*>(k.data()), k.size()))
                  throw std::runtime_error("pjson_json: a member name is not text (a lone surrogate)");
               k = std::string_view{reinterpret_cast<const char*>(key_text.data()), key_text.size()};
            }
            simdjson::ondemand::value v = field.value();
            if (N == cap)
            {
               heap.assign(records, records + N);
               heap.reserve(cap * 2);
               heap.resize(N);
               records = heap.data();
               cap     = heap.capacity();
            }
            records[N].voff =
                static_cast<std::uint32_t>(out.size() - value_data_start);
            records[N].hash = key_hash8(k);
            records[N].key_size =
                k.size() < 0xFFu
                    ? static_cast<std::uint8_t>(k.size())
                    : static_cast<std::uint8_t>(0xFFu);
            ++N;

            // Append key bytes (with long-key escape if needed) then
            // the value's encoded bytes.
            if (k.size() >= 0xFFu)
            {
               std::size_t excess     = k.size() - 0xFFu;
               std::size_t bc         = varuint62_byte_count(excess);
               std::size_t excess_pos = reserve_bytes(out, bc);
               write_varuint62(out.data(), excess_pos, excess);
            }
            std::size_t key_pos = reserve_bytes(out, k.size());
            std::memcpy(out.data() + key_pos, k.data(), k.size());
            od_encode_value(out, v);
         }

         // Pick slot_w from value_data size, backfill width byte, then
         // append hash[N] + slot[N] (adaptive width) + count u16.
         std::uint32_t vd_size =
            static_cast<std::uint32_t>(out.size() - value_data_start);
         std::uint8_t slot_w_code = width_code_for(vd_size);
         std::size_t  slot_w      = width_bytes(slot_w_code);
         std::size_t  slot_stride = slot_w + 1;  // offset + key_size byte
         out[width_byte_pos] = slot_w_code;

         std::size_t hash_pos = reserve_bytes(out, N);
         for (std::size_t i = 0; i < N; ++i)
            out[hash_pos + i] = records[i].hash;
         std::size_t slot_pos = reserve_bytes(out, slot_stride * N);
         for (std::size_t i = 0; i < N; ++i)
         {
            std::uint8_t* slot = out.data() + slot_pos + i * slot_stride;
            write_width(slot, slot_w_code, records[i].voff);
            slot[slot_w] = records[i].key_size;
         }
         std::size_t count_pos = reserve_bytes(out, 2);
         out[count_pos]     = static_cast<std::uint8_t>(N & 0xFF);
         out[count_pos + 1] = static_cast<std::uint8_t>((N >> 8) & 0xFF);
      }

      // A JSON number token → the pjson form that holds its VALUE exactly
      // (docs/pjson-spec.md §7.2, §15.2; gofractally/psio#17):
      //   * minus zero: the float that has one, at the smallest width;
      //   * an integer of up to 128 bits of magnitude: the integer forms;
      //   * anything else: a decimal, or a binary64 when that is shorter
      //     AND holds the value exactly (its shortest round-trip decimal is
      //     the number);
      //   * a number none of these can hold exactly is an error
      //     (parse_json_number, and the mantissa check below): never zero,
      //     never a rounded double, never infinity.
      inline void od_encode_number(pjson_out_buffer& out, std::string_view token)
      {
         json_number num = parse_json_number(token);
         // A mantissa of more than 127 bits is only an integer's: if it has
         // an exponent, its trailing zeros belong there.
         while ((num.mag >> 127) && num.scale != 0 && num.mag % 10 == 0)
         {
            num.mag /= 10;
            ++num.scale;
         }
         if (num.mag == 0 && num.neg)
         {
            // -0: binary16 negative zero (§15.2.1: the smallest width).
            const std::size_t tp = reserve_bytes(out, 3);
            out.data()[tp]     = static_cast<std::uint8_t>((t_ieee_float << 4) | ieee_width_f16);
            out.data()[tp + 1] = 0x00;
            out.data()[tp + 2] = 0x80;
            return;
         }
         if (num.mag >> 127)
         {
            // 2^127 .. 2^128 - 1: the magnitude is 16 raw bytes, with no
            // room for a sign bit; only an integer has that much.
            if (num.scale != 0)
               throw std::runtime_error(
                   "pjson: a number with more than 127 bits of significant digits and an exponent has no pjson form");
            const std::size_t tp = reserve_bytes(out, 17);
            out.data()[tp] = static_cast<std::uint8_t>(((num.neg ? t_negint : t_uint) << 4) | 15u);
            unsigned __int128 mag = num.mag;
            std::memcpy(out.data() + tp + 1, &mag, 16);
            return;
         }
         const __int128     mant = static_cast<__int128>(num.mag);
         const pjson_number n{num.neg ? -mant : mant, num.scale};
         // int tag for scale = 0, decimal for scale != 0 unless the
         // binary64 form is shorter and exact.
         const std::size_t dec = number_size(n);
         if (n.scale != 0 && dec >= 9)
         {
            double d = 0;
            const auto r = std::from_chars(token.data(), token.data() + token.size(), d);
            if (r.ec == std::errc{} && std::isfinite(d))
            {
               char        text[40];
               const auto  w = std::to_chars(text, text + sizeof(text), d);
               json_number back;
               bool        parsed = true;
               try { back = parse_json_number(std::string_view{text, static_cast<std::size_t>(w.ptr - text)}); }
               catch (const std::exception&) { parsed = false; }
               if (parsed && back.neg == num.neg && back.mag == num.mag && back.scale == num.scale)
               {
                  const std::size_t tp = reserve_bytes(out, 9);
                  encode_double_raw_at(out.data(), tp, d);
                  return;
               }
            }
         }
         const std::size_t tp = reserve_bytes(out, dec);
         encode_number_at(out.data(), tp, n);
      }

      // The bytes between the quotes of a JSON string token, kept in
      // their JSON-escape form.
      inline void od_encode_string(pjson_out_buffer& out, std::string_view raw)
      {
         std::size_t end = raw.size();
         while (end > 0 &&
                (raw[end - 1] == ' ' || raw[end - 1] == '\t' ||
                 raw[end - 1] == '\n' || raw[end - 1] == '\r'))
            --end;
         if (end < 2 || raw[0] != '"' || raw[end - 1] != '"')
            throw std::runtime_error("pjson_json: malformed string token");
         std::string_view content = raw.substr(1, end - 2);
         std::size_t tp = reserve_bytes(out, 1u + content.size());
         out.data()[tp] = static_cast<std::uint8_t>(
             (t_string << 4) | string_flag_escape_form);
         if (!content.empty())
            std::memcpy(out.data() + tp + 1, content.data(), content.size());
      }

      inline void od_encode_value(pjson_out_buffer& out,
                                  simdjson::ondemand::value  v)
      {
         simdjson::ondemand::json_type t;
         if (v.type().get(t) != simdjson::SUCCESS)
            throw std::runtime_error("pjson_json: value type");
         using JT = simdjson::ondemand::json_type;
         switch (t)
         {
            case JT::null:
               out.push_back(static_cast<std::uint8_t>(t_null << 4));
               return;
            case JT::boolean:
            {
               bool b;
               if (v.get_bool().get(b) != simdjson::SUCCESS)
                  throw std::runtime_error("pjson_json: bad bool");
               out.push_back(static_cast<std::uint8_t>(
                   (t_bool << 4) | (b ? 1u : 0u)));
               return;
            }
            case JT::number:
            {
               // Use raw text to avoid the text → double → digit-string
               // round-trip for fractional numbers. Integer literals
               // also flow through the same path; from_string handles
               // both.
               std::string_view raw = v.raw_json_token();
               // raw_json_token may include trailing whitespace/comma;
               // from_string trims/rejects. Trim ourselves to the
               // number's actual extent first.
               std::size_t end = 0;
               while (end < raw.size() &&
                      (raw[end] == '-' || raw[end] == '+' ||
                       raw[end] == '.' || raw[end] == 'e' ||
                       raw[end] == 'E' ||
                       (raw[end] >= '0' && raw[end] <= '9')))
                  ++end;
               od_encode_number(out, raw.substr(0, end));
               return;
            }
            case JT::string:
            {
               // Store the JSON-escape form of the string: the bytes
               // between the surrounding quotes, including any escape
               // sequences (\", \\, \uXXXX, etc.) as literal text.
               // This is symmetric with how we store keys (escaped_key)
               // and lets view_to_json emit JSON with no per-char
               // escape pass — just memcpy with surrounding quotes.
               //
               // Caveat: callers reading via as_string() get the
               // escape-form bytes. For escape-free strings (the
               // overwhelming common case in API JSON) these match the
               // unescaped form bit-for-bit.
               std::string_view raw = v.raw_json_token();
               // raw is [opening_quote, optional_trailing_ws). Trim
               // trailing whitespace; the byte immediately before is
               // the closing quote.
               std::size_t end = raw.size();
               while (end > 0 &&
                      (raw[end - 1] == ' ' || raw[end - 1] == '\t' ||
                       raw[end - 1] == '\n' || raw[end - 1] == '\r'))
                  --end;
               if (end < 2 || raw[0] != '"' || raw[end - 1] != '"')
                  throw std::runtime_error(
                      "pjson_json: malformed string token");
               std::string_view content = raw.substr(1, end - 2);
               std::size_t tp = reserve_bytes(out, 1u + content.size());
               // simdjson handed us escape-form bytes — set flag so
               // JSON emit later can skip the per-char escape pass.
               out.data()[tp] = static_cast<std::uint8_t>(
                   (t_string << 4) | string_flag_escape_form);
               if (!content.empty())
                  std::memcpy(out.data() + tp + 1, content.data(),
                              content.size());
               return;
            }
            case JT::array:
            {
               simdjson::ondemand::array a;
               if (v.get_array().get(a) != simdjson::SUCCESS)
                  throw std::runtime_error("pjson_json: bad array");
               od_encode_array(out, a);
               return;
            }
            case JT::object:
            {
               simdjson::ondemand::object o;
               if (v.get_object().get(o) != simdjson::SUCCESS)
                  throw std::runtime_error("pjson_json: bad object");
               od_encode_object(out, o);
               return;
            }
            default:
               throw std::runtime_error("pjson_json: unknown type");
         }
         throw std::runtime_error("pjson_json: unknown type");
      }
      // Compiler asks for an exhaustive switch including 'unknown'; the
      // throw above covers the path but the warning still fires without
      // an explicit default.

      // Top-level: same logic as od_encode_value but operates on
      // ondemand::document which is a slightly different type.
      inline void od_encode_document(pjson_out_buffer&             out,
                                     simdjson::ondemand::document& doc)
      {
         simdjson::ondemand::json_type t;
         if (doc.type().get(t) != simdjson::SUCCESS)
            throw std::runtime_error("pjson_json: doc type");
         using JT = simdjson::ondemand::json_type;
         switch (t)
         {
            case JT::array:
            {
               simdjson::ondemand::array a;
               if (doc.get_array().get(a) != simdjson::SUCCESS)
                  throw std::runtime_error("pjson_json: doc.array");
               od_encode_array(out, a);
               return;
            }
            case JT::object:
            {
               simdjson::ondemand::object o;
               if (doc.get_object().get(o) != simdjson::SUCCESS)
                  throw std::runtime_error("pjson_json: doc.object");
               od_encode_object(out, o);
               return;
            }
            // A document may be a single scalar (§6: a pjson document is
            // one value, any value). simdjson gives a root scalar through
            // the document, not through a value.
            case JT::null:
            {
               bool is_null = false;
               if (doc.is_null().get(is_null) != simdjson::SUCCESS || !is_null)
                  throw std::runtime_error("pjson_json: bad null");
               out.push_back(static_cast<std::uint8_t>(t_null << 4));
               return;
            }
            case JT::boolean:
            {
               bool b;
               if (doc.get_bool().get(b) != simdjson::SUCCESS)
                  throw std::runtime_error("pjson_json: bad bool");
               out.push_back(static_cast<std::uint8_t>((t_bool << 4) | (b ? 1u : 0u)));
               return;
            }
            case JT::number:
            {
               std::string_view raw;
               if (doc.raw_json_token().get(raw) != simdjson::SUCCESS)
                  throw std::runtime_error("pjson_json: bad number");
               std::size_t end = 0;
               while (end < raw.size() &&
                      (raw[end] == '-' || raw[end] == '+' || raw[end] == '.' || raw[end] == 'e' ||
                       raw[end] == 'E' || (raw[end] >= '0' && raw[end] <= '9')))
                  ++end;
               // (The whole document is this token: anything after it is
               // not JSON. simdjson's own number check runs in get_number.)
               simdjson::ondemand::number ignored;
               const auto err = doc.get_number().get(ignored);
               if (err != simdjson::SUCCESS && err != simdjson::BIGINT_ERROR &&
                   err != simdjson::NUMBER_ERROR)
                  throw std::runtime_error("pjson_json: bad number");
               od_encode_number(out, raw.substr(0, end));
               return;
            }
            case JT::string:
            {
               std::string_view raw;
               if (doc.raw_json_token().get(raw) != simdjson::SUCCESS)
                  throw std::runtime_error("pjson_json: bad string");
               std::string_view checked;
               if (doc.get_string().get(checked) != simdjson::SUCCESS)
                  throw std::runtime_error("pjson_json: bad string");
               od_encode_string(out, raw);
               return;
            }
            default:
               throw std::runtime_error("pjson_json: unknown type");
         }
      }

   }  // namespace pjson_detail

   // JSON text that is not a type -> pjson. Two transcodes, and the caller
   // says which:
   //
   //   from_json       the canonical pjson of the document (psio::pjson):
   //                   one encoding per value, row arrays and typed arrays
   //                   where the values allow. It looks at every array.
   //   from_json_lazy  the quick transcode (psio::pjson_lazy): what it
   //                   parses is copied as it comes — strings in escape
   //                   form, arrays generic, numbers as spelled where that
   //                   is exact. Well-formed, NOT canonical; the one writer
   //                   that is not. `psio::pjson::canonicalize` turns its
   //                   output into what from_json gives.
   //
   // (JSON text of a known type is not either of these: it is read as the
   // type and encoded by the typed encoder.)
   struct pjson_json
   {
      static std::vector<std::uint8_t> from_json(std::string_view text)
      {
         simdjson::ondemand::parser parser;
         return from_json(parser, text);
      }
      static std::vector<std::uint8_t>
      from_json(simdjson::ondemand::parser& parser, std::string_view text)
      {
         const auto lazy = from_json_lazy(parser, text);
         std::vector<std::uint8_t> out;
         out.reserve(lazy.size());
         pjson_detail::append_canonical(out, lazy);
         return out;
      }
      static std::vector<std::uint8_t> from_json_lazy(std::string_view text)
      {
         simdjson::ondemand::parser parser;
         return from_json_lazy(parser, text);
      }
      static std::vector<std::uint8_t>
      from_json_lazy(simdjson::ondemand::parser& parser, std::string_view text)
      {
         simdjson::padded_string padded(text);
         simdjson::ondemand::document doc;
         if (parser.iterate(padded).get(doc) != simdjson::SUCCESS)
            throw std::runtime_error("pjson_json: parse failed");
         // pjson is ≤ JSON text size for any realistic API payload.
         // Reserve text.size() rounded up to the next 128-byte cache
         // line — covers the micro-array / tiny-object edges and
         // aligns the allocation to a cache line boundary. One
         // allocation, no growth in practice.
         //
         // Buffer uses uninit_alloc so resize/memmove paths skip the
         // standard zero-init pass — every byte we allocate gets
         // overwritten by the encoder anyway.
         pjson_out_buffer scratch;
         scratch.reserve((text.size() + 127) & ~std::size_t{127});
         pjson_detail::od_encode_document(scratch, doc);

         // Convert to std::vector<uint8_t> for the public API. One
         // allocation + one memcpy; cheaper than the per-byte zero-
         // init we'd pay using a default-allocator vector throughout.
         std::vector<std::uint8_t> out;
         out.reserve(scratch.size());
         out.insert(out.end(), scratch.begin(), scratch.end());
         return out;
      }
   };

}  // namespace psio

#endif  // PSIO_HAVE_SIMDJSON
