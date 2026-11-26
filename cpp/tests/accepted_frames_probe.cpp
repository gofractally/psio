// Views and the trusted decode over frames the VALIDATOR ACCEPTS
// (gofractally/psio#15).
//
// What a node relies on: bytes off the network are validated once
// (`validate_pssz`, the validating `from_pssz`) and then walked through
// trusted views and trusted decodes with no further check. So on EVERY frame
// the validator accepts — not only the canonical bytes of a value, also
// whatever else it lets through: a newer writer's unknown trailing fields,
// pruned trailing optionals, a frame whose bytes happen to be another value
// — a view of either kind and the trusted decode must read inside the bytes
// and must not fail.
//
//   accepted_frames_probe [values] [random-rounds-per-value]
//
// Frames: random values of each shape; from each, the frame with trailing
// bytes and cut short, every byte set to every value (for the first few
// values), and random edits (a byte, a small step, a plausible offset, two
// offsets moved together, bytes removed, bytes inserted, the tail dropped).
// Each frame the validator accepts is copied into a heap block of exactly
// its size and walked through both kinds of view and the trusted decode.
// Counted per shape: frames tried, frames accepted, and accepted frames that
// are not the canonical bytes of their value.
//
// It uses only what the library had before the views were changed, so the
// same file builds against an older tree to ask the question of that code:
//
//   clang++ -std=c++23 -O1 -g -fsanitize=address,undefined
//           -fno-sanitize-recover=all -DPSIO_EXCEPTIONS_ENABLED=1
//           -I <tree>/cpp/include -I <tree>/cpp/external/xxhash
//           accepted_frames_probe.cpp
//
// Under the sanitizers a report prints the shape and the bytes of the frame.
#include <psio/nested.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/ssz.hpp>
#include <psio/ssz_view.hpp>
#include <psio/wit.hpp>
#include <psio/wit_view.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#if defined(__SANITIZE_ADDRESS__)
#define PSIO_PROBE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PSIO_PROBE_ASAN 1
#endif
#endif
#ifdef PSIO_PROBE_ASAN
extern "C" void __asan_set_death_callback(void (*)(void));
extern "C" const char* __asan_default_options() { return "detect_leaks=0:max_allocation_size_mb=512"; }
#endif

struct Item { std::string product; std::uint32_t qty = 0; friend bool operator==(const Item&, const Item&) = default; };
PSIO_REFLECT(Item, product, qty)
struct Point { std::uint32_t x = 0; std::uint16_t y = 0; bool on = false; friend bool operator==(const Point&, const Point&) = default; };
PSIO_REFLECT(Point, x, y, on)
struct Row
{
   std::uint64_t id = 0; std::uint32_t n = 0; bool flag = false; std::string a, b, c; Point at; Item first;
   std::vector<Item> items; std::vector<std::uint32_t> numbers; std::vector<std::string> names; std::vector<Point> points;
   friend bool operator==(const Row&, const Row&) = default;
};
PSIO_REFLECT(Row, id, n, flag, a, b, c, at, first, items, numbers, names, points)
struct Rich
{
   std::uint32_t id = 0; std::optional<std::string> note; std::optional<std::uint32_t> count; std::optional<Item> item;
   std::variant<std::uint32_t, std::string, Point> either; std::array<std::uint16_t, 3> triple{};
   std::vector<std::vector<std::uint32_t>> grid; std::vector<std::optional<std::string>> maybe; Row row;
   std::tuple<std::uint32_t, std::string, Point> tup; std::map<std::string, std::uint32_t> limits; std::set<std::int32_t> tags;
   std::array<std::string, 2> two; std::optional<std::vector<std::uint8_t>> maybeList; std::optional<std::string> tail; std::optional<std::uint32_t> tail2;
   friend bool operator==(const Rich&, const Rich&) = default;
};
PSIO_REFLECT(Rich, id, note, count, item, either, triple, grid, maybe, row, tup, limits, tags, two, maybeList, tail, tail2)
// Rich as a NEWER writer wrote it (two more trailing optionals) and as an
// OLDER one did (the last two optionals missing): extension and pruning.
struct RichNewer
{
   std::uint32_t id = 0; std::optional<std::string> note; std::optional<std::uint32_t> count; std::optional<Item> item;
   std::variant<std::uint32_t, std::string, Point> either; std::array<std::uint16_t, 3> triple{};
   std::vector<std::vector<std::uint32_t>> grid; std::vector<std::optional<std::string>> maybe; Row row;
   std::tuple<std::uint32_t, std::string, Point> tup; std::map<std::string, std::uint32_t> limits; std::set<std::int32_t> tags;
   std::array<std::string, 2> two; std::optional<std::vector<std::uint8_t>> maybeList; std::optional<std::string> tail; std::optional<std::uint32_t> tail2;
   std::optional<std::string> extra1; std::optional<Item> extra2;
};
PSIO_REFLECT(RichNewer, id, note, count, item, either, triple, grid, maybe, row, tup, limits, tags, two, maybeList, tail, tail2, extra1, extra2)
struct Narrow
{
   std::uint32_t id = 0; std::string name; std::vector<std::string> tags; std::vector<Item> items;
   friend bool operator==(const Narrow&, const Narrow&) = default;
   PSIO_REFLECT(Narrow, id, attr(name, max<40>), attr(tags, max<8> | element(max<20>)), attr(items, max<300>), definitionWillNotChange())
};
// What a node reads off the network: an outer message with packed blocks.
struct Header { std::uint32_t num = 0; std::string producer; std::vector<std::uint64_t> xs; friend bool operator==(const Header&, const Header&) = default; };
PSIO_REFLECT(Header, num, producer, xs)
struct Block { Header header; std::vector<std::string> trx; std::optional<std::string> sig; friend bool operator==(const Block&, const Block&) = default; };
PSIO_REFLECT(Block, header, trx, sig)
struct Msg
{
   std::uint32_t kind = 0; psio::shared_pssz<Block> block; std::optional<psio::shared_pssz<Block>> extra;
   std::vector<psio::shared_pssz<Block>> more; psio::pssz_buffer<Header> head; psio::nested<Block> nest;
};
PSIO_REFLECT(Msg, kind, block, extra, more, head, nest)

