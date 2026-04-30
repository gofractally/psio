#pragma once
//
// psio/dynamic_bin.hpp — schema-driven bin encoder/decoder over
// dynamic_value.

#include <psio/bin.hpp>
#include <psio/dynamic_value.hpp>
#include <psio/schema.hpp>

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace psio {

   namespace detail::dyn_bin_impl {

      using sink_t = std::vector<char>;

      inline void append_bytes(sink_t& s, const void* p, std::size_t n)
      {
         const char* cp = static_cast<const char*>(p);
         s.insert(s.end(), cp, cp + n);
      }

      inline void append_u32(sink_t& s, std::uint32_t v)
      {
         append_bytes(s, &v, 4);
      }

      inline std::uint32_t read_u32(std::span<const char> src, std::size_t pos)
      {
         std::uint32_t v{};
         std::memcpy(&v, src.data() + pos, 4);
         return v;
      }

      inline void encode_dv(const schema& sc, const dynamic_value& dv,
                            sink_t& s);

      inline void encode_prim(primitive_kind k, const dynamic_value& dv,
                              sink_t& s)
      {
         switch (k)
         {
            case primitive_kind::Bool:
               s.push_back(dv.as<bool>() ? '\x01' : '\x00'); break;
            case primitive_kind::Int8:
            { auto v = dv.as<std::int8_t>();   append_bytes(s, &v, 1); break; }
            case primitive_kind::Uint8:
            { auto v = dv.as<std::uint8_t>();  append_bytes(s, &v, 1); break; }
            case primitive_kind::Int16:
            { auto v = dv.as<std::int16_t>();  append_bytes(s, &v, 2); break; }
            case primitive_kind::Uint16:
            { auto v = dv.as<std::uint16_t>(); append_bytes(s, &v, 2); break; }
            case primitive_kind::Int32:
            { auto v = dv.as<std::int32_t>();  append_bytes(s, &v, 4); break; }
            case primitive_kind::Uint32:
            { auto v = dv.as<std::uint32_t>(); append_bytes(s, &v, 4); break; }
            case primitive_kind::Int64:
            { auto v = dv.as<std::int64_t>();  append_bytes(s, &v, 8); break; }
            case primitive_kind::Uint64:
            { auto v = dv.as<std::uint64_t>(); append_bytes(s, &v, 8); break; }
            case primitive_kind::Float32:
            { auto v = dv.as<float>();         append_bytes(s, &v, 4); break; }
            case primitive_kind::Float64:
            { auto v = dv.as<double>();        append_bytes(s, &v, 8); break; }
            case primitive_kind::String:
            case primitive_kind::Bytes:
            {
               const auto& str = dv.as<std::string>();
               append_u32(s, static_cast<std::uint32_t>(str.size()));
               append_bytes(s, str.data(), str.size());
               break;
            }
         }
      }

      inline void encode_dv(const schema& sc, const dynamic_value& dv,
                            sink_t& s)
      {
         if (sc.is_primitive())
         {
            encode_prim(sc.as_primitive(), dv, s);
         }
         else if (sc.is_projected())
         {
            // Bin frames projected payloads with a u32 length prefix
            // (matches bin's own variable-payload convention).
            const auto& str = dv.as<std::string>();
            append_u32(s, static_cast<std::uint32_t>(str.size()));
            append_bytes(s, str.data(), str.size());
         }
         else if (sc.is_sequence())
         {
            const auto& seq  = sc.as_sequence();
            const auto& dseq = dv.as<dynamic_sequence>();
            if (!seq.fixed_count.has_value())
               append_u32(s, static_cast<std::uint32_t>(dseq.elements.size()));
            for (const auto& el : dseq.elements)
               encode_dv(*seq.element, el, s);
         }
         else if (sc.is_optional())
         {
            const auto& opt = dv.as<dynamic_optional>();
            s.push_back(opt.value ? '\x01' : '\x00');
            if (opt.value)
               encode_dv(*sc.as_optional().value_type, *opt.value, s);
         }
         else if (sc.is_record())
         {
            const auto& rec = dv.as<dynamic_record>();
            auto find = [&](std::string_view n) -> const dynamic_value* {
               for (const auto& kv : rec.fields)
                  if (kv.first == n)
                     return &kv.second;
               return nullptr;
            };
            for (const auto& f : sc.as_record().fields)
            {
               const auto* d = find(f.name);
               if (d)
                  encode_dv(*f.type, *d, s);
            }
         }
      }

      inline dynamic_value decode_dv(const schema& sc,
                                     std::span<const char> src,
                                     std::size_t& pos);

      inline dynamic_value decode_prim(primitive_kind k,
                                       std::span<const char> src,
                                       std::size_t& pos)
      {
         switch (k)
         {
            case primitive_kind::Bool:
               return dynamic_value{static_cast<unsigned char>(src[pos++]) != 0};
            case primitive_kind::Int8:
            { std::int8_t v{};   std::memcpy(&v, src.data() + pos, 1); pos += 1; return dynamic_value{v}; }
            case primitive_kind::Uint8:
            { std::uint8_t v{};  std::memcpy(&v, src.data() + pos, 1); pos += 1; return dynamic_value{v}; }
            case primitive_kind::Int16:
            { std::int16_t v{};  std::memcpy(&v, src.data() + pos, 2); pos += 2; return dynamic_value{v}; }
            case primitive_kind::Uint16:
            { std::uint16_t v{}; std::memcpy(&v, src.data() + pos, 2); pos += 2; return dynamic_value{v}; }
            case primitive_kind::Int32:
            { std::int32_t v{};  std::memcpy(&v, src.data() + pos, 4); pos += 4; return dynamic_value{v}; }
            case primitive_kind::Uint32:
            { std::uint32_t v{}; std::memcpy(&v, src.data() + pos, 4); pos += 4; return dynamic_value{v}; }
            case primitive_kind::Int64:
            { std::int64_t v{};  std::memcpy(&v, src.data() + pos, 8); pos += 8; return dynamic_value{v}; }
            case primitive_kind::Uint64:
            { std::uint64_t v{}; std::memcpy(&v, src.data() + pos, 8); pos += 8; return dynamic_value{v}; }
            case primitive_kind::Float32:
            { float v{};  std::memcpy(&v, src.data() + pos, 4); pos += 4; return dynamic_value{v}; }
            case primitive_kind::Float64:
            { double v{}; std::memcpy(&v, src.data() + pos, 8); pos += 8; return dynamic_value{v}; }
            case primitive_kind::String:
            case primitive_kind::Bytes:
            {
               const auto n = read_u32(src, pos);
               pos += 4;
               std::string s(src.data() + pos, src.data() + pos + n);
               pos += n;
               return dynamic_value{std::move(s)};
            }
         }
         return dynamic_value{};
      }

      inline dynamic_value decode_dv(const schema& sc,
                                     std::span<const char> src,
                                     std::size_t& pos)
      {
         if (sc.is_primitive())
            return decode_prim(sc.as_primitive(), src, pos);
         if (sc.is_projected())
         {
            const auto n = read_u32(src, pos);
            pos += 4;
            std::string s(src.data() + pos, src.data() + pos + n);
            pos += n;
            return dynamic_value{std::move(s)};
         }
         if (sc.is_sequence())
         {
            const auto& seq = sc.as_sequence();
            dynamic_sequence out;
            std::size_t n;
            if (seq.fixed_count.has_value())
            {
               n = *seq.fixed_count;
            }
            else
            {
               n = read_u32(src, pos);
               pos += 4;
            }
            for (std::size_t i = 0; i < n; ++i)
               out.elements.push_back(decode_dv(*seq.element, src, pos));
            return dynamic_value{std::move(out)};
         }
         if (sc.is_optional())
         {
            dynamic_optional o;
            const bool present = static_cast<unsigned char>(src[pos++]) != 0;
            if (present)
               o.value = std::make_unique<dynamic_value>(
                  decode_dv(*sc.as_optional().value_type, src, pos));
            return dynamic_value{std::move(o)};
         }
         // record
         dynamic_record rec;
         for (const auto& f : sc.as_record().fields)
            rec.fields.push_back(
               {std::string(f.name), decode_dv(*f.type, src, pos)});
         return dynamic_value{std::move(rec)};
      }

   }  // namespace detail::dyn_bin_impl

   inline std::vector<char> bin_encode_dynamic(const schema&        sc,
                                               const dynamic_value& dv)
   {
      std::vector<char> out;
      detail::dyn_bin_impl::encode_dv(sc, dv, out);
      return out;
   }

   inline dynamic_value bin_decode_dynamic(const schema&         sc,
                                           std::span<const char> bytes)
   {
      std::size_t pos = 0;
      return detail::dyn_bin_impl::decode_dv(sc, bytes, pos);
   }

   // ── CPO hidden-friend overloads (namespace-scope; ADL finds them) ──

   inline std::vector<char> tag_invoke(decltype(::psio::encode_dynamic),
                                       bin, const schema& sc,
                                       const dynamic_value& dv)
   {
      return bin_encode_dynamic(sc, dv);
   }

   inline dynamic_value tag_invoke(decltype(::psio::decode_dynamic), bin,
                                   const schema&         sc,
                                   std::span<const char> bytes)
   {
      return bin_decode_dynamic(sc, bytes);
   }

}  // namespace psio
