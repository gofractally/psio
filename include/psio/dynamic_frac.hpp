#pragma once
//
// psio/dynamic_frac.hpp — schema-driven fracpack codec over
// dynamic_value. Mirrors the shape of dynamic_ssz / dynamic_bin but
// uses frac's header + heap-offset + length-prefix wire format
// (design § 5.3.7 / Phase 8 / §14f).
//
// Provides:
//   psio::frac_encode_dynamic<W>(schema, dv)       → std::vector<char>
//   psio::frac_decode_dynamic<W>(schema, bytes)    → dynamic_value
//
// and the matching `tag_invoke` overloads so `psio::encode_dynamic(frac_<W>{}, …)`
// and `psio::decode_dynamic(frac_<W>{}, …)` route through these.
//
// For width-agnostic convenience, the tag_invoke overloads are
// instantiated for both frac16 and frac32.

#include <psio/dynamic_value.hpp>
#include <psio/frac.hpp>
#include <psio/schema.hpp>

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace psio
{

   namespace detail::dyn_frac_impl
   {

      // Shape → fixed? Same policy as dynamic_ssz: primitives except
      // String/Bytes are fixed; sequences with fixed_count + fixed
      // element are fixed; records with all-fixed fields are fixed.
      inline bool sc_is_fixed(const schema& sc) noexcept
      {
         if (sc.is_primitive())
         {
            auto k = sc.as_primitive();
            return k != primitive_kind::String && k != primitive_kind::Bytes;
         }
         if (sc.is_sequence())
         {
            const auto& seq = sc.as_sequence();
            return seq.fixed_count.has_value() && sc_is_fixed(*seq.element);
         }
         if (sc.is_optional())
            return false;
         if (sc.is_projected())
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

      inline std::size_t prim_size(primitive_kind k) noexcept
      {
         switch (k)
         {
            case primitive_kind::Bool:
               return 1;
            case primitive_kind::Int8:
            case primitive_kind::Uint8:
               return 1;
            case primitive_kind::Int16:
            case primitive_kind::Uint16:
               return 2;
            case primitive_kind::Int32:
            case primitive_kind::Uint32:
            case primitive_kind::Float32:
               return 4;
            case primitive_kind::Int64:
            case primitive_kind::Uint64:
            case primitive_kind::Float64:
               return 8;
            case primitive_kind::String:
            case primitive_kind::Bytes:
               return 0;
         }
         return 0;
      }

      inline std::size_t sc_fixed_size(const schema& sc) noexcept
      {
         if (sc.is_primitive())
            return prim_size(sc.as_primitive());
         if (sc.is_sequence())
         {
            const auto& seq = sc.as_sequence();
            return seq.fixed_count.value_or(0) * sc_fixed_size(*seq.element);
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

      template <std::size_t W>
      void append_word(sink_t& s, std::size_t v)
      {
         using O        = typename ::psio::detail::frac_impl::width_info<W>::word_t;
         O           ov = static_cast<O>(v);
         const char* cp = reinterpret_cast<const char*>(&ov);
         s.insert(s.end(), cp, cp + W);
      }

      template <std::size_t W>
      void write_word(sink_t& s, std::size_t pos, std::size_t v)
      {
         using O = typename ::psio::detail::frac_impl::width_info<W>::word_t;
         O ov    = static_cast<O>(v);
         std::memcpy(s.data() + pos, &ov, W);
      }

      template <std::size_t W>
      std::uint32_t read_word(std::span<const char> src, std::size_t pos)
      {
         using O = typename ::psio::detail::frac_impl::width_info<W>::word_t;
         O v{};
         std::memcpy(&v, src.data() + pos, W);
         return static_cast<std::uint32_t>(v);
      }

      template <std::size_t W>
      void encode_prim(primitive_kind k, const dynamic_value& dv, sink_t& s)
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
               // v1 frac: [W-byte byte_count][bytes]. Matches the
               // static encoder's std::string path.
               const auto& str = dv.as<std::string>();
               append_word<W>(s, str.size());
               if (!str.empty())
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
            encode_prim<W>(sc.as_primitive(), dv, s);
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
            if (seq.fixed_count.has_value())
            {
               // Fixed-size array — no prefix, elements concatenated.
               for (const auto& el : dseq.elements)
                  encode_dv<W>(element, el, s);
            }
            else
            {
               // v1 fracpack vector wire: [W-byte byte_count][payload].
               // For primitive fixed-width elements this is just
               // element_count * sizeof(element).
               const std::size_t len_pos = s.size();
               s.resize(s.size() + W, 0);
               const std::size_t start = s.size();
               for (const auto& el : dseq.elements)
                  encode_dv<W>(element, el, s);
               write_word<W>(s, len_pos, s.size() - start);
            }
         }
         else if (sc.is_optional())
         {
            // v1 fracpack optional (top-level) uses a W-byte slot with
            // sentinel values: 1 = None, 0 = Some(empty), else = offset.
            // At top-level every non-None value is its own heap payload.
            const auto&       opt  = dv.as<dynamic_optional>();
            const std::size_t slot = s.size();
            s.resize(s.size() + W, 0);
            if (!opt.value)
            {
               write_word<W>(s, slot, 1);
            }
            else
            {
               const auto& inner = *sc.as_optional().value_type;
               // Empty-container shortcut — Some({}) sentinel 0.
               bool empty = false;
               if (inner.is_sequence() && !inner.as_sequence().fixed_count.has_value())
                  empty = opt.value->template as<dynamic_sequence>().elements.empty();
               else if (inner.is_primitive() && (inner.as_primitive() == primitive_kind::String ||
                                                 inner.as_primitive() == primitive_kind::Bytes))
                  empty = opt.value->template as<std::string>().empty();
               if (empty)
               {
                  write_word<W>(s, slot, 0);
               }
               else
               {
                  write_word<W>(s, slot, s.size() - slot);
                  encode_dv<W>(inner, *opt.value, s);
               }
            }
         }
         else if (sc.is_record())
         {
            // v1 fracpack record wire (matches static encoder):
            //   [u16 header = fixed_region size]
            //   [fixed_region: inline fixed fields + W-byte offset
            //                  slots for variable fields]
            //   [heap: variable-field payloads, pointer-relative
            //                  offsets = heap_pos − slot_pos]
            // Slot sentinels in fixed_region: 1 = None (optional
            // field), 0 = Some(empty container) or non-optional empty
            // container.
            const auto& rec  = dv.as<dynamic_record>();
            auto        find = [&](std::string_view n) -> const dynamic_value*
            {
               for (const auto& kv : rec.fields)
                  if (kv.first == n)
                     return &kv.second;
               return nullptr;
            };

            const auto& fields       = sc.as_record().fields;
            std::size_t fixed_region = 0;
            for (const auto& f : fields)
               fixed_region += sc_is_fixed(*f.type) ? sc_fixed_size(*f.type) : W;

            // Always u16 header (matches v1 default).
            append_word<2>(s, fixed_region);
            const std::size_t fixed_start = s.size();
            s.resize(fixed_start + fixed_region, 0);

            // Phase 1 — emit fixed fields inline; variable fields get
            // placeholder slots (1 for optional, 0 for others).
            std::size_t cursor = fixed_start;
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
                  write_word<W>(s, cursor, f.type->is_optional() ? 1 : 0);
                  cursor += W;
               }
            }

            // Phase 2 — variable fields: write payloads, backpatch
            // pointer-relative offsets. Skip empty containers (slot
            // stays at its sentinel 0).
            cursor = fixed_start;
            for (const auto& f : fields)
            {
               if (sc_is_fixed(*f.type))
               {
                  cursor += sc_fixed_size(*f.type);
                  continue;
               }
               const auto*       d    = find(f.name);
               const std::size_t slot = cursor;
               cursor += W;

               if (!d)
                  continue;  // leave sentinel

               if (f.type->is_optional())
               {
                  const auto& opt = d->template as<dynamic_optional>();
                  if (!opt.value)
                     continue;  // sentinel 1 already set (None)
                  // Some: overwrite with pointer-relative offset.
                  const auto& inner = *f.type->as_optional().value_type;
                  bool        empty = false;
                  if (inner.is_sequence() && !inner.as_sequence().fixed_count.has_value())
                     empty = opt.value->template as<dynamic_sequence>().elements.empty();
                  else if (inner.is_primitive() &&
                           (inner.as_primitive() == primitive_kind::String ||
                            inner.as_primitive() == primitive_kind::Bytes))
                     empty = opt.value->template as<std::string>().empty();
                  if (empty)
                  {
                     write_word<W>(s, slot, 0);
                     continue;
                  }
                  write_word<W>(s, slot, s.size() - slot);
                  encode_dv<W>(inner, *opt.value, s);
               }
               else
               {
                  bool empty = false;
                  if (f.type->is_sequence() && !f.type->as_sequence().fixed_count.has_value())
                     empty = d->template as<dynamic_sequence>().elements.empty();
                  else if (f.type->is_primitive() &&
                           (f.type->as_primitive() == primitive_kind::String ||
                            f.type->as_primitive() == primitive_kind::Bytes))
                     empty = d->template as<std::string>().empty();
                  if (empty)
                     continue;  // sentinel 0 stays
                  write_word<W>(s, slot, s.size() - slot);
                  encode_dv<W>(*f.type, *d, s);
               }
            }
         }
      }

      inline dynamic_value decode_prim(primitive_kind        k,
                                       std::span<const char> src,
                                       std::size_t           pos,
                                       std::size_t           end)
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
               return dynamic_value{std::string(src.data() + pos, src.data() + end)};
         }
         return dynamic_value{};
      }

      inline dynamic_value empty_value_for(const schema& sc)
      {
         if (sc.is_primitive())
         {
            auto k = sc.as_primitive();
            if (k == primitive_kind::String || k == primitive_kind::Bytes)
               return dynamic_value{std::string{}};
         }
         if (sc.is_sequence())
            return dynamic_value{dynamic_sequence{}};
         if (sc.is_optional())
            return dynamic_value{dynamic_optional{}};
         if (sc.is_record())
            return dynamic_value{dynamic_record{}};
         return dynamic_value{};
      }

      template <std::size_t W>
      dynamic_value decode_dv(const schema&         sc,
                              std::span<const char> src,
                              std::size_t           pos,
                              std::size_t           end)
      {
         if (sc.is_primitive())
         {
            auto k = sc.as_primitive();
            if (k == primitive_kind::String || k == primitive_kind::Bytes)
            {
               const std::uint32_t byte_count = read_word<W>(src, pos);
               return dynamic_value{
                   std::string(src.data() + pos + W, src.data() + pos + W + byte_count)};
            }
            return decode_prim(sc.as_primitive(), src, pos, end);
         }
         if (sc.is_projected())
            return dynamic_value{std::string(src.data() + pos, src.data() + end)};
         if (sc.is_sequence())
         {
            const auto&      seq = sc.as_sequence();
            dynamic_sequence out;
            std::size_t      cursor      = pos;
            std::size_t      payload_end = end;
            if (seq.fixed_count.has_value())
            {
               payload_end = end;
            }
            else
            {
               const std::uint32_t byte_count = read_word<W>(src, cursor);
               cursor += W;
               payload_end = cursor + byte_count;
            }
            const auto& element = *seq.element;
            if (sc_is_fixed(element))
            {
               const std::size_t esz = sc_fixed_size(element);
               const std::size_t n   = seq.fixed_count.value_or((payload_end - cursor) / esz);
               for (std::size_t i = 0; i < n; ++i)
               {
                  out.elements.push_back(decode_dv<W>(element, src, cursor, cursor + esz));
                  cursor += esz;
               }
            }
            else
            {
               const std::size_t n = seq.fixed_count.value_or(0);
               for (std::size_t i = 0; i < n; ++i)
               {
                  const std::uint32_t len = read_word<W>(src, cursor);
                  cursor += W;
                  out.elements.push_back(decode_dv<W>(element, src, cursor, cursor + len));
                  cursor += len;
               }
            }
            return dynamic_value{std::move(out)};
         }
         if (sc.is_optional())
         {
            dynamic_optional    o;
            const auto&         inner = *sc.as_optional().value_type;
            const std::uint32_t slot  = read_word<W>(src, pos);
            if (slot == 1)
               return dynamic_value{std::move(o)};
            if (slot == 0)
            {
               o.value = std::make_unique<dynamic_value>(empty_value_for(inner));
               return dynamic_value{std::move(o)};
            }
            const std::size_t payload_pos = pos + slot;
            o.value = std::make_unique<dynamic_value>(decode_dv<W>(inner, src, payload_pos, end));
            return dynamic_value{std::move(o)};
         }
         // record — u16 fixed-region header, W-byte pointer-relative slots
         const std::uint32_t fixed_region = read_word<2>(src, pos);
         const std::size_t   fixed_start  = pos + 2;
         dynamic_record      rec;
         const auto&         fields = sc.as_record().fields;
         std::size_t         cursor = fixed_start;
         for (const auto& f : fields)
         {
            if (sc_is_fixed(*f.type))
            {
               const std::size_t esz = sc_fixed_size(*f.type);
               rec.fields.push_back(
                   {std::string(f.name), decode_dv<W>(*f.type, src, cursor, cursor + esz)});
               cursor += esz;
            }
            else
            {
               const std::size_t   slot_pos = cursor;
               const std::uint32_t slot     = read_word<W>(src, slot_pos);
               cursor += W;
               if (f.type->is_optional())
               {
                  const auto&      inner = *f.type->as_optional().value_type;
                  dynamic_optional opt;
                  if (slot == 1)
                  {
                     rec.fields.push_back({std::string(f.name), dynamic_value{std::move(opt)}});
                     continue;
                  }
                  if (slot == 0)
                     opt.value = std::make_unique<dynamic_value>(empty_value_for(inner));
                  else
                  {
                     const std::size_t payload_pos = slot_pos + slot;
                     opt.value                     = std::make_unique<dynamic_value>(
                         decode_dv<W>(inner, src, payload_pos, end));
                  }
                  rec.fields.push_back({std::string(f.name), dynamic_value{std::move(opt)}});
               }
               else
               {
                  dynamic_value value = slot == 0
                                            ? empty_value_for(*f.type)
                                            : decode_dv<W>(*f.type, src, slot_pos + slot, end);
                  rec.fields.push_back({std::string(f.name), std::move(value)});
               }
            }
         }
         (void)fixed_region;
         return dynamic_value{std::move(rec)};
      }

   }  // namespace detail::dyn_frac_impl

   template <std::size_t W>
   std::vector<char> frac_encode_dynamic(const schema& sc, const dynamic_value& dv)
   {
      std::vector<char> out;
      detail::dyn_frac_impl::encode_dv<W>(sc, dv, out);
      return out;
   }

   template <std::size_t W>
   dynamic_value frac_decode_dynamic(const schema& sc, std::span<const char> bytes)
   {
      return detail::dyn_frac_impl::decode_dv<W>(sc, bytes, 0, bytes.size());
   }

   // tag_invoke overloads for frac16 / frac32.
   inline std::vector<char> tag_invoke(decltype(::psio::encode_dynamic),
                                       frac_<4>,
                                       const schema&        sc,
                                       const dynamic_value& dv)
   {
      return frac_encode_dynamic<4>(sc, dv);
   }
   inline std::vector<char> tag_invoke(decltype(::psio::encode_dynamic),
                                       frac_<2>,
                                       const schema&        sc,
                                       const dynamic_value& dv)
   {
      return frac_encode_dynamic<2>(sc, dv);
   }

   inline dynamic_value tag_invoke(decltype(::psio::decode_dynamic),
                                   frac_<4>,
                                   const schema&         sc,
                                   std::span<const char> bytes)
   {
      return frac_decode_dynamic<4>(sc, bytes);
   }
   inline dynamic_value tag_invoke(decltype(::psio::decode_dynamic),
                                   frac_<2>,
                                   const schema&         sc,
                                   std::span<const char> bytes)
   {
      return frac_decode_dynamic<2>(sc, bytes);
   }

}  // namespace psio