template <typename X> inline constexpr bool is_optional_v = false;
template <typename X> inline constexpr bool is_optional_v<std::optional<X>> = true;
template <typename X> inline constexpr bool is_variant_v = false;
template <typename... Xs> inline constexpr bool is_variant_v<std::variant<Xs...>> = true;
template <typename X> inline constexpr bool is_list_v = false;
template <typename X> inline constexpr bool is_list_v<std::vector<X>> = true;
template <typename X, std::size_t N> inline constexpr bool is_list_v<std::array<X, N>> = true;
template <typename X> inline constexpr bool is_tuple_v = false;
template <typename... Xs> inline constexpr bool is_tuple_v<std::tuple<Xs...>> = true;
template <typename X> inline constexpr bool is_map_v = false;
template <typename K, typename V> inline constexpr bool is_map_v<std::map<K, V>> = true;
template <typename X> inline constexpr bool is_set_v = false;
template <typename K> inline constexpr bool is_set_v<std::set<K>> = true;
template <typename X> inline constexpr bool is_packed_v = false;
template <typename X> inline constexpr bool is_packed_v<psio::nested<X>> = true;
template <typename X> inline constexpr bool is_packed_v<psio::shared_pssz<X>> = true;
template <typename X> inline constexpr bool is_packed_v<psio::pssz_buffer<X>> = true;
template <typename X> struct packed_of;
template <typename X> struct packed_of<psio::nested<X>> { using type = X; };
template <typename X> struct packed_of<psio::shared_pssz<X>> { using type = X; };
template <typename X> struct packed_of<psio::pssz_buffer<X>> { using type = X; };

