// psio::protobuf against the official C++ implementation, on the same
// messages: the schema psio emits for tests/protobuf_shapes.hpp, compiled by
// protoc, and values whose bytes are the same on both sides.
//
//   encode     psio::to_protobuf(v, reused vector)   | ByteSizeLong + SerializeWithCachedSizesToArray(reused buffer)
//   decode     protobuf::decode_checked(bytes, reused value) | ParseFromArray(reused message)
//   validate   psio::validate_protobuf<T>(bytes)     | (the official library has no check without a parse)
//
// ns per operation, the minimum of 11 trials of about 40 ms each.
// Run: psio_bench_protobuf   (docs/protobuf.md "Performance" has a run).

#include <psio/protobuf.hpp>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

#include "emitted.pb.h"
#include "protobuf_shapes.hpp"

namespace
{
   template <typename T>
   inline void keep(const T& value)
   {
      asm volatile("" : : "r,m"(value) : "memory");
   }

   template <typename F>
   double ns_per_op(F&& op)
   {
      using clock = std::chrono::steady_clock;
      std::size_t n = 1;
      for (;;)
      {
         const auto begin = clock::now();
         for (std::size_t i = 0; i < n; ++i)
            op();
         const double s = std::chrono::duration<double>(clock::now() - begin).count();
         if (s > 0.02)
            break;
         n *= 2;
      }
      double best = 1e300;
      for (int trial = 0; trial < 11; ++trial)
      {
         const auto begin = clock::now();
         for (std::size_t i = 0; i < n; ++i)
            op();
         const double s = std::chrono::duration<double>(clock::now() - begin).count();
         best           = std::min(best, s * 1e9 / static_cast<double>(n));
      }
      return best;
   }

   template <typename T, typename M>
   void row(const char* name, const T& value)
   {
      const std::vector<char> bytes = psio::to_protobuf(value);
      M                       message;
      if (!message.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())))
         std::abort();
      std::string official(bytes.size(), '\0');
      // (the same bytes, or the comparison is of two different things)
      {
         std::string check;
         google::protobuf::io::StringOutputStream s(&check);
         google::protobuf::io::CodedOutputStream  c(&s);
         c.SetSerializationDeterministic(true);
         if (!message.SerializeToCodedStream(&c))
            std::abort();
         c.Trim();
         if (check != std::string(bytes.begin(), bytes.end()))
            std::abort();
      }

      std::vector<char> sink;
      sink.reserve(bytes.size());
      const double encode_psio = ns_per_op([&] {
         sink.clear();
         psio::to_protobuf(value, sink);
         keep(sink.data());
      });
      const double encode_official = ns_per_op([&] {
         const std::size_t size = message.ByteSizeLong();
         const std::uint8_t* end =
            message.SerializeWithCachedSizesToArray(reinterpret_cast<std::uint8_t*>(official.data()));
         keep(end);
         keep(size);
         keep(official.data());
      });

      T            out{};
      const double decode_psio = ns_per_op([&] {
         if (!psio::protobuf::decode_checked(std::span<const char>{bytes}, out).ok())
            std::abort();
         keep(out);
      });
      const double decode_fresh = ns_per_op([&] {
         T fresh{};
         if (!psio::protobuf::decode_checked(std::span<const char>{bytes}, fresh).ok())
            std::abort();
         keep(fresh);
      });
      M            parsed;
      const double decode_official = ns_per_op([&] {
         if (!parsed.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())))
            std::abort();
         keep(parsed);
      });
      const double validate_psio = ns_per_op([&] {
         const bool ok = psio::validate_protobuf<T>(std::span<const char>{bytes}).ok();
         keep(ok);
      });

      std::printf("| %-9s | %6zu | %8.1f | %8.1f | %5.2f | %8.1f | %8.1f | %8.1f | %5.2f | %8.1f |\n",
                  name, bytes.size(), encode_psio, encode_official, encode_official / encode_psio,
                  decode_psio, decode_fresh, decode_official, decode_official / decode_psio,
                  validate_psio);
   }

   PbTree tree(unsigned depth, unsigned fanout, unsigned& id)
   {
      PbTree t;
      t.label = "node-" + std::to_string(id++);
      if (id % 3 == 0)
         t.weight = static_cast<std::int32_t>(id * 7);
      if (depth)
         for (unsigned i = 0; i < fanout; ++i)
            t.children.push_back(tree(depth - 1, fanout, id));
      return t;
   }
}  // namespace

