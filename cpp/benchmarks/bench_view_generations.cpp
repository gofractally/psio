//
// bench_view_generations.cpp — head-to-head view benchmark (stage F0 of the
// formats migration).
//
//   g1 = the first-generation library (psio1): the pointer-based
//        `view<T, Fmt>` frame and its capnp / flatbuf views.
//   g2 = this library: the span-based `view<T, Fmt, Store>` frame, in its
//        two kinds: the safe view (`F_view<T>`, which compares every offset
//        it reads) and the trusted view (`F_trusted_view<T>`, over bytes
//        that validated: "g2 trusted").
//
// Same shapes, same values, same operations; for SSZ and the canonical ABI
// the two generations read the same bytes. Cap'n Proto and FlatBuffers
// messages are WRITTEN BY THE OFFICIAL LIBRARIES and read by g1's views and
// by the official readers.
//
// Built only when PSIO1_ROOT points at a first-generation checkout (see
// CMakeLists.txt); nothing in the library depends on it.
//
// Every cell does observed work: the encoded buffer's address is passed
// through an optimisation barrier before each operation (so the read cannot
// be hoisted out of the loop) and the result is consumed through one.
//

#include <psio1/capnp_view.hpp>
#include <psio1/flatbuf.hpp>
#include <psio1/frac_ref.hpp>
#include <psio1/fracpack.hpp>
#include <psio1/pssz_view.hpp>
#include <psio1/ssz_view.hpp>
#include <psio1/to_pssz.hpp>
#include <psio1/to_ssz.hpp>
#include <psio1/wit_view.hpp>

#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/ssz.hpp>
#include <psio/ssz_view.hpp>
#include <psio/wit.hpp>
#include <psio/wit_view.hpp>
#if __has_include(<psio/capnp_view.hpp>)
#include <psio/capnp_view.hpp>
#define G2_HAVE_CAPNP_VIEW 1
#endif
#include <psio/capnp.hpp>
#include <psio/flatbuf.hpp>
#if __has_include(<psio/flatbuf_view.hpp>)
#include <psio/flatbuf_view.hpp>
#define G2_HAVE_FLATBUF_VIEW 1
#endif

#ifdef PSIO_HAVE_CAPNP
#include "bench_schemas.capnp.h"
#include <capnp/message.h>
#include <capnp/serialize.h>
#endif
#ifdef PSIO_HAVE_FLATBUF
#include "bench_schemas_generated.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// ── Shapes (identical members; one reflection per generation) ─────────────

#define VG_SHAPES                                                          \
   struct UserProfile                                                      \
   {                                                                       \
      std::uint64_t id = 0;                                                \
      std::string   name;                                                  \
      std::string   email;                                                 \
      std::uint32_t age      = 0;                                          \
      bool          verified = false;                                      \
   };                                                                      \
   struct LineItem                                                         \
   {                                                                       \
      std::string   product;                                               \
      std::uint32_t qty        = 0;                                        \
      double        unit_price = 0.0;                                      \
   };                                                                      \
   struct Order                                                            \
   {                                                                       \
      std::uint64_t         id = 0;                                        \
      UserProfile           customer;                                      \
      std::vector<LineItem> items;                                         \
      double                total = 0.0;                                   \
      std::string           note;                                          \
   };                                                                      \
   struct Tagged                                                           \
   {                                                                       \
      std::uint32_t          id = 0;                                       \
      std::set<std::int32_t> tags;                                         \
   };                                                                      \
   struct Config                                                           \
   {                                                                       \
      std::uint32_t                        version = 0;                    \
      std::map<std::string, std::uint32_t> limits;                         \
   };                                                                      \
   /* A psibase-style table row: a key, two numbers, a short string. */    \
   struct Row                                                              \
   {                                                                       \
      std::uint64_t key     = 0;                                           \
      std::uint64_t balance = 0;                                           \
      std::uint64_t updated = 0;                                           \
      std::string   memo;                                                  \
   };

namespace g1
{
   VG_SHAPES
   PSIO1_REFLECT(UserProfile, id, name, email, age, verified)
   PSIO1_REFLECT(LineItem, product, qty, unit_price)
   PSIO1_REFLECT(Order, id, customer, items, total, note)
   PSIO1_REFLECT(Tagged, id, tags)
   PSIO1_REFLECT(Config, version, limits)
   PSIO1_REFLECT(Row, key, balance, updated, memo)
}  // namespace g1

namespace g2
{
   VG_SHAPES
   PSIO_REFLECT(UserProfile, id, name, email, age, verified)
   PSIO_REFLECT(LineItem, product, qty, unit_price)
   PSIO_REFLECT(Order, id, customer, items, total, note)
   PSIO_REFLECT(Tagged, id, tags)
   PSIO_REFLECT(Config, version, limits)
   PSIO_REFLECT(Row, key, balance, updated, memo)
}  // namespace g2

static constexpr int n_items = 16;
static constexpr int n_tags  = 64;
static constexpr int n_keys  = 32;

template <typename NS_Order>
NS_Order make_order()
{
   NS_Order o;
   o.id                = 0x1122334455667788ull;
   o.customer.id       = 77;
   o.customer.name     = "Alice Example";
   o.customer.email    = "alice.example@example.org";
   o.customer.age      = 37;
   o.customer.verified = true;
   for (int i = 0; i < n_items; ++i)
   {
      o.items.emplace_back();
      o.items.back().product    = "product-" + std::to_string(i);
      o.items.back().qty        = static_cast<std::uint32_t>(i + 1);
      o.items.back().unit_price = 1.5 * i;
   }
   o.total = 1234.5;
   o.note  = "leave at the door";
   return o;
}
template <typename T>
T make_tagged()
{
   T t;
   t.id = 9;
   for (int i = 0; i < n_tags; ++i)
      t.tags.insert(i * 7 - 100);
   return t;
}
static std::string key_name(int i)
{
   char b[32];
   std::snprintf(b, sizeof b, "limit-%03d", i * 3);
   return b;
}
template <typename T>
T make_config()
{
   T c;
   c.version = 3;
   for (int i = 0; i < n_keys; ++i)
      c.limits[key_name(i)] = static_cast<std::uint32_t>(1000 + i);
   return c;
}

