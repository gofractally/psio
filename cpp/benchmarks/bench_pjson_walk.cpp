// bench_pjson_walk.cpp — walk a representative pjson document through
// every available access path. Compares throughput of:
//
//   1. simdjson DOM walk (parse JSON → DOM, sum integer fields)
//   2. simdjson on-demand walk (parse JSON → on-demand, sum)
//   3. psio::pjson_value tree walk (decode pjson bytes → tree → sum)
//   4. psio::pjson_view dynamic walk (find by name on raw bytes)
//   5. psio::typed_pjson_view<T> non-canonical (find by name)
//   6. psio::typed_pjson_view<T> canonical (memcmp validate, then
//      indexed offset-table reads — the spec's "lock-step fast path")
//   7. psio::typed_pjson_view<T> ::to_struct (full materialize)
//
// All paths sum the same scalar field (`age`) over a representative
// document with several integer fields, a string, a boolean, a double,
// and a small nested array, so the cost difference between paths is
// dominated by the lookup mechanism, not the leaf decode.
//
// Run: psio_bench_pjson_walk
//      Optional --doc-count N (default: 200,000 trials)

#include <psio/pjson.hpp>
#include <psio/pjson_view.hpp>
#include <psio/pjson_typed.hpp>
#include <psio/reflect.hpp>

#if defined(PSIO_HAVE_SIMDJSON) && PSIO_HAVE_SIMDJSON
#include <psio/pjson_json.hpp>
#include <psio/pjson_json_typed.hpp>
#include <psio/view_to_json.hpp>
#include <psio/pjson_to_json.hpp>
#include <simdjson.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace bench {

   struct User
   {
      std::string  name;
      std::int64_t age;
      bool         active;
      double       score;
      std::int64_t id;
      std::string  email;
   };
   PSIO_REFLECT(User, name, age, active, score, id, email)

   // Single representative JSON doc the benchmark hammers. The
   // pretty-printed form is what most APIs return; the compact form is
   // what a size-conscious client would send. We print both so the
   // pjson size comparison is honest.
   inline constexpr std::string_view kJsonPretty = R"({
      "name": "alice",
      "age": 30,
      "active": true,
      "score": 3.14,
      "id": 1234567890,
      "email": "alice@example.com"
   })";
   inline constexpr std::string_view kJsonCompact =
       R"({"name":"alice","age":30,"active":true,"score":3.14,"id":1234567890,"email":"alice@example.com"})";

   // ── A larger representative document (~1 KB) ──────────────────────────
   // GitHub-API-style user response. Mix of strings (some long),
   // ints (small + big), bools, doubles, a nested object, and a
   // small array of nested objects. Compact (no whitespace) to match
   // typical wire-form JSON.
   inline constexpr std::string_view kJsonLarge =
       R"({"id":1234567890,"login":"alice","name":"Alice Q. Smith","email":"alice@example.com","bio":"Software engineer passionate about distributed systems and high-performance computing.","company":"Acme Corp","location":"San Francisco, CA","blog":"https://example.com/alice","twitter_username":"alice_codes","public_repos":142,"public_gists":17,"followers":4321,"following":234,"created_at":"2014-04-12T18:34:56Z","updated_at":"2026-04-26T09:12:33Z","site_admin":false,"hireable":true,"avatar_url":"https://avatars.example.com/u/1234567890?v=4","html_url":"https://example.com/alice","plan":{"name":"pro","space":976562499,"private_repos":9999,"collaborators":42,"discount":0.15},"addresses":[{"city":"San Francisco","zip":"94105","primary":true},{"city":"New York","zip":"10001","primary":false},{"city":"Austin","zip":"78701","primary":false}],"languages":["c++","rust","python","go"],"score":98.7})";

   // The bench hammers the compact form (what real-world transit looks
   // like).
   inline constexpr std::string_view kJson = kJsonCompact;

   inline User make_user()
   {
      return User{"alice", 30, true, 3.14, 1234567890, "alice@example.com"};
   }

   template <typename Fn>
   double run_loop(int iters, Fn&& fn)
   {
      auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; i < iters; ++i)
         fn();
      auto t1 = std::chrono::steady_clock::now();
      return std::chrono::duration<double>(t1 - t0).count();
   }

   void report(const char* label, int iters, double secs,
               std::size_t doc_bytes, std::int64_t sink)
   {
      double ns_per   = (secs / iters) * 1e9;
      double mb_per_s = (double(iters) * doc_bytes) / secs / (1024 * 1024);
      std::printf("%-44s  %8.0f ns/iter  %7.1f MB/s  sink=%lld\n",
                  label, ns_per, mb_per_s,
                  static_cast<long long>(sink));
   }

}  // namespace bench