int main()
{
   PbPoint point{1234, -5};

   PbScalars scalars;
   scalars.b = true, scalars.i8 = -8, scalars.i16 = 1600, scalars.i32 = 320000, scalars.i64 = -64000000000;
   scalars.u8 = 200, scalars.u16 = 60000, scalars.u32 = 4000000000u, scalars.u64 = 18000000000000000000ull;
   scalars.f = 1.5f, scalars.d = -2.25, scalars.c = 'A', scalars.color = PbColor::blue;
   scalars.level = PbLevel::mid, scalars.plain = PbPlain::big;

   PbText text;
   text.s = std::string(200, 'x') + " — ünïcödé";
   text.bytes.assign(1024, 0x5a);
   text.chars.assign(64, 'c');
   text.quad = {1, 2, 3, 4};

   PbRepeated repeated;
   for (int i = 0; i < 256; ++i)
   {
      repeated.ints.push_back(i * 37 - 4000);
      repeated.doubles.push_back(i * 0.25);
      repeated.u64s.push_back(static_cast<std::uint64_t>(i) << (i % 60));
   }
   for (int i = 0; i < 32; ++i)
      repeated.strings.push_back("string-" + std::to_string(i * 1001));
   for (int i = 0; i < 64; ++i)
      repeated.points.push_back({i, -i * 1000});

   PbMaps maps;
   for (int i = 0; i < 32; ++i)
   {
      maps.counts["key-" + std::to_string(i)] = i * 100;
      maps.points[static_cast<std::uint64_t>(i) * 1000003] = {i, i};
      maps.names[i - 16]                                   = "name-" + std::to_string(i);
   }

   unsigned id = 0;
   PbTree   t  = tree(4, 4, id);  // 341 nodes

   PbOrder order;
   order.id      = 987654321;
   order.origin  = point;
   order.scalars = scalars;
   order.text.s  = "an order";
   for (int i = 0; i < 8; ++i)
   {
      PbChoice c;
      c.before = i;
      if (i % 2)
         c.pick = std::string{"choice"};
      else
         c.pick = PbPoint{i, i};
      c.always = 0.5 * i;
      c.after  = "after";
      order.choices.push_back(c);
   }
   order.maps.counts = {{"a", 1}, {"b", 2}, {"c", 3}};
   unsigned id2      = 0;
   order.tree        = tree(2, 3, id2);
   order.lists["l"].ints = {1, 2, 3, 4, 5, 6, 7, 8};

   std::printf("ns per operation (minimum of 11 trials); ×: official / psio, above 1 psio is faster\n\n");
   std::printf("| shape     |  bytes | enc psio | enc offi |   ×   | dec psio | dec fresh| dec offi |   ×   | validate |\n");
   std::printf("|-----------|-------:|---------:|---------:|------:|---------:|---------:|---------:|------:|---------:|\n");
   row<PbPoint, pbe::PbPoint>("Point", point);
   row<PbScalars, pbe::PbScalars>("Scalars", scalars);
   row<PbText, pbe::PbText>("Text", text);
   row<PbRepeated, pbe::PbRepeated>("Repeated", repeated);
   row<PbMaps, pbe::PbMaps>("Maps", maps);
   row<PbTree, pbe::PbTree>("Tree", t);
   row<PbOrder, pbe::PbOrder>("Order", order);
   return 0;
}
