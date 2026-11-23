// psio::validate_strict<T>(psio::pssz{}, bytes) walks the bytes in place: it
// does not allocate, whatever the size of the message.
//
// Built the way a wasm guest builds psio — -fno-exceptions -fno-rtti,
// PSIO_EXCEPTIONS_ENABLED=0 — with the global operator new replaced by one
// that counts. Messages of several megabytes (a psibase transaction in
// miniature: 100000 actions carrying raw byte payloads; a list of 100000
// records with strings and optionals; the transaction as a nested member)
// are validated and the number of allocations made during the call is
// required to be zero. The reference implementation (pssz_strict_oracle.hpp:
// decode, encode again, compare) is run on the same bytes for its allocation
// count, its peak memory and its time.
//
//    psio_pssz_strict_alloc_check            the checks, and the numbers
//    psio_pssz_strict_alloc_check <rounds>   more timing rounds

#include "pssz_strict_oracle.hpp"

#include <psio/nested.hpp>
#include <psio/pssz.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <vector>

#if defined(__cpp_exceptions)
#error "this file is built with -fno-exceptions"
#endif

// The build-time abort handler of a guest (-DPSIO_CODEC_ABORT_HANDLER=...):
// validation returns a status and never reaches it.
extern "C" void psio_strict_alloc_abort(const char* what, std::size_t what_size, std::uint32_t,
                                        const char*, std::size_t)
{
   std::printf("FAILED: abort handler called: %.*s\n", int(what_size), what);
   std::_Exit(2);
}

// ── A counting allocator ─────────────────────────────────────────────────
namespace
{
   std::size_t g_allocations = 0;
   std::size_t g_live        = 0;
   std::size_t g_peak        = 0;
   constexpr std::size_t kHeader = 16;

   void* counted_alloc(std::size_t size) noexcept
   {
      void* raw = std::malloc(size + kHeader);
      if (!raw)
         std::abort();
      *static_cast<std::size_t*>(raw) = size;
      ++g_allocations;
      g_live += size;
      if (g_live > g_peak)
         g_peak = g_live;
      return static_cast<char*>(raw) + kHeader;
   }
   void counted_free(void* p) noexcept
   {
      if (!p)
         return;
      void* raw = static_cast<char*>(p) - kHeader;
      g_live -= *static_cast<std::size_t*>(raw);
      std::free(raw);
   }
}  // namespace

