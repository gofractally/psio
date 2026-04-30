// bench_pjson_matched.cpp — pjson encode/decode/validate/view_one/view_find
// across the 5 spec Appendix-C shapes (Point, NameRecord, FlatRecord,
// Record, Validator).  The matrix mirrors the Rust criterion bench at
// `rust/psio/benches/pjson_bench.rs` 1:1 so cells can be diffed
// language-to-language without methodology mismatches.
//
// Each cell measures one operation on one shape with the same iteration
// rotation and antidce pattern.  Output is one row per cell:
//
//     cell_label,shape,op,ns_per_iter,wire_bytes,iters,trials
//
// Run:  psio_bench_pjson_matched [output.csv]
//       (CSV path defaults to stdout if not given)

#include "harness.hpp"
#include "shapes.hpp"

#include <psio/pjson.hpp>
#include <psio/pjson_typed.hpp>
#include <psio/pjson_view.hpp>
#include <psio/reflect.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kAntiDceK = 16;

template <typename T>
T vary(const T& base, std::size_t /*idx*/)
{
   //  Antidce variation: the bench loop does not need to vary the value
   //  for compiler antidce — a `volatile sink ^= ...` pattern is
   //  sufficient on Apple Clang at -O3.  We keep K copies anyway so the
   //  loop's cache locality matches the Rust criterion bench.
   return base;
}

template <typename T>
void run_shape(std::ostream& out, const std::string& shape_name, const T& v)
{
   using namespace psio_bench;

   //  Pre-encode K copies of the buffer so encode/decode reads share
   //  the same input rotation as the Rust bench.
   std::array<T, kAntiDceK>                             vals;
   std::array<std::vector<std::uint8_t>, kAntiDceK>     bufs;
   std::array<std::span<const std::uint8_t>, kAntiDceK> spans;
   for (std::size_t i = 0; i < kAntiDceK; ++i)
   {
      vals[i]  = vary(v, i);
      bufs[i]  = ::psio::from_struct(vals[i]);
      spans[i] = std::span<const std::uint8_t>{bufs[i].data(),
                                               bufs[i].size()};
   }
   const std::size_t wire = bufs[0].size();

   auto write_row = [&](std::string_view op, double ns,
                        double median_ns, double cv,
                        std::size_t iters, int trials)
   {
      out << shape_name << ',' << op << ',' << ns << ',' << median_ns
          << ',' << cv << ',' << wire << ',' << iters << ',' << trials
          << '\n';
   };
   auto cv_of = [](const timing& t) {
      return t.min_ns > 0.0 ? t.stddev_ns / t.min_ns * 100.0 : 0.0;
   };

   //  encode — to_pjson into a reused sink (same as Rust criterion's
   //  to_pjson(&val) call which Vec<u8>-allocates each iter).
   {
      volatile std::uint64_t sink_enc = 0;
      auto t = ns_per_iter(0u, [&](std::size_t i) {
         auto b = ::psio::from_struct(vals[i & (kAntiDceK - 1)]);
         sink_enc ^= b.size();
         if (!b.empty()) sink_enc ^= b[0];
      });
      write_row("encode", t.min_ns, t.median_ns, cv_of(t), t.iters,
                t.trials);
   }

   //  decode — from_pjson via view::to_struct().
   {
      volatile std::uint64_t sink_dec = 0;
      auto t = ns_per_iter(0u, [&](std::size_t i) {
         const auto& sp = spans[i & (kAntiDceK - 1)];
         auto raw       = ::psio::pjson_view{sp.data(), sp.size()};
         auto tv = ::psio::view<T, ::psio::pjson_format>::from_pjson(raw);
         T native = tv.to_struct();
         //  Pull a scalar field through the volatile sink so the decode
         //  isn't elided.
         sink_dec ^= reinterpret_cast<const std::uint8_t*>(&native)[0];
      });
      write_row("decode", t.min_ns, t.median_ns, cv_of(t), t.iters,
                t.trials);
   }

   //  validate — pjson::validate (recursive walk).
   {
      volatile std::uint64_t sink_val = 0;
      auto t = ns_per_iter(0u, [&](std::size_t i) {
         const auto& sp = spans[i & (kAntiDceK - 1)];
         sink_val ^=
            static_cast<std::uint64_t>(::psio::pjson::validate(sp));
      });
      write_row("validate", t.min_ns, t.median_ns, cv_of(t), t.iters,
                t.trials);
   }

   //  view_one — typed canonical view, single field<0> read.
   {
      volatile std::uint64_t sink_v = 0;
      auto t = ns_per_iter(0u, [&](std::size_t i) {
         const auto& sp = spans[i & (kAntiDceK - 1)];
         auto raw = ::psio::pjson_view{sp.data(), sp.size()};
         auto tv = ::psio::view<T, ::psio::pjson_format>::from_pjson(raw);
         //  field<0>() goes through the canonical fast path when the
         //  buffer was produced by from_struct (always the case here).
         auto f0 = tv.template get<0>();
         sink_v ^= static_cast<std::uint64_t>(f0);
      });
      write_row("view_one", t.min_ns, t.median_ns, cv_of(t), t.iters,
                t.trials);
   }

   //  view_find — schemaless lookup of the first field by name.  We
   //  use the spec-canonical first key for each shape so the result is
   //  matched to the Rust bench's `pjson_view_find` cell.
   using R = ::psio::reflect<T>;
   static_assert(R::is_reflected);
   const std::string_view first_key = R::template member_name<0>;
   {
      volatile std::uint64_t sink_f = 0;
      auto t = ns_per_iter(0u, [&](std::size_t i) {
         const auto& sp = spans[i & (kAntiDceK - 1)];
         auto raw = ::psio::pjson_view{sp.data(), sp.size()};
         auto fv = raw.find(first_key);
         if (fv) sink_f ^= static_cast<std::uint64_t>(fv->size());
      });
      write_row("view_find", t.min_ns, t.median_ns, cv_of(t), t.iters,
                t.trials);
   }
}

}  // namespace

int main(int argc, char** argv)
{
   std::ostream* out = &std::cout;
   std::ofstream file_out;
   if (argc > 1)
   {
      file_out.open(argv[1]);
      if (!file_out)
      {
         std::fprintf(stderr, "failed to open %s for writing\n", argv[1]);
         return 1;
      }
      out = &file_out;
   }

   *out << "shape,op,ns_min,ns_median,cv_pct,wire_bytes,iters,trials\n";

   run_shape(*out, "Point",       psio_bench::point());
   run_shape(*out, "NameRecord",  psio_bench::namerec());
   run_shape(*out, "FlatRecord",  psio_bench::flatrec());
   run_shape(*out, "Record",      psio_bench::record());
   run_shape(*out, "Validator",   psio_bench::validator());

   return 0;
}