struct walker
{
   const char* begin = nullptr; const char* end = nullptr; std::uint64_t hash = 1469598103934665603ull;
   bool inside = true; bool bounded = true;
   void mix(std::uint64_t v) { hash = (hash ^ v) * 1099511628211ull; }
   void bytes(std::span<const char> s)
   {
      if (s.empty() || s.size() == 1) return;
      if (s.data() < begin || s.data() + s.size() > end) { inside = false; return; }
      for (const char c : s) mix(static_cast<unsigned char>(c));
   }
   template <typename F, typename V> void value(const V& v)
   {
      if constexpr (std::is_arithmetic_v<F> || std::is_enum_v<F>) mix(static_cast<std::uint64_t>(static_cast<F>(v)));
      else if constexpr (is_tuple_v<F>)
         [&]<std::size_t... Is>(std::index_sequence<Is...>) { (value<std::tuple_element_t<Is, F>>(v.template get<Is>()), ...); }(std::make_index_sequence<std::tuple_size_v<F>>{});
      else if constexpr (is_map_v<F> || is_set_v<F>)
      {
         const std::size_t n = v.size(); mix(n);
         if (n > static_cast<std::size_t>(end - begin) + 16) { bounded = false; return; }
         for (std::size_t i = 0; i < n; ++i)
         {
            if constexpr (is_map_v<F>) { value<typename F::key_type>(v[i].template get<0>()); value<typename F::mapped_type>(v[i].template get<1>()); }
            else value<typename F::key_type>(v[i]);
         }
         mix(v.contains(typename F::key_type{}));
      }
      else if constexpr (is_packed_v<F>) { bytes(v._psio_data()); value<typename packed_of<F>::type>(v.get()); }
      else if constexpr (std::is_same_v<F, std::string>) { mix(v.size()); bytes(v._psio_data()); }
      else if constexpr (is_optional_v<F>) { const bool has = v.has_value(); mix(has); if (has) value<typename F::value_type>(*v); }
      else if constexpr (is_variant_v<F>) { mix(v.index()); v.visit([&](const auto& alt) { value<typename std::remove_cvref_t<decltype(alt)>::element_type>(alt); }); }
      else if constexpr (is_list_v<F>)
      {
         using E = typename F::value_type; const std::size_t n = v.size(); mix(n);
         if (n > static_cast<std::size_t>(end - begin) + 16) { bounded = false; return; }
         for (std::size_t i = 0; i < n; ++i) value<E>(v[i]);
      }
      else
      {
         bytes(v._psio_data());
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (value<std::remove_cvref_t<typename psio::reflect<F>::template member_type<Is>>>(v.template get<Is>()), ...);
         }(std::make_index_sequence<psio::reflect<F>::member_count>{});
      }
   }
};

const char*           g_type = "";
std::span<const char> g_frame;
void                  on_death()
{
   std::fprintf(stderr, "\n*** SANITIZER REPORT on a validator-accepted frame of %s (%zu bytes):\n", g_type, g_frame.size());
   for (const char c : g_frame) std::fprintf(stderr, "%02x", static_cast<unsigned char>(c));
   std::fprintf(stderr, "\n");
}
[[noreturn]] void finding(const char* what)
{
   std::fprintf(stderr, "\n*** FINDING: %s, on a validator-accepted frame of %s (%zu bytes):\n", what, g_type, g_frame.size());
   for (const char c : g_frame) std::fprintf(stderr, "%02x", static_cast<unsigned char>(c));
   std::fprintf(stderr, "\n");
   std::exit(3);
}

struct counts { std::size_t tried = 0, accepted = 0, odd = 0; };

template <typename T, typename Fmt>
void probe(const char* name, Fmt fmt, std::span<const char> bytes, counts& c, bool reencode)
{
   ++c.tried;
   std::unique_ptr<char[]> p(new char[bytes.size() ? bytes.size() : 1]);
   if (!bytes.empty()) std::memcpy(p.get(), bytes.data(), bytes.size());
   const std::span<const char> b{p.get(), bytes.size()};
   if (!psio::validate<T>(fmt, b).ok()) return;
   ++c.accepted;
   g_type = name; g_frame = b;
   std::uint64_t hashes[2] = {0, 0};
   for (int kind = 0; kind < 2; ++kind)
   {
      walker w; w.begin = b.data(); w.end = b.data() + b.size();
      try
      {
         if (kind == 0) w.template value<T>(psio::view<T, Fmt, psio::storage::const_borrow>{b});
         else           w.template value<T>(psio::view<T, Fmt, psio::storage::trusted_borrow>{psio::trusted{b}});
      }
      catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); finding(kind ? "the trusted view FAILED" : "the safe view FAILED"); }
      if (!w.inside) finding(kind ? "the trusted view handed out a span outside the bytes" : "the safe view handed out a span outside the bytes");
      if (!w.bounded) finding("a list announced more elements than the bytes hold");
      hashes[kind] = w.hash;
   }
   if (hashes[0] != hashes[1]) finding("the safe and the trusted view read different things");
   try
   {
      const T value = psio::decode<T>(fmt, b);
      if (reencode)
      {
         const auto again = psio::encode(fmt, value);
         if (again.size() != b.size() || std::memcmp(again.data(), b.data(), b.size()) != 0) ++c.odd;
      }
   }
   catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); finding("the trusted decode FAILED"); }
}

