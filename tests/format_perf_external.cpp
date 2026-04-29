// Head-to-head: psio::msgpack vs msgpack-cxx.
//
// Three benchmarks:
//
//   (1) Typed encode/decode  — psio::encode<T>/decode<T> with
//       PSIO_REFLECT vs msgpack::pack/msgpack::unpack with
//       MSGPACK_DEFINE.  Same struct, identical wire bytes.
//
//   (2) Schemaless name-lookup, single random access — both
//       surfaces resolve the SAME path by name (no positional
//       shortcut for either).  Measured START to FINISH:
//       hand the binary in, get the typed value out.
//
//   (3) Schemaless name-lookup, K random accesses — same pattern
//       but K accesses against the same blob, K varying.  Surfaces
//       the parse-amortization curve: msgpack pays a one-time
//       unpack cost, then per-access is cheap; pjson skips the
//       parse but per-access is costlier.  We measure (parse + K
//       accesses) total ns and report ns/access for K in
//       {1, 2, 4, 8, 16, 32, 64} so the crossover is visible.
//
// Build only when msgpack-cxx is present; see the parent
// CMakeLists.txt.  Run manually:
//
//     ./build/Debug-rename/bin/psio_format_perf_external

#include <psio/msgpack.hpp>
#include <psio/pjson_typed.hpp>
#include <psio/pjson_view.hpp>

#include <msgpack.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace bench
{
   struct Sub
   {
      std::int32_t id;
      std::string  label;

      MSGPACK_DEFINE_MAP(id, label);
   };

   struct Bag
   {
      std::string                 name;
      std::vector<std::uint8_t>   payload;
      std::vector<std::int32_t>   ids;
      std::vector<Sub>            entries;
      std::int64_t                seq;
      double                      score;

      MSGPACK_DEFINE_MAP(name, payload, ids, entries, seq, score);
   };

   PSIO_REFLECT(Sub, id, label)
   PSIO_REFLECT(Bag, name, payload, ids, entries, seq, score)

   inline Bag make_sample()
   {
      return Bag{
         .name    = std::string{"alice-the-quick-brown-fox"},
         .payload = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
         .ids     = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
         .entries = {Sub{1, std::string{"alpha"}},
                     Sub{2, std::string{"beta"}},
                     Sub{3, std::string{"gamma"}},
                     Sub{4, std::string{"delta"}}},
         .seq     = 123456789012LL,
         .score   = 3.14159265358979};
   }
}  // namespace bench

//  Tell psio's msgpack codec to encode Bag/Sub as fixmap (named) so
//  the schemaless walks below are apples-to-apples vs msgpack-cxx's
//  MSGPACK_DEFINE_MAP form.
template <>
struct psio::msgpack_record_form<bench::Bag>
{
   static constexpr bool as_map = true;
};
template <>
struct psio::msgpack_record_form<bench::Sub>
{
   static constexpr bool as_map = true;
};

using clk = std::chrono::steady_clock;

template <typename F>
double bench_ns(F&& f, std::size_t iters)
{
   auto t0 = clk::now();
   for (std::size_t i = 0; i < iters; ++i)
      f();
   auto t1 = clk::now();
   auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                .count();
   return static_cast<double>(ns) / static_cast<double>(iters);
}

//  ── Path representation for the schemaless walks ──────────────────
//
//  Each step is either a name lookup (into a map) or an index lookup
//  (into an array).  Paths are short (≤ 3 steps for our struct) and
//  resolve to one of the leaf scalar types we can extract: string,
//  int64, or double.
struct Step
{
   enum Kind { Name, Index } kind;
   std::string_view          name;
   std::size_t               idx;
};

struct Path
{
   std::array<Step, 3>       steps;
   std::size_t               len;
   enum Leaf { String, Int64, Dbl }
        leaf;
};

//  Helpers to make Path literals readable.
inline Step name_(std::string_view n) { return Step{Step::Name, n, 0}; }
inline Step idx_(std::size_t i)       { return Step{Step::Index, {}, i}; }

inline Path p1(Step a, Path::Leaf leaf)
{
   Path p;
   p.steps[0] = a;
   p.len      = 1;
   p.leaf     = leaf;
   return p;
}
inline Path p2(Step a, Step b, Path::Leaf leaf)
{
   Path p;
   p.steps[0] = a;
   p.steps[1] = b;
   p.len      = 2;
   p.leaf     = leaf;
   return p;
}
inline Path p3(Step a, Step b, Step c, Path::Leaf leaf)
{
   Path p;
   p.steps[0] = a;
   p.steps[1] = b;
   p.steps[2] = c;
   p.len      = 3;
   p.leaf     = leaf;
   return p;
}