int main(int argc, char** argv)
{
   using namespace bench;
   int iters = 200000;
   if (argc > 1)
      iters = std::atoi(argv[1]);

   // Pre-encode the same doc as pjson bytes so binary-side benches
   // don't pay the JSON parse cost.
   std::vector<std::uint8_t> bytes;
   {
      User u = make_user();
      bytes  = psio::from_struct(u);
   }
   auto raw_view = psio::pjson_view{bytes.data(), bytes.size()};
   auto typed = psio::typed_pjson_view<User>::from_pjson(raw_view);
   if (!typed.is_canonical())
      std::fprintf(stderr,
                   "warning: from_struct produced a non-canonical doc!\n");

   std::printf("doc bytes: pjson=%zu  json_compact=%zu  json_pretty=%zu  iters=%d\n\n",
               bytes.size(), kJsonCompact.size(), kJsonPretty.size(),
               iters);

#if defined(PSIO_HAVE_SIMDJSON) && PSIO_HAVE_SIMDJSON
   // 1. simdjson DOM
   {
      simdjson::dom::parser parser;
      std::int64_t          sink = 0;
      double secs = run_loop(iters, [&] {
         auto         doc = parser.parse(kJson.data(), kJson.size());
         std::int64_t age = std::int64_t(doc["age"]);
         sink ^= age;
      });
      report("simdjson DOM", iters, secs, kJson.size(), sink);
   }

   // 2. simdjson on-demand
   {
      simdjson::ondemand::parser parser;
      std::int64_t               sink = 0;
      simdjson::padded_string padded(kJson.data(), kJson.size());
      double secs = run_loop(iters, [&] {
         auto         doc = parser.iterate(padded);
         std::int64_t age = std::int64_t(doc["age"]);
         sink ^= age;
      });
      report("simdjson on-demand", iters, secs, kJson.size(), sink);
   }

   // 3. JSON → pjson bytes via simdjson on-demand (no DOM tape).
   {
      std::int64_t sink = 0;
      double       secs = run_loop(iters, [&] {
         auto b = psio::pjson_json::from_json(kJson);
         sink ^= static_cast<std::int64_t>(b.size());
      });
      report("json_to_pjson (encode)", iters, secs, kJson.size(),
             sink);
   }
   // 3b. JSON → pjson bytes with reused parser (no per-call malloc).
   {
      simdjson::ondemand::parser parser;
      std::int64_t               sink = 0;
      double                     secs = run_loop(iters, [&] {
         auto b = psio::pjson_json::from_json(parser, kJson);
         sink ^= static_cast<std::int64_t>(b.size());
      });
      report("json_to_pjson (reused parser)", iters, secs,
             kJson.size(), sink);
   }
   // 3c. Direct JSON → T returning form (fresh T per call).
   {
      simdjson::ondemand::parser parser;
      std::int64_t               sink = 0;
      double                     secs = run_loop(iters, [&] {
         User u = psio::json_to<User>(parser, kJson);
         sink ^= u.age;
      });
      report("json_to<T> returning", iters, secs, kJson.size(), sink);
   }
   // 3d. Direct JSON → T in-place (caller-owned T, reuses string
   //     buffers across calls — the hot-loop pattern).
   {
      simdjson::ondemand::parser parser;
      User                       u;
      std::int64_t               sink = 0;
      double                     secs = run_loop(iters, [&] {
         psio::json_to<User>(parser, kJson, u);
         sink ^= u.age;
      });
      report("json_to<T> in-place", iters, secs, kJson.size(), sink);
   }
#endif

   // 4. pjson_value tree walk (current "AnyType variant tree" cost)
   {
      std::int64_t sink = 0;
      double       secs = run_loop(iters, [&] {
         auto v   = psio::pjson::decode({bytes.data(), bytes.size()});
         auto& o  = v.as<psio::pjson_object>();
         for (const auto& [k, val] : o)
            if (k == "age")
            {
               sink ^= val.as<std::int64_t>();
               break;
            }
      });
      report("pjson_value decode + linear find", iters, secs,
             bytes.size(), sink);
   }

   // 5. pjson_view dynamic find (hash prefilter + verify)
   {
      std::int64_t sink = 0;
      double       secs = run_loop(iters, [&] {
         auto v = psio::pjson_view{bytes.data(), bytes.size()};
         auto age = v["age"].as_int64();
         sink ^= age;
      });
      report("pjson_view find (hash prefilter)", iters, secs,
             bytes.size(), sink);
   }

   // 6. typed_pjson_view<T> NON-canonical: forced via find()
   {
      std::int64_t sink = 0;
      double       secs = run_loop(iters, [&] {
         auto v   = psio::pjson_view{bytes.data(), bytes.size()};
         // skip the canonical fast-fail: just use find by name
         auto age = v.find("age").value().as_int64();
         sink ^= age;
      });
      report("pjson_view find by name (no canonical opt)",
             iters, secs, bytes.size(), sink);
   }

   // 7. typed_pjson_view<T> canonical fast path
   {
      std::int64_t sink = 0;
      double       secs = run_loop(iters, [&] {
         auto t = psio::typed_pjson_view<User>::from_pjson(psio::pjson_view{bytes.data(), bytes.size()});
         auto age = t.get<1>();  // age is field index 1
         sink ^= age;
      });
      report("typed_view<T> canonical (memcmp + index)",
             iters, secs, bytes.size(), sink);
   }

   // 8. typed_pjson_view<T>::to_struct — full materialize
   {
      std::int64_t sink = 0;
      double       secs = run_loop(iters, [&] {
         auto t = psio::typed_pjson_view<User>::from_pjson(psio::pjson_view{bytes.data(), bytes.size()});
         User u = t.to_struct();
         sink ^= u.age;
      });
      report("typed_view<T>::to_struct (full T)", iters, secs,
             bytes.size(), sink);
   }

   // 9. from_struct(T) — encode T → pjson bytes (returning form)
   {
      User           u    = make_user();
      std::int64_t   sink = 0;
      double         secs = run_loop(iters, [&] {
         auto out = psio::from_struct(u);
         sink ^= static_cast<std::int64_t>(out.size());
      });
      report("from_struct(T) returning", iters, secs, bytes.size(),
             sink);
   }
   // 10. to_pjson(T, out&) — encode T → pjson bytes in-place (reuses
   //     vector capacity across calls — hot-loop pattern).
   {
      User                      u    = make_user();
      std::vector<std::uint8_t> out;
      std::int64_t              sink = 0;
      double                    secs = run_loop(iters, [&] {
         psio::to_pjson(u, out);
         sink ^= static_cast<std::int64_t>(out.size());
      });
      report("to_pjson(T, out&) in-place", iters, secs, bytes.size(),
             sink);
   }
#if defined(PSIO_HAVE_SIMDJSON) && PSIO_HAVE_SIMDJSON
   // ────────────────────────────────────────────────────────────────────
   // Large-doc bench (~1 KB representative API payload).
   // ────────────────────────────────────────────────────────────────────
   {
      std::printf("\n── large doc: %zu bytes JSON, schemaless paths ──\n",
                  kJsonLarge.size());
      // Pre-encode pjson once for the read-side benches.
      std::vector<std::uint8_t> large_bytes;
      {
         simdjson::ondemand::parser parser;
         large_bytes = psio::pjson_json::from_json(parser, kJsonLarge);
      }
      std::printf("pjson size: %zu bytes  (%.1f%% of JSON)\n\n",
                  large_bytes.size(),
                  100.0 * large_bytes.size() / kJsonLarge.size());

      // simdjson DOM parse only.
      {
         simdjson::dom::parser parser;
         std::int64_t          sink = 0;
         double                secs = run_loop(iters, [&] {
            auto doc = parser.parse(kJsonLarge.data(),
                                    kJsonLarge.size());
            sink ^= std::int64_t(doc["id"]);
         });
         report("simdjson DOM (parse + 1 access)", iters, secs,
                kJsonLarge.size(), sink);
      }
      // simdjson on-demand parse + 1 access.
      {
         simdjson::ondemand::parser parser;
         simdjson::padded_string    padded(kJsonLarge);
         std::int64_t               sink = 0;
         double                     secs = run_loop(iters, [&] {
            auto doc = parser.iterate(padded);
            sink ^= std::int64_t(doc["id"]);
         });
         report("simdjson on-demand (parse + 1 access)",
                iters, secs, kJsonLarge.size(), sink);
      }
      // JSON → pjson, fresh parser per call.
      {
         std::int64_t sink = 0;
         double       secs = run_loop(iters, [&] {
            auto b = psio::pjson_json::from_json(kJsonLarge);
            sink ^= static_cast<std::int64_t>(b.size());
         });
         report("json_to_pjson (fresh parser)", iters, secs,
                kJsonLarge.size(), sink);
      }
      // JSON → pjson, reused parser.
      {
         simdjson::ondemand::parser parser;
         std::int64_t               sink = 0;
         double                     secs = run_loop(iters, [&] {
            auto b = psio::pjson_json::from_json(parser, kJsonLarge);
            sink ^= static_cast<std::int64_t>(b.size());
         });
         report("json_to_pjson (reused parser)", iters, secs,
                kJsonLarge.size(), sink);
      }
      // pjson_view: dynamic field access by name.
      {
         std::int64_t sink = 0;
         double       secs = run_loop(iters, [&] {
            psio::pjson_view v{large_bytes.data(), large_bytes.size()};
            sink ^= v["public_repos"].as_int64();
            sink ^= static_cast<std::int64_t>(
                v["plan"]["space"].as_int64());
         });
         report("pjson_view (2 nested accesses)", iters, secs,
                large_bytes.size(), sink);
      }
      // pjson_value tree decode.
      {
         std::int64_t sink = 0;
         double       secs = run_loop(iters, [&] {
            auto v = psio::pjson::decode(
                {large_bytes.data(), large_bytes.size()});
            sink ^= static_cast<std::int64_t>(
                v.as<psio::pjson_object>().size());
         });
         report("pjson_value decode (tree)", iters, secs,
                large_bytes.size(), sink);
      }
      // view_to_json: pjson → JSON text.
      {
         std::int64_t sink = 0;
         double       secs = run_loop(iters, [&] {
            psio::pjson_view v{large_bytes.data(), large_bytes.size()};
            std::string      s = psio::view_to_json(v);
            sink ^= static_cast<std::int64_t>(s.size());
         });
         report("view_to_json (pjson_view → JSON)", iters, secs,
                large_bytes.size(), sink);
      }
   }

   // ────────────────────────────────────────────────────────────────────
   // 100 KB bench: array of ~110 user records (~893 B each).
   // Representative of API list-responses, database row exports,
   // bulk-ingest payloads.
   // ────────────────────────────────────────────────────────────────────
   {
      // Construct a 100 KB JSON array at runtime by repeating the
      // user-record template. Inline string literal would be limited
      // by compiler constants; runtime concat works regardless.
      std::string bigjson;
      bigjson.reserve(110 * 920);
      bigjson.push_back('[');
      const std::size_t target = 100 * 1024;  // 100 KB
      bool              first  = true;
      while (bigjson.size() < target)
      {
         if (!first) bigjson.push_back(',');
         first = false;
         bigjson.append(kJsonLarge);
      }
      bigjson.push_back(']');

      std::printf("\n── 100 KB doc: %zu bytes JSON, schemaless paths ──\n",
                  bigjson.size());
      std::vector<std::uint8_t> big_bytes;
      {
         simdjson::ondemand::parser parser;
         big_bytes = psio::pjson_json::from_json(parser, bigjson);
      }
      std::printf("pjson size: %zu bytes  (%.1f%% of JSON)\n\n",
                  big_bytes.size(),
                  100.0 * big_bytes.size() / bigjson.size());

      // Use fewer iters because each iteration handles 100 KB.
      int big_iters = std::max(1, iters / 200);

      // simdjson DOM (parse only, no extraction).
      {
         simdjson::dom::parser parser;
         std::int64_t          sink = 0;
         double                secs = run_loop(big_iters, [&] {
            auto doc = parser.parse(bigjson.data(), bigjson.size());
            sink ^= static_cast<std::int64_t>(doc.is_array() ? 1 : 0);
         });
         report("simdjson DOM (parse only)", big_iters, secs,
                bigjson.size(), sink);
      }
      // simdjson on-demand parse + small access pattern.
      {
         simdjson::ondemand::parser parser;
         simdjson::padded_string    padded(bigjson);
         std::int64_t               sink = 0;
         double                     secs = run_loop(big_iters, [&] {
            auto doc  = parser.iterate(padded);
            auto arr  = doc.get_array();
            std::size_t n = 0;
            for (auto el : arr)
            {
               (void)el;
               ++n;
            }
            sink ^= static_cast<std::int64_t>(n);
         });
         report("simdjson on-demand (iterate all)", big_iters,
                secs, bigjson.size(), sink);
      }
      // JSON → pjson, reused parser.
      {
         simdjson::ondemand::parser parser;
         std::int64_t               sink = 0;
         double                     secs = run_loop(big_iters, [&] {
            auto b = psio::pjson_json::from_json(parser, bigjson);
            sink ^= static_cast<std::int64_t>(b.size());
         });
         report("json_to_pjson (reused parser)", big_iters, secs,
                bigjson.size(), sink);
      }
      // pjson_view: full walk via for_each_element. Read each child's
      // tag byte so the compiler can't elide the per-element work.
      {
         std::int64_t sink = 0;
         double       secs = run_loop(big_iters, [&] {
            psio::pjson_view v{big_bytes.data(), big_bytes.size()};
            std::int64_t     accum = 0;
            v.for_each_element([&](psio::pjson_view child) {
               accum += static_cast<std::int64_t>(*child.data());
            });
            sink ^= accum;
            asm volatile("" ::"r"(sink) : "memory");
         });
         report("pjson_view for_each_element (read tag)", big_iters, secs,
                big_bytes.size(), sink);
      }
      // pjson_view: full walk + read one field from each user.
      {
         std::int64_t sink = 0;
         double       secs = run_loop(big_iters, [&] {
            psio::pjson_view v{big_bytes.data(), big_bytes.size()};
            std::int64_t     accum = 0;
            v.for_each_element([&](psio::pjson_view child) {
               accum += child["public_repos"].as_int64();
            });
            sink ^= accum;
            asm volatile("" ::"r"(sink) : "memory");
         });
         report("pjson_view iterate + 1 field per user",
                big_iters, secs, big_bytes.size(), sink);
      }
      // pjson_view: random access — pick element 50, get its name length.
      {
         std::int64_t sink = 0;
         double       secs = run_loop(big_iters, [&] {
            psio::pjson_view v{big_bytes.data(), big_bytes.size()};
            sink ^= static_cast<std::int64_t>(
                v[50]["name"].as_string().size());
            asm volatile("" ::"r"(sink) : "memory");
         });
         report("pjson_view v[50][\"name\"] random",
                big_iters, secs, big_bytes.size(), sink);
      }
      // view_to_json: pjson → JSON text.
      {
         std::int64_t sink = 0;
         double       secs = run_loop(big_iters, [&] {
            psio::pjson_view v{big_bytes.data(), big_bytes.size()};
            std::string      s = psio::view_to_json(v);
            sink ^= static_cast<std::int64_t>(s.size());
         });
         report("view_to_json (pjson → JSON)", big_iters, secs,
                big_bytes.size(), sink);
      }

      // ── head-to-head: dynamic random access ──────────────────────────
      // Both backends are pre-built ONCE; inside the timed loop we do
      // 10 random (element, field) accesses. This is the cache /
      // database read pattern: parse once, query many times.
      std::printf(
          "\n── dynamic random access (10 accesses per iter, both "
          "backends pre-built) ──\n");
      simdjson::dom::parser dom_parser;
      simdjson::dom::element dom_root;
      {
         auto err =
             dom_parser.parse(bigjson.data(), bigjson.size()).get(dom_root);
         if (err != simdjson::SUCCESS)
            throw std::runtime_error("dom parse for bench");
      }
      psio::pjson_view pv{big_bytes.data(), big_bytes.size()};

      // 10 (element_index, field_name) pairs spread through the doc.
      // Picked to exercise the full range of array indices and a
      // variety of object field-name lookups.
      auto run_dom = [&](int it) {
         std::int64_t sink = 0;
         return std::pair{run_loop(it, [&] {
            sink ^= std::int64_t(dom_root.at(5)["id"]);
            sink ^= std::int64_t(dom_root.at(15)["public_repos"]);
            sink ^= std::int64_t(dom_root.at(25)["followers"]);
            sink ^= std::int64_t(dom_root.at(35)["following"]);
            sink ^= std::int64_t(dom_root.at(45)["public_gists"]);
            sink ^= std::int64_t(dom_root.at(55)["plan"]["space"]);
            sink ^= std::int64_t(dom_root.at(65)["id"]);
            sink ^= std::int64_t(dom_root.at(75)["public_repos"]);
            sink ^= std::int64_t(dom_root.at(85)["followers"]);
            sink ^= std::int64_t(dom_root.at(95)["plan"]["collaborators"]);
            asm volatile("" ::"r"(sink) : "memory");
         }), sink};
      };
      auto run_pjson = [&](int it) {
         std::int64_t sink = 0;
         return std::pair{run_loop(it, [&] {
            sink ^= pv[5]["id"].as_int64();
            sink ^= pv[15]["public_repos"].as_int64();
            sink ^= pv[25]["followers"].as_int64();
            sink ^= pv[35]["following"].as_int64();
            sink ^= pv[45]["public_gists"].as_int64();
            sink ^= pv[55]["plan"]["space"].as_int64();
            sink ^= pv[65]["id"].as_int64();
            sink ^= pv[75]["public_repos"].as_int64();
            sink ^= pv[85]["followers"].as_int64();
            sink ^= pv[95]["plan"]["collaborators"].as_int64();
            asm volatile("" ::"r"(sink) : "memory");
         }), sink};
      };

      // Use a higher iteration count since each access is cheap.
      int access_iters = std::max(1, iters * 5);
      {
         auto [secs, sink] = run_dom(access_iters);
         double per_access = secs / access_iters / 10 * 1e9;
         std::printf("%-44s  %8.1f ns/iter  %7.1f ns/access  sink=%lld\n",
                     "simdjson DOM 10 random accesses",
                     secs / access_iters * 1e9, per_access,
                     static_cast<long long>(sink));
      }
      {
         auto [secs, sink] = run_pjson(access_iters);
         double per_access = secs / access_iters / 10 * 1e9;
         std::printf("%-44s  %8.1f ns/iter  %7.1f ns/access  sink=%lld\n",
                     "pjson_view 10 random accesses",
                     secs / access_iters * 1e9, per_access,
                     static_cast<long long>(sink));
      }
   }

   std::printf("\n── small doc (User struct), comparison paths ──\n");
   // 11. struct_to_json(T) — T → JSON text via reflection.
   {
      User         u    = make_user();
      std::int64_t sink = 0;
      double       secs = run_loop(iters, [&] {
         std::string s = psio::struct_to_json(u);
         sink ^= static_cast<std::int64_t>(s.size());
      });
      report("struct_to_json(T)", iters, secs, kJson.size(), sink);
   }
   // 12. pjson_view → JSON via the generic view-to-json walker.
   {
      std::int64_t sink = 0;
      double       secs = run_loop(iters, [&] {
         psio::pjson_view view{bytes.data(), bytes.size()};
         std::string s = psio::view_to_json(view);
         sink ^= static_cast<std::int64_t>(s.size());
      });
      report("view_to_json [pjson → JSON]", iters, secs, bytes.size(),
             sink);
   }
   // 13. T → pjson → JSON (compose, fresh buffer each call).
   {
      User                      u    = make_user();
      std::vector<std::uint8_t> buf;
      std::int64_t              sink = 0;
      double                    secs = run_loop(iters, [&] {
         psio::to_pjson(u, buf);
         psio::pjson_view view{buf.data(), buf.size()};
         std::string s = psio::view_to_json(view);
         sink ^= static_cast<std::int64_t>(s.size());
      });
      report("T → pjson → JSON (compose)", iters, secs,
             kJson.size(), sink);
   }
   // 14. pjson → JSON via direct walker (skips view abstraction).
   {
      std::int64_t sink = 0;
      double       secs = run_loop(iters, [&] {
         std::string s = psio::pjson_to_json(
             {bytes.data(), bytes.size()});
         sink ^= static_cast<std::int64_t>(s.size());
      });
      report("pjson_to_json [direct, no view]", iters, secs,
             bytes.size(), sink);
   }
   // 15. pjson → JSON direct, in-place output buffer.
   {
      std::int64_t sink = 0;
      std::string  out_buf;
      double       secs = run_loop(iters, [&] {
         psio::pjson_to_json({bytes.data(), bytes.size()}, out_buf);
         sink ^= static_cast<std::int64_t>(out_buf.size());
      });
      report("pjson_to_json [direct, in-place buf]", iters, secs,
             bytes.size(), sink);
   }
#endif

   return 0;
}
