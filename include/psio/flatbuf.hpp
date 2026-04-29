#pragma once
//
// psio/flatbuf.hpp — native (zero-dep) FlatBuffer format tag.
//
// Produces a wire-compatible FlatBuffer buffer without depending on
// the Google FlatBuffers runtime. Intended to be byte-identical to
// psio/flatbuf_lib.hpp on the shapes they both support — this is
// what lets a consumer freely swap between native and library paths
// during migration.
//
// MVP scope (same as flatbuf_lib):
//   - primitives (arithmetic + bool)
//   - std::string
//   - std::vector of arithmetic / std::string / nested tables
//   - std::optional<arithmetic | bool | string>
//   - reflected records (root or nested tables)
//
// Wire reference: FlatBuffers binary format spec
// (https://flatbuffers.dev/internals.html). Field layout is encoded
// via PSIO_REFLECT order — field I maps to vtable voffset 4 + 2*I.

#include <psio/cpo.hpp>
#include <psio/error.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/adapter.hpp>
#include <psio/reflect.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace psio {

   struct flatbuf;

   namespace detail::flatbuf_impl {

      template <typename T>
      concept Record = ::psio::Reflected<T>;

      template <typename T>
      struct is_optional : std::false_type {};
      template <typename T>
      struct is_optional<std::optional<T>> : std::true_type {};

      template <typename T>
      struct is_vector : std::false_type {};
      template <typename T, typename A>
      struct is_vector<std::vector<T, A>> : std::true_type {};

      template <typename T, typename = void>
      struct is_table : std::false_type {};
      template <typename T>
      struct is_table<T, std::void_t<decltype(::psio::reflect<T>::member_count)>>
         : std::bool_constant<!std::is_arithmetic_v<T> &&
                              !std::is_same_v<T, std::string>> {};

      // Back-to-front buffer: writes grow toward the front, so finished
      // offsets count *backwards* from the buffer's "end" (its capacity).
      class builder
      {
         //  Small-buffer optimisation: 1 KB inline buffer held on the
         //  stack alongside the builder.  Most bench shapes (Point /
         //  NameRecord / Validator / FlatRecord / Record / Order ≤ 320 B
         //  wire) fit in 1 KB and skip heap allocation entirely.  Bulk
         //  shapes (ValidatorList(100) at ~7.7 KB) overflow into a
         //  heap-allocated chunk via grow().  Mirrors the libflatbuffers
         //  default 1024-byte allocator-pool slice but without going
         //  through malloc for the common case.
         //
         //  Layout invariants:
         //    - `buf_`  always points at the active backing storage:
         //               either local_  (when heap_ is null)
         //               or     heap_.get() (after grow()).
         //    - `cap_`  is the size of the active backing.
         //    - Valid bytes live at buf_[head_ .. cap_).
         static constexpr std::size_t kLocalBytes = 1024;
         alignas(16) std::uint8_t        local_[kLocalBytes];
         std::uint8_t*                   buf_       = nullptr;
         std::unique_ptr<std::uint8_t[]> heap_;
         std::size_t                     cap_       = 0;
         std::size_t                     head_      = 0;
         std::size_t                     min_align_ = 1;

         struct field_loc
         {
            std::uint32_t off;
            std::uint16_t vt;
         };
         static constexpr std::size_t kMaxFields = 64;
         field_loc                    fields_[kMaxFields]{};
         std::size_t                  nfields_   = 0;

         // Vtable deduplication cache.  libflatbuffers writes one
         // canonical vtable per (max_vt, table_size, field-skip pattern)
         // and shares its offset across every table that produces the
         // same byte sequence.  For workloads like vector<Validator(100)>
         // this collapses 100 redundant vtables to 1 — the dominant
         // savings vs the canonical lib in the bench.
         // Cap matches end_table's local_vt buffer (512 bytes = up to
         // 254 fields via 4 + 2*254).  vtables larger than that fall
         // back to a runtime check in end_table.
         struct vt_cache_entry
         {
            std::uint32_t                 off;
            std::uint16_t                 size;
            std::array<std::uint8_t, 512> bytes;
         };
         std::vector<vt_cache_entry>      vt_cache_;
         std::uint32_t                tbl_start_ = 0;

         std::size_t sz() const { return cap_ - head_; }

         //  Grow without zero-initialising the new trailing bytes.
         //  First overflow from local_ allocates the heap; subsequent
         //  growths reallocate the heap chunk.  `new u8[N]` is
         //  default-init (no-op for u8) so the new tail past the
         //  copied bytes is uninitialised — the bytes past head_ are
         //  never read until written.
         void grow(std::size_t needed)
         {
            const std::size_t tail = sz();
            const std::size_t new_cap = std::max(cap_ * 2, cap_ + needed);
            std::unique_ptr<std::uint8_t[]> nb(new std::uint8_t[new_cap]);
            std::memcpy(nb.get() + new_cap - tail,
                        buf_ + head_, tail);
            heap_ = std::move(nb);
            buf_  = heap_.get();
            cap_  = new_cap;
            head_ = new_cap - tail;
         }

         //  Hot-path inner functions — invoked once per field.  Force
         //  inlining so the templated pre_create dispatch chain folds
         //  to a flat sequence of writes rather than a chain of small
         //  function calls.  Matches what flatc-generated CreateXxx()
         //  achieves naturally.
         [[gnu::always_inline]] std::uint8_t* alloc(std::size_t n)
         {
            if (n > head_)
               grow(n);
            head_ -= n;
            return buf_ + head_;
         }

         [[gnu::always_inline]] void zero_pad(std::size_t n)
         { std::memset(alloc(n), 0, n); }

         [[gnu::always_inline]] void track(std::size_t a)
         {
            if (a > min_align_)
               min_align_ = a;
         }

         [[gnu::always_inline]] void align(std::size_t a)
         {
            const std::size_t p = (~sz() + 1) & (a - 1);
            if (p)
               zero_pad(p);
            track(a);
         }

         [[gnu::always_inline]] void pre_align(std::size_t len,
                                                std::size_t a)
         {
            if (!len)
               return;
            const std::size_t p = (~(sz() + len) + 1) & (a - 1);
            if (p)
               zero_pad(p);
            track(a);
         }

         template <typename T>
         [[gnu::always_inline]] void push(T v)
         {
            auto* p = alloc(sizeof(T));
            std::memcpy(p, &v, sizeof(T));
         }

        public:
         builder()
            : buf_(local_), cap_(kLocalBytes), head_(kLocalBytes)
         {
         }

         //  Pre-allocate enough heap space for `hint` bytes so the
         //  encode never needs to grow().  Used by the two-pass
         //  encode CPO (size walk → reserve → real encode).
         //  Hints ≤ kLocalBytes stay on the stack buffer.
         [[gnu::always_inline]] void reserve_capacity(std::size_t hint)
         {
            if (hint <= cap_) return;
            heap_ = std::unique_ptr<std::uint8_t[]>(
               new std::uint8_t[hint]);
            buf_  = heap_.get();
            cap_  = hint;
            head_ = hint;
         }

         std::uint32_t create_string(const char* s, std::size_t len)
         {
            pre_align(len + 1, sizeof(std::uint32_t));
            *alloc(1) = 0;  // trailing null
            if (len)
               std::memcpy(alloc(len), s, len);
            align(sizeof(std::uint32_t));
            push(static_cast<std::uint32_t>(len));
            return static_cast<std::uint32_t>(sz());
         }

         template <typename T>
         std::uint32_t create_vec_scalar(const T* data, std::size_t count)
         {
            const std::size_t body = count * sizeof(T);
            pre_align(body, sizeof(std::uint32_t));
            pre_align(body, sizeof(T));
            if (body)
               std::memcpy(alloc(body), data, body);
            align(sizeof(std::uint32_t));
            push(static_cast<std::uint32_t>(count));
            return static_cast<std::uint32_t>(sz());
         }

         std::uint32_t create_vec_offsets(const std::uint32_t* offs,
                                          std::size_t          count)
         {
            pre_align(count * sizeof(std::uint32_t),
                      sizeof(std::uint32_t));
            for (std::size_t i = count; i > 0;)
            {
               --i;
               align(sizeof(std::uint32_t));
               push(static_cast<std::uint32_t>(sz()) - offs[i] +
                    std::uint32_t(4));
            }
            align(sizeof(std::uint32_t));
            push(static_cast<std::uint32_t>(count));
            return static_cast<std::uint32_t>(sz());
         }

         [[gnu::always_inline]] void start_table()
         {
            nfields_   = 0;
            tbl_start_ = static_cast<std::uint32_t>(sz());
         }

         template <typename T>
         [[gnu::always_inline]] void
         add_scalar(std::uint16_t vt, T val, T def)
         {
            if (val == def)
               return;
            align(sizeof(T));
            push(val);
            fields_[nfields_++] = {static_cast<std::uint32_t>(sz()), vt};
         }

         template <typename T>
         [[gnu::always_inline]] void
         add_scalar_force(std::uint16_t vt, T val)
         {
            align(sizeof(T));
            push(val);
            fields_[nfields_++] = {static_cast<std::uint32_t>(sz()), vt};
         }

         [[gnu::always_inline]]
         void add_offset_field(std::uint16_t vt, std::uint32_t off)
         {
            if (!off)
               return;
            align(sizeof(std::uint32_t));
            // uoffset_t stored = target_addr - cell_addr. Push evaluates
            // the value before alloc, so sz() here is the pre-push sz;
            // the cell occupies the next 4 bytes, making its sz_at_addr
            // sz() + 4. target sz = off. value = (sz+4) - off.
            push(static_cast<std::uint32_t>(sz()) - off +
                 std::uint32_t(4));
            fields_[nfields_++] = {static_cast<std::uint32_t>(sz()), vt};
         }

         // Write N bytes at an absolute sz() position (back-to-front).
         void store_at(std::uint32_t at_sz, const void* src,
                       std::size_t n)
         {
            // buf_[cap - at_sz..cap - at_sz + n] = src
            std::memcpy(buf_ + cap_ - at_sz, src, n);
         }

         std::uint32_t end_table()
         {
            // Reserve the 4-byte soffset_t back-pointer (patched later).
            align(sizeof(std::int32_t));
            push(std::int32_t{0});
            const std::uint32_t tbl_off = static_cast<std::uint32_t>(sz());

            // Vtable size = 4 (own size + table size) + 2 per slot up to
            // max declared voffset.
            std::uint16_t max_vt = 0;
            for (std::size_t i = 0; i < nfields_; ++i)
               if (fields_[i].vt > max_vt)
                  max_vt = fields_[i].vt;
            const std::uint16_t vt_size =
               static_cast<std::uint16_t>(std::max<int>(max_vt + 2, 4));
            const std::uint16_t tbl_obj_sz =
               static_cast<std::uint16_t>(tbl_off - tbl_start_);

            // Compose the vtable bytes in a stack buffer first so we can
            // hash-match against the cache without committing to the
            // main buffer.  Cap = 512 bytes (4 + 2*254) — well past any
            // realistic record width.  vt_cache_entry::bytes is sized
            // to match.
            std::uint8_t local_vt[512] = {0};
            std::memcpy(local_vt + 0, &vt_size, 2);
            std::memcpy(local_vt + 2, &tbl_obj_sz, 2);
            for (std::size_t i = 0; i < nfields_; ++i)
            {
               const std::uint16_t fo = static_cast<std::uint16_t>(
                  tbl_off - fields_[i].off);
               std::memcpy(local_vt + fields_[i].vt, &fo, 2);
            }

            // Linear-scan dedup — typical workloads have at most a few
            // unique vtables per builder (one per record type), so a
            // sequential scan beats any hash-table for realistic N.
            std::uint32_t vt_off = 0;
            for (const auto& e : vt_cache_)
            {
               if (e.size == vt_size
                   && std::memcmp(e.bytes.data(), local_vt, vt_size) == 0)
               {
                  vt_off = e.off;
                  break;
               }
            }

            if (vt_off == 0)
            {
               // Cache miss — commit the vtable to the main buffer.
               std::uint8_t* vt = alloc(vt_size);
               std::memcpy(vt, local_vt, vt_size);
               vt_off = static_cast<std::uint32_t>(sz());
               vt_cache_entry entry{};
               entry.off  = vt_off;
               entry.size = vt_size;
               std::memcpy(entry.bytes.data(), local_vt, vt_size);
               vt_cache_.push_back(entry);
            }

            // Patch the soffset_t in the table (at byte address
            // buf_[cap - tbl_off]).
            const std::int32_t soff =
               static_cast<std::int32_t>(vt_off) -
               static_cast<std::int32_t>(tbl_off);
            store_at(tbl_off, &soff, 4);

            return tbl_off;
         }

         std::vector<char> finish(std::uint32_t root)
         {
            std::vector<char> out;
            finish_into(root, out);
            return out;
         }

         //  Sink form — write the finished bytes into a caller-supplied
         //  vector, reusing its capacity if it has any.  Used by the
         //  encode-into-sink CPO so a hot-loop caller can keep one
         //  output vector across iterations.  The builder's internal
         //  buf_ still allocates fresh per-call (top-level builder
         //  reuse would require a separate bench mode).
         void finish_into(std::uint32_t       root,
                          std::vector<char>&  sink)
         {
            pre_align(sizeof(std::uint32_t), min_align_);
            align(sizeof(std::uint32_t));
            push(static_cast<std::uint32_t>(sz()) - root +
                 std::uint32_t(4));
            const std::size_t out_size = sz();
            sink.resize(out_size);
            std::memcpy(sink.data(), buf_ + head_, out_size);
         }
      };

      //  fb_size_counter — drop-in replacement for `builder` that
      //  counts bytes instead of writing them.  Mirrors capnp's
      //  capnp_word_counter pattern.  pre_create runs against this
      //  counter as a "dry pass" first, returning the exact number of
      //  bytes the real encode will produce.  We then alloc that
      //  many bytes once and run the real encode, eliminating all
      //  grow() reallocs.
      //
      //  Note: vtable dedup is NOT mirrored here.  The counter assumes
      //  every vtable is fresh (never deduped) so it OVER-counts by
      //  vt_size × (n_repeated_records − 1) bytes.  That over-allocation
      //  is fine — the heap chunk just has unused tail.  Mirroring the
      //  full dedup logic would require duplicating the cache
      //  machinery, which isn't worth the precision win.
      class fb_size_counter
      {
         std::size_t bytes_     = 0;
         std::size_t min_align_ = 1;
         std::size_t nfields_   = 0;
         std::uint32_t tbl_start_ = 0;
         std::uint16_t max_vt_  = 0;

        public:
         static constexpr bool counts_only = true;

         std::size_t total() const { return bytes_; }
         std::size_t sz() const { return bytes_; }

         [[gnu::always_inline]] void track(std::size_t a)
         {
            if (a > min_align_) min_align_ = a;
         }
         [[gnu::always_inline]] void zero_pad(std::size_t n) { bytes_ += n; }
         [[gnu::always_inline]] void align(std::size_t a)
         {
            const std::size_t p = (~bytes_ + 1) & (a - 1);
            if (p) bytes_ += p;
            track(a);
         }
         [[gnu::always_inline]] void pre_align(std::size_t len,
                                                std::size_t a)
         {
            if (!len) return;
            const std::size_t p = (~(bytes_ + len) + 1) & (a - 1);
            if (p) bytes_ += p;
            track(a);
         }
         template <typename T>
         [[gnu::always_inline]] void push(T)
         {
            bytes_ += sizeof(T);
         }

         [[gnu::always_inline]] void start_table()
         {
            nfields_   = 0;
            tbl_start_ = static_cast<std::uint32_t>(bytes_);
            max_vt_    = 0;
         }
         template <typename T>
         [[gnu::always_inline]] void add_scalar(std::uint16_t vt, T val, T def)
         {
            if (val == def) return;
            align(sizeof(T));
            bytes_ += sizeof(T);
            ++nfields_;
            if (vt > max_vt_) max_vt_ = vt;
         }
         template <typename T>
         [[gnu::always_inline]] void add_scalar_force(std::uint16_t vt, T)
         {
            align(sizeof(T));
            bytes_ += sizeof(T);
            ++nfields_;
            if (vt > max_vt_) max_vt_ = vt;
         }
         [[gnu::always_inline]] void add_offset_field(std::uint16_t vt,
                                                      std::uint32_t off)
         {
            if (!off) return;
            align(sizeof(std::uint32_t));
            bytes_ += sizeof(std::uint32_t);
            ++nfields_;
            if (vt > max_vt_) max_vt_ = vt;
         }

         std::uint32_t end_table()
         {
            // Mirror builder's end_table: 4-byte soffset + vtable bytes.
            align(sizeof(std::int32_t));
            bytes_ += sizeof(std::int32_t);
            const std::uint32_t tbl_off =
               static_cast<std::uint32_t>(bytes_);
            const std::uint16_t vt_size =
               static_cast<std::uint16_t>(std::max<int>(max_vt_ + 2, 4));
            // Always count vtable bytes (no dedup simulation).
            bytes_ += vt_size;
            return tbl_off;
         }

         std::uint32_t create_string(const char*, std::size_t len)
         {
            pre_align(len + 1, sizeof(std::uint32_t));
            bytes_ += 1 + len;  // null terminator + chars
            align(sizeof(std::uint32_t));
            bytes_ += sizeof(std::uint32_t);  // length prefix
            return static_cast<std::uint32_t>(bytes_);
         }
         template <typename T>
         std::uint32_t create_vec_scalar(const T*, std::size_t count)
         {
            const std::size_t body = count * sizeof(T);
            pre_align(body, sizeof(std::uint32_t));
            pre_align(body, sizeof(T));
            bytes_ += body;
            align(sizeof(std::uint32_t));
            bytes_ += sizeof(std::uint32_t);
            return static_cast<std::uint32_t>(bytes_);
         }
         std::uint32_t create_vec_offsets(const std::uint32_t*,
                                           std::size_t count)
         {
            const std::size_t body = count * sizeof(std::uint32_t);
            pre_align(body, sizeof(std::uint32_t));
            bytes_ += body;
            align(sizeof(std::uint32_t));
            bytes_ += sizeof(std::uint32_t);
            return static_cast<std::uint32_t>(bytes_);
         }
      };

      // Pre-create a record's child offsets, then emit its vtable +
      // table. Returns the table's offset (which the caller either
      // records in a parent table's field slot, or finishes as the
      // root).  Generic over Builder type (real builder OR
      // fb_size_counter).
      template <typename Builder, typename T>
      std::uint32_t pre_create(Builder& b, const T& val);

      template <typename Builder, typename V>
      std::uint32_t pre_create_child(Builder& b, const V& val)
      {
         if constexpr (std::is_arithmetic_v<V>)
            return 0;
         else if constexpr (std::is_same_v<V, std::string>)
            return b.create_string(val.data(), val.size());
         else if constexpr (is_optional<V>::value)
         {
            if (val.has_value())
               return pre_create_child<Builder,
                  typename V::value_type>(b, *val);
            return 0;
         }
         else if constexpr (is_vector<V>::value)
         {
            using E = typename V::value_type;
            if (val.empty())
               return 0;
            if constexpr (std::is_arithmetic_v<E>)
               return b.create_vec_scalar(val.data(), val.size());
            else if constexpr (std::is_same_v<E, std::string>)
            {
               std::vector<std::uint32_t> offs(val.size());
               for (std::size_t i = 0; i < val.size(); ++i)
                  offs[i] =
                     b.create_string(val[i].data(), val[i].size());
               return b.create_vec_offsets(offs.data(), offs.size());
            }
            else if constexpr (is_table<E>::value)
            {
               std::vector<std::uint32_t> offs(val.size());
               for (std::size_t i = 0; i < val.size(); ++i)
                  offs[i] = pre_create(b, val[i]);
               return b.create_vec_offsets(offs.data(), offs.size());
            }
            else
            {
               static_assert(sizeof(E) == 0,
                             "psio::flatbuf: unsupported vector element");
            }
         }
         else if constexpr (is_table<V>::value)
         {
            return pre_create(b, val);
         }
         else
         {
            static_assert(sizeof(V) == 0,
                          "psio::flatbuf: unsupported type");
         }
      }

      //  Compile-time check: is every reflected field of T a leaf
      //  scalar (arithmetic / bool / enum) — no children to pre-create?
      //  When true, pre_create skips the offsets[] array entirely and
      //  emits a flat sequence of add_scalar calls.  Matches what flatc
      //  generates for "all-fixed" tables like Validator and Point.
      template <typename T>
      consteval bool all_fields_are_scalars()
      {
         using R = ::psio::reflect<T>;
         return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            return (([&]() {
               using F = typename R::template member_type<Is>;
               return std::is_arithmetic_v<F> || std::is_same_v<F, bool>;
            }()) && ...);
         }(std::make_index_sequence<R::member_count>{});
      }

      template <typename Builder, typename T>
      [[gnu::always_inline]]
      std::uint32_t pre_create(Builder& b, const T& val)
      {
         using R                 = ::psio::reflect<T>;
         constexpr std::size_t N = R::member_count;

         //  Fast path: all-scalar table (Validator, Point, NameRecord).
         //  No children to pre-create → skip the offsets[] array and
         //  the first fold pass.  Just start_table → reverse-order
         //  add_scalar → end_table.
         if constexpr (all_fields_are_scalars<T>())
         {
            b.start_table();
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (([&] {
                  constexpr std::size_t i = N - 1 - Is;
                  using F = typename R::template member_type<i>;
                  const F& fref = val.*(R::template member_pointer<i>);
                  const std::uint16_t vt =
                     static_cast<std::uint16_t>(4 + 2 * i);
                  if constexpr (std::is_same_v<F, bool>)
                     b.template add_scalar<std::uint8_t>(
                        vt, fref ? 1 : 0, 0);
                  else
                     b.template add_scalar<F>(vt, fref, F{});
               }()), ...);
            }(std::make_index_sequence<N>{});
            return b.end_table();
         }
         else
         {
            std::uint32_t offsets[N > 0 ? N : 1]{};
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               ((offsets[Is] =
                    pre_create_child<Builder,
                       typename R::template member_type<Is>>(
                       b, val.*(R::template member_pointer<Is>))),
                ...);
            }(std::make_index_sequence<N>{});

            b.start_table();
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               (([&]
                 {
                    constexpr std::size_t i = N - 1 - Is;
                    using F = typename R::template member_type<i>;
                    const F& fref = val.*(R::template member_pointer<i>);
                    const std::uint16_t vt =
                       static_cast<std::uint16_t>(4 + 2 * i);
                    if constexpr (std::is_same_v<F, bool>)
                       b.template add_scalar<std::uint8_t>(
                          vt, fref ? 1 : 0, 0);
                    else if constexpr (std::is_arithmetic_v<F>)
                       b.template add_scalar<F>(vt, fref, F{});
                    else if constexpr (is_optional<F>::value)
                    {
                       using Inner = typename F::value_type;
                       if (fref.has_value())
                       {
                          if constexpr (std::is_same_v<Inner, bool>)
                             b.template add_scalar_force<std::uint8_t>(
                                vt, *fref ? 1 : 0);
                          else if constexpr (std::is_arithmetic_v<Inner>)
                             b.template add_scalar_force<Inner>(vt, *fref);
                          else if (offsets[i])
                             b.add_offset_field(vt, offsets[i]);
                       }
                    }
                    else
                    {
                       if (offsets[i])
                          b.add_offset_field(vt, offsets[i]);
                    }
                 }()),
                ...);
            }(std::make_index_sequence<N>{});
            return b.end_table();
         }
      }

      // ── Decoder ───────────────────────────────────────────────────────
      //
      // Buffer layout: [u32 root_offset at byte 0]. Root offset counts
      // forward from byte 0 to the start of the root table. A table
      // starts with an int32 back-pointer to its vtable.

      inline std::uint32_t read_u32(const std::uint8_t* p)
      {
         std::uint32_t v;
         std::memcpy(&v, p, 4);
         return v;
      }

      inline std::int32_t read_i32(const std::uint8_t* p)
      {
         std::int32_t v;
         std::memcpy(&v, p, 4);
         return v;
      }

      inline std::uint16_t read_u16(const std::uint8_t* p)
      {
         std::uint16_t v;
         std::memcpy(&v, p, 2);
         return v;
      }

      struct table_ptr
      {
         const std::uint8_t* buf;
         std::uint32_t       tbl_pos;  // offset of table in `buf`

         const std::uint8_t* vtable() const
         {
            return buf + tbl_pos - read_i32(buf + tbl_pos);
         }

         // Returns the field's offset relative to tbl_pos (or 0 if
         // not present in the vtable).
         std::uint16_t field_off(std::uint16_t voffset) const
         {
            const std::uint8_t* vt     = vtable();
            const std::uint16_t vtsize = read_u16(vt);
            if (voffset + 2 > vtsize)
               return 0;
            return read_u16(vt + voffset);
         }

         template <typename T>
         T scalar(std::uint16_t voff, T def) const
         {
            const std::uint16_t fo = field_off(voff);
            if (!fo)
               return def;
            T v;
            std::memcpy(&v, buf + tbl_pos + fo, sizeof(T));
            return v;
         }

         std::string string_at(std::uint16_t voff) const
         {
            const std::uint16_t fo = field_off(voff);
            if (!fo)
               return {};
            // Field cell holds a forward offset to the string header.
            const std::uint32_t abs =
               tbl_pos + fo + read_u32(buf + tbl_pos + fo);
            const std::uint32_t len = read_u32(buf + abs);
            return std::string(reinterpret_cast<const char*>(buf + abs + 4),
                               len);
         }

         table_ptr nested_at(std::uint16_t voff) const
         {
            const std::uint16_t fo = field_off(voff);
            if (!fo)
               return {buf, 0};
            const std::uint32_t abs =
               tbl_pos + fo + read_u32(buf + tbl_pos + fo);
            return {buf, abs};
         }

         std::uint32_t vector_pos(std::uint16_t voff) const
         {
            const std::uint16_t fo = field_off(voff);
            if (!fo)
               return 0;
            return tbl_pos + fo + read_u32(buf + tbl_pos + fo);
         }
      };

      template <typename T>
      void unpack_table(table_ptr t, T& out);

      template <typename V>
      void unpack_field(table_ptr t, int idx, V& out)
      {
         const std::uint16_t vt =
            static_cast<std::uint16_t>(4 + 2 * idx);
         if constexpr (std::is_same_v<V, bool>)
            out = t.scalar<std::uint8_t>(vt, 0) != 0;
         else if constexpr (std::is_arithmetic_v<V>)
            out = t.scalar<V>(vt, V{});
         else if constexpr (std::is_same_v<V, std::string>)
            out = t.string_at(vt);
         else if constexpr (is_optional<V>::value)
         {
            using Inner = typename V::value_type;
            if (!t.field_off(vt))
               return;
            if constexpr (std::is_same_v<Inner, bool>)
               out = t.scalar<std::uint8_t>(vt, 0) != 0;
            else if constexpr (std::is_arithmetic_v<Inner>)
               out = t.scalar<Inner>(vt, Inner{});
            else if constexpr (std::is_same_v<Inner, std::string>)
               out = t.string_at(vt);
         }
         else if constexpr (is_vector<V>::value)
         {
            using E = typename V::value_type;
            const std::uint32_t vec_pos = t.vector_pos(vt);
            if (!vec_pos)
               return;
            const std::uint32_t n = read_u32(t.buf + vec_pos);
            out.resize(n);
            if constexpr (std::is_arithmetic_v<E>)
            {
               if (n)
                  std::memcpy(out.data(), t.buf + vec_pos + 4,
                              n * sizeof(E));
            }
            else if constexpr (std::is_same_v<E, std::string>)
            {
               for (std::uint32_t i = 0; i < n; ++i)
               {
                  const std::uint32_t cell = vec_pos + 4 + i * 4;
                  const std::uint32_t sp =
                     cell + read_u32(t.buf + cell);
                  const std::uint32_t slen = read_u32(t.buf + sp);
                  out[i] = std::string(
                     reinterpret_cast<const char*>(t.buf + sp + 4),
                     slen);
               }
            }
            else if constexpr (is_table<E>::value)
            {
               for (std::uint32_t i = 0; i < n; ++i)
               {
                  const std::uint32_t cell = vec_pos + 4 + i * 4;
                  const std::uint32_t sub =
                     cell + read_u32(t.buf + cell);
                  unpack_table<E>(table_ptr{t.buf, sub}, out[i]);
               }
            }
         }
         else if constexpr (is_table<V>::value)
         {
            const table_ptr nested = t.nested_at(vt);
            if (nested.tbl_pos)
               unpack_table<V>(nested, out);
         }
      }

      template <typename T>
      void unpack_table(table_ptr t, T& out)
      {
         using R = ::psio::reflect<T>;
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            (unpack_field(t, static_cast<int>(Is),
                          out.*(R::template member_pointer<Is>)),
             ...);
         }(std::make_index_sequence<R::member_count>{});
      }

   }  // namespace detail::flatbuf_impl

   struct flatbuf : format_tag_base<flatbuf>
   {
      using preferred_presentation_category = ::psio::binary_category;

      //  Single-pass encode.  Reverted from a two-pass size+write
      //  scheme (mirror of capnp::word_count_of) because flatbuf's
      //  encode logic is too complex to dry-walk cheaply: alignment
      //  padding is position-dependent, vtables need mirrored
      //  construction, offset tables need element counts, etc.  The
      //  twin-walk cost exceeded the savings from skipping the 3-4
      //  grow() reallocs.  SBO + dedup + uninit-growth handles the
      //  amortisation better — see flatbuf encode bench history.
      template <typename T>
         requires detail::flatbuf_impl::is_table<T>::value
      friend std::vector<char> tag_invoke(decltype(::psio::encode),
                                          flatbuf, const T& v)
      {
         detail::flatbuf_impl::builder b;
         const auto root = detail::flatbuf_impl::pre_create(b, v);
         return b.finish(root);
      }

      //  Sink-form encode — caller-supplied vector<char> reused across
      //  iterations.  Builder still allocates its internal buf_ per
      //  call; only the output buffer is reused.
      template <typename T>
         requires detail::flatbuf_impl::is_table<T>::value
      friend void tag_invoke(decltype(::psio::encode),
                             flatbuf,
                             const T&            v,
                             std::vector<char>&  sink)
      {
         detail::flatbuf_impl::builder b;
         const auto root = detail::flatbuf_impl::pre_create(b, v);
         b.finish_into(root, sink);
      }

      template <typename T>
         requires detail::flatbuf_impl::is_table<T>::value
      friend T tag_invoke(decltype(::psio::decode<T>), flatbuf, T*,
                          std::span<const char> bytes)
      {
         const auto* buf =
            reinterpret_cast<const std::uint8_t*>(bytes.data());
         const std::uint32_t root_off =
            detail::flatbuf_impl::read_u32(buf);
         T out{};
         detail::flatbuf_impl::unpack_table<T>(
            detail::flatbuf_impl::table_ptr{buf, root_off}, out);
         return out;
      }

      template <typename T>
         requires detail::flatbuf_impl::is_table<T>::value
      friend std::size_t tag_invoke(decltype(::psio::size_of), flatbuf,
                                    const T& v)
      {
         // One-shot: build the buffer, return its size. No cheaper
         // estimate is available without replicating the pack math.
         const auto out = tag_invoke(::psio::encode, flatbuf{}, v);
         return out.size();
      }

      template <typename T>
         requires detail::flatbuf_impl::is_table<T>::value
      friend codec_status tag_invoke(decltype(::psio::validate<T>),
                                     flatbuf, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (bytes.size() < 4)
            return codec_fail("flatbuf: buffer too small", 0, "flatbuf");
         if (auto st = ::psio::check_max_dynamic_cap<T>(bytes.size(),
                                                         "flatbuf");
             !st.ok())
            return st;
         return codec_ok();
      }

      template <typename T>
         requires detail::flatbuf_impl::is_table<T>::value
      friend codec_status
      tag_invoke(decltype(::psio::validate_strict<T>), flatbuf, T*,
                 std::span<const char> bytes) noexcept
      {
         return tag_invoke(::psio::validate<T>, flatbuf{}, (T*)nullptr,
                           bytes);
      }

      template <typename T>
         requires detail::flatbuf_impl::is_table<T>::value
      friend std::unique_ptr<T>
      tag_invoke(decltype(::psio::make_boxed<T>), flatbuf, T*,
                 std::span<const char> bytes) noexcept
      {
         auto v = std::make_unique<T>();
         const auto* buf =
            reinterpret_cast<const std::uint8_t*>(bytes.data());
         const std::uint32_t root_off =
            detail::flatbuf_impl::read_u32(buf);
         detail::flatbuf_impl::unpack_table<T>(
            detail::flatbuf_impl::table_ptr{buf, root_off}, *v);
         return v;
      }
   };

}  // namespace psio
