#pragma once
//
// psio/pjson_typed.hpp — schema-aware view + canonical fast path.
//
// For a reflected struct T:
//   * pjson_hash_template<T>()   — per-T precomputed hash array.
//   * is_canonical_for<T>(view)  — single memcmp fast-fail check.
//   * typed_pjson_view<T>        — field-by-index access on canonical
//                                   buffers; falls back to find() on
//                                   non-canonical.
//   * to_struct<T>(view)         — materialize T.
//   * from_struct(t)             — encode T → pjson bytes.

#include <psio/format.hpp>
#include <psio/pjson_view.hpp>
#include <psio/reflect.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psio {

   namespace pjson_detail {
      template <typename T>
      inline const std::array<std::uint8_t, reflect<T>::member_count>&
      compute_hash_template() noexcept
      {
         using R = reflect<T>;
         static const auto arr = []() {
            std::array<std::uint8_t, R::member_count> a{};
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               ((a[Is] = key_hash8(R::template member_name<Is>)), ...);
            }(std::make_index_sequence<R::member_count>{});
            return a;
         }();
         return arr;
      }
   }

   template <typename T>
   inline const auto& pjson_hash_template() noexcept
   {
      return pjson_detail::compute_hash_template<T>();
   }

   template <typename T>
   inline bool is_canonical_for(pjson_view v) noexcept
   {
      using R = reflect<T>;
      if (v.type() != pjson_view::kind::object) return false;
      if (v.size() < 4) return false;
      // count is the last 2 bytes (u16 LE).
      std::size_t N = static_cast<std::size_t>(v.data()[v.size() - 2]) |
                      (static_cast<std::size_t>(v.data()[v.size() - 1])
                       << 8);
      if (N != R::member_count) return false;
      std::uint8_t slot_w_code = v.data()[1] & 0x03;
      std::size_t  slot_w      =
         pjson_detail::width_bytes(slot_w_code);
      std::size_t  slot_stride = slot_w + 1;
      std::size_t slot_table_pos = v.size() - 2 - slot_stride * N;
      std::size_t hash_table_pos = slot_table_pos - N;
      const auto& tmpl = pjson_hash_template<T>();
      return std::memcmp(v.data() + hash_table_pos, tmpl.data(),
                         R::member_count) == 0;
   }

   template <typename T>
   class view<T, pjson_format>
   {
     public:
      using R      = reflect<T>;
      using format = pjson_format;
      static_assert(R::is_reflected,
                    "view<T,pjson_format> requires PSIO_REFLECT(T,...)");

      view() = default;

      static view from_pjson(pjson_view raw) noexcept
      {
         view r;
         r.raw_       = raw;
         r.canonical_ = is_canonical_for<T>(raw);
         return r;
      }

      // Throwing factory: applies the type-level `maxDynamicData(N)`
      // cap as a defensive ceiling on the input buffer size before
      // surfacing any field accessors.  Use this when accepting
      // untrusted pjson — it rejects oversized payloads up front.
      static view from_pjson_checked(pjson_view raw)
      {
         ::psio::enforce_max_dynamic_cap<T>(raw.size(), "pjson");
         return from_pjson(raw);
      }

      bool       is_canonical() const noexcept { return canonical_; }
      pjson_view raw() const noexcept { return raw_; }

      template <std::size_t I>
      pjson_view field() const
      {
         if (canonical_) return field_by_index<I>();
         auto v = raw_.find(R::template member_name<I>);
         if (!v)
            throw std::runtime_error("view<T,pjson_format>: field missing");
         return *v;
      }
      template <std::size_t I>
      auto get() const
      {
         using FT = typename R::template member_type<I>;
         return read_as<FT>(field<I>());
      }

      T to_struct() const
      {
         T t{};
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            ((t.*R::template member_pointer<Is> = get<Is>()), ...);
         }(std::make_index_sequence<R::member_count>{});
         return t;
      }

     private:
      template <std::size_t I>
      pjson_view field_by_index() const noexcept
      {
         using namespace pjson_detail;
         constexpr std::size_t N = R::member_count;
         std::uint8_t slot_w_code = raw_.data()[1] & 0x03;
         std::size_t  slot_w      = width_bytes(slot_w_code);
         std::size_t  slot_stride = slot_w + 1;
         std::size_t  slot_table_pos   = raw_.size() - 2 - slot_stride * N;
         std::size_t  hash_table_pos   = slot_table_pos - N;
         std::size_t  value_data_start = 2;
         std::size_t  value_data_size  =
            hash_table_pos - value_data_start;
         (void)hash_table_pos;
         const std::uint8_t* slot =
            raw_.data() + slot_table_pos + I * slot_stride;
         std::uint32_t off_i = read_width(slot, slot_w_code);
         std::uint8_t  ks    = slot[slot_w];
         std::uint32_t off_next;
         if constexpr (I + 1 < N)
            off_next = read_width(
               raw_.data() + slot_table_pos + (I + 1) * slot_stride,
               slot_w_code);
         else
            off_next = static_cast<std::uint32_t>(value_data_size);
         const std::uint8_t* entry      = raw_.data() + value_data_start + off_i;
         std::size_t         entry_size = off_next - off_i;
         std::size_t klen, klen_bytes;
         if (ks != 0xFF) { klen = ks; klen_bytes = 0; }
         else
         {
            std::uint64_t excess;
            klen_bytes = read_varuint62(entry, entry_size, excess);
            klen = 0xFFu + static_cast<std::size_t>(excess);
         }
         std::size_t value_off = klen_bytes + klen;
         return pjson_view{entry + value_off, entry_size - value_off};
      }

      template <typename F>
      struct is_std_vector_ : std::false_type {};
      template <typename E, typename A>
      struct is_std_vector_<std::vector<E, A>> : std::true_type
      {
         using elem = E;
      };

      template <typename F>
      struct is_std_optional_ : std::false_type {};
      template <typename U>
      struct is_std_optional_<std::optional<U>> : std::true_type
      {
         using elem = U;
      };

      template <typename F>
      static F read_as(pjson_view v)
      {
         if constexpr (std::is_same_v<F, bool>) return v.as_bool();
         else if constexpr (std::is_integral_v<F>)
            return static_cast<F>(v.as_int64());
         else if constexpr (std::is_floating_point_v<F>)
            return static_cast<F>(v.as_double());
         else if constexpr (std::is_same_v<F, std::string>)
            return std::string(v.as_string());
         else if constexpr (std::is_same_v<F, std::string_view>)
            return v.as_string();
         else if constexpr (is_std_optional_<F>::value)
         {
            // null on the wire → empty optional; otherwise recurse.
            if (v.is_null())
               return F{};
            return F{read_as<typename is_std_optional_<F>::elem>(v)};
         }
         else if constexpr (is_std_vector_<F>::value)
         {
            using E = typename is_std_vector_<F>::elem;
            F out;
            // vector<uint8_t> / vector<std::byte> route through the
            // dedicated t_bytes tag rather than typed-array — bytes
            // are a blob, not a list of small ints; JSON renders
            // base64 rather than [1,2,3,…].
            if constexpr (std::is_same_v<E, std::uint8_t> ||
                          std::is_same_v<E, std::byte>)
            {
               if (v.is_bytes())
               {
                  auto span = v.as_bytes();
                  out.resize(span.size());
                  if (!span.empty())
                     std::memcpy(out.data(), span.data(), span.size());
                  return out;
               }
               // Fall through to array decode for back-compat data
               // emitted before the t_bytes routing landed.
            }
            // Typed-array fast path: bulk-memcpy when the wire form
            // is a typed array whose element matches E.
            if constexpr (
               std::is_same_v<E, std::int8_t>  ||
               std::is_same_v<E, std::int16_t> ||
               std::is_same_v<E, std::int32_t> ||
               std::is_same_v<E, std::int64_t> ||
               std::is_same_v<E, std::uint8_t> ||
               std::is_same_v<E, std::uint16_t> ||
               std::is_same_v<E, std::uint32_t> ||
               std::is_same_v<E, std::uint64_t> ||
               std::is_same_v<E, float>        ||
               std::is_same_v<E, double>)
            {
               if (v.is_typed_array() &&
                   v.typed_array_elem_code() ==
                      pjson_detail::tac_for<E>())
               {
                  std::size_t n = v.count();
                  out.resize(n);
                  if (n)
                     std::memcpy(out.data(), v.data() + 1,
                                 n * sizeof(E));
                  return out;
               }
            }
            //  Fallback: walk a generic array, one element at a time.
            std::size_t n = v.count();
            out.reserve(n);
            for (std::size_t i = 0; i < n; ++i)
               out.push_back(read_as<E>(v.at(i)));
            return out;
         }
         else if constexpr (Reflected<F>)
         {
            //  Recursive nested object — make a typed view of the
            //  inline pjson_view and ask it to materialise.
            return view<F, pjson_format>::from_pjson(v).to_struct();
         }
         else
            static_assert(sizeof(F) == 0,
                          "view<T,pjson_format>: unsupported field type");
      }

      pjson_view raw_;
      bool       canonical_ = false;
   };

   // Backward-compat alias. New code should prefer view<T, pjson_format>.
   template <typename T>
   using typed_pjson_view = view<T, pjson_format>;

   template <typename F>
   inline std::size_t typed_field_size(const F& v);

   //  Forward decls for the recursive Reflected branches in
   //  typed_field_size_dispatch / typed_field_encode below.  Both are
   //  defined further down once typed_field_size and typed_field_encode
   //  are in place.
   template <typename T>
   inline std::size_t pjson_encoded_size(const T& t);
   template <typename T>
   inline std::size_t to_pjson_at(std::uint8_t* dst, std::size_t pos,
                                  const T& t);

   // Forward decls for the typed row_array path (reflected vector
   // elements). Bodies live in pjson_detail and are defined after
   // typed_field_size_dispatch / typed_field_encode are in scope.
   namespace pjson_detail {
      template <typename E>
      inline std::size_t typed_row_array_size(std::span<const E>) noexcept;
      template <typename E>
      inline std::size_t encode_typed_row_array_at(
         std::uint8_t*      dst,
         std::size_t        pos,
         std::span<const E> records);
   }

   namespace pjson_detail {
      // Local std::vector / std::optional detectors so the typed
      // size/encode dispatch can route them without bouncing through
      // view<T, pjson_format>'s private detectors.
      template <typename F>
      struct typed_is_vector_ : std::false_type {};
      template <typename E, typename A>
      struct typed_is_vector_<std::vector<E, A>> : std::true_type
      {
         using elem = E;
      };

      template <typename F>
      struct typed_is_optional_ : std::false_type {};
      template <typename U>
      struct typed_is_optional_<std::optional<U>> : std::true_type
      {
         using elem = U;
      };

      template <typename E>
      constexpr bool is_typed_array_elem_ =
         std::is_same_v<E, std::int8_t>  ||
         std::is_same_v<E, std::int16_t> ||
         std::is_same_v<E, std::int32_t> ||
         std::is_same_v<E, std::int64_t> ||
         std::is_same_v<E, std::uint8_t> ||
         std::is_same_v<E, std::uint16_t> ||
         std::is_same_v<E, std::uint32_t> ||
         std::is_same_v<E, std::uint64_t> ||
         std::is_same_v<E, float>        ||
         std::is_same_v<E, double>;

      template <typename F>
      inline std::size_t typed_field_size_dispatch(const F& v)
      {
         if constexpr (std::is_same_v<F, bool>) return 1;
         else if constexpr (std::is_integral_v<F>)
            return int_size(static_cast<std::int64_t>(v));
         else if constexpr (std::is_floating_point_v<F>)
         {
            if (!std::isfinite(v)) return 9;
            pjson_number n   = pjson_number::from_double(static_cast<double>(v));
            std::size_t  dec = number_size(n);
            return dec < 9 ? dec : 9;
         }
         else if constexpr (std::is_same_v<F, std::string> ||
                            std::is_same_v<F, std::string_view>)
            return 1u + std::string_view(v).size();
         else if constexpr (typed_is_optional_<F>::value)
         {
            if (!v.has_value())
               return 1;  // null tag
            return typed_field_size_dispatch<
               typename typed_is_optional_<F>::elem>(*v);
         }
         else if constexpr (typed_is_vector_<F>::value)
         {
            using E = typename typed_is_vector_<F>::elem;
            // vector<u8>/vector<std::byte> → t_bytes: 1 tag + N raw bytes.
            if constexpr (std::is_same_v<E, std::uint8_t> ||
                          std::is_same_v<E, std::byte>)
            {
               return 1u + v.size();
            }
            else if constexpr (is_typed_array_elem_<E>)
            {
               // Typed-array body: 1 tag + N*sizeof(E) + 2 count.
               return 1u + v.size() * sizeof(E) + 2u;
            }
            else if constexpr (Reflected<E>)
            {
               // Reflected element type → row_array (§5.2.1). Schema is
               // shared across records (compile-time-known from
               // reflect<E>); two passes over records to size and pick
               // adaptive widths.
               return typed_row_array_size<E>(
                  std::span<const E>{v.data(), v.size()});
            }
            else
            {
               // Generic-array body: tag + Σ(child sizes) + 4*N slot
               // table + 2 count.
               std::size_t total = 1u + 4u * v.size() + 2u;
               for (const auto& x : v)
                  total += typed_field_size_dispatch<E>(x);
               return total;
            }
         }
         else if constexpr (Reflected<F>)
         {
            // Recursive object: pjson_encoded_size walks F's reflected
            // members at the same depth as the top-level entry point.
            return pjson_encoded_size(v);
         }
         else
            static_assert(sizeof(F) == 0,
                          "from_struct: unsupported field type");
      }
   }

   template <typename F>
   inline std::size_t typed_field_size(const F& v)
   {
      return pjson_detail::typed_field_size_dispatch(v);
   }

   namespace pjson_detail {
      template <typename F>
      inline std::size_t typed_field_encode(std::uint8_t* dst,
                                            std::size_t   pos,
                                            const F&      v)
      {
         if constexpr (std::is_same_v<F, bool>)
         {
            dst[pos] = static_cast<std::uint8_t>(
                (t_bool << 4) | (v ? 1u : 0u));
            return 1;
         }
         else if constexpr (std::is_integral_v<F>)
            return encode_int64_at(dst, pos,
                                   static_cast<std::int64_t>(v));
         else if constexpr (std::is_floating_point_v<F>)
            return encode_double_at(dst, pos, static_cast<double>(v));
         else if constexpr (std::is_same_v<F, std::string> ||
                            std::is_same_v<F, std::string_view>)
            return encode_string_at(dst, pos, std::string_view(v));
         else if constexpr (typed_is_optional_<F>::value)
         {
            if (!v.has_value())
            {
               dst[pos] = static_cast<std::uint8_t>(t_null << 4);
               return 1;
            }
            return typed_field_encode<typename typed_is_optional_<F>::elem>(
               dst, pos, *v);
         }
         else if constexpr (typed_is_vector_<F>::value)
         {
            using E = typename typed_is_vector_<F>::elem;
            // vector<u8>/vector<std::byte> → t_bytes (single memcpy,
            // no count tail; consumers see "binary blob" rather than
            // "list of small ints").
            if constexpr (std::is_same_v<E, std::uint8_t> ||
                          std::is_same_v<E, std::byte>)
            {
               dst[pos] = static_cast<std::uint8_t>(t_bytes << 4);
               if (!v.empty())
                  std::memcpy(dst + pos + 1,
                              reinterpret_cast<const std::uint8_t*>(
                                 v.data()),
                              v.size());
               return 1u + v.size();
            }
            else if constexpr (is_typed_array_elem_<E>)
            {
               // Bulk typed-array fast path — single memcpy per
               // vector regardless of length.
               return encode_typed_array_at<E>(
                  dst, pos,
                  std::span<const E>{v.data(), v.size()});
            }
            else if constexpr (Reflected<E>)
            {
               // vector<ReflectedT> → row_array (§5.2.1). Reflect gives
               // us homogeneity for free: every element has the same K
               // fields in the same order, so the schema hoists out of
               // each record by construction.
               return encode_typed_row_array_at<E>(
                  dst, pos,
                  std::span<const E>{v.data(), v.size()});
            }
            else
            {
               // Generic-array form: [tag][value_data][slot[N]][count].
               std::size_t start = pos;
               dst[pos++] = static_cast<std::uint8_t>(t_array << 4);
               std::size_t N  = v.size();
               std::size_t vd = 0;
               for (const auto& x : v)
                  vd += typed_field_size_dispatch<E>(x);
               std::size_t value_data_start = pos;
               std::size_t slot_table_pos   = value_data_start + vd;
               std::size_t count_pos        = slot_table_pos + 4 * N;
               for (std::size_t i = 0; i < N; ++i)
               {
                  std::uint32_t off =
                     static_cast<std::uint32_t>(pos - value_data_start);
                  write_u32_le(dst + slot_table_pos + i * 4,
                               pack_slot(off, 0));
                  pos += typed_field_encode<E>(dst, pos, v[i]);
               }
               dst[count_pos]     =
                  static_cast<std::uint8_t>(N & 0xFF);
               dst[count_pos + 1] =
                  static_cast<std::uint8_t>((N >> 8) & 0xFF);
               return count_pos + 2 - start;
            }
         }
         else if constexpr (Reflected<F>)
         {
            //  Recursive nested object — same machinery the top-level
            //  to_pjson uses, applied at this position.
            return ::psio::to_pjson_at<F>(dst, pos, v);
         }
      }

      // ── Typed row_array (for vector<ReflectedT>) ────────────────────────
      //
      // Two-pass walker. Pass 1: per-record body sizes + max(body), used
      // to pick adaptive widths. Pass 2: write header + shared schema +
      // per-record bytes + record_offsets + count. Uses reflect<E> for
      // the schema (member_name, member_pointer) and typed_field_encode
      // for value bytes.

      template <typename E>
      inline std::size_t typed_row_array_size(
         std::span<const E> records) noexcept
      {
         using R                 = reflect<E>;
         constexpr std::size_t K = R::member_count;
         std::size_t           N = records.size();
         if (N == 0)
            return 1 /*tag*/ + 1 /*width*/ + varuint62_byte_count(K)
                  + 4 * K + K /*hashes*/
                  + [] {
                       std::size_t s = 0;
                       [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                          ((s += R::template member_name<Is>.size()), ...);
                       }(std::make_index_sequence<K>{});
                       return s;
                    }()
                  + 0 /*records body*/ + 0 /*record_offsets*/
                  + 2 /*count*/;

         std::vector<std::uint32_t> body_sizes(N);
         std::uint32_t              max_body = 0;
         for (std::size_t i = 0; i < N; ++i)
         {
            std::uint32_t b = 0;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               ((b += static_cast<std::uint32_t>(typed_field_size_dispatch(
                    records[i].*R::template member_pointer<Is>))),
                ...);
            }(std::make_index_sequence<K>{});
            body_sizes[i] = b;
            if (b > max_body) max_body = b;
         }
         std::uint8_t slot_w_code = width_code_for(max_body);
         std::size_t  slot_w      = width_bytes(slot_w_code);

         std::uint32_t total_body = 0;
         for (std::size_t i = 0; i < N; ++i)
            total_body +=
               body_sizes[i] + static_cast<std::uint32_t>(K * slot_w);
         std::uint8_t recoff_w_code = width_code_for(total_body);
         std::size_t  recoff_w      = width_bytes(recoff_w_code);

         std::uint32_t keys_area = 0;
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            ((keys_area += static_cast<std::uint32_t>(
                  R::template member_name<Is>.size())),
             ...);
         }(std::make_index_sequence<K>{});

         return 1 /*tag*/ + 1 /*width byte*/
                + varuint62_byte_count(K) + 4 * K /*key slots*/
                + K /*hash[K]*/ + keys_area + total_body
                + N * recoff_w + 2 /*count*/;
      }

      template <typename E>
      inline std::size_t encode_typed_row_array_at(
         std::uint8_t*      dst,
         std::size_t        pos,
         std::span<const E> records)
      {
         using R                 = reflect<E>;
         constexpr std::size_t K = R::member_count;
         std::size_t           start = pos;
         std::size_t           N     = records.size();

         // Pass 1: per-field offsets within each record + body sizes.
         std::vector<std::uint32_t> per_field_offset(N * K);
         std::vector<std::uint32_t> body_sizes(N);
         std::uint32_t              max_body = 0;
         for (std::size_t i = 0; i < N; ++i)
         {
            std::uint32_t off = 0;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               ((per_field_offset[i * K + Is] = off,
                 off += static_cast<std::uint32_t>(typed_field_size_dispatch(
                    records[i].*R::template member_pointer<Is>))),
                ...);
            }(std::make_index_sequence<K>{});
            body_sizes[i] = off;
            if (off > max_body) max_body = off;
         }
         std::uint8_t slot_w_code = width_code_for(max_body);
         std::size_t  slot_w      = width_bytes(slot_w_code);

         std::vector<std::uint32_t> record_offsets(N);
         std::uint32_t              total_body = 0;
         for (std::size_t i = 0; i < N; ++i)
         {
            record_offsets[i] = total_body;
            total_body +=
               body_sizes[i] + static_cast<std::uint32_t>(K * slot_w);
         }
         std::uint8_t recoff_w_code = width_code_for(total_body);
         std::size_t  recoff_w      = width_bytes(recoff_w_code);

         // Pass 2: write header + shared schema.
         dst[pos++] = static_cast<std::uint8_t>(
            (t_object << 4) | object_form_row_array);
         dst[pos++] = static_cast<std::uint8_t>(
            slot_w_code | (recoff_w_code << 2));
         pos += write_varuint62(dst, pos, K);

         std::size_t key_slots_pos = pos;
         pos += 4 * K;
         std::size_t hash_pos = pos;
         pos += K;

         // Reuse the constexpr hash array computed once per E.
         const auto& hash_template = pjson_hash_template<E>();
         std::memcpy(dst + hash_pos, hash_template.data(), K);

         std::uint32_t key_off_running = 0;
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            ((
               [&] {
                  constexpr auto name = R::template member_name<Is>;
                  std::uint8_t   ks_byte =
                     name.size() < 0xFFu
                        ? static_cast<std::uint8_t>(name.size())
                        : static_cast<std::uint8_t>(0xFFu);
                  write_u32_le(
                     dst + key_slots_pos + Is * 4,
                     pack_slot(key_off_running, ks_byte));
                  std::memcpy(dst + pos, name.data(), name.size());
                  pos += name.size();
                  key_off_running +=
                     static_cast<std::uint32_t>(name.size());
               }()),
             ...);
         }(std::make_index_sequence<K>{});

         // Records body.
         for (std::size_t i = 0; i < N; ++i)
         {
            std::size_t record_start = pos;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               ((pos += typed_field_encode(
                    dst, pos,
                    records[i].*R::template member_pointer<Is>)),
                ...);
            }(std::make_index_sequence<K>{});
            std::size_t slot_pos = record_start + body_sizes[i];
            for (std::size_t j = 0; j < K; ++j)
               write_width(dst + slot_pos + j * slot_w, slot_w_code,
                           per_field_offset[i * K + j]);
            pos = slot_pos + K * slot_w;
         }

         // record_offsets[N], then count u16.
         for (std::size_t i = 0; i < N; ++i)
            write_width(dst + pos + i * recoff_w, recoff_w_code,
                        record_offsets[i]);
         pos += N * recoff_w;

         dst[pos]     = static_cast<std::uint8_t>(N & 0xFF);
         dst[pos + 1] = static_cast<std::uint8_t>((N >> 8) & 0xFF);
         pos += 2;

         return pos - start;
      }
   }

   // Compile-time-derivable size of T's pjson encoding for a given t.
   // (Used by the in-place writer to size the buffer up front.)
   template <typename T>
   inline std::size_t pjson_encoded_size(const T& t)
   {
      using R = reflect<T>;
      std::size_t value_data_size = 0;
      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
         ((value_data_size +=
              R::template member_name<Is>.size() +
              typed_field_size(t.*R::template member_pointer<Is>)),
          ...);
      }(std::make_index_sequence<R::member_count>{});
      std::size_t  N = R::member_count;
      std::uint8_t code = pjson_detail::width_code_for(value_data_size);
      std::size_t  slot_w = pjson_detail::width_bytes(code);
      // 1 tag + 1 width + value_data + N hash + (slot_w+1)*N slot + 2 count.
      return 1 + 1 + value_data_size + N + (slot_w + 1) * N + 2;
   }

   // In-place reflected-object encoder.  Tail-indexed: writes
   // value_data first, then appends hash[N], slot[N], count u16.
   // Returns total bytes written so callers can use it as a building
   // block for nested structs.
   //
   // Caller pre-sizes dst with at least pjson_encoded_size(t) bytes
   // available starting at pos.
   template <typename T>
   inline std::size_t to_pjson_at(std::uint8_t* dst, std::size_t pos,
                                  const T& t)
   {
      using R = reflect<T>;
      static_assert(R::is_reflected,
                    "to_pjson_at requires PSIO_REFLECT(T,...)");
      std::size_t           total = pjson_encoded_size(t);
      constexpr std::size_t N     = R::member_count;
      std::size_t           start = pos;
      dst[pos++] =
         static_cast<std::uint8_t>(pjson_detail::t_object << 4);
      // value_data size derives from total - constant overhead. Use it
      // to pick the slot_w_code, then emit the width byte.
      std::size_t value_data_size  =
         total - 1 /*tag*/ - 1 /*width*/ - N /*hash*/ - 2 /*count*/;
      // Solve for slot_w by trying each width: total = 1+1+vd+N+(slot_w+1)*N+2,
      // so (slot_w+1)*N = total - 4 - vd - N, thus slot_w = (total - 4 - vd - N)/N - 1.
      // But pjson_encoded_size already picked slot_w from value_data_size.
      // Recompute consistently here.
      std::uint8_t slot_w_code =
         pjson_detail::width_code_for(value_data_size);
      std::size_t  slot_w      = pjson_detail::width_bytes(slot_w_code);
      // Adjust value_data_size given the picked slot_w (the size formula
      // depends on slot_w). pjson_encoded_size used the same formula so
      // the totals agree; recompute vd from the remainder.
      value_data_size =
         total - 1 - 1 - N - (slot_w + 1) * N - 2;
      dst[pos++] = slot_w_code;  // width byte
      std::size_t value_data_start = pos;
      std::size_t hash_table_pos   = value_data_start + value_data_size;
      std::size_t slot_table_pos   = hash_table_pos + N;
      std::size_t count_pos        = slot_table_pos + (slot_w + 1) * N;

      const auto& tmpl = pjson_hash_template<T>();
      std::memcpy(dst + hash_table_pos, tmpl.data(), N);

      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
         ((pos = [&] {
             constexpr auto name = R::template member_name<Is>;
             std::uint32_t  off =
                static_cast<std::uint32_t>(pos - value_data_start);
             std::uint8_t ks =
                name.size() < 0xFFu
                   ? static_cast<std::uint8_t>(name.size())
                   : static_cast<std::uint8_t>(0xFFu);
             std::uint8_t* slot =
                dst + slot_table_pos + Is * (slot_w + 1);
             pjson_detail::write_width(slot, slot_w_code, off);
             slot[slot_w] = ks;
             std::memcpy(dst + pos, name.data(), name.size());
             pos += name.size();
             pos += pjson_detail::typed_field_encode(
                dst, pos, t.*R::template member_pointer<Is>);
             return pos;
          }()),
          ...);
      }(std::make_index_sequence<R::member_count>{});

      dst[count_pos]     = static_cast<std::uint8_t>(N & 0xFF);
      dst[count_pos + 1] = static_cast<std::uint8_t>((N >> 8) & 0xFF);
      return count_pos + 2 - start;
   }

   // Top-level entry — resizes the output vector and calls to_pjson_at.
   template <typename T>
   inline void to_pjson(const T& t, std::vector<std::uint8_t>& out)
   {
      const std::size_t total = pjson_encoded_size(t);
      ::psio::enforce_max_dynamic_cap<T>(total, "pjson");
      out.resize(total);
      to_pjson_at(out.data(), 0, t);
   }

   // Returning form. Allocates a fresh vector each call. The in-place
   // `to_pjson(t, out&)` form is preferred for hot loops that can
   // reuse a buffer.
   template <typename T>
   inline std::vector<std::uint8_t> from_struct(const T& t)
   {
      std::vector<std::uint8_t> out;
      to_pjson(t, out);
      return out;
   }

}  // namespace psio
