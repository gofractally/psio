#pragma once
//
// psio/capnp.hpp — Cap'n Proto format tag (single-segment, unpacked).
//
// Byte-identical to psio2's `capnp_pack` / `capnp_unpack` on the shape
// set both support. Single-segment flat-array messages only, no packed
// encoding, no far pointers — matching capnp's messageToFlatArray output.
//
// Supported shapes:
//   - primitives (arithmetic + bool), enums
//   - std::string (capnp Text: u8 list with trailing NUL)
//   - std::vector / std::array of: arithmetic / bool / enum / string /
//     nested tables
//   - std::optional<T> as a record field (null ptr = None,
//     list<T> length-1 = Some)
//   - std::variant<Ts...> as a record field — pointer slot to a
//     sub-struct {u16 tag, ptr per alt}; live alt is encoded into
//     its slot via the same length-1-list wrapper used for optional
//   - reflected records (root or nested, recursive)
//
// Wire reference: https://capnproto.org/encoding.html. Field layout
// allocates sequentially in PSIO_REFLECT order using capnp's slot
// allocator (smaller-before-larger into bit-holes left by earlier
// slots), which produces the same offsets the upstream capnp compiler
// emits for sequentially-numbered ordinals.
//
// Not yet supported: std::tuple (no native capnp counterpart),
// per-field binary_format adapter overrides, top-level adapter
// dispatch. variant uses the length-1-list wrapper convention rather
// than capnp's native union encoding (which would require slot-allocator
// awareness of the discriminant + shared per-alt storage); native
// unions are a follow-up.