void* operator new(std::size_t size) { return counted_alloc(size); }
void* operator new[](std::size_t size) { return counted_alloc(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return counted_alloc(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
   return counted_alloc(size);
}
void operator delete(void* p) noexcept { counted_free(p); }
void operator delete[](void* p) noexcept { counted_free(p); }
void operator delete(void* p, std::size_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::size_t) noexcept { counted_free(p); }

namespace
{
   using u8  = std::uint8_t;
   using u16 = std::uint16_t;
   using u32 = std::uint32_t;
   using u64 = std::uint64_t;

   // psibase::Transaction in miniature.
   struct Tapos
   {
      u32 expiration     = 0;
      u32 refBlockSuffix = 0;
      u16 flags          = 0;
      u8  refBlockIndex  = 0;
      PSIO_REFLECT(Tapos, definitionWillNotChange(), expiration, refBlockSuffix, flags,
                   refBlockIndex)
   };
   struct Account
   {
      u64 value = 0;
      PSIO_REFLECT(Account, definitionWillNotChange(), value)
   };
   struct Action
   {
      Account           sender;
      Account           service;
      Account           method;
      std::vector<char> rawData;
      PSIO_REFLECT(Action, sender, service, method, rawData)
   };
   struct Claim
   {
      Account           service;
      std::vector<char> rawData;
      PSIO_REFLECT(Claim, service, rawData)
   };
   struct Transaction
   {
      Tapos               tapos;
      std::vector<Action> actions;
      std::vector<Claim>  claims;
      PSIO_REFLECT(Transaction, tapos, actions, claims)
   };
   struct SignedTransaction
   {
      psio::nested<Transaction>                     transaction;
      std::vector<std::vector<char>>                proofs;
      std::optional<std::vector<std::vector<char>>> subjectiveData;
      PSIO_REFLECT(SignedTransaction, transaction, proofs, subjectiveData)
   };
   // Records with strings, an optional, a variable list and a utf8 member.
   struct Row
   {
      u64                        id = 0;
      std::string                name;
      std::optional<std::string> memo;
      std::vector<u32>           numbers;
      std::string                text;
      PSIO_REFLECT(Row, id, name, memo, numbers, attr(text, utf8))
   };
   struct Table
   {
      std::string      title;
      std::vector<Row> rows;
      PSIO_REFLECT(Table, title, rows)
   };

   std::uint64_t g_state = 0x9e3779b97f4a7c15ull;
   std::uint64_t next()
   {
      g_state ^= g_state << 13;
      g_state ^= g_state >> 7;
      g_state ^= g_state << 17;
      return g_state;
   }

   Transaction make_transaction(std::size_t actions)
   {
      Transaction t;
      t.tapos = Tapos{1, 2, 0, 3};
      t.actions.reserve(actions);
      for (std::size_t i = 0; i < actions; ++i)
      {
         Action a{Account{next()}, Account{next()}, Account{next()}, {}};
         a.rawData.resize(16 + next() % 64);
         for (char& c : a.rawData)
            c = static_cast<char>(next());
         t.actions.push_back(std::move(a));
      }
      for (std::size_t i = 0; i < 100; ++i)
         t.claims.push_back(Claim{Account{next()}, std::vector<char>(33, char(i))});
      return t;
   }
   Table make_table(std::size_t rows)
   {
      Table t;
      t.title = "rows";
      t.rows.reserve(rows);
      for (std::size_t i = 0; i < rows; ++i)
      {
         Row r;
         r.id   = i;
         r.name = std::string(4 + next() % 24, char('a' + i % 26));
         if (i % 3)
            r.memo = std::string(next() % 12, 'm');
         r.numbers.assign(next() % 5, static_cast<u32>(i));
         r.text = std::string(next() % 20, 't');
         t.rows.push_back(std::move(r));
      }
      return t;
   }

   struct Measure
   {
      bool        ok          = false;
      std::size_t allocations = 0;
      std::size_t peak        = 0;  // bytes live above the level at entry
      double      seconds     = 0;  // best of the rounds
   };
   template <typename F>
   Measure measure(std::size_t rounds, F&& f)
   {
      Measure m;
      m.seconds = 1e30;
      for (std::size_t round = 0; round < rounds; ++round)
      {
         const std::size_t allocations = g_allocations;
         const std::size_t live        = g_live;
         g_peak                        = g_live;
         const auto begin              = std::chrono::steady_clock::now();
         m.ok                          = f();
         const auto end                = std::chrono::steady_clock::now();
         m.allocations                 = g_allocations - allocations;
         m.peak                        = g_peak - live;
         m.seconds = std::min(m.seconds, std::chrono::duration<double>(end - begin).count());
      }
      return m;
   }

   int failures = 0;
   void expect(bool condition, const char* what)
   {
      if (!condition)
      {
         std::printf("FAILED: %s\n", what);
         ++failures;
      }
   }

   template <typename T>
   void run(const char* name, const std::vector<char>& bytes, std::size_t rounds)
   {
      const std::span<const char> span{bytes};
      const double                mb = double(bytes.size()) / (1024.0 * 1024.0);
      const Measure in_place = measure(rounds, [&] {
         return psio::validate_strict<T>(psio::pssz{}, span).ok();
      });
      const Measure plain = measure(rounds, [&] {
         return psio::validate<T>(psio::pssz{}, span).ok();
      });
      const Measure reference = measure(rounds, [&] {
         return strict_oracle::validate<T>(psio::pssz{}, span).ok();
      });
      std::printf("%s: %.2f MB\n", name, mb);
      std::printf("  validate_strict (in place)   allocations=%zu peak=%zu bytes  %.3f ms  "
                  "%.0f MB/s\n",
                  in_place.allocations, in_place.peak, in_place.seconds * 1e3,
                  mb / in_place.seconds);
      std::printf("  validate (structural)        allocations=%zu peak=%zu bytes  %.3f ms  "
                  "%.0f MB/s\n",
                  plain.allocations, plain.peak, plain.seconds * 1e3, mb / plain.seconds);
      std::printf("  reference (decode+re-encode) allocations=%zu peak=%zu bytes (%.2fx the "
                  "message)  %.3f ms  %.0f MB/s\n",
                  reference.allocations, reference.peak,
                  double(reference.peak) / double(bytes.size()), reference.seconds * 1e3,
                  mb / reference.seconds);
      std::printf("  in place vs reference: %.1fx faster\n", reference.seconds / in_place.seconds);
      expect(in_place.ok, "validate_strict accepts the encoding");
      expect(reference.ok, "the reference accepts the encoding");
      expect(in_place.allocations == 0, "validate_strict allocates nothing");
      expect(in_place.peak == 0, "validate_strict holds no heap memory");
      // (A nested member is bytes to the reference's decode: it copies them.)
      expect(reference.peak >= bytes.size(), "the reference materializes the value");

      // A rejected message costs one allocation, the error a failed
      // codec_status carries: half the message, a changed record header.
      // (The last byte removed or one appended is a shorter or longer last
      // payload here: the two validators must agree on it.)
      for (int kind = 0; kind < 4; ++kind)
      {
         std::vector<char> bad = bytes;
         if (kind == 0)
            bad.resize(bad.size() / 2);
         else if (kind == 1)
            bad[0] ^= 0x01;
         else if (kind == 2)
            bad.pop_back();
         else
            bad.push_back(0);
         const std::span<const char> bad_span{bad};
         const Measure               rejected = measure(1, [&] {
            return psio::validate_strict<T>(psio::pssz{}, bad_span).ok();
         });
         expect(rejected.allocations <= 1, "a rejected message allocates its error only");
         expect(rejected.peak < 256, "a rejected message holds no heap memory");
         expect(rejected.ok == strict_oracle::validate<T>(psio::pssz{}, bad_span).ok(),
                "validate_strict and the reference agree on a damaged message");
         if (kind < 2)
            expect(!rejected.ok, "a damaged message is rejected");
      }
   }
}  // namespace

int main(int argc, char** argv)
{
   const std::size_t rounds = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 5;
   {
      const auto bytes = psio::to_pssz(make_transaction(100000));
      run<Transaction>("Transaction, 100000 actions with raw payloads", bytes, rounds);
      // As psibase carries it: the transaction's bytes in a nested member.
      SignedTransaction signed_tx;
      signed_tx.transaction = psio::nested<Transaction>::unchecked(bytes);
      signed_tx.proofs      = {std::vector<char>(64, 1), std::vector<char>(64, 2)};
      run<SignedTransaction>("SignedTransaction, the transaction nested",
                             psio::to_pssz(signed_tx), rounds);
   }
   {
      const auto bytes = psio::to_pssz(make_table(100000));
      run<Table>("Table, 100000 rows with strings", bytes, rounds);
   }
   if (failures)
      return 1;
   std::printf("pssz-strict-alloc=PASS\n");
   return 0;
}
