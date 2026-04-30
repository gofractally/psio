#pragma once
//
// psio/dynamic_json.hpp — schema-driven JSON encoder/decoder over
// dynamic_value.
//
// Unlike the compile-time JSON codec (json.hpp), these functions are
// driven by a runtime `psio::schema`, so they can emit/parse payloads
// whose types are only known at runtime. This is the foundation for the
// schema-is-contract DX:
//
//   - tooling that pretty-prints opaque bytes given just a schema
//   - transcode(JSON → dynamic → any other format) once each format has a
//     dynamic codec
//   - validating arbitrary JSON against a runtime schema
//
// Phase 14b ships JSON on both ends; other formats plug in the same way
// in follow-up work.

#include <psio/dynamic_value.hpp>
#include <psio/json.hpp>
#include <psio/schema.hpp>

#include <charconv>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace psio {

   namespace detail::dyn_json_impl {

      inline void write_escaped(std::string_view sv, std::string& s)
      {
         s.push_back('"');
         for (char c : sv)
         {
            switch (c)
            {
               case '"':  s += R"(\")"; break;
               case '\\': s += R"(\\)"; break;
               case '\n': s += R"(\n)"; break;
               case '\r': s += R"(\r)"; break;
               case '\t': s += R"(\t)"; break;
               default:   s.push_back(c);
            }
         }
         s.push_back('"');
      }

      template <typename I>
      void write_int(I v, std::string& s)
      {
         char buf[32];
         auto r = std::to_chars(buf, buf + sizeof(buf), v);
         s.append(buf, r.ptr);
      }

      inline void encode_dv(const schema& sc, const dynamic_value& dv,
                            std::string& s);

      inline void encode_dv_primitive(primitive_kind k,
                                      const dynamic_value& dv,
                                      std::string& s)
      {
         switch (k)
         {
            case primitive_kind::Bool:
               s += dv.as<bool>() ? "true" : "false"; break;
            case primitive_kind::Int8:   write_int(dv.as<std::int8_t>(), s); break;
            case primitive_kind::Uint8:  write_int(dv.as<std::uint8_t>(), s); break;
            case primitive_kind::Int16:  write_int(dv.as<std::int16_t>(), s); break;
            case primitive_kind::Uint16: write_int(dv.as<std::uint16_t>(), s); break;
            case primitive_kind::Int32:  write_int(dv.as<std::int32_t>(), s); break;
            case primitive_kind::Uint32: write_int(dv.as<std::uint32_t>(), s); break;
            case primitive_kind::Int64:  write_int(dv.as<std::int64_t>(), s); break;
            case primitive_kind::Uint64: write_int(dv.as<std::uint64_t>(), s); break;
            case primitive_kind::Float32:
            {
               char buf[64];
               auto r =
                  std::to_chars(buf, buf + sizeof(buf), dv.as<float>());
               s.append(buf, r.ptr);
               break;
            }
            case primitive_kind::Float64:
            {
               char buf[64];
               auto r =
                  std::to_chars(buf, buf + sizeof(buf), dv.as<double>());
               s.append(buf, r.ptr);
               break;
            }
            case primitive_kind::String:
               write_escaped(dv.as<std::string>(), s);
               break;
            case primitive_kind::Bytes:
               // Opaque bytes-in-json: emit as a string (bytes are
               // held as std::string inside dynamic_value).
               write_escaped(dv.as<std::string>(), s);
               break;
         }
      }

      inline void encode_dv(const schema& sc, const dynamic_value& dv,
                            std::string& s)
      {
         if (sc.is_primitive())
         {
            encode_dv_primitive(sc.as_primitive(), dv, s);
         }
         else if (sc.is_sequence())
         {
            const auto& seq     = dv.as<dynamic_sequence>();
            const auto& element = *sc.as_sequence().element;
            s.push_back('[');
            bool first = true;
            for (const auto& el : seq.elements)
            {
               if (!first)
                  s.push_back(',');
               first = false;
               encode_dv(element, el, s);
            }
            s.push_back(']');
         }
         else if (sc.is_optional())
         {
            const auto& od  = sc.as_optional();
            const auto& opt = dv.as<dynamic_optional>();
            if (!opt.value)
               s += "null";
            else
               encode_dv(*od.value_type, *opt.value, s);
         }
         else if (sc.is_projected())
         {
            // The dynamic_value already holds the adapter's
            // payload (string for text, bytes-as-string for binary).
            // JSON wraps it in quotes + escaping — the same work it
            // does for a plain string.
            write_escaped(dv.as<std::string>(), s);
         }
         else if (sc.is_record())
         {
            const auto& rec = dv.as<dynamic_record>();
            s.push_back('{');
            bool first = true;
            for (const auto& fd : sc.as_record().fields)
            {
               if (!first)
                  s.push_back(',');
               first = false;
               write_escaped(fd.name, s);
               s.push_back(':');

               // Look up the matching dynamic field by name.
               const dynamic_value* found = nullptr;
               for (const auto& kv : rec.fields)
                  if (kv.first == fd.name)
                  {
                     found = &kv.second;
                     break;
                  }
               if (found)
                  encode_dv(*fd.type, *found, s);
               else
                  s += "null";  // missing field → null
            }
            s.push_back('}');
         }
      }

      // ── Decoder ────────────────────────────────────────────────────────
      struct parser
      {
         const char* p;
         const char* end;

         void skip_ws()
         {
            while (p < end &&
                   (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
               ++p;
         }

         bool consume(char c)
         {
            skip_ws();
            if (p < end && *p == c)
            {
               ++p;
               return true;
            }
            return false;
         }

         bool consume_null()
         {
            skip_ws();
            if (end - p >= 4 && std::memcmp(p, "null", 4) == 0)
            {
               p += 4;
               return true;
            }
            return false;
         }
      };

      inline std::string parse_string(parser& pr)
      {
         pr.skip_ws();
         if (pr.p >= pr.end || *pr.p != '"')
            return {};
         ++pr.p;
         std::string out;
         while (pr.p < pr.end && *pr.p != '"')
         {
            if (*pr.p == '\\' && pr.p + 1 < pr.end)
            {
               char c = pr.p[1];
               switch (c)
               {
                  case '"':  out.push_back('"'); break;
                  case '\\': out.push_back('\\'); break;
                  case 'n':  out.push_back('\n'); break;
                  case 'r':  out.push_back('\r'); break;
                  case 't':  out.push_back('\t'); break;
                  default:   out.push_back(c);
               }
               pr.p += 2;
            }
            else
            {
               out.push_back(*pr.p++);
            }
         }
         if (pr.p < pr.end && *pr.p == '"')
            ++pr.p;
         return out;
      }

      inline dynamic_value decode_dv(const schema& sc, parser& pr);

      inline dynamic_value decode_dv_primitive(primitive_kind k,
                                               parser& pr)
      {
         pr.skip_ws();
         switch (k)
         {
            case primitive_kind::Bool:
            {
               if (pr.p < pr.end && *pr.p == 't')
               {
                  pr.p += 4;
                  return dynamic_value{true};
               }
               pr.p += 5;
               return dynamic_value{false};
            }
            case primitive_kind::String:
            case primitive_kind::Bytes:
               return dynamic_value{parse_string(pr)};
            default:
            {
               // Numeric — read the literal and convert by kind.
               std::string num;
               while (pr.p < pr.end &&
                      (*pr.p == '-' || *pr.p == '+' || *pr.p == '.' ||
                       *pr.p == 'e' || *pr.p == 'E' ||
                       (*pr.p >= '0' && *pr.p <= '9')))
                  num.push_back(*pr.p++);
               auto to_i64 = [&]() {
                  std::int64_t v{};
                  std::from_chars(num.data(), num.data() + num.size(), v);
                  return v;
               };
               auto to_u64 = [&]() {
                  std::uint64_t v{};
                  std::from_chars(num.data(), num.data() + num.size(), v);
                  return v;
               };
               auto to_d = [&]() {
                  double v{};
                  std::from_chars(num.data(), num.data() + num.size(), v);
                  return v;
               };
               switch (k)
               {
                  case primitive_kind::Int8:   return dynamic_value{static_cast<std::int8_t>(to_i64())};
                  case primitive_kind::Uint8:  return dynamic_value{static_cast<std::uint8_t>(to_u64())};
                  case primitive_kind::Int16:  return dynamic_value{static_cast<std::int16_t>(to_i64())};
                  case primitive_kind::Uint16: return dynamic_value{static_cast<std::uint16_t>(to_u64())};
                  case primitive_kind::Int32:  return dynamic_value{static_cast<std::int32_t>(to_i64())};
                  case primitive_kind::Uint32: return dynamic_value{static_cast<std::uint32_t>(to_u64())};
                  case primitive_kind::Int64:  return dynamic_value{static_cast<std::int64_t>(to_i64())};
                  case primitive_kind::Uint64: return dynamic_value{to_u64()};
                  case primitive_kind::Float32: return dynamic_value{static_cast<float>(to_d())};
                  case primitive_kind::Float64: return dynamic_value{to_d()};
                  default:                       return dynamic_value{};
               }
            }
         }
      }

      inline dynamic_value decode_dv(const schema& sc, parser& pr)
      {
         if (sc.is_primitive())
            return decode_dv_primitive(sc.as_primitive(), pr);
         if (sc.is_projected())
         {
            // Dynamic adapters read the JSON string; the payload
            // string is whatever the adapter emitted. Static
            // decoders pair this with Proj::decode to reconstitute the
            // logical type.
            return dynamic_value{parse_string(pr)};
         }
         if (sc.is_sequence())
         {
            dynamic_sequence seq;
            pr.consume('[');
            pr.skip_ws();
            bool first = true;
            while (pr.p < pr.end && *pr.p != ']')
            {
               if (!first)
                  pr.consume(',');
               first = false;
               seq.elements.push_back(decode_dv(*sc.as_sequence().element, pr));
               pr.skip_ws();
            }
            pr.consume(']');
            return dynamic_value{std::move(seq)};
         }
         if (sc.is_optional())
         {
            dynamic_optional o;
            if (pr.consume_null())
               return dynamic_value{std::move(o)};
            o.value = std::make_unique<dynamic_value>(
               decode_dv(*sc.as_optional().value_type, pr));
            return dynamic_value{std::move(o)};
         }
         // record
         dynamic_record rec;
         pr.consume('{');
         pr.skip_ws();
         bool first = true;
         while (pr.p < pr.end && *pr.p != '}')
         {
            if (!first)
               pr.consume(',');
            first              = false;
            std::string key    = parse_string(pr);
            pr.consume(':');
            // Find the matching field descriptor.
            const field_descriptor* fd = nullptr;
            for (const auto& f : sc.as_record().fields)
               if (f.name == key)
               {
                  fd = &f;
                  break;
               }
            if (fd)
               rec.fields.push_back({key, decode_dv(*fd->type, pr)});
            else
            {
               // Unknown field — skip via a no-op parse.
               // Minimal: consume any value by matching brackets/quotes.
               // For the MVP, rely on callers to send well-formed input.
            }
            pr.skip_ws();
         }
         pr.consume('}');
         return dynamic_value{std::move(rec)};
      }

   }  // namespace detail::dyn_json_impl

   inline std::string json_encode_dynamic(const schema&        sc,
                                          const dynamic_value& dv)
   {
      std::string out;
      detail::dyn_json_impl::encode_dv(sc, dv, out);
      return out;
   }

   inline dynamic_value json_decode_dynamic(const schema&        sc,
                                            std::span<const char> bytes)
   {
      detail::dyn_json_impl::parser pr{bytes.data(),
                                        bytes.data() + bytes.size()};
      return detail::dyn_json_impl::decode_dv(sc, pr);
   }

   // ── CPO hidden-friend overloads ────────────────────────────────────────
   //
   // Defined at namespace scope (ADL finds them on the `json` tag) so
   // `psio::encode_dynamic(psio::json{}, …)` dispatches correctly.

   inline std::string tag_invoke(decltype(::psio::encode_dynamic), json,
                                 const schema& sc, const dynamic_value& dv)
   {
      return json_encode_dynamic(sc, dv);
   }

   inline void tag_invoke(decltype(::psio::encode_dynamic), json,
                          const schema& sc, const dynamic_value& dv,
                          std::string& sink)
   {
      detail::dyn_json_impl::encode_dv(sc, dv, sink);
   }

   inline dynamic_value tag_invoke(decltype(::psio::decode_dynamic), json,
                                   const schema&         sc,
                                   std::span<const char> bytes)
   {
      return json_decode_dynamic(sc, bytes);
   }

}  // namespace psio
