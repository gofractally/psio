#pragma once
//
// psio/view_to_json.hpp — generic View → JSON text writer.
//
// Templated on the View type, which is required to satisfy the
// schemaless accessor concept:
//
//   View::kind type() const           — returns one of {null, boolean,
//                                          integer, decimal, floating,
//                                          string, bytes, array, object,
//                                          invalid}.
//   bool   is_null/is_bool/is_object/...() const
//   bool   as_bool() const
//   int64  as_int64() const           — for integer/decimal/floating that
//                                          fit
//   double as_double() const
//   string_view as_string() const
//   span<byte>  as_bytes() const
//   for_each_field(fn(k, V child))    — objects
//   for_each_element(fn(V child))     — arrays
//
// `pjson_view` already satisfies it. A future `dyn_view<json_format>`
// or `dyn_view<ssz_format>` would too — the writer below works against
// any of them without modification.

#include <psio/pjson.hpp>

#include <charconv>
#include <cstring>
#include <string>
#include <string_view>

namespace psio {

   namespace view_to_json_detail {

      // Strings in pjson are stored in JSON-escape form already
      // (matching how keys are stored). Emitting JSON is just
      // wrapping the stored bytes in quotes — no per-character
      // escape detection needed.
      //
      // For paths that store unescaped strings (e.g., values built
      // from a pjson_value tree where the user supplied raw bytes),
      // see emit_string_with_escape below.
      inline void emit_string(std::string& out, std::string_view s)
      {
         out.push_back('"');
         out.append(s);
         out.push_back('"');
      }

      // Slow path for cases where the source bytes are not yet in
      // JSON-escape form (e.g. pjson_value variant constructed from
      // arbitrary user bytes). Currently unused in the hot pipeline;
      // kept for callers building docs from user data.
      inline void emit_string_with_escape(std::string&     out,
                                          std::string_view s)
      {
         out.push_back('"');
         for (char c : s)
         {
            switch (c)
            {
               case '"':  out.append("\\\"", 2); break;
               case '\\': out.append("\\\\", 2); break;
               case '\b': out.append("\\b", 2); break;
               case '\f': out.append("\\f", 2); break;
               case '\n': out.append("\\n", 2); break;
               case '\r': out.append("\\r", 2); break;
               case '\t': out.append("\\t", 2); break;
               default:
                  if (static_cast<unsigned char>(c) < 0x20)
                  {
                     char buf[8];
                     auto n = std::snprintf(buf, sizeof(buf),
                                            "\\u%04x", c);
                     out.append(buf, n);
                  }
                  else
                  {
                     out.push_back(c);
                  }
                  break;
            }
         }
         out.push_back('"');
      }

      inline void emit_int(std::string& out, std::int64_t v)
      {
         char buf[24];
         auto r = std::to_chars(buf, buf + sizeof(buf), v);
         out.append(buf, r.ptr);
      }

      inline void emit_double(std::string& out, double d)
      {
         char buf[32];
         auto r = std::to_chars(buf, buf + sizeof(buf), d);
         out.append(buf, r.ptr);
      }

      // Emit a pjson_number as canonical decimal text.
      inline void emit_number(std::string& out, const pjson_number& n)
      {
         if (n.scale == 0)
         {
            // Integer; print mantissa as i128.
            __int128 m = n.mantissa;
            if (m < 0) { out.push_back('-'); m = -m; }
            if (m == 0) { out.push_back('0'); return; }
            char tmp[40];
            int  i = sizeof(tmp);
            while (m > 0)
            {
               tmp[--i] = static_cast<char>('0' +
                                            static_cast<int>(m % 10));
               m /= 10;
            }
            out.append(tmp + i, sizeof(tmp) - i);
            return;
         }
         // Decimal: print as <int_part>.<frac_part> when scale<0,
         // <mantissa>e<scale> otherwise. Keeps it round-trippable.
         emit_double(out, n.to_double());
      }

   }  // namespace view_to_json_detail

   // Generic walker. View can be any type satisfying the schemaless
   // accessor concept (pjson_view today; dyn_view<Fmt> tomorrow).
   template <typename View>
   inline void write_json(std::string& out, View v)
   {
      using K = typename View::kind;
      switch (v.type())
      {
         case K::null:     out.append("null", 4); return;
         case K::boolean:
            out.append(v.as_bool() ? "true" : "false");
            return;
         case K::integer:
            view_to_json_detail::emit_int(out, v.as_int64());
            return;
         case K::decimal:
            view_to_json_detail::emit_number(out, v.as_number());
            return;
         case K::floating:
            view_to_json_detail::emit_double(out, v.as_double());
            return;
         case K::string:
         {
            // Dispatch on the string flag: escape_form → verbatim,
            // raw_text → run JSON escape, binary → base64 (TODO; stub
            // as verbatim for now).
            std::string_view s = v.as_string();
            std::uint8_t     f = v.string_flag();
            if (f == pjson_detail::string_flag_escape_form)
               view_to_json_detail::emit_string(out, s);
            else  // raw_text — must escape per char
               view_to_json_detail::emit_string_with_escape(out, s);
            return;
         }
         case K::bytes:
            // TODO: proper base64. Stub: emit as quoted bytes.
            view_to_json_detail::emit_string(
                out, std::string_view{
                         reinterpret_cast<const char*>(v.as_bytes().data()),
                         v.as_bytes().size()});
            return;
         case K::array:
         {
            out.push_back('[');
            // Typed homogeneous arrays expose typed_*_at accessors
            // instead of for_each_element (no per-element view to
            // hand out). Dispatch on the element kind: floats and
            // unsigned 64-bit ints emit via the float/uint paths;
            // everything else fits int64 cleanly.
            if (v.is_typed_array())
            {
               std::uint8_t code = v.typed_array_elem_code();
               std::size_t  N    = v.count();
               for (std::size_t i = 0; i < N; ++i)
               {
                  if (i) out.push_back(',');
                  if (psio::pjson_detail::typed_array_is_float(code))
                     view_to_json_detail::emit_double(
                         out, v.typed_double_at(i));
                  else if (code == psio::pjson_detail::tac_u64)
                  {
                     // u64 may exceed i64 range; render as decimal
                     // text via to_chars on uint64.
                     char buf[24];
                     auto r = std::to_chars(
                         buf, buf + sizeof(buf), v.typed_uint64_at(i));
                     out.append(buf, r.ptr);
                  }
                  else
                     view_to_json_detail::emit_int(
                         out, v.typed_int64_at(i));
               }
               out.push_back(']');
               return;
            }
            bool first = true;
            v.for_each_element([&](View child) {
               if (!first) out.push_back(',');
               first = false;
               write_json(out, child);
            });
            out.push_back(']');
            return;
         }
         case K::object:
         {
            out.push_back('{');
            bool first = true;
            v.for_each_field([&](std::string_view k, View child) {
               if (!first) out.push_back(',');
               first = false;
               view_to_json_detail::emit_string(out, k);
               out.push_back(':');
               write_json(out, child);
            });
            out.push_back('}');
            return;
         }
         case K::invalid: throw std::runtime_error(
             "write_json: invalid value");
      }
   }

   // Convenience: returning form.
   template <typename View>
   inline std::string view_to_json(View v)
   {
      std::string out;
      out.reserve(64);
      write_json(out, v);
      return out;
   }

}  // namespace psio
