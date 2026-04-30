#pragma once
//
// psio/wit_encode.hpp — Encode a wit_world to Component Model binary
// format.  Direct port of psio1/wit_encode.hpp; the encoder is purely
// data-driven (consumes wit_world / wit_func / wit_type_def, all of
// which already exist in v3 with identical shape) so the body is
// near-verbatim.
//
// Usage:
//   auto world  = psio::generate_wit<MyExports>("test:pkg@1.0.0");
//   auto binary = psio::encode_wit_binary(world);                   // ready
//                                                                    // for a
//                                                                    // component-type
//                                                                    // custom
//                                                                    // section
//   // or, in one step:
//   auto binary = psio::generate_wit_binary<MyExports>("test:pkg@1.0.0");
//
// Output matches wit-component::metadata::encode().

#include <psio/wit_gen.hpp>
#include <psio/wit_types.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace psio
{

   namespace detail
   {

      // ── Component Model binary opcodes ──────────────────────────────
      namespace cm
      {
         // Section IDs
         constexpr std::uint8_t section_custom = 0x00;
         constexpr std::uint8_t section_type   = 0x07;
         constexpr std::uint8_t section_export = 0x0b;

         // Container type constructors
         constexpr std::uint8_t type_component = 0x41;
         constexpr std::uint8_t type_instance  = 0x42;
         constexpr std::uint8_t type_func      = 0x40;

         // Defined value type constructors (negative SLEB128)
         constexpr std::uint8_t def_record  = 0x72;
         constexpr std::uint8_t def_variant = 0x71;
         constexpr std::uint8_t def_list    = 0x70;
         constexpr std::uint8_t def_tuple   = 0x6f;
         constexpr std::uint8_t def_flags   = 0x6e;
         constexpr std::uint8_t def_enum_   = 0x6d;
         constexpr std::uint8_t def_option  = 0x6b;
         constexpr std::uint8_t def_result  = 0x6a;

         // Primitive value types (negative SLEB128)
         constexpr std::uint8_t prim_bool   = 0x7f;
         constexpr std::uint8_t prim_s8     = 0x7e;
         constexpr std::uint8_t prim_u8     = 0x7d;
         constexpr std::uint8_t prim_s16    = 0x7c;
         constexpr std::uint8_t prim_u16    = 0x7b;
         constexpr std::uint8_t prim_s32    = 0x7a;
         constexpr std::uint8_t prim_u32    = 0x79;
         constexpr std::uint8_t prim_s64    = 0x78;
         constexpr std::uint8_t prim_u64    = 0x77;
         constexpr std::uint8_t prim_f32    = 0x76;
         constexpr std::uint8_t prim_f64    = 0x75;
         constexpr std::uint8_t prim_char   = 0x74;
         constexpr std::uint8_t prim_string = 0x73;

         // Instance/component item tags
         constexpr std::uint8_t item_type_def = 0x01;
         constexpr std::uint8_t item_import   = 0x03;
         constexpr std::uint8_t item_export   = 0x04;

         // Export sort
         constexpr std::uint8_t sort_func      = 0x01;
         constexpr std::uint8_t sort_type      = 0x03;
         constexpr std::uint8_t sort_component = 0x04;
         constexpr std::uint8_t sort_instance  = 0x05;

         // Extern name discriminant
         constexpr std::uint8_t name_kebab = 0x00;

         // Type bound
         constexpr std::uint8_t bound_eq = 0x00;

         // Function result
         constexpr std::uint8_t result_single = 0x00;
         constexpr std::uint8_t result_named  = 0x01;

         // Option encoding (variant cases / result types)
         constexpr std::uint8_t option_none = 0x00;
         constexpr std::uint8_t option_some = 0x01;
      }  // namespace cm

      // ── Bit-level emitter ───────────────────────────────────────────

      class wit_binary_writer
      {
         std::vector<std::uint8_t> buf_;

       public:
         void emit_byte(std::uint8_t b) { buf_.push_back(b); }

         void emit_uleb128(std::uint32_t val)
         {
            do
            {
               std::uint8_t b = val & 0x7f;
               val >>= 7;
               if (val != 0)
                  b |= 0x80;
               buf_.push_back(b);
            } while (val != 0);
         }

         void emit_string(std::string_view s)
         {
            emit_uleb128(static_cast<std::uint32_t>(s.size()));
            buf_.insert(buf_.end(), s.begin(), s.end());
         }

         void emit_bytes(const std::vector<std::uint8_t>& data)
         {
            buf_.insert(buf_.end(), data.begin(), data.end());
         }

         std::size_t                       size() const { return buf_.size(); }
         std::vector<std::uint8_t>         take() { return std::move(buf_); }
         const std::vector<std::uint8_t>&  data() const { return buf_; }
      };

      // ── Primitive → CM byte ─────────────────────────────────────────
      inline std::uint8_t prim_to_cm_byte(wit_prim p)
      {
         switch (p)
         {
            case wit_prim::bool_:   return cm::prim_bool;
            case wit_prim::u8:      return cm::prim_u8;
            case wit_prim::s8:      return cm::prim_s8;
            case wit_prim::u16:     return cm::prim_u16;
            case wit_prim::s16:     return cm::prim_s16;
            case wit_prim::u32:     return cm::prim_u32;
            case wit_prim::s32:     return cm::prim_s32;
            case wit_prim::u64:     return cm::prim_u64;
            case wit_prim::s64:     return cm::prim_s64;
            case wit_prim::f32:     return cm::prim_f32;
            case wit_prim::f64:     return cm::prim_f64;
            case wit_prim::char_:   return cm::prim_char;
            case wit_prim::string_: return cm::prim_string;
         }
         return cm::prim_u32;  // fallback
      }

      // ── Encoder ─────────────────────────────────────────────────────
      //
      // Builds an instance type per interface, wraps them in nested
      // component types, produces a complete Component binary suitable
      // for a `component-type:NAME` custom section.

      class wit_cm_encoder
      {
         const wit_world& world_;

         struct instance_encoding
         {
            std::vector<std::uint8_t> bytes;
            std::uint32_t             item_count = 0;
         };

         using remap_t = std::unordered_map<std::int32_t, std::uint32_t>;

         void emit_valtype(wit_binary_writer&  w,
                           std::int32_t        type_idx,
                           const remap_t&      remap) const
         {
            if (is_prim_idx(type_idx))
            {
               w.emit_byte(prim_to_cm_byte(idx_to_prim(type_idx)));
               return;
            }
            auto it  = remap.find(type_idx);
            auto idx = (it != remap.end()) ? it->second
                                           : static_cast<std::uint32_t>(type_idx);
            w.emit_uleb128(idx);
         }

         void emit_defined_type(wit_binary_writer&  w,
                                const wit_type_def& td,
                                const remap_t&      remap) const
         {
            switch (static_cast<wit_type_kind>(td.kind))
            {
               case wit_type_kind::record_:
                  w.emit_byte(cm::def_record);
                  w.emit_uleb128(static_cast<std::uint32_t>(td.fields.size()));
                  for (auto& f : td.fields)
                  {
                     w.emit_string(f.name);
                     emit_valtype(w, f.type_idx, remap);
                  }
                  break;
               case wit_type_kind::list_:
                  w.emit_byte(cm::def_list);
                  emit_valtype(w, td.element_type_idx, remap);
                  break;
               case wit_type_kind::option_:
                  w.emit_byte(cm::def_option);
                  emit_valtype(w, td.element_type_idx, remap);
                  break;
               case wit_type_kind::result_:
                  w.emit_byte(cm::def_result);
                  if (td.element_type_idx == WIT_NO_TYPE)
                     w.emit_byte(cm::option_none);
                  else
                  {
                     w.emit_byte(cm::option_some);
                     emit_valtype(w, td.element_type_idx, remap);
                  }
                  if (td.error_type_idx == WIT_NO_TYPE)
                     w.emit_byte(cm::option_none);
                  else
                  {
                     w.emit_byte(cm::option_some);
                     emit_valtype(w, td.error_type_idx, remap);
                  }
                  break;
               case wit_type_kind::variant_:
                  w.emit_byte(cm::def_variant);
                  w.emit_uleb128(static_cast<std::uint32_t>(td.fields.size()));
                  for (auto& f : td.fields)
                  {
                     w.emit_string(f.name);
                     if (f.type_idx == WIT_NO_TYPE)
                        w.emit_byte(cm::option_none);
                     else
                     {
                        w.emit_byte(cm::option_some);
                        emit_valtype(w, f.type_idx, remap);
                     }
                     w.emit_byte(cm::option_none);  // refines: none
                  }
                  break;
               case wit_type_kind::enum_:
                  w.emit_byte(cm::def_enum_);
                  w.emit_uleb128(static_cast<std::uint32_t>(td.fields.size()));
                  for (auto& f : td.fields)
                     w.emit_string(f.name);
                  break;
               case wit_type_kind::flags_:
                  w.emit_byte(cm::def_flags);
                  w.emit_uleb128(static_cast<std::uint32_t>(td.fields.size()));
                  for (auto& f : td.fields)
                     w.emit_string(f.name);
                  break;
               case wit_type_kind::tuple_:
                  w.emit_byte(cm::def_tuple);
                  w.emit_uleb128(static_cast<std::uint32_t>(td.fields.size()));
                  for (auto& f : td.fields)
                     emit_valtype(w, f.type_idx, remap);
                  break;
               default:
                  // resource_ / own_ / borrow_ encoding lives in the
                  // canonical-ABI follow-up; raw record/list/etc are
                  // sufficient for the WASI 2.3 surface.
                  break;
            }
         }

         void emit_func_type(wit_binary_writer&  w,
                             const wit_func&     func,
                             const remap_t&      remap) const
         {
            w.emit_byte(cm::type_func);
            w.emit_uleb128(static_cast<std::uint32_t>(func.params.size()));
            for (auto& p : func.params)
            {
               w.emit_string(p.name);
               emit_valtype(w, p.type_idx, remap);
            }
            if (func.results.size() == 1 && func.results[0].name.empty())
            {
               w.emit_byte(cm::result_single);
               emit_valtype(w, func.results[0].type_idx, remap);
            }
            else
            {
               w.emit_byte(cm::result_named);
               w.emit_uleb128(static_cast<std::uint32_t>(func.results.size()));
               for (auto& r : func.results)
               {
                  w.emit_string(r.name);
                  emit_valtype(w, r.type_idx, remap);
               }
            }
         }

         void emit_extern_name(wit_binary_writer& w, std::string_view name) const
         {
            w.emit_byte(cm::name_kebab);
            w.emit_string(name);
         }

         // Emit a type, recursively emitting dependencies first.
         void ensure_emitted(std::int32_t       type_idx,
                             wit_binary_writer& w,
                             remap_t&           remap,
                             std::uint32_t&     next_type_idx,
                             std::uint32_t&     item_count) const
         {
            if (type_idx == WIT_NO_TYPE)
               return;
            if (is_prim_idx(type_idx))
               return;
            if (remap.count(type_idx))
               return;

            auto idx = static_cast<std::size_t>(type_idx);
            if (idx >= world_.types.size())
               return;

            const auto& td = world_.types[idx];
            switch (static_cast<wit_type_kind>(td.kind))
            {
               case wit_type_kind::record_:
               case wit_type_kind::tuple_:
                  for (auto& f : td.fields)
                     ensure_emitted(f.type_idx, w, remap, next_type_idx, item_count);
                  break;
               case wit_type_kind::list_:
               case wit_type_kind::option_:
                  ensure_emitted(td.element_type_idx, w, remap, next_type_idx,
                                 item_count);
                  break;
               case wit_type_kind::result_:
                  ensure_emitted(td.element_type_idx, w, remap, next_type_idx,
                                 item_count);
                  ensure_emitted(td.error_type_idx, w, remap, next_type_idx,
                                 item_count);
                  break;
               case wit_type_kind::variant_:
                  for (auto& f : td.fields)
                     if (f.type_idx != WIT_NO_TYPE)
                        ensure_emitted(f.type_idx, w, remap, next_type_idx,
                                       item_count);
                  break;
               default:
                  break;
            }

            // Dependency resolution may have emitted us already (cycles
            // caught above by the remap check, but defensive).
            if (remap.count(type_idx))
               return;

            w.emit_byte(cm::item_type_def);
            emit_defined_type(w, td, remap);
            std::uint32_t def_idx = next_type_idx++;
            ++item_count;

            if (!td.name.empty())
            {
               w.emit_byte(cm::item_export);
               emit_extern_name(w, td.name);
               w.emit_byte(cm::sort_type);
               w.emit_byte(cm::bound_eq);
               w.emit_uleb128(def_idx);
               remap[type_idx] = next_type_idx++;
               ++item_count;
            }
            else
            {
               remap[type_idx] = def_idx;
            }
         }

         instance_encoding encode_interface(const wit_interface& iface) const
         {
            remap_t           remap;
            std::uint32_t     next_type_idx = 0;
            std::uint32_t     item_count    = 0;
            wit_binary_writer w;

            // Phase 1: named types in declaration order.
            for (auto type_idx : iface.type_idxs)
               ensure_emitted(static_cast<std::int32_t>(type_idx), w, remap,
                              next_type_idx, item_count);

            // Phase 2: functions, lazy-emitting any remaining anonymous types.
            for (auto func_idx : iface.func_idxs)
            {
               if (func_idx >= world_.funcs.size())
                  continue;
               const auto& func = world_.funcs[func_idx];

               for (auto& p : func.params)
                  ensure_emitted(p.type_idx, w, remap, next_type_idx, item_count);
               for (auto& r : func.results)
                  ensure_emitted(r.type_idx, w, remap, next_type_idx, item_count);

               w.emit_byte(cm::item_type_def);
               emit_func_type(w, func, remap);
               std::uint32_t func_type_idx = next_type_idx++;
               ++item_count;

               w.emit_byte(cm::item_export);
               emit_extern_name(w, func.name);
               w.emit_byte(cm::sort_func);
               w.emit_uleb128(func_type_idx);
               ++item_count;
            }

            return {w.take(), item_count};
         }

         std::string qualified_interface_name(const wit_interface& iface) const
         {
            const auto& pkg    = world_.package;
            auto        at_pos = pkg.find('@');
            if (at_pos != std::string::npos)
               return pkg.substr(0, at_pos) + "/" + iface.name + pkg.substr(at_pos);
            return pkg + "/" + iface.name;
         }

         std::string qualified_world_name() const
         {
            const auto& pkg    = world_.package;
            auto        at_pos = pkg.find('@');
            if (at_pos != std::string::npos)
               return pkg.substr(0, at_pos) + "/" + world_.name + pkg.substr(at_pos);
            return pkg + "/" + world_.name;
         }

       public:
         explicit wit_cm_encoder(const wit_world& world) : world_(world) {}

         std::vector<std::uint8_t> encode() const
         {
            wit_binary_writer out;

            // Component header.
            out.emit_byte(0x00);
            out.emit_byte(0x61);
            out.emit_byte(0x73);
            out.emit_byte(0x6d);
            out.emit_byte(0x0d);
            out.emit_byte(0x00);
            out.emit_byte(0x01);
            out.emit_byte(0x00);

            // Custom section: wit-component-encoding (version 4, UTF-8).
            {
               wit_binary_writer cs;
               cs.emit_string("wit-component-encoding");
               cs.emit_byte(0x04);  // version
               cs.emit_byte(0x00);  // encoding: UTF-8
               out.emit_byte(cm::section_custom);
               out.emit_uleb128(static_cast<std::uint32_t>(cs.size()));
               out.emit_bytes(cs.data());
            }

            if (world_.exports.empty() && world_.imports.empty())
               return out.take();

            // Pre-encode all interfaces.
            struct iface_entry
            {
               const wit_interface* iface;
               instance_encoding    enc;
               bool                 is_import;
            };
            std::vector<iface_entry> entries;
            entries.reserve(world_.imports.size() + world_.exports.size());
            for (auto& imp : world_.imports)
               entries.push_back({&imp, encode_interface(imp), true});
            for (auto& exp : world_.exports)
               entries.push_back({&exp, encode_interface(exp), false});

            const auto inner_item_count =
               static_cast<std::uint32_t>(entries.size() * 2);

            // Type section: 1 type = outer COMPONENT.
            {
               wit_binary_writer type_sec;
               type_sec.emit_uleb128(1);

               type_sec.emit_byte(cm::type_component);
               type_sec.emit_uleb128(2);  // inner component + world export

               type_sec.emit_byte(cm::item_type_def);
               type_sec.emit_byte(cm::type_component);
               type_sec.emit_uleb128(inner_item_count);

               std::uint32_t inner_type_idx = 0;
               for (const auto& entry : entries)
               {
                  type_sec.emit_byte(cm::item_type_def);
                  type_sec.emit_byte(cm::type_instance);
                  type_sec.emit_uleb128(entry.enc.item_count);
                  type_sec.emit_bytes(entry.enc.bytes);

                  type_sec.emit_byte(entry.is_import ? cm::item_import
                                                     : cm::item_export);
                  emit_extern_name(type_sec,
                                   qualified_interface_name(*entry.iface));
                  type_sec.emit_byte(cm::sort_instance);
                  type_sec.emit_uleb128(inner_type_idx);
                  ++inner_type_idx;
               }

               type_sec.emit_byte(cm::item_export);
               emit_extern_name(type_sec, qualified_world_name());
               type_sec.emit_byte(cm::sort_component);
               type_sec.emit_uleb128(0);

               out.emit_byte(cm::section_type);
               out.emit_uleb128(static_cast<std::uint32_t>(type_sec.size()));
               out.emit_bytes(type_sec.data());
            }

            // Export section: world name → type(0).
            {
               wit_binary_writer exp_sec;
               exp_sec.emit_uleb128(1);
               emit_extern_name(exp_sec, world_.name);
               exp_sec.emit_byte(cm::sort_type);
               exp_sec.emit_uleb128(0);
               exp_sec.emit_byte(0x00);  // no explicit type annotation

               out.emit_byte(cm::section_export);
               out.emit_uleb128(static_cast<std::uint32_t>(exp_sec.size()));
               out.emit_bytes(exp_sec.data());
            }

            // Custom section: producers.
            {
               wit_binary_writer cs;
               cs.emit_string("producers");
               cs.emit_uleb128(1);
               cs.emit_string("processed-by");
               cs.emit_uleb128(1);
               cs.emit_string("psio-wit-gen");
               cs.emit_string("1.0.0");

               out.emit_byte(cm::section_custom);
               out.emit_uleb128(static_cast<std::uint32_t>(cs.size()));
               out.emit_bytes(cs.data());
            }

            return out.take();
         }
      };

   }  // namespace detail

   // =====================================================================
   // Public API
   // =====================================================================

   /// Encode a wit_world to Component Model binary suitable for a
   /// `component-type:NAME` custom section.
   inline std::vector<std::uint8_t> encode_wit_binary(const wit_world& world)
   {
      detail::wit_cm_encoder encoder(world);
      return encoder.encode();
   }

   /// Build the wit_world for the given interface tag and encode it.
   template <typename Tag>
   std::vector<std::uint8_t> generate_wit_binary(std::string_view ns,
                                                 std::string_view name,
                                                 std::string_view version,
                                                 std::string_view world_name = {})
   {
      return encode_wit_binary(generate_wit<Tag>(ns, name, version, world_name));
   }

   template <typename Tag>
   std::vector<std::uint8_t> generate_wit_binary(const std::string& package,
                                                 std::string_view   world_name = {})
   {
      return encode_wit_binary(generate_wit<Tag>(package, world_name));
   }

}  // namespace psio