template <typename T>
T make_row()
{
   T r;
   r.key     = 0x0102030405060708ull;
   r.balance = 1'000'000'007ull;
   r.updated = 1'700'000'000ull;
   r.memo    = "rent, march";
   return r;
}

// ── Barriers ──────────────────────────────────────────────────────────────

template <typename T>
[[gnu::always_inline]] inline void consume(const T& v)
{
   asm volatile("" : : "r,m"(v) : "memory");
}
[[gnu::always_inline]] inline void consume(std::string_view v)
{
   const char* p = v.data();
   std::size_t n = v.size();
   asm volatile("" : : "r"(p), "r"(n) : "memory");
}
// The address of the bytes, as the optimiser cannot know it.
template <typename P>
[[gnu::always_inline]] inline P* hide(P* p)
{
   asm volatile("" : "+r"(p) : : "memory");
   return p;
}

// ── Timing ────────────────────────────────────────────────────────────────

struct stats
{
   double min = 0, median = 0, max = 0;
};

using clock_type = std::chrono::steady_clock;

static double g_trial_ns_v = 20'000'000.0;
inline double g_trial_ns() { return g_trial_ns_v; }
static int    g_trials = 11;

template <typename F>
double time_batch(std::size_t iters, F& f)
{
   auto t0 = clock_type::now();
   for (std::size_t i = 0; i < iters; ++i)
      f();
   auto t1 = clock_type::now();
   return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(iters);
}

template <typename F>
std::size_t calibrate(F& f)
{
   std::size_t iters = 1024;
   for (;;)
   {
      auto t0 = clock_type::now();
      for (std::size_t i = 0; i < iters; ++i)
         f();
      double ns = std::chrono::duration<double, std::nano>(clock_type::now() - t0).count();
      if (ns > 2'000'000.0 || iters >= (1u << 26))
      {
         double per = ns / static_cast<double>(iters);
         auto   n   = static_cast<std::size_t>(g_trial_ns() / std::max(per, 0.01));
         return std::max<std::size_t>(n, 1024);
      }
      iters *= 4;
   }
}

static stats summarise(std::vector<double>& v)
{
   std::sort(v.begin(), v.end());
   return {v.front(), v[v.size() / 2], v.back()};
}

struct row
{
   std::string group, op, impl;
   stats       s;
};
static std::vector<row> g_rows;

// Measures a set of implementations of one operation, interleaving their
// trials so that drift in machine load lands on all of them alike.
struct cell
{
   std::string                         impl;
   std::function<double(std::size_t)>  run;        // ns/op over n iterations
   std::function<std::size_t()>        calibrate;  // iterations per trial
};

static void measure(const std::string& group, const std::string& op, std::vector<cell> cells)
{
   std::vector<std::size_t>         iters;
   std::vector<std::vector<double>> samples(cells.size());
   for (auto& c : cells)
   {
      iters.push_back(c.calibrate());
      (void)c.run(iters.back() / 4 + 1);  // warm-up
   }
   for (int t = 0; t < g_trials; ++t)
      for (std::size_t i = 0; i < cells.size(); ++i)
         samples[i].push_back(cells[i].run(iters[i]));
   for (std::size_t i = 0; i < cells.size(); ++i)
   {
      row r{group, op, cells[i].impl, summarise(samples[i])};
      std::printf("%-10s %-14s %-22s %9.3f %9.3f %9.3f  %5.1f%%\n", group.c_str(), op.c_str(),
                  r.impl.c_str(), r.s.min, r.s.median, r.s.max,
                  100.0 * (r.s.max - r.s.min) / r.s.min);
      g_rows.push_back(std::move(r));
   }
   std::fflush(stdout);
}

template <typename F>
cell make_cell(std::string impl, F f)
{
   // One shared_ptr'd copy of the callable so the two closures time the
   // same object; the lambdas are instantiated per F, so the loop body is
   // inlined into time_batch.
   auto fp = std::make_shared<F>(std::move(f));
   return cell{std::move(impl), [fp](std::size_t n) { return time_batch(n, *fp); },
               [fp] { return calibrate(*fp); }};
}

// ── The operations, written once against "an order view" ──────────────────
//
// `Get` is a callable taking the laundered buffer address and returning the
// view; the operation reads through it.