inline std::vector<Path> all_paths()
{
   //  16 distinct paths covering every leaf the Bag has.  The
   //  benchmark shuffles this list and uses the first K each pass.
   return {
      p1(name_("name"),  Path::String),
      p1(name_("score"), Path::Dbl),
      p1(name_("seq"),   Path::Int64),
      p2(name_("ids"), idx_(0),  Path::Int64),
      p2(name_("ids"), idx_(7),  Path::Int64),
      p2(name_("ids"), idx_(15), Path::Int64),
      p2(name_("ids"), idx_(3),  Path::Int64),
      p2(name_("ids"), idx_(11), Path::Int64),
      p3(name_("entries"), idx_(0), name_("id"),    Path::Int64),
      p3(name_("entries"), idx_(0), name_("label"), Path::String),
      p3(name_("entries"), idx_(1), name_("id"),    Path::Int64),
      p3(name_("entries"), idx_(1), name_("label"), Path::String),
      p3(name_("entries"), idx_(2), name_("id"),    Path::Int64),
      p3(name_("entries"), idx_(2), name_("label"), Path::String),
      p3(name_("entries"), idx_(3), name_("id"),    Path::Int64),
      p3(name_("entries"), idx_(3), name_("label"), Path::String),
   };
}

//  ── msgpack::object resolver ──────────────────────────────────────
//
//  Walks a path through an already-unpacked msgpack::object tree.
//  Map step = O(N) scan over kv pairs (msgpack::object has no key
//  index).  Array step = O(1) pointer offset.
inline msgpack::object const& mp_resolve(msgpack::object const& root,
                                         Path const&            p)
{
   msgpack::object const* cur = &root;
   for (std::size_t i = 0; i < p.len; ++i)
   {
      auto const& step = p.steps[i];
      if (step.kind == Step::Name)
      {
         //  Linear scan of the map's key/value pairs.
         bool       found = false;
         auto const sz    = cur->via.map.size;
         for (std::uint32_t k = 0; k < sz; ++k)
         {
            auto const& kv = cur->via.map.ptr[k];
            if (kv.key.type == msgpack::type::STR &&
                kv.key.via.str.size == step.name.size() &&
                std::memcmp(kv.key.via.str.ptr, step.name.data(),
                            step.name.size()) == 0)
            {
               cur   = &kv.val;
               found = true;
               break;
            }
         }
         if (!found)
            throw std::runtime_error("mp_resolve: key not found");
      }
      else
      {
         cur = &cur->via.array.ptr[step.idx];
      }
   }
   return *cur;
}

//  ── Consume the leaf ──────────────────────────────────────────────
//  (forward-declared here so the resolvers below can call them)
inline void consume_mp(msgpack::object const& obj, Path::Leaf leaf);
inline void consume_pj(psio::pjson_view v, Path::Leaf leaf);

//  ── pjson_view walker + consumer ─────────────────────────────────
//
//  pjson_view::at() now works uniformly on typed and generic arrays
//  (returns a synthetic view over typed-element bytes when the parent
//  is a typed array).  No more bespoke at-the-leaf branching here.
inline void pj_walk_consume(psio::pjson_view root, Path const& p)
{
   psio::pjson_view cur = root;
   for (std::size_t i = 0; i < p.len; ++i)
   {
      auto const& step = p.steps[i];
      if (step.kind == Step::Name)
      {
         auto found = cur.find(step.name);
         if (!found)
            throw std::runtime_error("pj_walk: key not found");
         cur = *found;
      }
      else
      {
         cur = cur.at(step.idx);
      }
   }
   consume_pj(cur, p.leaf);
}

//  ── Consume the leaf ──────────────────────────────────────────────
//
//  "Consume" = extract the leaf as a strongly-typed value the way an
//  application would actually use it (string copy / int / double).
//  We sink the value into a volatile sink to keep the optimizer from
//  hoisting the work.
inline void consume_mp(msgpack::object const& obj, Path::Leaf leaf)
{
   switch (leaf)
   {
      case Path::String:
      {
         std::string s;
         obj.convert(s);
         asm volatile("" : : "r"(s.data()) : "memory");
         break;
      }
      case Path::Int64:
      {
         std::int64_t i;
         obj.convert(i);
         asm volatile("" : : "r"(&i) : "memory");
         break;
      }
      case Path::Dbl:
      {
         double d;
         obj.convert(d);
         asm volatile("" : : "r"(&d) : "memory");
         break;
      }
   }
}

inline void consume_pj(psio::pjson_view v, Path::Leaf leaf)
{
   switch (leaf)
   {
      case Path::String:
      {
         std::string s = std::string(v.as_string());
         asm volatile("" : : "r"(s.data()) : "memory");
         break;
      }
      case Path::Int64:
      {
         std::int64_t i = v.as_int64();
         asm volatile("" : : "r"(&i) : "memory");
         break;
      }
      case Path::Dbl:
      {
         double d = v.as_double();
         asm volatile("" : : "r"(&d) : "memory");
         break;
      }
   }
}

