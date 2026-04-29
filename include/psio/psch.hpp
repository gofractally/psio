#pragma once
//
// psio/psch.hpp — pSSZ-schema binary format (writer + read-only view).
//
// Spec: .issues/psch-format-design.md.
//
// Use:
//   psch::writer w;
//   auto u32_id   = w.add_scalar(psch::kind::u32);
//   auto str_id   = w.add_bytes();
//   auto p_id     = w.add_container({{"name", str_id}, {"age", u32_id}});
//   auto vec_id   = w.add_vector(p_id, 1024);
//   auto root_id  = w.add_container({{"users", vec_id}});
//   auto bytes    = w.finalize(root_id);
//
//   psch::view v(bytes.data(), bytes.size());
//   auto t   = v.type(root_id);              // kind + payload
//   auto fld = v.field_by_name(root_id, "users");
//   auto j   = fld->ordered_index;
//   auto et  = v.type(fld->type_id).vector_elem_type();

#define XXH_INLINE_ALL
#include <hash/xxhash.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace psio::psch {

   // ── Format constants ────────────────────────────────────────────────────

   inline constexpr std::array<char, 4> magic{'P', 'S', 'C', 'H'};

   inline constexpr std::uint8_t flag_wide_type_ids = 0x01;
   // When set, slots in fields_pool are 4 bytes
   //   ({ name_off:u16, name_len:u8, type_id:u8 }).
   // When unset, slots are 8 bytes
   //   ({ name_off:u24, name_len:u8, type_id:u16, pad:u16 }).
   // Encoder picks compact when name_pool < 64 KiB AND type_count ≤ 254;
   // both fit. Saves ~50% on fields_pool for typical schemas.
   inline constexpr std::uint8_t flag_compact_slots = 0x02;

   enum class kind : std::uint8_t
   {
      none      = 0x00,  // reserved
      bool_     = 0x01,
      u8        = 0x02,
      u16       = 0x03,
      u32       = 0x04,
      u64       = 0x05,
      u128      = 0x06,
      u256      = 0x07,
      bytes     = 0x08,  // variable-length bytes
      bytes_n   = 0x09,  // fixed-length bytes (length in payload)
      container = 0x0A,
      vector_   = 0x0B,
      list_     = 0x0C,
      union_    = 0x0D,
   };

   inline bool is_scalar_kind(kind k) noexcept
   {
      return static_cast<std::uint8_t>(k) >= 0x01 &&
             static_cast<std::uint8_t>(k) <= 0x07;
   }

   // Per-type entry stride in bytes. Fixed for all kinds — payload
   // interpretation differs but storage cost is uniform.
   inline constexpr std::size_t type_stride = 8;

   // Per-field slot stride in fields_pool ordered_fields array.
   // 4 bytes when compact_slots flag is set; 8 bytes otherwise.
   inline constexpr std::size_t field_slot_stride_compact = 4;
   inline constexpr std::size_t field_slot_stride_wide    = 8;

   // ── Internal layout helpers ─────────────────────────────────────────────

   inline std::uint32_t read_u32_le(const std::uint8_t* p) noexcept
   {
      std::uint32_t v;
      std::memcpy(&v, p, 4);
      return v;
   }
   inline void write_u32_le(std::uint8_t* p, std::uint32_t v) noexcept
   {
      std::memcpy(p, &v, 4);
   }
   inline std::uint16_t read_u16_le(const std::uint8_t* p) noexcept
   {
      std::uint16_t v;
      std::memcpy(&v, p, 2);
      return v;
   }
   inline void write_u16_le(std::uint8_t* p, std::uint16_t v) noexcept
   {
      std::memcpy(p, &v, 2);
   }

   // varuint8: 1 byte if < 0xFF, else 0xFF + u16. Used for
   // type_count and root_type_id at the header.
   inline std::size_t varuint8_size(std::uint64_t v) noexcept
   {
      return v < 0xFFu ? 1 : 3;
   }
   inline std::size_t write_varuint8(std::uint8_t* dst,
                                     std::size_t   pos,
                                     std::uint64_t v) noexcept
   {
      if (v < 0xFFu)
      {
         dst[pos] = static_cast<std::uint8_t>(v);
         return 1;
      }
      dst[pos] = 0xFFu;
      write_u16_le(dst + pos + 1, static_cast<std::uint16_t>(v));
      return 3;
   }
   inline std::size_t read_varuint8(const std::uint8_t* p,
                                    std::size_t         remaining,
                                    std::uint64_t&      out) noexcept
   {
      if (remaining == 0) return 0;
      if (p[0] != 0xFFu)
      {
         out = p[0];
         return 1;
      }
      if (remaining < 3) return 0;
      out = read_u16_le(p + 1);
      return 3;
   }

   // varuint16: 2 bytes if < 0xFFFF, else 0xFFFF + u32. Used for
   // pool sizes at the header.
   inline std::size_t varuint16_size(std::uint64_t v) noexcept
   {
      return v < 0xFFFFu ? 2 : 6;
   }
   inline std::size_t write_varuint16(std::uint8_t* dst,
                                      std::size_t   pos,
                                      std::uint64_t v) noexcept
   {
      if (v < 0xFFFFu)
      {
         write_u16_le(dst + pos, static_cast<std::uint16_t>(v));
         return 2;
      }
      write_u16_le(dst + pos, 0xFFFFu);
      write_u32_le(dst + pos + 2, static_cast<std::uint32_t>(v));
      return 6;
   }
   inline std::size_t read_varuint16(const std::uint8_t* p,
                                     std::size_t         remaining,
                                     std::uint64_t&      out) noexcept
   {
      if (remaining < 2) return 0;
      std::uint16_t lo = read_u16_le(p);
      if (lo != 0xFFFFu)
      {
         out = lo;
         return 2;
      }
      if (remaining < 6) return 0;
      out = read_u32_le(p + 2);
      return 6;
   }

   // 8-bit prefilter hash, same recipe as pjson. xxh3_64 with seed,
   // low byte as the index input.
   inline std::uint64_t hash_with_seed(std::string_view name,
                                       std::uint64_t    seed) noexcept
   {
      return XXH3_64bits_withSeed(name.data(), name.size(), seed);
   }

   // ── PHF construction (brute-force seed search) ──────────────────────────

   struct phf_result
   {
      std::uint8_t              seed;
      std::uint8_t              size_log2;
      std::vector<std::uint8_t> lookup;  // table_size entries, 0xFF = empty
   };

   inline std::uint8_t ceil_log2(std::size_t n) noexcept
   {
      if (n <= 1) return 0;
      std::uint8_t r = 0;
      std::size_t  v = n - 1;
      while (v) { ++r; v >>= 1; }
      return r;
   }

   // Build a PHF for `names`. Returns the smallest size_log2 (≥
   // ceil_log2(K)) and a seed in [0, 256) such that all names hash to
   // distinct slots. Caps size_log2 at 8 (table_size 256).
   inline phf_result build_phf(std::span<const std::string_view> names)
   {
      std::size_t  K          = names.size();
      std::uint8_t min_log2   = ceil_log2(K);
      if (K == 0)
         return phf_result{0, min_log2, {}};
      for (std::uint8_t size_log2 = min_log2; size_log2 <= 8; ++size_log2)
      {
         std::size_t  table_size = std::size_t{1} << size_log2;
         std::uint64_t mask      = table_size - 1;
         std::vector<std::uint8_t> lookup(table_size, 0xFFu);
         for (std::uint16_t s = 0; s <= 255; ++s)
         {
            std::fill(lookup.begin(), lookup.end(), 0xFFu);
            bool ok = true;
            for (std::size_t i = 0; i < K; ++i)
            {
               std::uint64_t idx = hash_with_seed(names[i], s) & mask;
               if (lookup[idx] != 0xFFu) { ok = false; break; }
               lookup[idx] = static_cast<std::uint8_t>(i);
            }
            if (ok)
               return phf_result{static_cast<std::uint8_t>(s),
                                 size_log2, std::move(lookup)};
         }
      }
      throw std::runtime_error(
         "psch: PHF construction failed (raise size_log2 cap)");
   }

   // ── writer ──────────────────────────────────────────────────────────────

   class writer
   {
     public:
      writer() = default;

      // Add a primitive scalar (bool, u8..u256). Returns the assigned id.
      std::uint16_t add_scalar(kind k)
      {
         if (!is_scalar_kind(k))
            throw std::invalid_argument("psch: not a scalar kind");
         types_.push_back(type_entry{k, 0, 0, 0, 0});
         return next_id_();
      }
      std::uint16_t add_bool()    { return add_scalar(kind::bool_); }
      std::uint16_t add_u8()      { return add_scalar(kind::u8); }
      std::uint16_t add_u16()     { return add_scalar(kind::u16); }
      std::uint16_t add_u32()     { return add_scalar(kind::u32); }
      std::uint16_t add_u64()     { return add_scalar(kind::u64); }
      std::uint16_t add_u128()    { return add_scalar(kind::u128); }
      std::uint16_t add_u256()    { return add_scalar(kind::u256); }

      std::uint16_t add_bytes()
      {
         types_.push_back(type_entry{kind::bytes, 0, 0, 0, 0});
         return next_id_();
      }

      std::uint16_t add_bytes_n(std::uint32_t length)
      {
         types_.push_back(type_entry{kind::bytes_n, length, 0, 0, 0});
         return next_id_();
      }

      std::uint16_t add_vector(std::uint16_t elem_type_id,
                               std::uint32_t length)
      {
         types_.push_back(
            type_entry{kind::vector_, length, elem_type_id, 0, 0});
         return next_id_();
      }

      std::uint16_t add_list(std::uint16_t elem_type_id)
      {
         types_.push_back(type_entry{kind::list_, 0, elem_type_id, 0, 0});
         return next_id_();
      }

      // Add a container with K (name, type_id) fields. PHF construction
      // happens here; failures throw.
      std::uint16_t add_container(
         std::initializer_list<
            std::pair<std::string_view, std::uint16_t>> fields)
      {
         std::vector<std::pair<std::string_view, std::uint16_t>> v(
            fields.begin(), fields.end());
         return add_container(v);
      }
      std::uint16_t add_container(
         std::span<
            const std::pair<std::string_view, std::uint16_t>> fields)
      {
         std::vector<std::string_view> names;
         names.reserve(fields.size());
         for (const auto& [n, _] : fields) names.push_back(n);
         phf_result phf = build_phf(names);

         container_block blk;
         blk.K         = static_cast<std::uint8_t>(fields.size());
         blk.seed      = phf.seed;
         blk.size_log2 = phf.size_log2;
         blk.lookup    = std::move(phf.lookup);
         blk.fields.reserve(fields.size());
         for (const auto& [n, tid] : fields)
            blk.fields.push_back(
               {intern_name_(n),
                static_cast<std::uint8_t>(n.size()), tid});

         // Fields_offset depends on slot_stride which is decided at
         // finalize time; record the container index here, resolve to
         // a byte offset later.
         std::uint32_t container_idx =
            static_cast<std::uint32_t>(containers_.size());
         containers_.push_back(std::move(blk));
         types_.push_back(
            type_entry{kind::container, container_idx,
                       static_cast<std::uint16_t>(fields.size()), 0, 0});
         return next_id_();
      }

      std::uint16_t add_union(std::span<const std::uint16_t> variants)
      {
         std::uint32_t variants_offset =
            static_cast<std::uint32_t>(variants_pool_size_());
         variants_blocks_.push_back(
            std::vector<std::uint16_t>(variants.begin(), variants.end()));
         types_.push_back(
            type_entry{kind::union_, variants_offset,
                       static_cast<std::uint16_t>(variants.size()), 0, 0});
         return next_id_();
      }

      std::vector<std::uint8_t> finalize(std::uint16_t root_type_id)
      {
         bool wide    = types_.size() > 254;
         bool compact = !wide && name_pool_.size() < 0x10000;
         std::size_t slot_stride =
            compact ? field_slot_stride_compact : field_slot_stride_wide;

         std::size_t header_size =
            4                                // magic
            + 1                              // flags
            + varuint8_size(types_.size())   // type_count
            + varuint16_size(name_pool_.size())
            + varuint16_size(fields_pool_size_(slot_stride))
            + varuint16_size(variants_pool_size_())
            + (wide ? 3 : varuint8_size(root_type_id));

         std::size_t type_table_size = types_.size() * type_stride;
         std::size_t fp_size = fields_pool_size_(slot_stride);
         std::size_t total =
            header_size + name_pool_.size() + type_table_size +
            fp_size + variants_pool_size_();

         std::vector<std::uint8_t> out(total);
         std::size_t pos = 0;

         std::memcpy(out.data() + pos, magic.data(), 4);
         pos += 4;
         out[pos++] = static_cast<std::uint8_t>(
            (wide ? flag_wide_type_ids : 0) |
            (compact ? flag_compact_slots : 0));
         pos += write_varuint8(out.data(), pos, types_.size());
         pos += write_varuint16(out.data(), pos, name_pool_.size());
         pos += write_varuint16(out.data(), pos, fp_size);
         pos += write_varuint16(out.data(), pos, variants_pool_size_());
         if (wide)
         {
            out[pos] = 0xFFu;
            write_u16_le(out.data() + pos + 1, root_type_id);
            pos += 3;
         }
         else
            pos += write_varuint8(out.data(), pos, root_type_id);

         // Sanity: header_size matched.
         if (pos != header_size)
            throw std::runtime_error(
               "psch: internal header-size mismatch");

         // name_pool
         std::memcpy(out.data() + pos, name_pool_.data(), name_pool_.size());
         pos += name_pool_.size();

         // Resolve container_idx → fields_offset using the picked
         // slot_stride. Walk containers in declaration order.
         std::vector<std::uint32_t> container_offsets(containers_.size());
         {
            std::uint32_t off = 0;
            for (std::size_t i = 0; i < containers_.size(); ++i)
            {
               container_offsets[i] = off;
               const auto& b = containers_[i];
               off += static_cast<std::uint32_t>(
                  3 + b.lookup.size() + b.fields.size() * slot_stride);
            }
         }

         // type_table — fixed 8-byte stride per entry.
         for (const auto& t : types_)
         {
            std::uint8_t* p = out.data() + pos;
            p[0] = static_cast<std::uint8_t>(t.k);
            std::memset(p + 1, 0, 7);
            switch (t.k)
            {
               case kind::bool_: case kind::u8:  case kind::u16:
               case kind::u32:   case kind::u64: case kind::u128:
               case kind::u256:  case kind::bytes:
                  break;
               case kind::bytes_n:
                  write_u32_le(p + 1, t.payload32);
                  break;
               case kind::container:
                  // payload32 = container_idx; resolve to byte offset.
                  write_u32_le(p + 1, container_offsets[t.payload32]);
                  p[5] = static_cast<std::uint8_t>(t.payload16 & 0xFF); // K
                  break;
               case kind::vector_:
                  if (wide) write_u16_le(p + 1, t.payload16);
                  else      p[1] = static_cast<std::uint8_t>(t.payload16 & 0xFF);
                  // length stored in payload32 (≤ 32-bit; spec says u40 — for
                  // schemas that need >2^32 elements, widen the payload via
                  // the wide flag in a future revision).
                  write_u32_le(p + 3, t.payload32);
                  break;
               case kind::list_:
                  if (wide) write_u16_le(p + 1, t.payload16);
                  else      p[1] = static_cast<std::uint8_t>(t.payload16 & 0xFF);
                  break;
               case kind::union_:
                  write_u32_le(p + 1, t.payload32);     // variants_offset
                  p[5] = static_cast<std::uint8_t>(t.payload16 & 0xFF); // variant_count
                  break;
               default:
                  throw std::runtime_error("psch: unsupported kind");
            }
            pos += type_stride;
         }

         // fields_pool — per container: header(3) + lookup(table_size) +
         // ordered_fields(K * slot_stride). Compact slots are 4 bytes
         // ({u16 name_off, u8 name_len, u8 type_id}); wide slots are 8
         // bytes ({u24 name_off, u8 name_len, u16 type_id, u16 pad}).
         for (const auto& blk : containers_)
         {
            out[pos++] = blk.K;
            out[pos++] = blk.seed;
            out[pos++] = blk.size_log2;
            std::memcpy(out.data() + pos, blk.lookup.data(),
                        blk.lookup.size());
            pos += blk.lookup.size();
            for (const auto& fld : blk.fields)
            {
               std::uint8_t* p = out.data() + pos;
               if (compact)
               {
                  write_u16_le(p, static_cast<std::uint16_t>(fld.name_off));
                  p[2] = fld.name_len;
                  p[3] = static_cast<std::uint8_t>(fld.type_id);
               }
               else
               {
                  std::uint32_t packed =
                     (fld.name_off & 0x00FFFFFFu) |
                     (static_cast<std::uint32_t>(fld.name_len & 0xFFu) << 24);
                  write_u32_le(p, packed);
                  write_u16_le(p + 4, fld.type_id);
                  p[6] = 0; p[7] = 0;  // pad
               }
               pos += slot_stride;
            }
         }

         // variants_pool
         for (const auto& vs : variants_blocks_)
         {
            for (auto vid : vs)
            {
               if (wide)
               {
                  write_u16_le(out.data() + pos, vid);
                  pos += 2;
               }
               else
               {
                  out[pos++] = static_cast<std::uint8_t>(vid);
               }
            }
         }

         if (pos != total)
            throw std::runtime_error(
               "psch: internal total-size mismatch");
         return out;
      }

     private:
      struct type_entry
      {
         kind          k;
         std::uint32_t payload32;   // length / fields_offset / variants_offset
         std::uint16_t payload16;   // elem_type_id / K / variant_count
         std::uint8_t  pad8;
         std::uint8_t  pad8b;
      };
      struct container_field
      {
         std::uint32_t name_off;
         std::uint8_t  name_len;
         std::uint16_t type_id;
      };
      struct container_block
      {
         std::uint8_t              K;
         std::uint8_t              seed;
         std::uint8_t              size_log2;
         std::vector<std::uint8_t> lookup;
         std::vector<container_field> fields;
      };

      std::vector<type_entry>             types_;
      std::vector<container_block>        containers_;
      std::vector<std::vector<std::uint16_t>> variants_blocks_;
      std::vector<std::uint8_t>           name_pool_;

      std::uint16_t next_id_() const noexcept
      {
         return static_cast<std::uint16_t>(types_.size() - 1);
      }
      std::uint32_t intern_name_(std::string_view n)
      {
         // Trivial: append. Dedup is a nice-to-have optimization.
         std::uint32_t off = static_cast<std::uint32_t>(name_pool_.size());
         name_pool_.insert(name_pool_.end(), n.begin(), n.end());
         return off;
      }
      std::size_t fields_pool_size_(std::size_t slot_stride) const noexcept
      {
         std::size_t s = 0;
         for (const auto& b : containers_)
            s += 3 + b.lookup.size() + b.fields.size() * slot_stride;
         return s;
      }
      std::size_t variants_pool_size_() const noexcept
      {
         std::size_t s = 0;
         bool        wide = types_.size() > 254;
         for (const auto& v : variants_blocks_)
            s += v.size() * (wide ? 2 : 1);
         return s;
      }
   };

   // ── view ────────────────────────────────────────────────────────────────

   struct field_lookup
   {
      std::uint8_t  ordered_index;
      std::uint16_t type_id;
   };

   class view
   {
     public:
      view() = default;
      view(const std::uint8_t* data, std::size_t size)
         : data_(data), size_(size)
      {
         parse_header_();
      }

      bool          valid()           const noexcept { return data_ != nullptr; }
      std::size_t   type_count()      const noexcept { return type_count_; }
      std::uint16_t root_type_id()    const noexcept { return root_; }
      bool          wide_type_ids()   const noexcept { return wide_ids_; }
      bool          compact_slots()   const noexcept { return compact_slots_; }
      std::size_t   slot_stride()     const noexcept
      {
         return compact_slots_ ? field_slot_stride_compact
                               : field_slot_stride_wide;
      }

      kind type_kind(std::uint16_t id) const
      {
         require_type_(id);
         return static_cast<kind>(type_table_[id * type_stride]);
      }

      // Container: K (number of fields), 0 if not a container.
      std::size_t container_field_count(std::uint16_t id) const
      {
         require_type_(id);
         const std::uint8_t* e = type_table_ + id * type_stride;
         if (static_cast<kind>(e[0]) != kind::container) return 0;
         return e[5];
      }

      // Container: field by ordinal index (0..K).
      std::pair<std::string_view, std::uint16_t>
      field_by_index(std::uint16_t id, std::size_t j) const
      {
         require_type_(id);
         const std::uint8_t* e = type_table_ + id * type_stride;
         if (static_cast<kind>(e[0]) != kind::container)
            throw std::runtime_error("psch: not a container");
         std::uint32_t fields_offset = read_u32_le(e + 1);
         std::uint8_t  K             = e[5];
         if (j >= K) throw std::out_of_range("psch::field_by_index");
         const std::uint8_t* blk =
            fields_pool_ + fields_offset;
         std::uint8_t  size_log2  = blk[2];
         std::size_t   table_size = std::size_t{1} << size_log2;
         const std::uint8_t* ord = blk + 3 + table_size;
         const std::uint8_t* slot = ord + j * slot_stride();
         return decode_slot_(slot);
      }

      // Container: field by name. O(1): one PHF lookup + 1 memcmp.
      std::optional<field_lookup>
      field_by_name(std::uint16_t id, std::string_view name) const
      {
         require_type_(id);
         const std::uint8_t* e = type_table_ + id * type_stride;
         if (static_cast<kind>(e[0]) != kind::container) return std::nullopt;
         std::uint32_t fields_offset = read_u32_le(e + 1);
         const std::uint8_t* blk = fields_pool_ + fields_offset;
         std::uint8_t  K          = blk[0];
         if (K == 0) return std::nullopt;
         std::uint8_t  seed       = blk[1];
         std::uint8_t  size_log2  = blk[2];
         std::size_t   table_size = std::size_t{1} << size_log2;
         std::uint64_t mask       = table_size - 1;

         std::uint64_t idx = hash_with_seed(name, seed) & mask;
         std::uint8_t  ord_idx = blk[3 + idx];
         if (ord_idx == 0xFFu) return std::nullopt;

         const std::uint8_t* ord  = blk + 3 + table_size;
         const std::uint8_t* slot = ord + ord_idx * slot_stride();
         auto decoded = decode_slot_(slot);
         std::string_view stored = decoded.first;
         if (stored.size() != name.size()) return std::nullopt;
         if (std::memcmp(stored.data(), name.data(), stored.size()) != 0)
            return std::nullopt;
         return field_lookup{ord_idx, decoded.second};
      }

      // Vector: elem type + length.
      std::pair<std::uint16_t, std::uint64_t>
      vector_elem_and_length(std::uint16_t id) const
      {
         require_type_(id);
         const std::uint8_t* e = type_table_ + id * type_stride;
         if (static_cast<kind>(e[0]) != kind::vector_)
            throw std::runtime_error("psch: not a vector");
         std::uint16_t elem = wide_ids_
            ? read_u16_le(e + 1)
            : static_cast<std::uint16_t>(e[1]);
         std::uint32_t len = read_u32_le(e + 3);
         return {elem, len};
      }

      // List: elem type.
      std::uint16_t list_elem_type(std::uint16_t id) const
      {
         require_type_(id);
         const std::uint8_t* e = type_table_ + id * type_stride;
         if (static_cast<kind>(e[0]) != kind::list_)
            throw std::runtime_error("psch: not a list");
         return wide_ids_
            ? read_u16_le(e + 1)
            : static_cast<std::uint16_t>(e[1]);
      }

      // bytes_n: length.
      std::uint32_t bytes_n_length(std::uint16_t id) const
      {
         require_type_(id);
         const std::uint8_t* e = type_table_ + id * type_stride;
         if (static_cast<kind>(e[0]) != kind::bytes_n)
            throw std::runtime_error("psch: not bytes_n");
         return read_u32_le(e + 1);
      }

      // Union: variant count.
      std::size_t union_variant_count(std::uint16_t id) const
      {
         require_type_(id);
         const std::uint8_t* e = type_table_ + id * type_stride;
         if (static_cast<kind>(e[0]) != kind::union_)
            throw std::runtime_error("psch: not a union");
         return e[5];
      }
      std::uint16_t union_variant(std::uint16_t id, std::size_t n) const
      {
         require_type_(id);
         const std::uint8_t* e = type_table_ + id * type_stride;
         if (static_cast<kind>(e[0]) != kind::union_)
            throw std::runtime_error("psch: not a union");
         std::uint32_t off = read_u32_le(e + 1);
         std::uint8_t  cnt = e[5];
         if (n >= cnt) throw std::out_of_range("psch::union_variant");
         const std::uint8_t* p = variants_pool_ + off + n * (wide_ids_ ? 2 : 1);
         return wide_ids_ ? read_u16_le(p) : static_cast<std::uint16_t>(p[0]);
      }

     private:
      void require_type_(std::uint16_t id) const
      {
         if (id >= type_count_)
            throw std::out_of_range("psch::type_id");
      }

      // Decode one slot into (name, type_id) honoring slot stride.
      std::pair<std::string_view, std::uint16_t>
      decode_slot_(const std::uint8_t* slot) const
      {
         std::uint32_t name_off;
         std::uint8_t  name_len;
         std::uint16_t tid;
         if (compact_slots_)
         {
            name_off = read_u16_le(slot);
            name_len = slot[2];
            tid      = static_cast<std::uint16_t>(slot[3]);
         }
         else
         {
            std::uint32_t packed = read_u32_le(slot);
            name_off = packed & 0x00FFFFFFu;
            name_len =
               static_cast<std::uint8_t>((packed >> 24) & 0xFFu);
            tid      = read_u16_le(slot + 4);
         }
         std::string_view name(
            reinterpret_cast<const char*>(name_pool_ + name_off),
            name_len);
         return {name, tid};
      }

      void parse_header_()
      {
         if (size_ < 4 + 1) throw std::runtime_error("psch: short header");
         if (std::memcmp(data_, magic.data(), 4) != 0)
            throw std::runtime_error("psch: bad magic");
         std::size_t pos = 4;
         std::uint8_t flags = data_[pos++];
         wide_ids_      = (flags & flag_wide_type_ids) != 0;
         compact_slots_ = (flags & flag_compact_slots) != 0;

         std::uint64_t v;
         std::size_t   nb;

         nb = read_varuint8(data_ + pos, size_ - pos, v);
         if (nb == 0) throw std::runtime_error("psch: bad type_count");
         type_count_ = static_cast<std::size_t>(v);
         pos += nb;

         nb = read_varuint16(data_ + pos, size_ - pos, v);
         if (nb == 0) throw std::runtime_error("psch: bad name_pool_size");
         std::size_t name_pool_size = static_cast<std::size_t>(v);
         pos += nb;

         nb = read_varuint16(data_ + pos, size_ - pos, v);
         if (nb == 0) throw std::runtime_error("psch: bad fields_pool_size");
         std::size_t fields_pool_size = static_cast<std::size_t>(v);
         pos += nb;

         nb = read_varuint16(data_ + pos, size_ - pos, v);
         if (nb == 0) throw std::runtime_error("psch: bad variants_pool_size");
         std::size_t variants_pool_size = static_cast<std::size_t>(v);
         pos += nb;

         if (wide_ids_)
         {
            if (pos + 3 > size_) throw std::runtime_error("psch: bad root");
            if (data_[pos] != 0xFFu)
               throw std::runtime_error("psch: wide root expected");
            root_ = read_u16_le(data_ + pos + 1);
            pos += 3;
         }
         else
         {
            nb = read_varuint8(data_ + pos, size_ - pos, v);
            if (nb == 0) throw std::runtime_error("psch: bad root");
            root_ = static_cast<std::uint16_t>(v);
            pos += nb;
         }

         name_pool_      = data_ + pos;
         pos            += name_pool_size;
         type_table_     = data_ + pos;
         pos            += type_count_ * type_stride;
         fields_pool_    = data_ + pos;
         pos            += fields_pool_size;
         variants_pool_  = data_ + pos;
         pos            += variants_pool_size;

         if (pos != size_)
            throw std::runtime_error("psch: trailing bytes");
      }

      const std::uint8_t* data_           = nullptr;
      std::size_t         size_           = 0;
      const std::uint8_t* name_pool_      = nullptr;
      const std::uint8_t* type_table_     = nullptr;
      const std::uint8_t* fields_pool_    = nullptr;
      const std::uint8_t* variants_pool_  = nullptr;
      std::size_t         type_count_     = 0;
      std::uint16_t       root_           = 0;
      bool                wide_ids_       = false;
      bool                compact_slots_  = false;
   };

}  // namespace psio::psch
