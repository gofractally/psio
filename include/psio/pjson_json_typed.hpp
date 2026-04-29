#pragma once
//
// psio/pjson_json_typed.hpp — direct JSON text → T pipeline.
//
// Skips both the pjson byte buffer and the pjson_value tree. Each
// JSON field is looked up in T's reflected schema (constexpr name
// table) and the value is parsed straight into the corresponding T
// member. simdjson on-demand drives the walk; numbers go through
// `value::raw_json_token() → pjson_number::from_string` so exact
// decimals like "0.1" survive to T's field if T's field is a
// pjson_number-typed slot.
//
// Two API forms:
//
//   T t = psio::json_to_struct<T>(text);              // one-shot
//   T t = psio::json_to_struct<T>(parser, text);      // reused parser
//
// Reuses simdjson's per-thread parser (no per-call allocation) when
// the caller hands one in.

#include <psio/pjson.hpp>
#include <psio/pjson_typed.hpp>

#if defined(PSIO_HAVE_SIMDJSON) && PSIO_HAVE_SIMDJSON

#include <simdjson.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace psio {

   namespace pjson_detail {

      // Assign a single typed field from a simdjson on-demand value.
      // Specialize on F to keep the hot path branch-light.
      template <typename F>
      inline void assign_from_json(F& member, simdjson::ondemand::value v)
      {
         if constexpr (std::is_same_v<F, bool>)
         {
            bool b;
            if (v.get_bool().get(b) == simdjson::SUCCESS)
               member = b;
         }
         else if constexpr (std::is_integral_v<F> &&
                            std::is_signed_v<F>)
         {
            std::int64_t x;
            if (v.get_int64().get(x) == simdjson::SUCCESS)
               member = static_cast<F>(x);
         }
         else if constexpr (std::is_integral_v<F> &&
                            std::is_unsigned_v<F>)
         {
            std::uint64_t x;
            if (v.get_uint64().get(x) == simdjson::SUCCESS)
               member = static_cast<F>(x);
         }
         else if constexpr (std::is_floating_point_v<F>)
         {
            double d;
            if (v.get_double().get(d) == simdjson::SUCCESS)
               member = static_cast<F>(d);
         }
         else if constexpr (std::is_same_v<F, std::string>)
         {
            std::string_view s;
            if (v.get_string().get(s) == simdjson::SUCCESS)
               member.assign(s);
         }
         else if constexpr (std::is_same_v<F, std::string_view>)
         {
            std::string_view s;
            if (v.get_string().get(s) == simdjson::SUCCESS)
               member = s;
         }
         else if constexpr (std::is_same_v<F, pjson_number>)
         {
            // Exact-decimal: parse digit string straight into (m, s).
            std::string_view raw = v.raw_json_token();
            std::size_t       end = 0;
            while (end < raw.size() &&
                   (raw[end] == '-' || raw[end] == '+' ||
                    raw[end] == '.' || raw[end] == 'e' ||
                    raw[end] == 'E' ||
                    (raw[end] >= '0' && raw[end] <= '9')))
               ++end;
            member = pjson_number::from_string(raw.substr(0, end));
         }
         else
         {
            static_assert(sizeof(F) == 0,
                          "json_to_struct: unsupported field type");
         }
      }

   }  // namespace pjson_detail

   // Streaming JSON → T, in-place. Each reflected field of T is
   // fetched from the JSON object via simdjson's `find_field`
   // (canonical-aware: O(1) per field when JSON keys are in
   // declaration order; falls back to a forward scan otherwise).
   //
   // Missing JSON fields are left untouched in `out`. This is the
   // hot-path API — caller-owned T's existing allocations (string
   // buffers especially) get reused via assign() rather than
   // re-allocated.
   //
   // Unknown JSON fields are silently ignored.
   template <typename T>
   inline void json_to(simdjson::ondemand::parser& parser,
                       std::string_view            text,
                       T&                          out)
   {
      using R = reflect<T>;
      static_assert(R::is_reflected,
                    "json_to requires PSIO_REFLECT(T,...)");

      simdjson::padded_string padded(text);
      simdjson::ondemand::document doc;
      if (parser.iterate(padded).get(doc) != simdjson::SUCCESS)
         throw std::runtime_error("json_to: parse failed");

      simdjson::ondemand::object obj;
      if (doc.get_object().get(obj) != simdjson::SUCCESS)
         throw std::runtime_error("json_to: not an object");

      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
         (
             [&] {
                constexpr auto name = R::template member_name<Is>;
                simdjson::ondemand::value v;
                auto err = obj.find_field(name).get(v);
                if (err != simdjson::SUCCESS)
                {
                   err = obj.find_field_unordered(name).get(v);
                   if (err != simdjson::SUCCESS) return;
                }
                pjson_detail::assign_from_json(
                    out.*R::template member_pointer<Is>, v);
             }(),
             ...);
      }(std::make_index_sequence<R::member_count>{});
   }

   // Returning form. T is default-constructed; missing JSON fields
   // remain at their default-init values.
   template <typename T>
   inline T json_to(simdjson::ondemand::parser& parser,
                    std::string_view            text)
   {
      T t{};
      json_to<T>(parser, text, t);
      return t;
   }

   // One-shot forms (fresh parser per call — for hot loops, hold a
   // parser per thread and pass it in).
   template <typename T>
   inline void json_to(std::string_view text, T& out)
   {
      simdjson::ondemand::parser parser;
      json_to<T>(parser, text, out);
   }
   template <typename T>
   inline T json_to(std::string_view text)
   {
      simdjson::ondemand::parser parser;
      return json_to<T>(parser, text);
   }

   // Symmetric writer: T → JSON text. Walks T's reflected fields and
   // emits a compact JSON object, no quotes wasted on whitespace,
   // numbers via std::to_chars.
   template <typename T>
   inline std::string struct_to_json(const T& t)
   {
      using R = reflect<T>;
      static_assert(R::is_reflected,
                    "struct_to_json requires PSIO_REFLECT(T,...)");

      std::string out;
      out.reserve(64);
      out.push_back('{');
      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
         std::size_t emitted = 0;
         (
             [&] {
                constexpr auto name = R::template member_name<Is>;
                if (emitted++) out.push_back(',');
                out.push_back('"');
                out.append(name);
                out.push_back('"');
                out.push_back(':');

                using F = typename R::template member_type<Is>;
                const F& v = t.*R::template member_pointer<Is>;
                if constexpr (std::is_same_v<F, bool>)
                   out.append(v ? "true" : "false");
                else if constexpr (std::is_integral_v<F>)
                {
                   char buf[24];
                   auto r = std::to_chars(buf, buf + sizeof(buf), v);
                   out.append(buf, r.ptr);
                }
                else if constexpr (std::is_floating_point_v<F>)
                {
                   char buf[32];
                   auto r = std::to_chars(buf, buf + sizeof(buf), v);
                   out.append(buf, r.ptr);
                }
                else if constexpr (std::is_same_v<F, std::string> ||
                                   std::is_same_v<F, std::string_view>)
                {
                   out.push_back('"');
                   for (char c : std::string_view(v))
                   {
                      // Minimal escaping; full JSON escapes deferred.
                      if (c == '"' || c == '\\') out.push_back('\\');
                      out.push_back(c);
                   }
                   out.push_back('"');
                }
                else
                {
                   static_assert(sizeof(F) == 0,
                                 "struct_to_json: unsupported type");
                }
             }(),
             ...);
      }(std::make_index_sequence<R::member_count>{});
      out.push_back('}');
      return out;
   }

}  // namespace psio

#endif  // PSIO_HAVE_SIMDJSON