template <typename Get>
auto op_one(Get get)
{
   return [get] { consume(get().id()); };
}
// customer: all five fields
template <typename GetProfile>
auto op_all(GetProfile get)
{
   return [get] {
      auto v = get();
      consume(static_cast<std::uint64_t>(v.id()));
      consume(std::string_view(v.name()));
      consume(std::string_view(v.email()));
      consume(static_cast<std::uint32_t>(v.age()));
      consume(static_cast<bool>(v.verified()));
   };
}
template <typename Get>
auto op_nested(Get get)
{
   return [get] { consume(static_cast<std::uint32_t>(get().customer().age())); };
}
template <typename Get>
auto op_nested_str(Get get)
{
   return [get] { consume(std::string_view(get().customer().name())); };
}
template <typename Get>
auto op_string(Get get)
{
   return [get] { consume(std::string_view(get().note())); };
}
// Sum of qty over the items by index.
template <typename Get>
auto op_iterate(Get get)
{
   return [get] {
      auto          items = get().items();
      std::uint64_t sum   = 0;
      const auto    n     = items.size();
      for (decltype(items.size()) i = 0; i < n; ++i)
         sum += static_cast<std::uint32_t>(items[i].qty());
      consume(sum);
   };
}
// The same by range-for.
template <typename Get>
auto op_range_for(Get get)
{
   return [get] {
      std::uint64_t sum = 0;
      for (auto it : get().items())
         sum += static_cast<std::uint32_t>(it.qty());
      consume(sum);
   };
}
// Every field of every item (string + two scalars).
template <typename Get>
auto op_iterate_all(Get get)
{
   return [get] {
      auto          items = get().items();
      std::uint64_t sum   = 0;
      double        d     = 0;
      const auto    n     = items.size();
      for (decltype(items.size()) i = 0; i < n; ++i)
      {
         auto it = items[i];
         sum += std::string_view(it.product()).size();
         sum += static_cast<std::uint32_t>(it.qty());
         d += static_cast<double>(it.unit_price());
      }
      consume(sum);
      consume(d);
   };
}
// The table-row operations: one field, two fields, all four.
template <typename Get>
auto op_row_one(Get get)
{
   return [get] { consume(static_cast<std::uint64_t>(get().balance())); };
}
template <typename Get>
auto op_row_two(Get get)
{
   return [get] {
      auto v = get();
      consume(static_cast<std::uint64_t>(v.balance()));
      consume(static_cast<std::uint64_t>(v.updated()));
   };
}
template <typename Get>
auto op_row_all(Get get)
{
   return [get] {
      auto v = get();
      consume(static_cast<std::uint64_t>(v.key()));
      consume(static_cast<std::uint64_t>(v.balance()));
      consume(static_cast<std::uint64_t>(v.updated()));
      consume(std::string_view(v.memo()));
   };
}
template <typename Get>
std::vector<std::pair<std::string, cell>> row_ops(const std::string& impl, Get get)
{
   std::vector<std::pair<std::string, cell>> r;
   r.emplace_back("row-1-field", make_cell(impl, op_row_one(get)));
   r.emplace_back("row-2-fields", make_cell(impl, op_row_two(get)));
   r.emplace_back("row-all-fields", make_cell(impl, op_row_all(get)));
   return r;
}

// What an implementation cannot do is left out of its column (and said so
// in the output) rather than approximated.
struct caps
{
   bool range_for = true;  // begin() / end() on the list view
   bool doubles   = true;  // a view of a double member
};

template <typename Get, caps C = caps{}>
std::vector<std::pair<std::string, cell>> order_ops(const std::string& impl, Get get)
{
   std::vector<std::pair<std::string, cell>> r;
   r.emplace_back("one-field", make_cell(impl, op_one(get)));
   r.emplace_back("all-fields", make_cell(impl, op_all([get] { return get().customer(); })));
   r.emplace_back("nested", make_cell(impl, op_nested(get)));
   r.emplace_back("nested-str", make_cell(impl, op_nested_str(get)));
   r.emplace_back("string", make_cell(impl, op_string(get)));
   r.emplace_back("iterate", make_cell(impl, op_iterate(get)));
   if constexpr (C.range_for)
      r.emplace_back("range-for", make_cell(impl, op_range_for(get)));
   if constexpr (C.doubles)
      r.emplace_back("iterate-all", make_cell(impl, op_iterate_all(get)));
   return r;
}

static void run_group(const std::string&                                              group,
                      std::vector<std::vector<std::pair<std::string, cell>>> impls)
{
   // Operations in the order of the implementation that has the most.
   std::size_t widest = 0;
   for (std::size_t i = 0; i < impls.size(); ++i)
      if (impls[i].size() > impls[widest].size())
         widest = i;
   if (impls.empty())
      return;
   for (auto& [op, unused] : impls[widest])
   {
      std::vector<cell> cells;
      for (auto& i : impls)
         for (auto& [name, c] : i)
            if (name == op)
               cells.push_back(c);
      measure(group, op, std::move(cells));
   }
}

// ── Official-library reads ────────────────────────────────────────────────
//
// libcapnp: the flat array is aligned words; a FlatArrayMessageReader is
// constructed per operation, as a program that receives a message does
// (constructing the reader parses the segment table). The "root held"
// variant holds one reader outside the loop and re-reads through the root
// each iteration — the lower bound for libcapnp's accessors.

#ifdef PSIO_HAVE_CAPNP
static kj::Array<capnp::word> official_capnp_order()
{
   auto                         o = make_order<g2::Order>();
   capnp::MallocMessageBuilder  mb;
   auto                         b = mb.initRoot<CpOrder>();
   b.setId(o.id);
   auto c = b.initCustomer();
   c.setId(o.customer.id);
   c.setName(o.customer.name);
   c.setEmail(o.customer.email);
   c.setAge(o.customer.age);
   c.setVerified(o.customer.verified);
   auto items = b.initItems(static_cast<unsigned>(o.items.size()));
   for (unsigned i = 0; i < o.items.size(); ++i)
   {
      items[i].setProduct(o.items[i].product);
      items[i].setQty(o.items[i].qty);
      items[i].setUnitPrice(o.items[i].unit_price);
   }
   b.setTotal(o.total);
   b.setNote(o.note);
   return capnp::messageToFlatArray(mb);
}

// Adapts libcapnp's getX() readers to the x() spelling the operations use.
struct cp_profile
{
   CpUserProfile::Reader r;
   auto                  id() const { return r.getId(); }
   std::string_view      name() const { auto t = r.getName(); return {t.begin(), t.size()}; }
   std::string_view      email() const { auto t = r.getEmail(); return {t.begin(), t.size()}; }
   auto                  age() const { return r.getAge(); }
   bool                  verified() const { return r.getVerified(); }
};
struct cp_item
{
   CpLineItem::Reader r;
   std::string_view   product() const { auto t = r.getProduct(); return {t.begin(), t.size()}; }
   auto               qty() const { return r.getQty(); }
   auto               unit_price() const { return r.getUnitPrice(); }
};
struct cp_items
{
   capnp::List<CpLineItem>::Reader r;
   unsigned                        size() const { return r.size(); }
   cp_item                         operator[](unsigned i) const { return {r[i]}; }
   struct iterator
   {
      const cp_items* s;
      unsigned        i;
      cp_item         operator*() const { return (*s)[i]; }
      iterator&       operator++() { ++i; return *this; }
      bool            operator!=(const iterator& o) const { return i != o.i; }
   };
   iterator begin() const { return {this, 0}; }
   iterator end() const { return {this, size()}; }
};
struct cp_order
{
   CpOrder::Reader  r;
   auto             id() const { return r.getId(); }
   cp_profile       customer() const { return {r.getCustomer()}; }
   cp_items         items() const { return {r.getItems()}; }
   std::string_view note() const { auto t = r.getNote(); return {t.begin(), t.size()}; }
};
#endif

