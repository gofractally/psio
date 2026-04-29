#pragma once
//
// psio/dynamic_pssz.hpp — schema-driven pSSZ codec over dynamic_value.
//
// Same wire layout as SSZ (offset table + heap) but with a configurable
// offset width W (1/2/4 bytes). For a width-agnostic dynamic codec we
// parametrize on W and expose tag_invoke overloads for pssz8/16/32.

#include <psio/dynamic_value.hpp>
#include <psio/pssz.hpp>
#include <psio/schema.hpp>

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace psio {

   namespace detail::dyn_pssz_impl {

      inline bool sc_is_fixed(const schema& sc) noexcept
      {
         if (sc.is_primitive())
         {
            auto k = sc.as_primitive();
            return k != primitive_kind::String &&
                   k != primitive_kind::Bytes;
         }
         if (sc.is_sequence())
         {
            const auto& seq = sc.as_sequence();
            return seq.fixed_count.has_value() && sc_is_fixed(*seq.element);
         }
         if (sc.is_optional())    return false;
         if (sc.is_projected())   return false;
         if (sc.is_record())
         {
            for (const auto& f : sc.as_record().fields)
               if (!sc_is_fixed(*f.type))
                  return false;
            return true;
         }
         return false;
      }

      inline std::size_t prim_size(primitive_kind k) noexcept
      {
         switch (k)
         {
            case primitive_kind::Bool:    return 1;
            case primitive_kind::Int8:
            case primitive_kind::Uint8:   return 1;
            case primitive_kind::Int16:
            case primitive_kind::Uint16:  return 2;
            case primitive_kind::Int32:
            case primitive_kind::Uint32:
            case primitive_kind::Float32: return 4;
            case primitive_kind::Int64:
            case primitive_kind::Uint64:
            case primitive_kind::Float64: return 8;
            case primitive_kind::String:
            case primitive_kind::Bytes:   return 0;
         }
         return 0;
      }

      inline std::size_t sc_fixed_size(const schema& sc) noexcept
      {
         if (sc.is_primitive()) return prim_size(sc.as_primitive());
         if (sc.is_sequence())
         {
            const auto& seq = sc.as_sequence();
            return seq.fixed_count.value_or(0) *
                   sc_fixed_size(*seq.element);
         }
         if (sc.is_record())
         {
            std::size_t total = 0;
            for (const auto& f : sc.as_record().fields)
               total += sc_fixed_size(*f.type);
            return total;
         }
         return 0;
      }

      using sink_t = std::vector<char>;

      template <std::size_t W>
      void write_offset(sink_t& s, std::size_t pos, std::size_t v)
      {
         using O = typename ::psio::detail::pssz_impl::width_info<W>::offset_t;
         O ov = static_cast<O>(v);
         std::memcpy(s.data() + pos, &ov, W);
      }

      template <std::size_t W>
      std::uint32_t read_offset(std::span<const char> src, std::size_t pos)
      {
         using O = typename ::psio::detail::pssz_impl::width_info<W>::offset_t;
         O v{};
         std::memcpy(&v, src.data() + pos, W);
         return static_cast<std::uint32_t>(v);
      }

      inline void append_bytes(sink_t& s, const void* p, std::size_t n)
      {
         const char* cp = static_cast<const char*>(p);
         s.insert(s.end(), cp, cp + n);
      }

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
               append_bytes(s, str.data(), str.size());
               break;
            }
         }
      }

      template <std::size_t W>
      void encode_dv(const schema& sc, const dynamic_value& dv, sink_t& s)
      {
         if (sc.is_primitive())
         {
            encode_prim(sc.as_primitive(), dv, s);
         }
         else if (sc.is_projected())
         {
            const auto& str = dv.as<std::string>();
            append_bytes(s, str.data(), str.size());
         }
         else if (sc.is_sequence())
         {
            const auto& seq     = sc.as_sequence();
            const auto& element = *seq.element;
            const auto& dseq    = dv.as<dynamic_sequence>();
            if (sc_is_fixed(element))
            {
               for (const auto& el : dseq.elements)
                  encode_dv<W>(element, el, s);
            }
            else
            {
               const std::size_t n     = dseq.elements.size();
               const std::size_t table = s.size();
               s.resize(s.size() + n * W, 0);
               for (std::size_t i = 0; i < n; ++i)
               {
                  write_offset<W>(s, table + i * W, s.size() - table);
                  encode_dv<W>(element, dseq.elements[i], s);
               }
            }
         }
         else if (sc.is_optional())
         {
            const auto& opt = dv.as<dynamic_optional>();
            if (opt.value)
               encode_dv<W>(*sc.as_optional().value_type, *opt.value, s);
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
            const auto& fields       = sc.as_record().fields;
            const std::size_t container_start = s.size();
            std::size_t fixed_region = 0;
            for (const auto& f : fields)
               fixed_region +=
                  sc_is_fixed(*f.type) ? sc_fixed_size(*f.type) : W;
            s.resize(container_start + fixed_region, 0);
            std::size_t cursor = container_start;
            for (const auto& f : fields)
            {
               const auto* d = find(f.name);
               if (sc_is_fixed(*f.type))
               {
                  sink_t tmp;
                  if (d)
                     encode_dv<W>(*f.type, *d, tmp);
                  else
                     tmp.resize(sc_fixed_size(*f.type), 0);
                  std::memcpy(s.data() + cursor, tmp.data(), tmp.size());
                  cursor += sc_fixed_size(*f.type);
               }
               else
               {
                  write_offset<W>(s, cursor, s.size() - container_start);
                  cursor += W;
                  if (d)
                     encode_dv<W>(*f.type, *d, s);
               }
            }
         }
      }

      inline dynamic_value decode_prim(primitive_kind k,
                                       std::span<const char> src,
                                       std::size_t pos, std::size_t end)
      {
         switch (k)
         {
            case primitive_kind::Bool:
               return dynamic_value{static_cast<unsigned char>(src[pos]) != 0};
            case primitive_kind::Int8:
            { std::int8_t v{};   std::memcpy(&v, src.data() + pos, 1); return dynamic_value{v}; }
            case primitive_kind::Uint8:
            { std::uint8_t v{};  std::memcpy(&v, src.data() + pos, 1); return dynamic_value{v}; }
            case primitive_kind::Int16:
            { std::int16_t v{};  std::memcpy(&v, src.data() + pos, 2); return dynamic_value{v}; }
            case primitive_kind::Uint16:
            { std::uint16_t v{}; std::memcpy(&v, src.data() + pos, 2); return dynamic_value{v}; }
            case primitive_kind::Int32:
            { std::int32_t v{};  std::memcpy(&v, src.data() + pos, 4); return dynamic_value{v}; }
            case primitive_kind::Uint32:
            { std::uint32_t v{}; std::memcpy(&v, src.data() + pos, 4); return dynamic_value{v}; }
            case primitive_kind::Int64:
            { std::int64_t v{};  std::memcpy(&v, src.data() + pos, 8); return dynamic_value{v}; }
            case primitive_kind::Uint64:
            { std::uint64_t v{}; std::memcpy(&v, src.data() + pos, 8); return dynamic_value{v}; }
            case primitive_kind::Float32:
            { float v{};  std::memcpy(&v, src.data() + pos, 4); return dynamic_value{v}; }
            case primitive_kind::Float64:
            { double v{}; std::memcpy(&v, src.data() + pos, 8); return dynamic_value{v}; }
            case primitive_kind::String:
            case primitive_kind::Bytes:
               return dynamic_value{
                  std::string(src.data() + pos, src.data() + end)};
         }
         return dynamic_value{};
      }

      template <std::size_t W>
      dynamic_value decode_dv(const schema& sc, std::span<const char> src,
                              std::size_t pos, std::size_t end)
      {
         if (sc.is_primitive())
            return decode_prim(sc.as_primitive(), src, pos, end);
         if (sc.is_projected())
            return dynamic_value{
               std::string(src.data() + pos, src.data() + end)};
         if (sc.is_sequence())
         {
            const auto& seq     = sc.as_sequence();
            const auto& element = *seq.element;
            dynamic_sequence out;
            if (sc_is_fixed(element))
            {
               const std::size_t esz = sc_fixed_size(element);
               const std::size_t n   = seq.fixed_count.value_or(
                  (esz > 0 ? (end - pos) / esz : 0));
               for (std::size_t i = 0; i < n; ++i)
                  out.elements.push_back(decode_dv<W>(
                     element, src, pos + i * esz, pos + (i + 1) * esz));
            }
            else
            {
               std::size_t n = 0;
               if (seq.fixed_count.has_value())
                  n = *seq.fixed_count;
               else if (pos < end)
               {
                  std::uint32_t first = read_offset<W>(src, pos);
                  n = first / W;
               }
               std::vector<std::uint32_t> offsets(n);
               for (std::size_t i = 0; i < n; ++i)
                  offsets[i] = read_offset<W>(src, pos + i * W);
               for (std::size_t i = 0; i < n; ++i)
               {
                  const std::size_t beg = pos + offsets[i];
                  const std::size_t fin =
                     (i + 1 < n) ? (pos + offsets[i + 1]) : end;
                  out.elements.push_back(
                     decode_dv<W>(element, src, beg, fin));
               }
            }
            return dynamic_value{std::move(out)};
         }
         if (sc.is_optional())
         {
            dynamic_optional o;
            if (pos < end)
               o.value = std::make_unique<dynamic_value>(
                  decode_dv<W>(*sc.as_optional().value_type, src, pos, end));
            return dynamic_value{std::move(o)};
         }
         // record
         const auto&                fields = sc.as_record().fields;
         const std::size_t          N      = fields.size();
         std::vector<std::uint32_t> var_offsets(N);
         std::vector<bool>          is_var(N, false);
         std::size_t                cursor = pos;
         for (std::size_t i = 0; i < N; ++i)
         {
            if (sc_is_fixed(*fields[i].type))
               cursor += sc_fixed_size(*fields[i].type);
            else
            {
               is_var[i]       = true;
               var_offsets[i]  = read_offset<W>(src, cursor);
               cursor         += W;
            }
         }
         std::vector<std::size_t> var_end(N);
         {
            std::size_t last = end;
            for (std::size_t i = N; i-- > 0;)
               if (is_var[i])
               {
                  var_end[i] = last;
                  last       = pos + var_offsets[i];
               }
         }
         dynamic_record rec;
         std::size_t    fcursor = pos;
         for (std::size_t i = 0; i < N; ++i)
         {
            dynamic_value val;
            if (is_var[i])
            {
               val = decode_dv<W>(*fields[i].type, src,
                                  pos + var_offsets[i], var_end[i]);
               fcursor += W;
            }
            else
            {
               val = decode_dv<W>(*fields[i].type, src, fcursor,
                                  fcursor + sc_fixed_size(*fields[i].type));
               fcursor += sc_fixed_size(*fields[i].type);
            }
            rec.fields.push_back(
               {std::string(fields[i].name), std::move(val)});
         }
         return dynamic_value{std::move(rec)};
      }

   }  // namespace detail::dyn_pssz_impl

   template <std::size_t W>
   std::vector<char> pssz_encode_dynamic(const schema&        sc,
                                         const dynamic_value& dv)
   {
      std::vector<char> out;
      detail::dyn_pssz_impl::encode_dv<W>(sc, dv, out);
      return out;
   }

   template <std::size_t W>
   dynamic_value pssz_decode_dynamic(const schema&         sc,
                                     std::span<const char> bytes)
   {
      return detail::dyn_pssz_impl::decode_dv<W>(sc, bytes, 0, bytes.size());
   }

   // Public dynamic path: defaults to W=4. The runtime schema carries
   // no bounded-size metadata today, so we cannot compute a narrower
   // width at runtime. Callers that know the schema is bounded should
   // use the static `psio::encode(pssz{}, value)` path, which picks
   // the narrowest W via `max_encoded_size<T>()`.
   inline std::vector<char> tag_invoke(decltype(::psio::encode_dynamic),
                                       pssz, const schema& sc,
                                       const dynamic_value& dv)
   { return pssz_encode_dynamic<4>(sc, dv); }

   inline dynamic_value tag_invoke(decltype(::psio::decode_dynamic),
                                   pssz, const schema& sc,
                                   std::span<const char> bytes)
   { return pssz_decode_dynamic<4>(sc, bytes); }

   // Explicit-width dynamic overloads — retained for byte-parity
   // tests that drive the dynamic path at a specific width.
   inline std::vector<char> tag_invoke(decltype(::psio::encode_dynamic),
                                       pssz_<1>, const schema& sc,
                                       const dynamic_value& dv)
   { return pssz_encode_dynamic<1>(sc, dv); }
   inline std::vector<char> tag_invoke(decltype(::psio::encode_dynamic),
                                       pssz_<2>, const schema& sc,
                                       const dynamic_value& dv)
   { return pssz_encode_dynamic<2>(sc, dv); }
   inline std::vector<char> tag_invoke(decltype(::psio::encode_dynamic),
                                       pssz_<4>, const schema& sc,
                                       const dynamic_value& dv)
   { return pssz_encode_dynamic<4>(sc, dv); }

   inline dynamic_value tag_invoke(decltype(::psio::decode_dynamic),
                                   pssz_<1>, const schema& sc,
                                   std::span<const char> bytes)
   { return pssz_decode_dynamic<1>(sc, bytes); }
   inline dynamic_value tag_invoke(decltype(::psio::decode_dynamic),
                                   pssz_<2>, const schema& sc,
                                   std::span<const char> bytes)
   { return pssz_decode_dynamic<2>(sc, bytes); }
   inline dynamic_value tag_invoke(decltype(::psio::decode_dynamic),
                                   pssz_<4>, const schema& sc,
                                   std::span<const char> bytes)
   { return pssz_decode_dynamic<4>(sc, bytes); }

}  // namespace psio