#include <psio/cpo.hpp>
#include <psio/error.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/adapter.hpp>
#include <psio/reflect.hpp>
#include <psio/validate_strict_walker.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace psio {

   struct capnp;

   namespace detail::capnp_impl {

      // ── Shape classifiers ─────────────────────────────────────────────────

      template <typename T>
      struct is_vector : std::false_type {};
      template <typename E, typename A>
      struct is_vector<std::vector<E, A>> : std::true_type
      {
         using element_type = E;
      };

      // std::array<T, N> encodes as a capnp fixed-count List (or Data for
      // byte arrays). We materialize via std::vector on the wire — no
      // distinct capnp shape exists for fixed-size arrays, so the list
      // pointer carries the count as data.
      template <typename T>
      struct is_array : std::false_type {};
      template <typename E, std::size_t N>
      struct is_array<std::array<E, N>> : std::true_type
      {
         using element_type             = E;
         static constexpr std::size_t n = N;
      };

      template <typename T>
      struct is_optional : std::false_type {};
      template <typename E>
      struct is_optional<std::optional<E>> : std::true_type
      {
         using element_type = E;
      };

      template <typename T>
      struct is_variant : std::false_type {};
      template <typename... Ts>
      struct is_variant<std::variant<Ts...>> : std::true_type {};

      template <typename T>
      concept Record = ::psio::Reflected<T> && !std::is_enum_v<T>;

      template <typename F>
      consteval bool is_data_type()
      {
         return std::is_arithmetic_v<F> || std::is_enum_v<F>;
      }

      // Byte size for a data-section field. bool is 0 (sentinel = 1 bit).
      template <typename F>
      consteval uint32_t data_byte_size()
      {
         if constexpr (std::is_same_v<F, bool>)
            return 0;
         else if constexpr (std::is_enum_v<F>)
            return sizeof(std::underlying_type_t<F>);
         else if constexpr (std::is_arithmetic_v<F>)
            return sizeof(F);
         else
            return 0;
      }

      // ── Wire helpers ──────────────────────────────────────────────────────

      inline uint64_t read_u64(const uint8_t* p)
      {
         uint64_t v;
         std::memcpy(&v, p, 8);
         return v;
      }

      inline uint32_t read_u32(const uint8_t* p)
      {
         uint32_t v;
         std::memcpy(&v, p, 4);
         return v;
      }

      // Bits [2:31] of a pointer word = signed 30-bit offset in words.
      inline int32_t ptr_offset(uint64_t word)
      {
         return static_cast<int32_t>(static_cast<uint32_t>(word) & ~3u) >> 2;
      }

      struct capnp_ptr
      {
         const uint8_t* data       = nullptr;
         uint16_t       data_words = 0;
         uint16_t       ptr_count  = 0;
         bool operator==(std::nullptr_t) const { return data == nullptr; }
      };

      inline capnp_ptr resolve_struct_ptr(const uint8_t* ptr_loc)
      {
         uint64_t word = read_u64(ptr_loc);
         if (word == 0)
            return {};
         int32_t  off = ptr_offset(word);
         uint16_t dw  = static_cast<uint16_t>((word >> 32) & 0xFFFF);
         uint16_t pc  = static_cast<uint16_t>((word >> 48) & 0xFFFF);
         return {ptr_loc + 8 + static_cast<int64_t>(off) * 8, dw, pc};
      }

      struct list_info
      {
         const uint8_t* data            = nullptr;
         uint32_t       count           = 0;
         uint32_t       elem_stride     = 0;
         uint16_t       elem_data_words = 0;
         uint16_t       elem_ptr_count  = 0;
      };

      inline list_info resolve_list_ptr(const uint8_t* ptr_loc)
      {
         uint64_t word = read_u64(ptr_loc);
         if (word == 0)
            return {};

         int32_t  off            = ptr_offset(word);
         uint8_t  elem_sz_tag    = static_cast<uint8_t>((word >> 32) & 7);
         uint32_t count_or_words = static_cast<uint32_t>(word >> 35);

         const uint8_t* list_start =
            ptr_loc + 8 + static_cast<int64_t>(off) * 8;

         if (elem_sz_tag == 7)
         {
            // Composite list: first word is element-layout tag.
            uint64_t tag   = read_u64(list_start);
            uint32_t count = static_cast<uint32_t>(tag) >> 2;
            uint16_t dw    = static_cast<uint16_t>((tag >> 32) & 0xFFFF);
            uint16_t pc    = static_cast<uint16_t>((tag >> 48) & 0xFFFF);
            return {list_start + 8, count, (dw + pc) * 8u, dw, pc};
         }

         // 0=void 1=bit 2=byte 3=2B 4=4B 5=8B 6=ptr
         static constexpr uint32_t elem_sizes[] = {0, 0, 1, 2, 4, 8, 8, 0};
         return {list_start, count_or_words, elem_sizes[elem_sz_tag], 0, 0};
      }

      inline const uint8_t* ptr_section(capnp_ptr p)
      {
         return p.data + static_cast<uint32_t>(p.data_words) * 8;
      }

      inline const uint8_t* ptr_slot(capnp_ptr p, uint32_t idx)
      {
         return ptr_section(p) + idx * 8;
      }

      inline std::string_view read_text(const uint8_t* ptr_loc)
      {
         uint64_t word = read_u64(ptr_loc);
         if (word == 0)
            return {};
         int32_t  off   = ptr_offset(word);
         uint32_t count = static_cast<uint32_t>(word >> 35);
         auto*    data  = ptr_loc + 8 + static_cast<int64_t>(off) * 8;
         if (count == 0)
            return {};
         return {reinterpret_cast<const char*>(data), count - 1};
      }

      // ── Layout computation (data section slot allocator) ──────────────────
      //
      // Matches capnp's ordinal allocator: larger slots first, then smaller
      // ones fill bit-holes left behind. Our input is PSIO_REFLECT order
      // (already monotonic), so the resulting slots match what the official
      // compiler emits for sequential ordinals.

      template <typename T>
      struct capnp_layout
      {
         struct field_loc
         {
            bool     is_ptr    = false;
            uint32_t offset    = 0;
            uint8_t  bit_index = 0;
         };

         struct layout_result
         {
            field_loc fields[64] = {};
            uint16_t  data_words = 0;
            uint16_t  ptr_count  = 0;
         };

         static consteval uint32_t alloc_bits(layout_result& r,
                                              bool (&occupied)[2048],
                                              uint32_t bit_count,
                                              uint32_t bit_align)
         {
            for (uint32_t bit = 0;; bit += bit_align)
            {
               uint32_t end_bit      = bit + bit_count;
               uint32_t words_needed = (end_bit + 63) / 64;
               if (words_needed > r.data_words)
                  r.data_words = words_needed;

               bool ok = true;
               for (uint32_t b = bit; b < end_bit; ++b)
               {
                  if (occupied[b])
                  {
                     ok = false;
                     break;
                  }
               }
               if (ok)
               {
                  for (uint32_t b = bit; b < end_bit; ++b)
                     occupied[b] = true;
                  return bit;
               }
            }
         }

         template <typename F>
         static consteval field_loc alloc_type_slot(layout_result& r,
                                                    bool (&occupied)[2048])
         {
            if constexpr (!is_data_type<F>())
            {
               return {true, r.ptr_count++, 0};
            }
            else if constexpr (std::is_same_v<F, bool>)
            {
               uint32_t bit = alloc_bits(r, occupied, 1, 1);
               return {false, bit / 8,
                       static_cast<uint8_t>(bit % 8)};
            }
            else
            {
               uint32_t sz  = data_byte_size<F>();
               uint32_t bit = alloc_bits(r, occupied, sz * 8, sz * 8);
               return {false, bit / 8, 0};
            }
         }

         static consteval layout_result compute()
         {
            using R                 = ::psio::reflect<T>;
            constexpr std::size_t N = R::member_count;
            layout_result         r{};
            bool                  occupied[2048] = {};

            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               ((r.fields[Is] = alloc_type_slot<typename R::template member_type<Is>>(
                    r, occupied)),
                ...);
            }(std::make_index_sequence<N>{});

            return r;
         }

         static constexpr auto      result     = compute();
         static constexpr uint16_t  data_words = result.data_words;
         static constexpr uint16_t  ptr_count  = result.ptr_count;
         static consteval field_loc loc(std::size_t i)
         {
            return result.fields[i];
         }
      };

      // ── Default value extraction (C++ member initializers) ────────────────

      template <typename T, std::size_t I>
      auto field_default()
      {
         using R      = ::psio::reflect<T>;
         using F      = typename R::template member_type<I>;
         static const F val = [] {
            T obj{};
            return obj.*(R::template member_pointer<I>);
         }();
         return val;
      }

      template <typename F>
      F xor_default(F wire, F def)
      {
         if constexpr (std::is_same_v<F, bool>)
         {
            return wire != def;
         }
         else if constexpr (std::is_same_v<F, float>)
         {
            uint32_t w, d;
            std::memcpy(&w, &wire, 4);
            std::memcpy(&d, &def, 4);
            w ^= d;
            float r;
            std::memcpy(&r, &w, 4);
            return r;
         }
         else if constexpr (std::is_same_v<F, double>)
         {
            uint64_t w, d;
            std::memcpy(&w, &wire, 8);
            std::memcpy(&d, &def, 8);
            w ^= d;
            double r;
            std::memcpy(&r, &w, 8);
            return r;
         }
         else if constexpr (std::is_enum_v<F>)
         {
            using U = std::underlying_type_t<F>;
            return static_cast<F>(static_cast<U>(wire) ^
                                  static_cast<U>(def));
         }
         else
         {
            return static_cast<F>(wire ^ def);
         }
      }

      // ── Word buffer (back-to-front flat builder) ──────────────────────────

      class capnp_word_buf
      {
         std::vector<uint64_t> words_;

        public:
         explicit capnp_word_buf(uint32_t reserve_words = 0)
         {
            if (reserve_words)
               words_.reserve(reserve_words);
         }

         uint32_t alloc(uint32_t n)
         {
            uint32_t off = static_cast<uint32_t>(words_.size());
            words_.resize(off + n, 0);
            return off;
         }

         uint8_t* byte_ptr(uint32_t word_idx)
         {
            return reinterpret_cast<uint8_t*>(&words_[word_idx]);
         }

         void write_struct_ptr(uint32_t at, uint32_t target,
                               uint16_t dw, uint16_t pc)
         {
            int32_t  off  = static_cast<int32_t>(target) -
                           static_cast<int32_t>(at) - 1;
            uint64_t word = (uint64_t(pc) << 48) |
                            (uint64_t(dw) << 32) |
                            (static_cast<uint32_t>(off << 2) & 0xFFFFFFFFu);
            words_[at] = word;
         }

         void write_list_ptr(uint32_t at, uint32_t target, uint8_t elem_sz,
                             uint32_t count)
         {
            int32_t  off  = static_cast<int32_t>(target) -
                           static_cast<int32_t>(at) - 1;
            uint64_t word = (uint64_t(count) << 35) |
                            (uint64_t(elem_sz) << 32) |
                            (static_cast<uint32_t>(off << 2) & 0xFFFFFFFFu) |
                            1u;
            words_[at] = word;
         }

         void write_composite_tag(uint32_t at, uint32_t count,
                                  uint16_t dw, uint16_t pc)
         {
            words_[at] = (uint64_t(pc) << 48) | (uint64_t(dw) << 32) |
                         (static_cast<uint32_t>(count) << 2);
         }

         template <typename V>
         void write_field(uint32_t struct_start, uint32_t byte_offset, V val)
         {
            std::memcpy(byte_ptr(struct_start) + byte_offset, &val,
                        sizeof(val));
         }

         void write_bool(uint32_t struct_start, uint32_t byte_offset,
                         uint8_t bit, bool val)
         {
            auto* p = byte_ptr(struct_start) + byte_offset;
            if (val)
               *p |= (1u << bit);
         }

         void write_text(uint32_t ptr_word, std::string_view text)
         {
            uint32_t len     = static_cast<uint32_t>(text.size());
            uint32_t total   = len + 1;
            uint32_t n_words = (total + 7) / 8;
            uint32_t target  = alloc(n_words);
            write_list_ptr(ptr_word, target, 2, total);
            std::memcpy(byte_ptr(target), text.data(), len);
            byte_ptr(target)[len] = 0;
         }

         std::vector<char> finish()
         {
            std::vector<char> out;
            finish_into(out);
            return out;
         }

         //  Sink form — write the finished segmented-frame bytes into
         //  a caller-supplied vector, reusing its capacity.  Used by
         //  the encode-into-sink CPO so a hot-loop caller keeps one
         //  output vector across iterations.
         void finish_into(std::vector<char>& sink)
         {
            uint32_t seg_size = static_cast<uint32_t>(words_.size());
            sink.resize(8 + static_cast<std::size_t>(seg_size) * 8);
            uint32_t zero = 0;
            std::memcpy(sink.data(), &zero, 4);
            std::memcpy(sink.data() + 4, &seg_size, 4);
            std::memcpy(sink.data() + 8, words_.data(),
                        static_cast<std::size_t>(seg_size) * 8);
         }
      };

      // Counting builder for size_of / pre-sizing capnp_word_buf.
      class capnp_word_counter
      {
         uint32_t words_ = 0;

        public:
         static constexpr bool counts_only = true;

         uint32_t alloc(uint32_t n)
         {
            uint32_t off = words_;
            words_ += n;
            return off;
         }

         uint8_t* byte_ptr(uint32_t) { return nullptr; }
         void     write_struct_ptr(uint32_t, uint32_t, uint16_t, uint16_t) {}
         void     write_list_ptr(uint32_t, uint32_t, uint8_t, uint32_t) {}
         void     write_composite_tag(uint32_t, uint32_t, uint16_t, uint16_t)
         {
         }
         template <typename V>
         void write_field(uint32_t, uint32_t, V)
         {
         }
         void write_bool(uint32_t, uint32_t, uint8_t, bool) {}

         void write_text(uint32_t, std::string_view text)
         {
            uint32_t total = static_cast<uint32_t>(text.size()) + 1;
            alloc((total + 7) / 8);
         }

         uint32_t finish_size() const { return 8u + words_ * 8u; }
         uint32_t word_count() const { return words_; }
      };

      template <typename Builder, typename = void>
      struct is_counts_only_builder : std::false_type {};
      template <typename Builder>
      struct is_counts_only_builder<Builder,
                                    std::void_t<decltype(Builder::counts_only)>>
         : std::bool_constant<Builder::counts_only> {};
      template <typename Builder>
      inline constexpr bool counts_only_builder_v =
         is_counts_only_builder<Builder>::value;

      // ── Scalar list element-size tag ──────────────────────────────────────

      template <typename E>
      consteval uint8_t scalar_elem_tag()
      {
         if constexpr (std::is_same_v<E, bool>)
            return 1;
         else if constexpr (sizeof(E) == 1)
            return 2;
         else if constexpr (sizeof(E) == 2)
            return 3;
         else if constexpr (sizeof(E) == 4)
            return 4;
         else if constexpr (sizeof(E) == 8)
            return 5;
         else
            static_assert(!sizeof(E*), "capnp: unsupported scalar size");
      }

      // ── Packer ────────────────────────────────────────────────────────────

      template <typename T, typename Builder>
         requires Record<T>
      void pack_struct(Builder& buf, uint32_t data_start, uint32_t ptrs_start,
                       const T& obj);

      // Pack any contiguous sequence (std::vector or std::array) as a
      // capnp list. Factored out so std::array and std::vector share
      // every list-layout branch.
      template <typename E, typename Builder>
      void pack_sequence(Builder& buf, uint32_t ptr_word, const E* data,
                         uint32_t count)
      {
         if (count == 0)
            return;

         if constexpr (is_vector<E>::value)
         {
            uint32_t outer = buf.alloc(count);
            buf.write_list_ptr(ptr_word, outer, 6, count);
            for (uint32_t i = 0; i < count; ++i)
               pack_sequence<typename is_vector<E>::element_type>(
                  buf, outer + i,
                  data[i].empty() ? nullptr : data[i].data(),
                  static_cast<uint32_t>(data[i].size()));
         }
         else if constexpr (std::is_same_v<E, std::string>)
         {
            uint32_t outer = buf.alloc(count);
            buf.write_list_ptr(ptr_word, outer, 6, count);
            for (uint32_t i = 0; i < count; ++i)
               buf.write_text(outer + i, data[i]);
         }
         else if constexpr (Record<E>)
         {
            using EL           = capnp_layout<E>;
            uint32_t words_per = EL::data_words + EL::ptr_count;
            uint32_t tag       = buf.alloc(1);
            buf.write_composite_tag(tag, count, EL::data_words,
                                    EL::ptr_count);
            uint32_t first_elem = buf.alloc(count * words_per);
            buf.write_list_ptr(ptr_word, tag, 7, count * words_per);
            for (uint32_t i = 0; i < count; ++i)
            {
               uint32_t elem_start = first_elem + i * words_per;
               pack_struct(buf, elem_start, elem_start + EL::data_words,
                           data[i]);
            }
         }
         else if constexpr (std::is_same_v<E, bool>)
         {
            uint32_t n_words = (count + 7) / 8;
            uint32_t target  = buf.alloc(n_words);
            buf.write_list_ptr(ptr_word, target, 1, count);
            if constexpr (!counts_only_builder_v<Builder>)
            {
               for (uint32_t i = 0; i < count; ++i)
                  if (data[i])
                     buf.byte_ptr(target)[i / 8] |= (1u << (i % 8));
            }
         }
         else
         {
            constexpr uint32_t elem_sz     = sizeof(E);
            uint32_t           total_bytes = count * elem_sz;
            uint32_t           n_words     = (total_bytes + 7) / 8;
            uint32_t           target      = buf.alloc(n_words);

            if constexpr (std::is_enum_v<E>)
            {
               using U = std::underlying_type_t<E>;
               buf.write_list_ptr(ptr_word, target, scalar_elem_tag<U>(),
                                  count);
               if constexpr (!counts_only_builder_v<Builder>)
                  for (uint32_t i = 0; i < count; ++i)
                  {
                     U val = static_cast<U>(data[i]);
                     std::memcpy(buf.byte_ptr(target) + i * sizeof(U),
                                 &val, sizeof(U));
                  }
            }
            else
            {
               buf.write_list_ptr(ptr_word, target, scalar_elem_tag<E>(),
                                  count);
               if constexpr (!counts_only_builder_v<Builder>)
                  std::memcpy(buf.byte_ptr(target), data, total_bytes);
            }
         }
      }

      template <typename E, typename Builder>
      void pack_vec(Builder& buf, uint32_t ptr_word,
                    const std::vector<E>& vec)
      {
         pack_sequence<E>(buf, ptr_word,
                          vec.empty() ? nullptr : vec.data(),
                          static_cast<uint32_t>(vec.size()));
      }

      template <typename E, std::size_t N, typename Builder>
      void pack_arr(Builder& buf, uint32_t ptr_word,
                    const std::array<E, N>& arr)
      {
         pack_sequence<E>(buf, ptr_word,
                          N == 0 ? nullptr : arr.data(),
                          static_cast<uint32_t>(N));
      }

      template <typename T, std::size_t I, typename Builder>
      void pack_one_field(Builder& buf, uint32_t data_start,
                          uint32_t ptrs_start, const T& obj)
      {
         using R                  = ::psio::reflect<T>;
         using F                  = typename R::template member_type<I>;
         constexpr auto loc       = capnp_layout<T>::loc(I);
         const F&       field_val = obj.*(R::template member_pointer<I>);

         if constexpr (loc.is_ptr)
         {
            if constexpr (std::is_same_v<F, std::string>)
            {
               if (!field_val.empty())
                  buf.write_text(ptrs_start + loc.offset, field_val);
            }
            else if constexpr (is_vector<F>::value)
            {
               pack_vec(buf, ptrs_start + loc.offset, field_val);
            }
            else if constexpr (is_array<F>::value)
            {
               pack_arr(buf, ptrs_start + loc.offset, field_val);
            }
            else if constexpr (is_optional<F>::value)
            {
               // None → null pointer (slot left zero by alloc()).
               // Some(x) → list<T> with one element. Reuses the
               // pack_sequence<T> machinery; works uniformly across
               // primitives, strings, lists, and nested records.
               if (field_val.has_value())
               {
                  using E = typename is_optional<F>::element_type;
                  pack_sequence<E>(buf, ptrs_start + loc.offset,
                                    &(*field_val), 1);
               }
            }
            else if constexpr (is_variant<F>::value)
            {
               // variant<T0, T1, ..., TN-1> → pointer to a sub-struct
               // with 1 data word (u16 tag at byte 0) and N pointer
               // slots. Each alt is encoded into its slot via the
               // pack_sequence<A>(..., 1) wrapper, so primitives are
               // wrapped in a length-1 list and pointer types
               // (string/vec/record) get a direct pointer.
               constexpr std::size_t N    = std::variant_size_v<F>;
               constexpr uint16_t    dw   = 1;
               constexpr uint16_t    pc   = static_cast<uint16_t>(N);
               uint32_t sub_data = buf.alloc(dw + pc);
               uint32_t sub_ptrs = sub_data + dw;
               buf.write_struct_ptr(ptrs_start + loc.offset, sub_data,
                                     dw, pc);
               // Tag at byte 0 of sub-struct's data word.
               uint16_t tag = static_cast<uint16_t>(field_val.index());
               buf.write_field(sub_data, 0, tag);
               // Live alt → pack_sequence<A>(sub_ptrs[idx], &alt, 1).
               std::visit(
                  [&](const auto& alt) {
                     using A = std::remove_cvref_t<decltype(alt)>;
                     pack_sequence<A>(buf,
                                       sub_ptrs +
                                          static_cast<uint32_t>(
                                             field_val.index()),
                                       &alt, 1);
                  },
                  field_val);
            }
            else if constexpr (Record<F>)
            {
               using FL = capnp_layout<F>;
               uint32_t cd =
                  buf.alloc(FL::data_words + FL::ptr_count);
               uint32_t cp_ = cd + FL::data_words;
               buf.write_struct_ptr(ptrs_start + loc.offset, cd,
                                    FL::data_words, FL::ptr_count);
               pack_struct(buf, cd, cp_, field_val);
            }
            else
            {
               static_assert(!sizeof(F*),
                             "capnp: unsupported pointer field type");
            }
         }
         else
         {
            if constexpr (std::is_same_v<F, bool>)
            {
               F def = field_default<T, I>();
               buf.write_bool(data_start, loc.offset, loc.bit_index,
                              xor_default(field_val, def));
            }
            else if constexpr (std::is_enum_v<F>)
            {
               using U    = std::underlying_type_t<F>;
               F     def  = field_default<T, I>();
               U     wire = xor_default(static_cast<U>(field_val),
                                    static_cast<U>(def));
               buf.write_field(data_start, loc.offset, wire);
            }
            else if constexpr (std::is_arithmetic_v<F>)
            {
               F def = field_default<T, I>();
               buf.write_field(data_start, loc.offset,
                               xor_default(field_val, def));
            }
            else
            {
               static_assert(!sizeof(F*),
                             "capnp: unsupported data field type");
            }
         }
      }

      template <typename T, typename Builder>
         requires Record<T>
      void pack_struct(Builder& buf, uint32_t data_start, uint32_t ptrs_start,
                       const T& obj)
      {
         using R                 = ::psio::reflect<T>;
         constexpr std::size_t N = R::member_count;
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            (pack_one_field<T, Is>(buf, data_start, ptrs_start, obj), ...);
         }(std::make_index_sequence<N>{});
      }

      // ── Unpacker ──────────────────────────────────────────────────────────

      template <typename T>
         requires Record<T>
      void unpack_struct(capnp_ptr p, T& obj);

      // Unpack a capnp list body into a contiguous C++ sequence (vector
      // or array). `out_data` is the storage, `out_n` its element count.
      template <typename E>
      void unpack_sequence(list_info info, E* out_data, uint32_t out_n)
      {
         if (!info.data || info.count == 0 || out_n == 0)
            return;
         uint32_t n = std::min(info.count, out_n);

         if constexpr (is_vector<E>::value)
         {
            using Inner = typename is_vector<E>::element_type;
            for (uint32_t i = 0; i < n; ++i)
            {
               auto inner = resolve_list_ptr(info.data + i * 8);
               out_data[i].resize(inner.count);
               unpack_sequence<Inner>(inner,
                                      inner.count == 0 ? nullptr
                                                       : out_data[i].data(),
                                      inner.count);
            }
         }
         else if constexpr (std::is_same_v<E, std::string>)
         {
            for (uint32_t i = 0; i < n; ++i)
            {
               auto sv     = read_text(info.data + i * 8);
               out_data[i] = std::string(sv);
            }
         }
         else if constexpr (Record<E>)
         {
            for (uint32_t i = 0; i < n; ++i)
            {
               capnp_ptr elem{info.data + i * info.elem_stride,
                              info.elem_data_words, info.elem_ptr_count};
               unpack_struct<E>(elem, out_data[i]);
            }
         }
         else if constexpr (std::is_same_v<E, bool>)
         {
            for (uint32_t i = 0; i < n; ++i)
               out_data[i] = (info.data[i / 8] >> (i % 8)) & 1;
         }
         else if constexpr (std::is_enum_v<E>)
         {
            using U = std::underlying_type_t<E>;
            for (uint32_t i = 0; i < n; ++i)
            {
               U v;
               std::memcpy(&v, info.data + i * sizeof(U), sizeof(U));
               out_data[i] = static_cast<E>(v);
            }
         }
         else
         {
            std::memcpy(out_data, info.data,
                        static_cast<std::size_t>(n) * sizeof(E));
         }
      }

      template <typename E>
      std::vector<E> unpack_vec(list_info info)
      {
         std::vector<E> result;
         if (!info.data || info.count == 0)
            return result;
         result.resize(info.count);
         unpack_sequence<E>(info, result.data(), info.count);
         return result;
      }

      template <typename E, std::size_t N>
      std::array<E, N> unpack_arr(list_info info)
      {
         std::array<E, N> result{};
         if (!info.data || info.count == 0)
            return result;
         unpack_sequence<E>(info, result.data(), static_cast<uint32_t>(N));
         return result;
      }

      template <typename T, std::size_t I>
      void unpack_one_field(capnp_ptr p, T& obj)
      {
         using R                  = ::psio::reflect<T>;
         using F                  = typename R::template member_type<I>;
         constexpr auto loc       = capnp_layout<T>::loc(I);
         F&             field_ref = obj.*(R::template member_pointer<I>);

         if constexpr (loc.is_ptr)
         {
            const uint8_t* slot = ptr_slot(p, loc.offset);
            if constexpr (std::is_same_v<F, std::string>)
            {
               auto sv   = read_text(slot);
               field_ref = std::string(sv);
            }
            else if constexpr (is_vector<F>::value)
            {
               using E   = typename is_vector<F>::element_type;
               field_ref = unpack_vec<E>(resolve_list_ptr(slot));
            }
            else if constexpr (is_array<F>::value)
            {
               using E    = typename is_array<F>::element_type;
               constexpr std::size_t N = is_array<F>::n;
               field_ref = unpack_arr<E, N>(resolve_list_ptr(slot));
            }
            else if constexpr (is_optional<F>::value)
            {
               // Wire form: null pointer = None; non-null = list<E> of
               // length 1. Anything other than count 0 or 1 is treated
               // as None to keep the codec total — strict validation
               // can flag the malformed case separately.
               using E = typename is_optional<F>::element_type;
               auto info = resolve_list_ptr(slot);
               if (!info.data || info.count == 0)
                  field_ref = std::nullopt;
               else
               {
                  E tmp{};
                  unpack_sequence<E>(info, &tmp, 1);
                  field_ref = std::move(tmp);
               }
            }
            else if constexpr (is_variant<F>::value)
            {
               // Resolve the sub-struct, read u16 tag from data word,
               // then unpack_sequence<A>(slot, &tmp, 1) the active
               // alt.
               constexpr std::size_t N = std::variant_size_v<F>;
               auto sub = resolve_struct_ptr(slot);
               if (sub == nullptr)
               {
                  field_ref = F{};  // default-init: alt 0
                  return;
               }
               uint16_t tag = 0;
               std::memcpy(&tag, sub.data, 2);
               const uint32_t idx = static_cast<uint32_t>(tag);
               if (idx >= N)
               {
                  field_ref = F{};
                  return;
               }
               const uint8_t* alt_slot =
                  ptr_section(sub) + idx * 8;
               [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                  ((idx == Is
                       ? (field_ref =
                             F{std::in_place_index<Is>,
                                ([&] {
                                   using A =
                                      std::variant_alternative_t<Is, F>;
                                   A    tmp{};
                                   auto info =
                                      resolve_list_ptr(alt_slot);
                                   unpack_sequence<A>(info, &tmp, 1);
                                   return tmp;
                                }())},
                          true)
                       : false) ||
                   ...);
               }(std::make_index_sequence<N>{});
            }
            else if constexpr (Record<F>)
            {
               unpack_struct<F>(resolve_struct_ptr(slot), field_ref);
            }
            else
            {
               static_assert(!sizeof(F*),
                             "capnp: unsupported pointer field type");
            }
         }
         else
         {
            F def = field_default<T, I>();
            if constexpr (std::is_same_v<F, bool>)
            {
               bool wire = (p.data[loc.offset] >> loc.bit_index) & 1;
               field_ref = xor_default(wire, def);
            }
            else if constexpr (std::is_enum_v<F>)
            {
               using U = std::underlying_type_t<F>;
               U wire;
               std::memcpy(&wire, p.data + loc.offset, sizeof(U));
               field_ref = xor_default(static_cast<F>(wire), def);
            }
            else if constexpr (std::is_arithmetic_v<F>)
            {
               F wire;
               std::memcpy(&wire, p.data + loc.offset, sizeof(F));
               field_ref = xor_default(wire, def);
            }
            else
            {
               static_assert(!sizeof(F*),
                             "capnp: unsupported data field type");
            }
         }
      }

      template <typename T>
         requires Record<T>
      void unpack_struct(capnp_ptr p, T& obj)
      {
         if (p == nullptr)
            return;
         using R                 = ::psio::reflect<T>;
         constexpr std::size_t N = R::member_count;
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            (unpack_one_field<T, Is>(p, obj), ...);
         }(std::make_index_sequence<N>{});
      }

      // ── Message-level helpers ─────────────────────────────────────────────

      template <typename T>
         requires Record<T>
      capnp_ptr root_of(const void* buf)
      {
         auto*    msg          = static_cast<const uint8_t*>(buf);
         uint32_t seg_count_m1 = read_u32(msg);
         uint32_t table_bytes  = 4 + (seg_count_m1 + 1) * 4;
         table_bytes           = (table_bytes + 7) & ~7u;
         auto* segment         = msg + table_bytes;
         return resolve_struct_ptr(segment);
      }

      // Validate segment-table + traversal-budget for flat-array messages.
      // Budget = 8 × segment words, matching the official capnp default.
      inline bool validate_struct(const uint8_t* seg_start,
                                  const uint8_t* seg_end, capnp_ptr p,
                                  int64_t& words_left, int depth = 0)
      {
         if (p == nullptr)
            return true;
         if (depth > 64)
            return false;

         uint32_t struct_words =
            static_cast<uint32_t>(p.data_words) + p.ptr_count;
         words_left -= struct_words;
         if (words_left < 0)
            return false;

         const uint8_t* data_end = p.data + static_cast<uint32_t>(p.data_words) * 8;
         if (p.data < seg_start || data_end > seg_end)
            return false;

         const uint8_t* ps     = ptr_section(p);
         const uint8_t* ps_end = ps + static_cast<uint32_t>(p.ptr_count) * 8;
         if (ps < seg_start || ps_end > seg_end)
            return false;

         for (uint32_t i = 0; i < p.ptr_count; ++i)
         {
            const uint8_t* slot = ps + i * 8;
            uint64_t       word = read_u64(slot);
            if (word == 0)
               continue;

            uint8_t tag = word & 3;
            if (tag == 0)
            {
               auto child = resolve_struct_ptr(slot);
               if (!validate_struct(seg_start, seg_end, child, words_left,
                                    depth + 1))
                  return false;
            }
            else if (tag == 1)
            {
               auto info = resolve_list_ptr(slot);
               if (!info.data)
                  continue;

               uint32_t       total_bytes = info.count * info.elem_stride;
               const uint8_t* list_end    = info.data + total_bytes;
               if (info.data < seg_start || list_end > seg_end)
                  return false;

               uint32_t list_words = (total_bytes + 7) / 8;
               words_left -= list_words;
               if (words_left < 0)
                  return false;

               if (info.elem_data_words > 0 || info.elem_ptr_count > 0)
               {
                  for (uint32_t j = 0; j < info.count; ++j)
                  {
                     capnp_ptr elem{info.data + j * info.elem_stride,
                                    info.elem_data_words,
                                    info.elem_ptr_count};
                     if (!validate_struct(seg_start, seg_end, elem,
                                          words_left, depth + 1))
                        return false;
                  }
               }
               else if (info.elem_stride == 8)
               {
                  for (uint32_t j = 0; j < info.count; ++j)
                  {
                     const uint8_t* entry = info.data + j * 8;
                     uint64_t       w     = read_u64(entry);
                     if (w == 0)
                        continue;
                     uint8_t t = w & 3;
                     if (t == 0)
                     {
                        auto child = resolve_struct_ptr(entry);
                        if (!validate_struct(seg_start, seg_end, child,
                                             words_left, depth + 1))
                           return false;
                     }
                     else if (t == 1)
                     {
                        auto inner = resolve_list_ptr(entry);
                        if (inner.data)
                        {
                           uint32_t ib = inner.count * inner.elem_stride;
                           if (inner.data < seg_start ||
                               inner.data + ib > seg_end)
                              return false;
                           uint32_t iw = (ib + 7) / 8;
                           words_left -= iw;
                           if (words_left < 0)
                              return false;
                        }
                     }
                  }
               }
            }
            else
            {
               return false;  // far / reserved pointers rejected
            }
         }
         return true;
      }

      inline bool validate_message(const void* buf, std::size_t len)
      {
         auto* msg = static_cast<const uint8_t*>(buf);
         if (len < 8)
            return false;

         uint32_t seg_count_m1 = read_u32(msg);
         if (seg_count_m1 != 0)
            return false;  // single-segment only

         uint32_t seg_words   = read_u32(msg + 4);
         uint32_t table_bytes = 8;

         if (len < table_bytes + static_cast<std::size_t>(seg_words) * 8)
            return false;
         if (seg_words == 0)
            return false;

         const uint8_t* seg_start = msg + table_bytes;
         const uint8_t* seg_end   = seg_start + static_cast<std::size_t>(seg_words) * 8;

         int64_t words_left = static_cast<int64_t>(seg_words) * 8;

         auto root = resolve_struct_ptr(seg_start);
         return validate_struct(seg_start, seg_end, root, words_left);
      }

      template <typename T>
         requires Record<T>
      std::uint32_t word_count_of(const T& v)
      {
         using L = capnp_layout<T>;
         capnp_word_counter buf;
         uint32_t root = buf.alloc(1);
         uint32_t ds   = buf.alloc(L::data_words + L::ptr_count);
         uint32_t ps   = ds + L::data_words;
         buf.write_struct_ptr(root, ds, L::data_words, L::ptr_count);
         pack_struct(buf, ds, ps, v);
         return buf.word_count();
      }

   }  // namespace detail::capnp_impl

   // ── Format tag ──────────────────────────────────────────────────────────

   struct capnp : format_tag_base<capnp>
   {
      using preferred_presentation_category = ::psio::binary_category;

      template <typename T>
         requires detail::capnp_impl::Record<T>
      friend std::vector<char> tag_invoke(decltype(::psio::encode), capnp,
                                          const T& v)
      {
         using namespace detail::capnp_impl;
         using L = capnp_layout<T>;

         capnp_word_buf buf(word_count_of(v));
         uint32_t       root = buf.alloc(1);
         uint32_t       ds   = buf.alloc(L::data_words + L::ptr_count);
         uint32_t       ps   = ds + L::data_words;
         buf.write_struct_ptr(root, ds, L::data_words, L::ptr_count);
         pack_struct(buf, ds, ps, v);
         return buf.finish();
      }

      //  Sink-form encode — caller-supplied vector<char> reused across
      //  iterations.  Builder still allocates its words_ buffer per
      //  call; only the output bytes vector is reused.
      template <typename T>
         requires detail::capnp_impl::Record<T>
      friend void tag_invoke(decltype(::psio::encode), capnp,
                             const T&            v,
                             std::vector<char>&  sink)
      {
         using namespace detail::capnp_impl;
         using L = capnp_layout<T>;

         capnp_word_buf buf(word_count_of(v));
         uint32_t       root = buf.alloc(1);
         uint32_t       ds   = buf.alloc(L::data_words + L::ptr_count);
         uint32_t       ps   = ds + L::data_words;
         buf.write_struct_ptr(root, ds, L::data_words, L::ptr_count);
         pack_struct(buf, ds, ps, v);
         buf.finish_into(sink);
      }

      template <typename T>
         requires detail::capnp_impl::Record<T>
      friend T tag_invoke(decltype(::psio::decode<T>), capnp, T*,
                          std::span<const char> bytes)
      {
         using namespace detail::capnp_impl;
         T    out{};
         auto p = root_of<T>(bytes.data());
         unpack_struct<T>(p, out);
         return out;
      }

      template <typename T>
         requires detail::capnp_impl::Record<T>
      friend std::size_t tag_invoke(decltype(::psio::size_of), capnp,
                                    const T& v)
      {
         using namespace detail::capnp_impl;
         capnp_word_counter buf;
         using L = capnp_layout<T>;
         uint32_t root = buf.alloc(1);
         uint32_t ds   = buf.alloc(L::data_words + L::ptr_count);
         uint32_t ps   = ds + L::data_words;
         buf.write_struct_ptr(root, ds, L::data_words, L::ptr_count);
         pack_struct(buf, ds, ps, v);
         return buf.finish_size();
      }

      template <typename T>
         requires detail::capnp_impl::Record<T>
      friend codec_status tag_invoke(decltype(::psio::validate<T>), capnp,
                                     T*,
                                     std::span<const char> bytes) noexcept
      {
         if (auto st = ::psio::check_max_dynamic_cap<T>(bytes.size(),
                                                         "capnp");
             !st.ok())
            return st;
         if (!detail::capnp_impl::validate_message(bytes.data(),
                                                   bytes.size()))
            return codec_fail("capnp: invalid message", 0, "capnp");
         return codec_ok();
      }

      template <typename T>
         requires detail::capnp_impl::Record<T>
      friend codec_status
      tag_invoke(decltype(::psio::validate_strict<T>), capnp, T*,
                 std::span<const char> bytes) noexcept
      {
         if (!detail::capnp_impl::validate_message(bytes.data(),
                                                   bytes.size()))
            return codec_fail("capnp: invalid message", 0, "capnp");
         try
         {
            using namespace detail::capnp_impl;
            T    out{};
            auto p = root_of<T>(bytes.data());
            unpack_struct<T>(p, out);
            return ::psio::validate_specs_on_value(out);
         }
         catch (...)
         {
            return codec_fail(
               "capnp: decode failed during validate_strict", 0, "capnp");
         }
      }

      template <typename T>
         requires detail::capnp_impl::Record<T>
      friend std::unique_ptr<T>
      tag_invoke(decltype(::psio::make_boxed<T>), capnp, T*,
                 std::span<const char> bytes) noexcept
      {
         using namespace detail::capnp_impl;
         auto v = std::make_unique<T>();
         auto p = root_of<T>(bytes.data());
         unpack_struct<T>(p, *v);
         return v;
      }
   };

}  // namespace psio