#ifdef PSIO_HAVE_FLATBUF
// The official builder writing `o` (generated Create functions).
static void official_flatbuf_build(flatbuffers::FlatBufferBuilder& fbb, const g2::Order& o)
{
   // Children in field order, as the generated object API (`Pack`) does:
   // the bytes are then the ones psio::flatbuf writes.
   auto name = fbb.CreateString(o.customer.name);
   auto mail = fbb.CreateString(o.customer.email);
   auto cust = fb_bench::Createfb_UserProfile(fbb, o.customer.id, name, mail, o.customer.age,
                                              o.customer.verified);
   std::vector<flatbuffers::Offset<fb_bench::fb_LineItem>> items;
   items.reserve(o.items.size());
   for (auto& it : o.items)
   {
      auto p = fbb.CreateString(it.product);
      items.push_back(fb_bench::Createfb_LineItem(fbb, p, it.qty, it.unit_price));
   }
   auto iv   = fbb.CreateVector(items);
   auto note = fbb.CreateString(o.note);
   auto root = fb_bench::Createfb_Order(fbb, o.id, cust, iv, o.total, note);
   fbb.Finish(root);
}
static std::vector<std::uint8_t> official_flatbuf_order()
{
   auto                                                        o = make_order<g2::Order>();
   flatbuffers::FlatBufferBuilder                              fbb;
   std::vector<flatbuffers::Offset<fb_bench::fb_LineItem>>     items;
   for (auto& it : o.items)
   {
      auto p = fbb.CreateString(it.product);
      items.push_back(fb_bench::Createfb_LineItem(fbb, p, it.qty, it.unit_price));
   }
   auto iv   = fbb.CreateVector(items);
   auto name = fbb.CreateString(o.customer.name);
   auto mail = fbb.CreateString(o.customer.email);
   auto cust = fb_bench::Createfb_UserProfile(fbb, o.customer.id, name, mail, o.customer.age,
                                              o.customer.verified);
   auto note = fbb.CreateString(o.note);
   auto root = fb_bench::Createfb_Order(fbb, o.id, cust, iv, o.total, note);
   fbb.Finish(root);
   return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}
struct fbo_profile
{
   const fb_bench::fb_UserProfile* r;
   auto                            id() const { return r->id(); }
   std::string_view                name() const { return r->name()->string_view(); }
   std::string_view                email() const { return r->email()->string_view(); }
   auto                            age() const { return r->age(); }
   bool                            verified() const { return r->verified(); }
};
struct fbo_item
{
   const fb_bench::fb_LineItem* r;
   std::string_view             product() const { return r->product()->string_view(); }
   auto                         qty() const { return r->qty(); }
   auto                         unit_price() const { return r->unit_price(); }
};
struct fbo_items
{
   const flatbuffers::Vector<flatbuffers::Offset<fb_bench::fb_LineItem>>* r;
   unsigned size() const { return r->size(); }
   fbo_item operator[](unsigned i) const { return {r->Get(i)}; }
   struct iterator
   {
      const fbo_items* s;
      unsigned         i;
      fbo_item         operator*() const { return (*s)[i]; }
      iterator&        operator++() { ++i; return *this; }
      bool             operator!=(const iterator& o) const { return i != o.i; }
   };
   iterator begin() const { return {this, 0}; }
   iterator end() const { return {this, size()}; }
};
struct fbo_order
{
   const fb_bench::fb_Order* r;
   auto                      id() const { return r->id(); }
   fbo_profile               customer() const { return {r->customer()}; }
   fbo_items                 items() const { return {r->items()}; }
   std::string_view          note() const { return r->note()->string_view(); }
};
#endif

// ── main ──────────────────────────────────────────────────────────────────

