#pragma once
//
// psio/dynamic_ssz.hpp — schema-driven SSZ encoder/decoder over
// dynamic_value.
//
// With JSON and SSZ both having dynamic codecs, `transcode_via` can
// move bytes between the two using only a runtime `psio::schema` —
// no compile-time T at the transcoder site. This is the key DX promise
// of the v3 architecture: schema-is-contract carries the full load
// without users needing to expose their types to the tool.

#include <psio/dynamic_value.hpp>
#include <psio/schema.hpp>
#include <psio/ssz.hpp>

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace psio {

   namespace detail::dyn_ssz_impl {

      // Does this schema describe a value whose wire size is fixed?
      inline bool sc_is_fixed(const schema& sc) noexcept;

      inline std::size_t sc_fixed_size(const schema& sc) noexcept;

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
         if (sc.is_optional())
            return false;
         if (sc.is_projected())
            // Projected payloads are runtime-sized opaque bytes.
            return false;
         if (sc.is_record())
         {
            for (const auto& f : sc.as_record().fields)
               if (!sc_is_fixed(*f.type))
                  return false;
            return true;
         }
         return false;
      }

      inline std::size_t prim_fixed_size(primitive_kind k) noexcept
      {
         switch (k)
         {
            case primitive_kind::Bool:   return 1;
            case primitive_kind::Int8:   return 1;
            case primitive_kind::Uint8:  return 1;
            case primitive_kind::Int16:  return 2;
            case primitive_kind::Uint16: return 2;
            case primitive_kind::Int32:  return 4;
            case primitive_kind::Uint32: return 4;
            case primitive_kind::Int64:  return 8;
            case primitive_kind::Uint64: return 8;
            case primitive_kind::Float32: return 4;
            case primitive_kind::Float64: return 8;
            case primitive_kind::String:  return 0;  // variable
            case primitive_kind::Bytes:   return 0;  // variable
         }
         return 0;
      }

      inline std::size_t sc_fixed_size(const schema& sc) noexcept
      {
         if (sc.is_primitive())
            return prim_fixed_size(sc.as_primitive());
         if (sc.is_projected())
            return 0;  // runtime-sized
         if (sc.is_sequence())
         {
            const auto& seq = sc.as_sequence();
            return (seq.fixed_count.value_or(0)) *
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

      inline void append_bytes(sink_t& s, const void* p, std::size_t n)
      {
         const char* cp = static_cast<const char*>(p);
         s.insert(s.end(), cp, cp + n);
      }

      inline void encode_dv(const schema& sc, const dynamic_value& dv,
                            sink_t& s);

      inline void encode_primitive(primitive_kind k, const dynamic_value& dv,
                                   sink_t& s)
      {
         switch (k)
         {
            case primitive_kind::Bool:
               s.push_back(dv.as<bool>() ? '\x01' : '\x00');
               break;
            case primitive_kind::Int8:
            {
               auto v = dv.as<std::int8_t>();
               append_bytes(s, &v, 1);
               break;
            }
            case primitive_kind::Uint8:
            {
               auto v = dv.as<std::uint8_t>();
               append_bytes(s, &v, 1);
               break;
            }
            case primitive_kind::Int16:
            {
               auto v = dv.as<std::int16_t>();
               append_bytes(s, &v, 2);
               break;
            }
            case primitive_kind::Uint16:
            {
               auto v = dv.as<std::uint16_t>();
               append_bytes(s, &v, 2);
               break;
            }
            case primitive_kind::Int32:
            {
               auto v = dv.as<std::int32_t>();
               append_bytes(s, &v, 4);
               break;
            }
            case primitive_kind::Uint32:
            {
               auto v = dv.as<std::uint32_t>();
               append_bytes(s, &v, 4);
               break;
            }
            case primitive_kind::Int64:
            {
               auto v = dv.as<std::int64_t>();
               append_bytes(s, &v, 8);
               break;
            }
            case primitive_kind::Uint64:
            {
               auto v = dv.as<std::uint64_t>();
               append_bytes(s, &v, 8);
               break;
            }
            case primitive_kind::Float32:
            {
               auto v = dv.as<float>();
               append_bytes(s, &v, 4);
               break;
            }
            case primitive_kind::Float64:
            {
               auto v = dv.as<double>();
               append_bytes(s, &v, 8);
               break;
            }
            case primitive_kind::String:
            case primitive_kind::Bytes:
            {
               const auto& str = dv.as<std::string>();
               append_bytes(s, str.data(), str.size());
               break;
            }
         }
      }

      inline void encode_dv(const schema& sc, const dynamic_value& dv,
                            sink_t& s)
      {
         if (sc.is_primitive())
            encode_primitive(sc.as_primitive(), dv, s);
         else if (sc.is_projected())
         {
            // Dynamic_value holds the adapter's payload bytes (as a
            // string); write them verbatim. Outer framing (the record's
            // heap-slot length for example) is still SSZ's job.
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
                  encode_dv(element, el, s);
            }
            else
            {
               const std::size_t n     = dseq.elements.size();
               const std::size_t table = s.size();
               s.resize(s.size() + n * 4, 0);
               for (std::size_t i = 0; i < n; ++i)
               {
                  std::uint32_t rel =
                     static_cast<std::uint32_t>(s.size() - table);
                  std::memcpy(s.data() + table + i * 4, &rel, 4);
                  encode_dv(element, dseq.elements[i], s);
               }
            }
         }
         else if (sc.is_optional())
         {
            const auto& opt = dv.as<dynamic_optional>();
            if (opt.value)
               encode_dv(*sc.as_optional().value_type, *opt.value, s);
         }
         else if (sc.is_record())
         {
            const auto& rec = dv.as<dynamic_record>();
            // Build a fast lookup by name.
            auto find = [&](std::string_view n) -> const dynamic_value* {
               for (const auto& kv : rec.fields)
                  if (kv.first == n)
                     return &kv.second;
               return nullptr;
            };

            const auto& fields = sc.as_record().fields;

            // Compute fixed region first.
            const std::size_t container_start = s.size();
            std::size_t       fixed_region    = 0;
            for (const auto& f : fields)
            {
               if (sc_is_fixed(*f.type))
                  fixed_region += sc_fixed_size(*f.type);
               else
                  fixed_region += 4;
            }

            s.resize(container_start + fixed_region, 0);
            std::size_t cursor = container_start;

            for (const auto& f : fields)
            {
               const auto* d = find(f.name);
               if (sc_is_fixed(*f.type))
               {
                  // Encode into temp, splice at cursor.
                  sink_t tmp;
                  if (d)
                     encode_dv(*f.type, *d, tmp);
                  else
                     tmp.resize(sc_fixed_size(*f.type), 0);
                  std::memcpy(s.data() + cursor, tmp.data(), tmp.size());
                  cursor += sc_fixed_size(*f.type);
               }
               else
               {
                  std::uint32_t rel =
                     static_cast<std::uint32_t>(s.size() - container_start);
                  std::memcpy(s.data() + cursor, &rel, 4);
                  cursor += 4;
                  if (d)
                     encode_dv(*f.type, *d, s);
               }
            }
         }
      }

      inline dynamic_value decode_dv(const schema& sc,
                                     std::span<const char> src,
                                     std::size_t pos, std::size_t end);

      inline dynamic_value decode_primitive(primitive_kind k,
                                            std::span<const char> src,
                                            std::size_t pos,
                                            std::size_t end)
      {
         switch (k)
         {
            case primitive_kind::Bool:
               return dynamic_value{static_cast<unsigned char>(src[pos]) != 0};
            case primitive_kind::Int8:
            {
               std::int8_t v{};
               std::memcpy(&v, src.data() + pos, 1);
               return dynamic_value{v};
            }
            case primitive_kind::Uint8:
            {
               std::uint8_t v{};
               std::memcpy(&v, src.data() + pos, 1);
               return dynamic_value{v};
            }
            case primitive_kind::Int16:
            {
               std::int16_t v{};
               std::memcpy(&v, src.data() + pos, 2);
               return dynamic_value{v};
            }
            case primitive_kind::Uint16:
            {
               std::uint16_t v{};
               std::memcpy(&v, src.data() + pos, 2);
               return dynamic_value{v};
            }
            case primitive_kind::Int32:
            {
               std::int32_t v{};
               std::memcpy(&v, src.data() + pos, 4);
               return dynamic_value{v};
            }
            case primitive_kind::Uint32:
            {
               std::uint32_t v{};
               std::memcpy(&v, src.data() + pos, 4);
               return dynamic_value{v};
            }
            case primitive_kind::Int64:
            {
               std::int64_t v{};
               std::memcpy(&v, src.data() + pos, 8);
               return dynamic_value{v};
            }
            case primitive_kind::Uint64:
            {
               std::uint64_t v{};
               std::memcpy(&v, src.data() + pos, 8);
               return dynamic_value{v};
            }
            case primitive_kind::Float32:
            {
               float v{};
               std::memcpy(&v, src.data() + pos, 4);
               return dynamic_value{v};
            }
            case primitive_kind::Float64:
            {
               double v{};
               std::memcpy(&v, src.data() + pos, 8);
               return dynamic_value{v};
            }
            case primitive_kind::String:
            case primitive_kind::Bytes:
               return dynamic_value{
                  std::string(src.data() + pos, src.data() + end)};
         }
         return dynamic_value{};
      }

      inline dynamic_value decode_dv(const schema& sc,
                                     std::span<const char> src,
                                     std::size_t pos, std::size_t end)
      {
         if (sc.is_primitive())
            return decode_primitive(sc.as_primitive(), src, pos, end);
         if (sc.is_projected())
            // Payload span belongs to the adapter; grab it verbatim.
            return dynamic_value{
               std::string(src.data() + pos, src.data() + end)};
         if (sc.is_sequence())
         {
            const auto& seq     = sc.as_sequence();
            const auto& element = *seq.element;
            dynamic_sequence out;
            if (seq.fixed_count.has_value())
            {
               const std::size_t N = *seq.fixed_count;
               if (sc_is_fixed(element))
               {
                  const std::size_t esz = sc_fixed_size(element);
                  for (std::size_t i = 0; i < N; ++i)
                     out.elements.push_back(decode_dv(
                        element, src, pos + i * esz, pos + (i + 1) * esz));
               }
               else if (N > 0)
               {
                  std::vector<std::uint32_t> offs(N);
                  for (std::size_t i = 0; i < N; ++i)
                     std::memcpy(&offs[i], src.data() + pos + i * 4, 4);
                  for (std::size_t i = 0; i < N; ++i)
                  {
                     std::size_t beg = pos + offs[i];
                     std::size_t fin =
                        (i + 1 < N) ? (pos + offs[i + 1]) : end;
                     out.elements.push_back(decode_dv(element, src, beg, fin));
                  }
               }
            }
            else
            {
               if (pos == end)
                  return dynamic_value{std::move(out)};
               if (sc_is_fixed(element))
               {
                  const std::size_t esz = sc_fixed_size(element);
                  const std::size_t n   = (end - pos) / esz;
                  for (std::size_t i = 0; i < n; ++i)
                     out.elements.push_back(decode_dv(
                        element, src, pos + i * esz, pos + (i + 1) * esz));
               }
               else
               {
                  std::uint32_t first = 0;
                  std::memcpy(&first, src.data() + pos, 4);
                  const std::size_t          n = first / 4;
                  std::vector<std::uint32_t> offs(n);
                  for (std::size_t i = 0; i < n; ++i)
                     std::memcpy(&offs[i], src.data() + pos + i * 4, 4);
                  for (std::size_t i = 0; i < n; ++i)
                  {
                     std::size_t beg = pos + offs[i];
                     std::size_t fin =
                        (i + 1 < n) ? (pos + offs[i + 1]) : end;
                     out.elements.push_back(decode_dv(element, src, beg, fin));
                  }
               }
            }
            return dynamic_value{std::move(out)};
         }
         if (sc.is_optional())
         {
            dynamic_optional o;
            if (pos < end)
               o.value = std::make_unique<dynamic_value>(
                  decode_dv(*sc.as_optional().value_type, src, pos, end));
            return dynamic_value{std::move(o)};
         }
         // record
         const auto&    fields = sc.as_record().fields;
         const std::size_t N   = fields.size();
         std::vector<std::uint32_t> var_offsets(N);
         std::vector<bool>          is_var(N, false);
         std::size_t                cursor = pos;
         for (std::size_t i = 0; i < N; ++i)
         {
            if (sc_is_fixed(*fields[i].type))
               cursor += sc_fixed_size(*fields[i].type);
            else
            {
               is_var[i] = true;
               std::memcpy(&var_offsets[i], src.data() + cursor, 4);
               cursor += 4;
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
               val = decode_dv(*fields[i].type, src, pos + var_offsets[i],
                                 var_end[i]);
               fcursor += 4;
            }
            else
            {
               val = decode_dv(*fields[i].type, src, fcursor,
                                 fcursor + sc_fixed_size(*fields[i].type));
               fcursor += sc_fixed_size(*fields[i].type);
            }
            rec.fields.push_back(
               {std::string(fields[i].name), std::move(val)});
         }
         return dynamic_value{std::move(rec)};
      }

   }  // namespace detail::dyn_ssz_impl

   inline std::vector<char> ssz_encode_dynamic(const schema&        sc,
                                               const dynamic_value& dv)
   {
      std::vector<char> out;
      detail::dyn_ssz_impl::encode_dv(sc, dv, out);
      return out;
   }

   inline dynamic_value ssz_decode_dynamic(const schema&         sc,
                                           std::span<const char> bytes)
   {
      return detail::dyn_ssz_impl::decode_dv(sc, bytes, 0, bytes.size());
   }

}  // namespace psio

// ── CPO hidden-friend overloads for the ssz tag ───────────────────────────
//
// dynamic_ssz.hpp can't use hidden friends inside the ssz struct (it's
// defined in ssz.hpp) — provide namespace-scope tag_invoke overloads
// instead. ADL on the tag type still finds them.

namespace psio {

   inline std::vector<char>
   tag_invoke(decltype(::psio::encode_dynamic), ssz, const schema& sc,
              const dynamic_value& dv)
   {
      return ssz_encode_dynamic(sc, dv);
   }

   inline void tag_invoke(decltype(::psio::encode_dynamic), ssz,
                          const schema& sc, const dynamic_value& dv,
                          std::vector<char>& sink)
   {
      detail::dyn_ssz_impl::encode_dv(sc, dv, sink);
   }

   inline dynamic_value tag_invoke(decltype(::psio::decode_dynamic), ssz,
                                   const schema&         sc,
                                   std::span<const char> bytes)
   {
      return ssz_decode_dynamic(sc, bytes);
   }

}  // namespace psio
