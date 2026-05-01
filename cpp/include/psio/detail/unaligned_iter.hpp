#pragma once
//
// psio/detail/unaligned_iter.hpp — alignment-safe range copy from wire bytes.
//
// Several format decoders (bin / borsh / fracpack / pssz / ssz) hit a
// vector-of-arithmetic or vector-of-DWNC-record fast path that wants
// to bulk-populate `std::vector<E>` from a span of wire bytes. The
// natural code shape is:
//
//     const E* first = reinterpret_cast<const E*>(src.data() + pos);
//     out.assign(first, first + n);
//
// libc++ / libstdc++ recognize "pointer to trivially-copyable" and
// fold the assign into a single `__builtin_memcpy`. That's the win
// our format encoders deliberately preserve. The hazard: when the
// source span is not aligned to `alignof(E)`, dereferencing the
// `const E*` is undefined behavior. On strict-alignment hardware
// (aarch64 macOS, some Cortex-A targets) it traps with a bus error —
// SIGBUS — instead of falling through to a slow path. Wire data
// pulled from offset-table-driven heaps in pssz / fracpack is
// frequently misaligned this way.
//
// Fix: dispatch at runtime on `src` alignment.
//
//   - aligned src ⇒ use the `T*` path and keep libc++'s memcpy fast
//     path firing. Identical generated code to the original.
//   - misaligned src ⇒ use `unaligned_iter<E>`, whose `operator*`
//     does `std::memcpy(&v, p, sizeof(E))`. The compiler lowers that
//     to a single unaligned load (`ldur` on aarch64; plain `mov` on
//     x86, which has no alignment requirement). The libc++ memcpy
//     fast path doesn't fire (custom iterator, not a pointer), but
//     the per-element loop with unaligned loads/stores vectorizes
//     and stays within ~5% of the bulk path at any size that matters.
//
// Micro-bench (apple-silicon, clang-22 -O2, see /tmp/bench_assign2.cpp
// for the harness):
//
//     N=16384  T=u32  ptr.aligned=1067  iter.aligned=1074  iter.mis=1140
//     N=1024   T=u32  ptr.aligned=  55  iter.aligned=  88  iter.mis=  50
//
// At small/medium N the iterator path is occasionally 60% slower than
// the pointer path on aligned input, because libc++'s pointer
// fast-path detection bypasses the per-element constructor loop.
// Dispatch dodges that hazard while staying correct on misaligned
// input. The branch overhead (one and+cmp+test) is amortized over
// the assign work and is unmeasurable at every N tested.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

namespace psio::detail {

   // Random-access input iterator over a `std::byte`-typed buffer that
   // produces values of type `T` via memcpy. Used as the fallback path
   // by `assign_from_wire` when `src` is not `alignof(T)`-aligned.
   //
   // The returned reference type is `T` (by value) — `assign` and
   // friends accept this for InputIterator-shaped iterators with
   // trivial value types.
   template <class T>
   class unaligned_iter
   {
      const std::byte* p_ = nullptr;

     public:
      using value_type        = T;
      using reference         = T;
      using pointer           = void;
      using difference_type   = std::ptrdiff_t;
      using iterator_category = std::random_access_iterator_tag;

      constexpr unaligned_iter() = default;
      constexpr explicit unaligned_iter(const std::byte* p) noexcept : p_(p) {}
      constexpr explicit unaligned_iter(const char*      p) noexcept
          : p_(reinterpret_cast<const std::byte*>(p)) {}

      T operator*() const noexcept
      {
         T v;
         std::memcpy(&v, p_, sizeof(T));
         return v;
      }
      T operator[](difference_type n) const noexcept
      {
         T v;
         std::memcpy(&v, p_ + n * sizeof(T), sizeof(T));
         return v;
      }

      unaligned_iter& operator++() noexcept    { p_ += sizeof(T); return *this; }
      unaligned_iter  operator++(int) noexcept { auto t = *this; ++(*this); return t; }
      unaligned_iter& operator--() noexcept    { p_ -= sizeof(T); return *this; }
      unaligned_iter  operator--(int) noexcept { auto t = *this; --(*this); return t; }
      unaligned_iter& operator+=(difference_type n) noexcept { p_ += n * sizeof(T); return *this; }
      unaligned_iter& operator-=(difference_type n) noexcept { p_ -= n * sizeof(T); return *this; }
      unaligned_iter  operator+(difference_type n) const noexcept { auto t = *this; t += n; return t; }
      unaligned_iter  operator-(difference_type n) const noexcept { auto t = *this; t -= n; return t; }
      difference_type operator-(unaligned_iter o) const noexcept
      { return (p_ - o.p_) / static_cast<difference_type>(sizeof(T)); }

      bool operator==(unaligned_iter o) const noexcept { return p_ == o.p_; }
      bool operator!=(unaligned_iter o) const noexcept { return p_ != o.p_; }
      bool operator< (unaligned_iter o) const noexcept { return p_ <  o.p_; }
      bool operator<=(unaligned_iter o) const noexcept { return p_ <= o.p_; }
      bool operator> (unaligned_iter o) const noexcept { return p_ >  o.p_; }
      bool operator>=(unaligned_iter o) const noexcept { return p_ >= o.p_; }
   };

   // Bulk-populate `out` with `n` copies of `T` read from `src`. Picks
   // the libc++ pointer fast path when `src` is `alignof(T)`-aligned;
   // falls back to a memcpy-driven iterator on strict-alignment hardware
   // when the source is misaligned. T must be trivially copyable.
   template <class T, class Alloc>
   inline void assign_from_wire(std::vector<T, Alloc>& out,
                                const char*            src,
                                std::size_t            n) noexcept
   {
      static_assert(std::is_trivially_copyable_v<T>,
                    "assign_from_wire: T must be trivially copyable");
      const auto addr = reinterpret_cast<std::uintptr_t>(src);
      if ((addr & (alignof(T) - 1)) == 0)
      {
         const T* first = reinterpret_cast<const T*>(src);
         out.assign(first, first + n);
      }
      else
      {
         const auto* bp = reinterpret_cast<const std::byte*>(src);
         out.assign(unaligned_iter<T>{bp},
                    unaligned_iter<T>{bp + n * sizeof(T)});
      }
   }

}  // namespace psio::detail