static bool same(std::string_view a, std::string_view b) { return a == b; }
#define VG_CHECK(x)                                                        \
   do                                                                      \
   {                                                                       \
      if (!(x))                                                            \
      {                                                                    \
         std::fprintf(stderr, "check failed: %s (line %d)\n", #x, __LINE__); \
         std::exit(1);                                                     \
      }                                                                    \
   } while (0)

template <typename V>
void check_order(V v)
{
   VG_CHECK(static_cast<std::uint64_t>(v.id()) == 0x1122334455667788ull);
   VG_CHECK(same(std::string_view(v.customer().name()), "Alice Example"));
   VG_CHECK(same(std::string_view(v.customer().email()), "alice.example@example.org"));
   VG_CHECK(static_cast<std::uint32_t>(v.customer().age()) == 37);
   VG_CHECK(static_cast<bool>(v.customer().verified()));
   VG_CHECK(v.items().size() == static_cast<decltype(v.items().size())>(n_items));
   VG_CHECK(static_cast<std::uint32_t>(v.items()[3].qty()) == 4);
   VG_CHECK(same(std::string_view(v.items()[3].product()), "product-3"));
   VG_CHECK(same(std::string_view(v.note()), "leave at the door"));
}

int main(int argc, char** argv)
{
   for (int i = 1; i < argc; ++i)
   {
      std::string_view a = argv[i];
      if (a == "--quick")
      {
         g_trial_ns_v = 5'000'000.0;
         g_trials     = 5;
      }
      else if (a.starts_with("--trials="))
         g_trials = std::atoi(a.substr(9).data());
      else if (a.starts_with("--trial-ms="))
         g_trial_ns_v = 1e6 * std::atof(a.substr(11).data());
   }
   std::string only = argc > 1 && argv[argc - 1][0] != '-' ? argv[argc - 1] : "";
   auto        want = [&](std::string_view g) { return only.empty() || only == g; };

   std::printf("# ns/op: min, median, max over %d interleaved trials; spread = (max-min)/min\n",
               g_trials);
   std::printf("%-10s %-14s %-22s %9s %9s %9s  %6s\n", "format", "operation", "implementation",
               "min", "median", "max", "spread");

   const auto o1 = make_order<g1::Order>();
   const auto o2 = make_order<g2::Order>();

   // ── SSZ: both generations read the same bytes ──────────────────────────
   if (want("ssz"))
   {
      static const std::vector<char> b1 = psio1::convert_to_ssz(o1);
      static const auto              b2 = psio::to_ssz(o2);
      VG_CHECK(b1.size() == b2.size() && std::memcmp(b1.data(), b2.data(), b1.size()) == 0);
      VG_CHECK(psio::validate_ssz<g2::Order>(std::span<const char>(b2)).ok());
      static const std::size_t n = b2.size();
      auto get1 = [] { return psio1::ssz_view_of<g1::Order>(std::span<const char>(hide(b1.data()), n)); };
      auto get2 = [] { return psio::ssz_view<g2::Order>(std::span<const char>(hide(b2.data()), n)); };
      auto get2t = [] {
         return psio::ssz_trusted_view<g2::Order>(
            psio::trusted{std::span<const char>(hide(b2.data()), n)});
      };
      check_order(get1());
      check_order(get2());
      check_order(get2t());
      std::printf("# ssz: %zu bytes, identical in both generations\n", n);
      std::vector<std::vector<std::pair<std::string, cell>>> impls;
      impls.push_back(order_ops<decltype(get1), caps{false, false}>("g1 ssz_view", get1));
      std::printf("# ssz: g1 ssz_view has no begin()/end() and no view of a double member: "
                  "range-for and iterate-all are g2 only\n");
      impls.push_back(order_ops("g2 view<ssz>", get2));
      impls.push_back(order_ops("g2 trusted view<ssz>", get2t));
      run_group("ssz", std::move(impls));
   }

   // ── Canonical ABI: same bytes ──────────────────────────────────────────
   if (want("wit"))
   {
      static const std::vector<std::uint8_t> b1 = psio1::wit::pack(o1);
      static const auto                      b2 = psio::to_wit(o2);
      const bool identical =
         b1.size() == b2.size() && std::memcmp(b1.data(), b2.data(), b1.size()) == 0;
      static const std::size_t n2 = b2.size();
      auto get1 = [] { return psio1::wit_view<g1::Order>::from_buffer(hide(b1.data())); };
      auto get2 = [] { return psio::wit_view<g2::Order>(std::span<const char>(hide(b2.data()), n2)); };
      auto get2t = [] {
         return psio::wit_trusted_view<g2::Order>(
            psio::trusted{std::span<const char>(hide(b2.data()), n2)});
      };
      VG_CHECK(psio::validate_wit<g2::Order>(std::span<const char>(b2)).ok());
      check_order(get1());
      check_order(get2());
      check_order(get2t());
      std::printf("# wit: g1 %zu bytes, g2 %zu bytes, %s\n", b1.size(), b2.size(),
                  identical ? "identical" : "DIFFERENT");
      std::vector<std::vector<std::pair<std::string, cell>>> impls;
      impls.push_back(order_ops("g1 view<wit>", get1));
      impls.push_back(order_ops("g2 view<wit>", get2));
      impls.push_back(order_ops("g2 trusted view<wit>", get2t));
      run_group("wit", std::move(impls));
   }

   // ── PSSZ: each generation's own wire (g1's is the early form), and g1's
   //    fracpack, which was its primary format ────────────────────────────
   if (want("pssz"))
   {
      static const std::vector<char> b1 = psio1::convert_to_pssz<psio1::frac_format_pssz32>(o1);
      static const std::vector<char> bf = psio1::to_frac(o1);
      static const auto              b2 = psio::to_pssz(o2);
      static const std::size_t       n1 = b1.size(), n2 = b2.size();
      VG_CHECK(psio::validate_pssz<g2::Order>(std::span<const char>(b2)).ok());
      auto get1 = [] {
         return psio1::pssz_view_of<g1::Order, psio1::frac_format_pssz32>(
            std::span<const char>(hide(b1.data()), n1));
      };
      auto getf = [] { return psio1::view<g1::Order, psio1::frac>::from_buffer(hide(bf.data())); };
      auto get2 = [] { return psio::pssz_view<g2::Order>(std::span<const char>(hide(b2.data()), n2)); };
      auto get2t = [] {
         return psio::pssz_trusted_view<g2::Order>(
            psio::trusted{std::span<const char>(hide(b2.data()), n2)});
      };
      check_order(get1());
      check_order(getf());
      check_order(get2());
      check_order(get2t());
      std::printf("# pssz: g1 pssz32 %zu bytes, g1 fracpack %zu bytes, g2 pssz %zu bytes\n", n1,
                  bf.size(), n2);
      std::vector<std::vector<std::pair<std::string, cell>>> impls;
      impls.push_back(order_ops<decltype(get1), caps{false, true}>("g1 pssz_view(32)", get1));
      impls.push_back(order_ops("g1 view<frac>", getf));
      impls.push_back(order_ops("g2 view<pssz>", get2));
      impls.push_back(order_ops("g2 trusted view<pssz>", get2t));
      run_group("pssz", std::move(impls));
   }

   // ── A table row (psibase-style): g1 pssz32 and fracpack, g2 PSSZ safe and
   //    trusted; 1, 2 and all 4 fields ─────────────────────────────────────
   if (want("row"))
   {
      static const std::vector<char> b1 =
         psio1::convert_to_pssz<psio1::frac_format_pssz32>(make_row<g1::Row>());
      static const std::vector<char> bf = psio1::to_frac(make_row<g1::Row>());
      static const auto              b2 = psio::to_pssz(make_row<g2::Row>());
      static const std::size_t       n1 = b1.size(), n2 = b2.size();
      VG_CHECK(psio::validate_pssz<g2::Row>(std::span<const char>(b2)).ok());
      auto get1 = [] {
         return psio1::pssz_view_of<g1::Row, psio1::frac_format_pssz32>(
            std::span<const char>(hide(b1.data()), n1));
      };
      auto getf = [] { return psio1::view<g1::Row, psio1::frac>::from_buffer(hide(bf.data())); };
      auto get2 = [] { return psio::pssz_view<g2::Row>(std::span<const char>(hide(b2.data()), n2)); };
      auto get2t = [] {
         return psio::pssz_trusted_view<g2::Row>(
            psio::trusted{std::span<const char>(hide(b2.data()), n2)});
      };
      const auto check_row = [](auto v) {
         VG_CHECK(static_cast<std::uint64_t>(v.key()) == 0x0102030405060708ull);
         VG_CHECK(static_cast<std::uint64_t>(v.balance()) == 1'000'000'007ull);
         VG_CHECK(static_cast<std::uint64_t>(v.updated()) == 1'700'000'000ull);
         VG_CHECK(same(std::string_view(v.memo()), "rent, march"));
      };
      check_row(get1());
      check_row(getf());
      check_row(get2());
      check_row(get2t());
      std::printf("# row: g1 pssz32 %zu bytes, g1 fracpack %zu bytes, g2 pssz %zu bytes\n", n1,
                  bf.size(), n2);
      std::vector<std::vector<std::pair<std::string, cell>>> impls;
      impls.push_back(row_ops("g1 pssz_view(32)", get1));
      impls.push_back(row_ops("g1 view<frac>", getf));
      impls.push_back(row_ops("g2 view<pssz>", get2));
      impls.push_back(row_ops("g2 trusted view<pssz>", get2t));
      run_group("row", std::move(impls));
   }

   // ── Set / map lookup: g1 fracpack against g2 PSSZ ──────────────────────
   if (want("assoc"))
   {
      static const std::vector<char> t1 = psio1::to_frac(make_tagged<g1::Tagged>());
      static const std::vector<char> c1 = psio1::to_frac(make_config<g1::Config>());
      static const auto              t2 = psio::to_pssz(make_tagged<g2::Tagged>());
      static const auto              c2 = psio::to_pssz(make_config<g2::Config>());
      static const std::size_t       tn = t2.size(), cn = c2.size();
      VG_CHECK(psio::validate_pssz<g2::Tagged>(std::span<const char>(t2)).ok());
      VG_CHECK(psio::validate_pssz<g2::Config>(std::span<const char>(c2)).ok());
      static const std::int32_t      hit = 20 * 7 - 100, miss = 20 * 7 - 99;
      static const std::string       khit = key_name(20), kmiss = "limit-zzz";
      auto gt1 = [] { return psio1::view<g1::Tagged, psio1::frac>::from_buffer(hide(t1.data())); };
      auto gc1 = [] { return psio1::view<g1::Config, psio1::frac>::from_buffer(hide(c1.data())); };
      auto gt2 = [] { return psio::pssz_view<g2::Tagged>(std::span<const char>(hide(t2.data()), tn)); };
      auto gc2 = [] { return psio::pssz_view<g2::Config>(std::span<const char>(hide(c2.data()), cn)); };
      auto gt2t = [] {
         return psio::pssz_trusted_view<g2::Tagged>(
            psio::trusted{std::span<const char>(hide(t2.data()), tn)});
      };
      auto gc2t = [] {
         return psio::pssz_trusted_view<g2::Config>(
            psio::trusted{std::span<const char>(hide(c2.data()), cn)});
      };
      VG_CHECK(gt2t().tags().contains(hit) && !gt2t().tags().contains(miss));
      VG_CHECK(gc2t().limits().contains(khit) && !gc2t().limits().contains(kmiss));
      VG_CHECK(gc2t().limits().value_or(khit, 0u) == 1020);
      VG_CHECK(gt1().tags().contains(hit) && !gt1().tags().contains(miss));
      VG_CHECK(gt2().tags().contains(hit) && !gt2().tags().contains(miss));
      VG_CHECK(gc1().limits().contains(std::string_view(khit)) &&
               !gc1().limits().contains(std::string_view(kmiss)));
      VG_CHECK(gc2().limits().contains(khit) && !gc2().limits().contains(kmiss));
      VG_CHECK(gc1().limits().value_or(std::string_view(khit), 0u) == 1020);
      VG_CHECK(gc2().limits().value_or(khit, 0u) == 1020);

      measure("assoc", "set-hit",
              {make_cell("g1 frac set_view", [gt1] { consume(gt1().tags().contains(*hide(&hit))); }),
               make_cell("g2 pssz set view", [gt2] { consume(gt2().tags().contains(*hide(&hit))); }),
               make_cell("g2 trusted set view",
                         [gt2t] { consume(gt2t().tags().contains(*hide(&hit))); })});
      measure("assoc", "set-miss",
              {make_cell("g1 frac set_view", [gt1] { consume(gt1().tags().contains(*hide(&miss))); }),
               make_cell("g2 pssz set view", [gt2] { consume(gt2().tags().contains(*hide(&miss))); }),
               make_cell("g2 trusted set view",
                         [gt2t] { consume(gt2t().tags().contains(*hide(&miss))); })});
      measure("assoc", "map-hit",
              {make_cell("g1 frac map_view",
                         [gc1] { consume(gc1().limits().value_or(std::string_view(*hide(&khit)), 0u)); }),
               make_cell("g2 pssz map view",
                         [gc2] { consume(gc2().limits().value_or(*hide(&khit), 0u)); }),
               make_cell("g2 trusted map view",
                         [gc2t] { consume(gc2t().limits().value_or(*hide(&khit), 0u)); })});
      measure("assoc", "map-miss",
              {make_cell("g1 frac map_view",
                         [gc1] { consume(gc1().limits().contains(std::string_view(*hide(&kmiss)))); }),
               make_cell("g2 pssz map view",
                         [gc2] { consume(gc2().limits().contains(*hide(&kmiss))); }),
               make_cell("g2 trusted map view",
                         [gc2t] { consume(gc2t().limits().contains(*hide(&kmiss))); })});
   }

   // ── Cap'n Proto: bytes written by libcapnp ─────────────────────────────
#ifdef PSIO_HAVE_CAPNP
   if (want("capnp"))
   {
      static const kj::Array<capnp::word> words = official_capnp_order();
      static const auto*                  bytes = reinterpret_cast<const std::uint8_t*>(words.begin());
      static const std::size_t            nbytes = words.size() * sizeof(capnp::word);
      // g1's own encoding of the same value, to compare with libcapnp's.
      const auto own1 = psio1::capnp_pack(o1);
      std::printf("# capnp: libcapnp %zu bytes; g1 capnp_pack %zu bytes, %s\n", nbytes, own1.size(),
                  own1.size() == nbytes && std::memcmp(own1.data(), bytes, nbytes) == 0
                     ? "byte-identical"
                     : "not byte-identical");
      VG_CHECK(psio1::validate_capnp(bytes, nbytes));
      auto get1 = [] { return psio1::capnp_view<g1::Order>::from_buffer(hide(bytes)); };
      check_order(get1());

      // libcapnp, a reader per operation (segment table parsed each time).
      struct fresh
      {
         capnp::FlatArrayMessageReader reader;
         cp_order                      root;
         fresh()
            : reader(kj::ArrayPtr<const capnp::word>(
                 hide(reinterpret_cast<const capnp::word*>(bytes)), nbytes / sizeof(capnp::word))),
              root{reader.getRoot<CpOrder>()}
         {
         }
         auto id() const { return root.id(); }
         auto customer() const { return root.customer(); }
         auto items() const { return root.items(); }
         auto note() const { return root.note(); }
      };
      // libcapnp, reader held outside the loop: accessors only. The
      // traversal limit is raised so that the reader does not run out over
      // the millions of iterations.
      static capnp::ReaderOptions opts = [] {
         capnp::ReaderOptions o;
         o.traversalLimitInWords = ~0ull;
         return o;
      }();
      static capnp::FlatArrayMessageReader held(
         kj::ArrayPtr<const capnp::word>(words.begin(), words.size()), opts);
      static CpOrder::Reader held_root = held.getRoot<CpOrder>();
      auto getf = [] { return fresh{}; };
      auto geth = [] { return cp_order{*hide(&held_root)}; };
      check_order(cp_order{held_root});

      std::vector<std::vector<std::pair<std::string, cell>>> impls;
      impls.push_back(order_ops("g1 view<cp>", get1));
#ifdef G2_HAVE_CAPNP_VIEW
      static const std::size_t nb = nbytes;
      auto get2 = [] {
         return psio::capnp_view<g2::Order>(
            std::span<const char>(hide(reinterpret_cast<const char*>(bytes)), nb));
      };
      check_order(get2());
      impls.push_back(order_ops("g2 view<capnp>", get2));
#endif
      impls.push_back(order_ops("libcapnp fresh reader", getf));
      impls.push_back(order_ops("libcapnp held root", geth));
      run_group("capnp", std::move(impls));

      // Whole-message operations for reference.
      const auto own2 = psio::to_capnp(o2);
      std::printf("# capnp: g2 psio::capnp encodes %zu bytes, %s libcapnp's\n", own2.size(),
                  own2.size() == nbytes && std::memcmp(own2.data(), bytes, nbytes) == 0
                     ? "byte-identical to"
                     : "NOT byte-identical to");
      static const std::vector<char> g2bytes(own2.begin(), own2.end());
      static const std::size_t       g2n = g2bytes.size();
      measure("capnp", "decode-all",
              {make_cell("g1 capnp_unpack",
                         [] {
                            auto v = psio1::capnp_unpack<g1::Order>(hide(bytes));
                            consume(v.items.size());
                         }),
               make_cell("g2 decode (trusted)",
                         [] {
                            auto v = psio::from_capnp<g2::Order>(psio::trusted{
                               std::span<const char>(hide(g2bytes.data()), g2n)});
                            consume(v.items.size());
                         }),
               make_cell("libcapnp to struct", [] {
                  fresh     f;
                  g2::Order v;
                  v.id                = f.id();
                  auto c              = f.customer();
                  v.customer.id       = c.id();
                  v.customer.name     = std::string(c.name());
                  v.customer.email    = std::string(c.email());
                  v.customer.age      = c.age();
                  v.customer.verified = c.verified();
                  auto items          = f.items();
                  v.items.resize(items.size());
                  for (unsigned i = 0; i < items.size(); ++i)
                  {
                     auto it               = items[i];
                     v.items[i].product    = std::string(it.product());
                     v.items[i].qty        = it.qty();
                     v.items[i].unit_price = it.unit_price();
                  }
                  v.total = f.root.r.getTotal();
                  v.note  = std::string(f.note());
                  consume(v.items.size());
               })});
      measure("capnp", "validate",
              {make_cell("g1 validate_capnp", [] { consume(psio1::validate_capnp(hide(bytes), nbytes)); }),
               make_cell("g2 validate",
                         [] {
                            consume(psio::validate_capnp<g2::Order>(
                                       std::span<const char>(hide(g2bytes.data()), g2n))
                                       .ok());
                         })});
      static const auto so1 = o1;
      static const auto so2 = o2;
      measure("capnp", "encode",
              {make_cell("g1 capnp_pack",
                         [] {
                            auto b = psio1::capnp_pack(*hide(&so1));
                            consume(b.data());
                         }),
               make_cell("g2 encode",
                         [] {
                            auto b = psio::to_capnp(*hide(&so2));
                            consume(b.data());
                         }),
               make_cell("libcapnp build+flatten", [] {
                  auto a = official_capnp_order();
                  consume(a.begin());
               })});
   }
#endif

   // ── FlatBuffers: bytes written by Google's library ─────────────────────
#ifdef PSIO_HAVE_FLATBUF
   if (want("flatbuf"))
   {
      static const std::vector<std::uint8_t> bytes = official_flatbuf_order();
      static const std::size_t               nbytes = bytes.size();
      psio1::fb_builder                      bld;
      bld.pack(o1);
      std::printf("# flatbuf: flatbuffers %zu bytes; g1 fb_builder %zu bytes\n", nbytes,
                  static_cast<std::size_t>(bld.size()));
      auto get1 = [] { return psio1::fb_view<g1::Order>::from_buffer(hide(bytes.data())); };
      auto geto = [] { return fbo_order{fb_bench::Getfb_Order(hide(bytes.data()))}; };
      check_order(get1());
      check_order(geto());
      std::vector<std::vector<std::pair<std::string, cell>>> impls;
      impls.push_back(order_ops("g1 view<fb>", get1));
#ifdef G2_HAVE_FLATBUF_VIEW
      auto get2 = [] {
         return psio::flatbuf_view<g2::Order>(
            std::span<const char>(hide(reinterpret_cast<const char*>(bytes.data())), nbytes));
      };
      auto get2t = [] {
         return psio::flatbuf_trusted_view<g2::Order>(psio::trusted{
            std::span<const char>(hide(reinterpret_cast<const char*>(bytes.data())), nbytes)});
      };
      VG_CHECK(psio::validate_flatbuf<g2::Order>(
                  std::span<const char>(reinterpret_cast<const char*>(bytes.data()), nbytes))
                  .ok());
      check_order(get2());
      check_order(get2t());
      impls.push_back(order_ops("g2 view<flatbuf> (safe)", get2));
      impls.push_back(order_ops("g2 trusted view<flatbuf>", get2t));
#endif
      impls.push_back(order_ops("flatbuffers generated", geto));
      run_group("flatbuf", std::move(impls));

      static const auto so1 = o1;
      static const auto so2 = o2;
      const auto        own2 = psio::to_flatbuf(o2);
      static const std::vector<char> g2bytes(own2.begin(), own2.end());
      static const std::size_t       g2n = g2bytes.size();
      {
         flatbuffers::FlatBufferBuilder fbb;
         official_flatbuf_build(fbb, so2);
         std::printf("# flatbuf: g2 psio::flatbuf encodes %zu bytes, %s the official builder's "
                     "(children created in field order)\n",
                     g2n,
                     g2n == fbb.GetSize() &&
                           std::memcmp(g2bytes.data(), fbb.GetBufferPointer(), g2n) == 0
                        ? "identical to"
                        : "DIFFERENT from");
      }
      measure("flatbuf", "decode-all",
              {make_cell("g1 fb_unpack",
                         [] {
                            auto v = psio1::fb_unpack<g1::Order>(hide(bytes.data()));
                            consume(v.items.size());
                         }),
               make_cell("g2 decode (trusted)", [] {
                  auto v = psio::from_flatbuf<g2::Order>(
                     psio::trusted{std::span<const char>(hide(g2bytes.data()), g2n)});
                  consume(v.items.size());
               })});
      measure("flatbuf", "validate",
              {make_cell("g1 fb_verify",
                         [] { consume(psio1::fb_verify<g1::Order>(hide(bytes.data()), nbytes)); }),
               make_cell("g2 validate_flatbuf",
                         [] {
                            consume(psio::validate_flatbuf<g2::Order>(
                                       std::span<const char>(
                                          hide(reinterpret_cast<const char*>(bytes.data())), nbytes))
                                       .ok());
                         }),
               make_cell("flatbuffers Verifier", [] {
                  flatbuffers::Verifier v(hide(bytes.data()), nbytes);
                  consume(v.VerifyBuffer<fb_bench::fb_Order>(nullptr));
               })});
      measure("flatbuf", "encode",
              {make_cell("g1 fb_builder",
                         [] {
                            psio1::fb_builder b;
                            b.pack(*hide(&so1));
                            consume(b.data());
                         }),
               make_cell("g2 encode",
                         [] {
                            auto b = psio::to_flatbuf(*hide(&so2));
                            consume(b.data());
                         }),
               make_cell("g2 encode, sink reused",
                         [] {
                            static std::vector<char> sink;
                            psio::to_flatbuf(*hide(&so2), sink);
                            consume(sink.data());
                         }),
               // The official builder from the same held value: a builder
               // per message, and one builder cleared and reused (its best
               // case; the bytes stay in the builder in both).
               make_cell("flatbuffers builder",
                         [] {
                            flatbuffers::FlatBufferBuilder fbb;
                            official_flatbuf_build(fbb, *hide(&so2));
                            consume(fbb.GetBufferPointer());
                         }),
               make_cell("flatbuffers builder, reused", [] {
                  static flatbuffers::FlatBufferBuilder fbb;
                  fbb.Clear();
                  official_flatbuf_build(fbb, *hide(&so2));
                  consume(fbb.GetBufferPointer());
               })});
   }
#endif
   return 0;
}