int main()
{
   constexpr std::size_t N = 100'000;

   bench::Bag sample = bench::make_sample();

   //  ── (1) Typed encode/decode ────────────────────────────────────
   auto psio_bytes = psio::encode(psio::msgpack{}, sample);
   msgpack::sbuffer mp_sbuf;
   msgpack::pack(mp_sbuf, sample);
   bool wire_match = (mp_sbuf.size() == psio_bytes.size()) &&
                     (std::memcmp(mp_sbuf.data(), psio_bytes.data(),
                                  mp_sbuf.size()) == 0);

   std::printf("== Typed encode/decode (fixmap form on both sides) ==\n");
   std::printf("  wire size — psio       : %zu\n", psio_bytes.size());
   std::printf("  wire size — msgpack-cxx: %zu  (byte-identical: %s)\n",
               mp_sbuf.size(), wire_match ? "YES" : "no");

   auto enc_psio = bench_ns(
      [&] {
         auto out = psio::encode(psio::msgpack{}, sample);
         asm volatile("" : : "r"(out.data()) : "memory");
      },
      N);
   auto dec_psio = bench_ns(
      [&] {
         auto out = psio::decode<bench::Bag>(
            psio::msgpack{}, std::span<const char>{psio_bytes});
         asm volatile("" : : "r"(&out) : "memory");
      },
      N);
   auto enc_mpx = bench_ns(
      [&] {
         msgpack::sbuffer sbuf;
         msgpack::pack(sbuf, sample);
         asm volatile("" : : "r"(sbuf.data()) : "memory");
      },
      N);
   auto dec_mpx = bench_ns(
      [&] {
         msgpack::object_handle oh =
            msgpack::unpack(mp_sbuf.data(), mp_sbuf.size());
         bench::Bag out;
         oh.get().convert(out);
         asm volatile("" : : "r"(&out) : "memory");
      },
      N);
   std::printf("\n  ns/op  N=%zu\n", N);
   std::printf("                       encode      decode      round-trip\n");
   std::printf("  psio::msgpack    :  %8.1f   %8.1f   %8.1f\n",
               enc_psio, dec_psio, enc_psio + dec_psio);
   std::printf(
      "  msgpack-cxx      :  %8.1f   %8.1f   %8.1f   "
      "(psio is %.2fx encode, %.2fx decode)\n",
      enc_mpx, dec_mpx, enc_mpx + dec_mpx, enc_mpx / enc_psio,
      dec_mpx / dec_psio);

   //  ── (2)+(3) Schemaless lookup curve ────────────────────────────
   //
   //  Same fixmap-encoded bytes for psio->pjson is not directly
   //  comparable, so we encode the SAME logical sample into pjson
   //  and benchmark the schemaless walks side-by-side.  msgpack
   //  uses the fixmap-form bytes already in mp_sbuf; pjson uses
   //  pjson_bytes.
   auto pjson_bytes = psio::from_struct(sample);

   auto paths_master = all_paths();
   //  Deterministic shuffle for stable comparison across runs.
   std::mt19937 rng{0xc0ffeeu};

   std::printf(
      "\n== Schemaless name-lookup, random-order, "
      "(parse + K accesses) total ==\n");
   std::printf(
      "  Both surfaces resolve the same paths by NAME — no\n"
      "  positional shortcuts.  Timing starts when the binary is\n"
      "  handed in and ends when all K leaves have been consumed\n"
      "  as strongly-typed values.\n\n");
   std::printf("    K   msgpack::object   pjson_view   crossover\n");
   std::printf("        ns/iter          ns/iter      msgpack/pjson\n");
   std::printf("    -- ---------------- ------------ ----------------\n");

   for (std::size_t K : {std::size_t{1}, std::size_t{2}, std::size_t{4},
                         std::size_t{8}, std::size_t{16}, std::size_t{32},
                         std::size_t{64}})
   {
      //  Build a shuffled access list of length K (sample with
      //  repetition so K can exceed the path-master size).
      std::vector<Path> accesses;
      accesses.reserve(K);
      std::uniform_int_distribution<std::size_t> dist(
         0, paths_master.size() - 1);
      for (std::size_t i = 0; i < K; ++i)
         accesses.push_back(paths_master[dist(rng)]);

      auto walk_mp = bench_ns(
         [&] {
            //  Hand the binary in, parse, walk K paths, consume.
            msgpack::object_handle oh =
               msgpack::unpack(mp_sbuf.data(), mp_sbuf.size());
            msgpack::object root = oh.get();
            for (auto const& p : accesses)
               consume_mp(mp_resolve(root, p), p.leaf);
         },
         N);

      auto walk_pj = bench_ns(
         [&] {
            psio::pjson_view root{pjson_bytes.data(), pjson_bytes.size()};
            for (auto const& p : accesses)
               pj_walk_consume(root, p);
         },
         N);

      double ratio = walk_mp / walk_pj;
      const char* winner = ratio > 1.0 ? "pjson wins"
                          : ratio < 1.0 ? "msgpack wins"
                                        : "tied";
      std::printf("    %2zu   %12.1f    %10.1f    %5.2fx (%s)\n",
                  K, walk_mp, walk_pj, ratio, winner);
   }
   std::printf(
      "\n  Crossover heuristic: msgpack pays a one-time unpack cost\n"
      "  (~250 ns) then per-access is ~6 ns (linear scan over a\n"
      "  fixmap's kv pairs); pjson skips the parse but per-access\n"
      "  is ~22 ns.  Math says crossover ≈ 250 / (22 - 6) ≈ 16\n"
      "  accesses.  The table above shows it empirically.\n");

   return 0;
}