std::mt19937_64 rng{20261126};
std::size_t     below(std::size_t n) { return static_cast<std::size_t>(rng() % n); }
std::string     text(std::size_t max) { std::string s; for (std::size_t n = below(max + 1); n > 0; --n) s += static_cast<char>('a' + below(26)); return s; }
Item            item() { return {text(6), static_cast<std::uint32_t>(rng())}; }
Point           point() { return {static_cast<std::uint32_t>(rng()), static_cast<std::uint16_t>(rng()), below(2) == 1}; }
template <typename F> auto list(std::size_t max, F&& make) { std::vector<decltype(make())> v; for (std::size_t n = below(max + 1); n > 0; --n) v.push_back(make()); return v; }
Row row()
{
   return {rng(), static_cast<std::uint32_t>(rng()), below(2) == 1, text(8), text(3), text(12), point(), item(), list(3, item),
           list(4, [] { return static_cast<std::uint32_t>(rng()); }), list(3, [] { return text(5); }), list(2, point)};
}
template <typename R> R rich_as()
{
   R r; r.id = static_cast<std::uint32_t>(rng());
   if (below(2)) r.note = text(5);
   if (below(2)) r.count = static_cast<std::uint32_t>(rng());
   if (below(2)) r.item = item();
   switch (below(3)) { case 0: r.either = static_cast<std::uint32_t>(rng()); break; case 1: r.either = text(4); break; default: r.either = point(); }
   r.triple = {1, 2, static_cast<std::uint16_t>(rng())};
   r.grid = list(3, [] { return list(3, [] { return static_cast<std::uint32_t>(rng()); }); });
   r.maybe = list(3, [] { return below(2) ? std::optional<std::string>{text(3)} : std::nullopt; });
   r.row = row(); r.tup = {static_cast<std::uint32_t>(rng()), text(4), point()};
   for (std::size_t n = below(3); n > 0; --n) r.limits[text(4)] = static_cast<std::uint32_t>(rng());
   for (std::size_t n = below(4); n > 0; --n) r.tags.insert(static_cast<std::int32_t>(rng()));
   r.two = {text(3), text(3)};
   if (below(2)) r.maybeList = list(3, [] { return static_cast<std::uint8_t>(rng()); });
   if (below(2)) r.tail = text(4);
   if (below(2)) r.tail2 = static_cast<std::uint32_t>(rng());
   return r;
}
Narrow narrow() { return {static_cast<std::uint32_t>(rng()), text(10), list(3, [] { return text(6); }), list(3, item)}; }
Header header() { return {static_cast<std::uint32_t>(rng()), text(6), list(3, [] { return std::uint64_t{rng()}; })}; }
Block  block() { return {header(), list(3, [] { return text(5); }), below(2) ? std::optional<std::string>{text(4)} : std::nullopt}; }
Msg    msg()
{
   Msg m; m.kind = static_cast<std::uint32_t>(rng()); m.block = psio::shared_pssz<Block>{block()};
   if (below(2)) m.extra = psio::shared_pssz<Block>{block()};
   for (std::size_t n = below(3); n > 0; --n) m.more.push_back(psio::shared_pssz<Block>{block()});
   m.head = psio::pssz_buffer<Header>{header()}; m.nest = psio::nested<Block>{block()};
   return m;
}

