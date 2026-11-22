// bench_graphql — psio::graphql (docs/graphql.md) against the previous
// engine (psio1::gql_query) on the same data.
//
// Per case: the old engine; the new engine on C++ values and on PSSZ views
// of the same data, each with a reused plan ("plan") and compiling the
// query for every request ("text"). Reported: ns per request (minimum of
// repeated trials), heap allocations per request, response bytes.
//
// What each side is given:
//   * old: psio1::gql_query(root, text) returns a new std::string per call;
//     it interprets the text while executing and has no fragments or
//     variables (that case is "n/a").
//   * new: the response is appended to a std::string the caller reuses.
//   * A page of the old engine is a Connection of row VALUES (what its
//     consumer built from unpacked rows); the new one pages a row source
//     that yields references to values or views of encoded rows.
//
//   psio_bench_graphql [--quick]

#include "graphql_shapes.hpp"
#include "harness.hpp"

#include <psio/graphql.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <new>
#include <string>
#include <string_view>
#include <vector>

// ── Allocation counter ──────────────────────────────────────────────────
static std::atomic<std::uint64_t> g_allocations{0};
void* operator new(std::size_t size)
{
   g_allocations.fetch_add(1, std::memory_order_relaxed);
   if (void* p = std::malloc(size ? size : 1))
      return p;
   std::abort();
}
void* operator new[](std::size_t size) { return operator new(size); }
void  operator delete(void* p) noexcept { std::free(p); }
void  operator delete[](void* p) noexcept { std::free(p); }
void  operator delete(void* p, std::size_t) noexcept { std::free(p); }
void  operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace gqlbench
{
   PSIO_REFLECT(Small, id, name, a, b, c, score, active)
   PSIO_REFLECT(Row20, id, name, owner, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18,
                f19)
   PSIO_REFLECT(WideRow, id, text1, numbers, text2, strings, count)
   PSIO_REFLECT(Child, n, label)
   PSIO_REFLECT(Parent, id, name, children)

   template <typename T>
   using pv = psio::view<T, psio::pssz, psio::storage::const_borrow>;

   inline void index_key(std::size_t i, char out[8])
   {
      for (int k = 0; k < 8; ++k)
         out[7 - k] = static_cast<char>(i >> (8 * k));
   }
   inline std::size_t key_index(std::span<const char> key)
   {
      std::size_t i = 0;
      for (const char c : key)
         i = (i << 8) | static_cast<unsigned char>(c);
      return i;
   }

   // A row source over an array of rows keyed by position. `Yield` turns a
   // position into what the connection holds for a node.
   template <typename Yield>
   class ArraySource
   {
      std::size_t count_;
      Yield       yield_;
      std::size_t at_ = 0;
      char        key_[8]{};
      bool        on() const { return at_ < count_; }

     public:
      ArraySource(std::size_t count, Yield yield) : count_(count), yield_(yield) {}
      bool seek_first()
      {
         at_ = 0;
         return on();
      }
      bool seek_last()
      {
         at_ = count_ ? count_ - 1 : 0;
         return count_ != 0;
      }
      bool seek_after(std::span<const char> key)
      {
         at_ = key_index(key) + 1;
         return on();
      }
      bool seek_before(std::span<const char> key)
      {
         const std::size_t k = key_index(key);
         at_                 = k ? std::min(k, count_) - 1 : count_;
         return on();
      }
      bool next()
      {
         ++at_;
         return on();
      }
      bool prev()
      {
         at_ = at_ ? at_ - 1 : count_;
         return on();
      }
      std::span<const char> key()
      {
         index_key(at_, key_);
         return {key_, 8};
      }
      auto row() const { return yield_(at_); }
   };

   struct Query
   {
      const Data* d = nullptr;
      // PSSZ of each member of `d`.
      std::vector<char>              small_bytes, rows100_bytes, rows10k_bytes, wide_bytes, parents_bytes;
      std::vector<std::vector<char>> row_bytes;  // rows10k, one buffer per row

      const Small&                small() const { return d->small; }
      const std::vector<Row20>&   rows100() const { return d->rows100; }
      const std::vector<Row20>&   rows10k() const { return d->rows10k; }
      const std::vector<WideRow>& wide() const { return d->wide; }
      const std::vector<Parent>&  parents() const { return d->parents; }

      pv<Small>                smallView() const { return pv<Small>{std::span<const char>{small_bytes}}; }
      pv<std::vector<Row20>>   rows100View() const { return pv<std::vector<Row20>>{std::span<const char>{rows100_bytes}}; }
      pv<std::vector<Row20>>   rows10kView() const { return pv<std::vector<Row20>>{std::span<const char>{rows10k_bytes}}; }
      pv<std::vector<WideRow>> wideView() const { return pv<std::vector<WideRow>>{std::span<const char>{wide_bytes}}; }
      pv<std::vector<Parent>>  parentsView() const { return pv<std::vector<Parent>>{std::span<const char>{parents_bytes}}; }

      std::int32_t echo(std::int32_t n) const { return n; }

      auto page(std::optional<std::int32_t> first, std::optional<std::string> after) const
      {
         auto yield = [rows = &d->rows10k](std::size_t i) { return std::cref((*rows)[i]); };
         return psio::graphql::paginate(ArraySource{d->rows10k.size(), yield}, {first, {}, {}, after}, 10'000);
      }
      auto pageView(std::optional<std::int32_t> first, std::optional<std::string> after) const
      {
         auto yield = [rows = &row_bytes](std::size_t i) { return pv<Row20>{std::span<const char>{(*rows)[i]}}; };
         return psio::graphql::paginate(ArraySource{row_bytes.size(), yield}, {first, {}, {}, after}, 10'000);
      }
   };
   PSIO_REFLECT(Query,
                attr(d, psio::graphql::hidden),
                attr(small_bytes, psio::graphql::hidden),
                attr(rows100_bytes, psio::graphql::hidden),
                attr(rows10k_bytes, psio::graphql::hidden),
                attr(wide_bytes, psio::graphql::hidden),
                attr(parents_bytes, psio::graphql::hidden),
                attr(row_bytes, psio::graphql::hidden),
                method(small),
                method(rows100),
                method(rows10k),
                method(wide),
                method(parents),
                method(smallView),
                method(rows100View),
                method(rows10kView),
                method(wideView),
                method(parentsView),
                method(echo, n),
                method(page, first, after),
                method(pageView, first, after))

   using api = psio::graphql::schema<Query>;
}  // namespace gqlbench

namespace
{
   using namespace gqlbench;

   struct cell
   {
      double        ns     = -1;
      std::uint64_t allocs = 0;
      std::size_t   bytes  = 0;
   };

   bool g_quick = false;

   template <typename F>
   cell measure(F&& body)
   {
      cell              c;
      std::string       out;
      c.bytes = body(out);
      const auto t = psio_bench::ns_per_iter(
         0, [&](std::size_t) { body(out); }, g_quick ? 3 : 7, g_quick ? 10'000'000.0 : 50'000'000.0);
      c.ns = t.min_ns;
      const int           rounds = 16;
      const std::uint64_t before = g_allocations.load();
      for (int i = 0; i < rounds; ++i)
         body(out);
      c.allocs = (g_allocations.load() - before + rounds / 2) / rounds;
      return c;
   }

   void print(const char* name, const char* engine, const cell& c)
   {
      if (c.ns < 0)
         std::printf("| %-34s | %-18s | %12s | %8s | %9s |\n", name, engine, "n/a", "", "");
      else
         std::printf("| %-34s | %-18s | %12.0f | %8llu | %9zu |\n", name, engine, c.ns,
                     static_cast<unsigned long long>(c.allocs), c.bytes);
   }

   // `text`: the query on values; `%` in it is the place of "View" for the
   // view-backed field names (response keys are aliased, so both sources
   // give the same bytes).
   void run_case(const char* name, const Query& q, void* old, std::string_view text,
                 std::string_view variables = {}, bool old_supported = true)
   {
      auto expand = [&](std::string_view suffix) {
         std::string out;
         for (const char ch : text)
            if (ch == '%')
               out += suffix;
            else
               out += ch;
         return out;
      };
      const std::string     by_value = expand(""), by_view = expand("View");
      psio::graphql::limits lim;
      lim.max_complexity = 10'000'000;
      const psio::graphql::request req{.variables = variables};

      if (old_available())
      {
         cell c;
         if (old_supported)
         {
            const std::string check = old_run(old, by_value);
            if (check.find("\"errors\"") != std::string::npos)
               std::printf("old engine failed on %s: %.200s\n", name, check.c_str());
            c = measure([&](std::string& out) {
               out = old_run(old, by_value);
               return out.size();
            });
         }
         print(name, "old", c);
      }
      std::string reference;
      for (const std::string* query : {&by_value, &by_view})
      {
         const bool              view = query == &by_view;
         psio::graphql::document plan;
         std::vector<psio::graphql::error> errors;
         if (!api::compile(*query, plan, errors, lim))
         {
            std::printf("compile failed for %s: %s\n", name, errors[0].message.c_str());
            std::exit(1);
         }
         std::string check;
         if (api::execute(plan, q, check, req, lim) != 0)
         {
            std::printf("execute failed for %s: %.300s\n", name, check.c_str());
            std::exit(1);
         }
         if (!view)
            reference = check;
         else if (check != reference)
         {
            std::printf("view and value responses differ for %s\n", name);
            std::exit(1);
         }
         print(name, view ? "new view, plan" : "new value, plan", measure([&](std::string& out) {
                  out.clear();
                  api::execute(plan, q, out, req, lim);
                  return out.size();
               }));
         print(name, view ? "new view, text" : "new value, text", measure([&](std::string& out) {
                  out.clear();
                  api::run(q, *query, out, req, lim);
                  return out.size();
               }));
      }
   }
}  // namespace

int main(int argc, char** argv)
{
   g_quick = argc > 1 && std::string_view{argv[1]} == "--quick";
   const Data data = make_data();
   Query      q;
   q.d             = &data;
   q.small_bytes   = psio::encode(psio::pssz{}, data.small);
   q.rows100_bytes = psio::encode(psio::pssz{}, data.rows100);
   q.rows10k_bytes = psio::encode(psio::pssz{}, data.rows10k);
   q.wide_bytes    = psio::encode(psio::pssz{}, data.wide);
   q.parents_bytes = psio::encode(psio::pssz{}, data.parents);
   for (const Row20& r : data.rows10k)
      q.row_bytes.push_back(psio::encode(psio::pssz{}, r));
   void* old = old_root(data);

   std::printf("psio::graphql benchmark%s; old engine: %s\n\n", g_quick ? " (quick)" : "",
               old_available() ? "psio1::gql_query" : "not built");
   std::printf("| %-34s | %-18s | %12s | %8s | %9s |\n", "case", "engine", "ns/request", "allocs", "bytes out");
   std::printf("|%s|%s|%s|%s|%s|\n", std::string(36, '-').c_str(), std::string(20, '-').c_str(),
               std::string(13, '-').append(":").c_str(), std::string(9, '-').append(":").c_str(),
               std::string(10, '-').append(":").c_str());

   run_case("object, 3 of 7 fields", q, old, "{ small: small% { id name score } }");
   run_case("list 100 rows, 3 of 20 fields", q, old, "{ rows100: rows100% { id name f5 } }");
   run_case("list 10,000 rows, 3 of 20 fields", q, old, "{ rows10k: rows10k% { id name f5 } }");
   run_case("list 100 wide rows, 2 small fields", q, old, "{ wide: wide% { id count } }");
   run_case("nested lists 100 x 10", q, old, "{ parents: parents% { id children { n label } } }");
   run_case("page of 100 (connection)", q, old,
            "{ page: page%(first: 100) { edges { cursor node { id name f5 } } pageInfo { hasNextPage endCursor } } }");
   run_case("page of 10,000 (connection)", q, old,
            "{ page: page%(first: 10000) { edges { cursor node { id name f5 } } pageInfo { hasNextPage endCursor } } }");
   run_case("fragments + variables, 100 rows", q, old,
            "query Q($skip: Boolean!, $n: Int!) { rows100: rows100% { ...F } small: small% { id @skip(if: $skip) "
            "name ...G } echo(n: $n) } fragment F on Row20 { id name ... on Row20 { f5 } } "
            "fragment G on Small { score active @include(if: $skip) }",
            R"({"skip": false, "n": 7})", false);
   return 0;
}