// Every frame derived from one valid one.
template <typename T, typename Fmt, typename Bytes>
void derive(const char* name, Fmt fmt, const Bytes& good, counts& c, bool exhaustive, std::size_t random_rounds)
{
   const std::vector<char> g(good.begin(), good.end());
   probe<T>(name, fmt, g, c, true);
   // Trailing bytes, and the frame cut.
   for (std::size_t extra = 1; extra <= 4; ++extra) { auto more = g; more.insert(more.end(), extra, char(extra == 3 ? 0xff : 0)); probe<T>(name, fmt, more, c, true); }
   for (std::size_t n = 0; n < g.size(); n += 1 + g.size() / 64) probe<T>(name, fmt, std::span<const char>{g.data(), n}, c, true);
   if (exhaustive)
      for (std::size_t i = 0; i < g.size(); ++i)
         for (int v = 0; v < 256; ++v)
            if (static_cast<unsigned char>(g[i]) != v) { auto bad = g; bad[i] = static_cast<char>(v); probe<T>(name, fmt, bad, c, true); }
   for (std::size_t round = 0; round < random_rounds && !g.empty(); ++round)
   {
      auto bad = g;
      for (std::size_t edits = 1 + below(3); edits > 0 && !bad.empty(); --edits)
         switch (below(7))
         {
            case 0: bad[below(bad.size())] = static_cast<char>(rng()); break;
            case 1: { const std::size_t i = below(bad.size()); bad[i] = static_cast<char>(bad[i] + static_cast<int>(below(9)) - 4); break; }
            case 2:
            {  // a plausible offset
               const std::size_t w = std::size_t{1} << below(3); const std::uint32_t v = static_cast<std::uint32_t>(below(bad.size() + 2)); const std::size_t i = below(bad.size());
               for (std::size_t k = 0; k < w && i + k < bad.size(); ++k) bad[i + k] = static_cast<char>(v >> (8 * k));
               break;
            }
            case 3:
            {  // two adjacent offsets moved together
               const std::size_t w = std::size_t{1} << below(3); const std::size_t i = below(bad.size()); const int d = static_cast<int>(below(9)) - 4;
               if (i + 2 * w <= bad.size()) { bad[i] = static_cast<char>(bad[i] + d); bad[i + w] = static_cast<char>(bad[i + w] + d); }
               break;
            }
            case 4: { const std::size_t i = below(bad.size()); bad.erase(bad.begin() + static_cast<std::ptrdiff_t>(i), bad.begin() + static_cast<std::ptrdiff_t>(std::min(bad.size(), i + 1 + below(4)))); break; }
            case 5: { const std::size_t i = below(bad.size() + 1); bad.insert(bad.begin() + static_cast<std::ptrdiff_t>(i), 1 + below(4), static_cast<char>(below(2) ? 0 : rng())); break; }
            default: bad.resize(below(bad.size() + 1)); break;
         }
      probe<T>(name, fmt, bad, c, true);
   }
}

void report(const char* name, const counts& c)
{
   std::printf("%-28s tried %9zu  accepted %9zu  (of those, not the canonical bytes of their value: %zu)\n", name, c.tried, c.accepted, c.odd);
   std::fflush(stdout);
}

int main(int argc, char** argv)
{
#ifdef PSIO_PROBE_ASAN
   __asan_set_death_callback(on_death);
#else
   (void)on_death;
#endif
   const std::size_t values = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 40;
   const std::size_t rounds = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2000;
   {
      counts c;
      for (std::size_t i = 0; i < values; ++i) derive<Row>("pssz Row", psio::pssz{}, psio::to_pssz(row()), c, i < 6, rounds);
      report("pssz Row", c);
   }
   {
      counts c;
      for (std::size_t i = 0; i < values; ++i) derive<Rich>("pssz Rich", psio::pssz{}, psio::to_pssz(rich_as<Rich>()), c, i < 4, rounds);
      report("pssz Rich", c);
   }
   {
      // A newer writer's frame read as Rich: unknown trailing fields.
      counts c;
      for (std::size_t i = 0; i < values; ++i)
      {
         auto r = rich_as<RichNewer>();
         if (below(4)) r.extra1 = text(5);
         if (below(4)) r.extra2 = item();
         derive<Rich>("pssz Rich (newer writer)", psio::pssz{}, psio::to_pssz(r), c, i < 4, rounds);
      }
      report("pssz Rich (newer writer)", c);
   }
   {
      counts c;
      for (std::size_t i = 0; i < values; ++i) derive<Narrow>("pssz Narrow", psio::pssz{}, psio::to_pssz(narrow()), c, i < 8, rounds);
      report("pssz Narrow (1/2-byte offsets)", c);
   }
   {
      counts c;
      for (std::size_t i = 0; i < values; ++i) derive<Row>("pssz_<2> Row", psio::pssz_<2>{}, psio::encode(psio::pssz_<2>{}, row()), c, i < 6, rounds);
      report("pssz_<2> Row", c);
   }
   {
      counts c;
      for (std::size_t i = 0; i < values; ++i) derive<Msg>("pssz Msg", psio::pssz{}, psio::to_pssz(msg()), c, i < 4, rounds);
      report("pssz Msg (packed blocks)", c);
   }
   {
      counts c;
      for (std::size_t i = 0; i < values; ++i) derive<Row>("ssz Row", psio::ssz{}, psio::to_ssz(row()), c, i < 6, rounds);
      report("ssz Row", c);
   }
   {
      counts c;
      for (std::size_t i = 0; i < values; ++i) derive<Row>("wit Row", psio::wit{}, psio::to_wit(row()), c, i < 6, rounds);
      report("wit Row", c);
   }
   std::printf("no sanitizer report, no failure, no span outside the bytes\n");
   return 0;
}
