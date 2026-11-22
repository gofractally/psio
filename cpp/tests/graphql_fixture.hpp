#pragma once
// A schema that exercises every mapping of psio::graphql, shared by
// graphql_tests.cpp and graphql_noexceptions.cpp. No test framework here.

#include <psio/graphql.hpp>
#include <psio/key.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace gqlt
{
   using seconds_point = std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds>;
   template <typename T>
   using pssz_view = psio::view<T, psio::pssz, psio::storage::const_borrow>;

   enum class Status : std::uint8_t
   {
      active,
      frozen,
      closed,
   };
   PSIO_REFLECT_ENUM(Status, active, frozen, closed)

   // A strong typedef with a text form: a custom scalar.
   struct AccountName
   {
      std::uint64_t value = 0;
      friend bool   operator==(const AccountName&, const AccountName&) = default;
   };
   PSIO_REFLECT(AccountName, value)
   struct AccountNameText
   {
      static std::size_t packsize(const AccountName& a) { return 5 + std::to_string(a.value).size(); }
      static void encode(const AccountName& a, std::string& s) { s += "acct-" + std::to_string(a.value); }
      static bool parse(std::span<const char> in, std::uint64_t& out) noexcept
      {
         const std::string_view text{in.data(), in.size()};
         if (!text.starts_with("acct-") || text.size() == 5 || text.size() > 24)
            return false;
         out = 0;
         for (const char c : text.substr(5))
         {
            if (c < '0' || c > '9')
               return false;
            out = out * 10 + static_cast<std::uint64_t>(c - '0');
         }
         return true;
      }
      static AccountName decode(std::span<const char> in)
      {
         AccountName a;
         (void)parse(in, a.value);
         return a;
      }
      static psio::codec_status validate(std::span<const char> in) noexcept
      {
         std::uint64_t v = 0;
         return parse(in, v) ? psio::codec_ok() : psio::codec_fail("AccountName: expected acct-<n>", 0, "AccountName");
      }
      static psio::codec_status validate_strict(std::span<const char> in) noexcept { return validate(in); }
   };
}  // namespace gqlt
PSIO_ADAPTER(gqlt::AccountName, psio::text_category, gqlt::AccountNameText)
namespace gqlt
{

   struct Tag
   {
      std::string   label;
      std::uint32_t weight = 0;
   };
   PSIO_REFLECT(Tag, label, weight)

   struct Extra
   {
      std::string              text;
      std::vector<std::int32_t> codes;
   };
   PSIO_REFLECT(Extra, text, codes)

   // The row of before `rev` was appended.
   struct ItemV1
   {
      std::uint64_t                       id = 0;
      std::string                         name;
      AccountName                         owner;
      Status                              status = Status::active;
      std::int32_t                        count  = 0;
      double                              price  = 0;
      bool                                flag   = false;
      std::vector<std::uint8_t>           blob;
      seconds_point                       created{};
      std::chrono::milliseconds           ttl{};
      std::optional<std::string>          note;
      std::vector<Tag>                    tags;
      std::vector<std::string>            labels;
      std::tuple<std::int32_t, std::int32_t> range{};
      std::tuple<std::uint32_t, std::string> pair{};
      std::variant<Tag, Extra>            detail;
      std::int64_t                        big   = 0;
      std::uint16_t                       small = 0;
      std::optional<Tag>                  best;
   };
   PSIO_REFLECT(ItemV1, id, name, owner, status, count, price, flag, blob, created, ttl, note, tags, labels,
                range, pair, detail, big, small, best)

   struct Item
   {
      std::uint64_t                       id = 0;
      std::string                         name;
      AccountName                         owner;
      Status                              status = Status::active;
      std::int32_t                        count  = 0;
      double                              price  = 0;
      bool                                flag   = false;
      std::vector<std::uint8_t>           blob;
      seconds_point                       created{};
      std::chrono::milliseconds           ttl{};
      std::optional<std::string>          note;
      std::vector<Tag>                    tags;
      std::vector<std::string>            labels;
      std::tuple<std::int32_t, std::int32_t> range{};
      std::tuple<std::uint32_t, std::string> pair{};
      std::variant<Tag, Extra>            detail;
      std::int64_t                        big   = 0;
      std::uint16_t                       small = 0;
      std::optional<Tag>                  best;
      std::optional<std::uint32_t>        rev;

      // A computed field: needs the object (decoded when the source is a view).
      std::string title() const { return name + "#" + std::to_string(id); }
      std::int32_t scaled(std::int32_t by) const { return count * by; }
   };
   PSIO_REFLECT(Item, id, name, owner, status, count, price, flag, blob, created, ttl, note, tags, labels, range,
                pair, detail, big, small, best, rev, method(title), method(scaled, by))

   // Shapes the item does not have: wide integers, fixed bytes, nesting.
   struct Wide
   {
      psio::uint128                          u128 = 0;
      psio::int128                           i128 = 0;
      psio::uint256                          u256{};
      std::array<std::uint8_t, 4>            fixed{};
      std::optional<std::uint64_t>           maybe_big;
      std::vector<std::optional<std::string>> holes;
      std::vector<std::vector<Tag>>          nested;
      std::array<Tag, 2>                     two;
      std::optional<std::vector<std::int32_t>> maybe_list;
      std::optional<Tag>                     no_tag;
   };
   PSIO_REFLECT(Wide, u128, i128, u256, fixed, maybe_big, holes, nested, two, maybe_list, no_tag)

   struct Range
   {
      std::int32_t lo = 0;
      std::int32_t hi = 0;
   };
   PSIO_REFLECT(Range, lo, hi)

   struct Filter
   {
      std::optional<Status>      status;
      std::optional<std::int32_t> minCount;
      std::vector<std::string>   names;
      std::optional<Range>       range;
   };
   PSIO_REFLECT(Filter, status, minCount, names, range)

   // Strings a std::string can hold but JSON cannot.
   struct Nested
   {
      std::string  bad;
      std::int32_t ok = 1;
   };
   PSIO_REFLECT(Nested, bad, ok)

   struct Session
   {
      std::string user;
      int         calls = 0;
   };
   struct Other
   {
   };

   struct Viewer
   {
      std::string  name;
      std::int32_t calls = 0;
   };
   PSIO_REFLECT(Viewer, name, calls)

   // Annotations: a renamed type and member, IDs, a hidden member, docs, a
   // deprecated member, a cost.
   struct Annotated
   {
      std::uint64_t id = 7;
      std::string   key = "k";
      std::string   secret = "s";
      std::int32_t  renamed_member = 3;
      std::int32_t  old            = 1;
      std::int32_t  heavy() const { return 9; }
   };
   PSIO_REFLECT(Annotated, id, key, secret, renamed_member, old, method(heavy))
   PSIO_TYPE_ATTRS(Annotated, psio::graphql::name_as{"Node"} | psio::graphql::doc{"An annotated node."})
   PSIO_FIELD_ATTRS(Annotated, id, psio::graphql::id)
   PSIO_FIELD_ATTRS(Annotated, key, psio::graphql::id | psio::graphql::doc{"The key."})
   PSIO_FIELD_ATTRS(Annotated, secret, psio::graphql::hidden)
   PSIO_FIELD_ATTRS(Annotated, renamed_member, psio::graphql::name_as{"renamed"})
   PSIO_FIELD_ATTRS(Annotated, old, psio::graphql::deprecated{"use renamed"})
   PSIO_FIELD_ATTRS(Annotated, heavy, psio::graphql::cost{500})

   // An ordered in-memory "index": key bytes (psio::key) -> encoded row.
   struct MemoryIndex
   {
      std::map<std::string, std::vector<char>, std::less<>> rows;
   };
   // Row source over the rows whose key starts with `prefix` (all of them
   // when it is empty). Yields views of the encoded rows.
   class IndexSource
   {
      using map_t = std::map<std::string, std::vector<char>, std::less<>>;
      const map_t*            rows_;
      std::string             lower_;
      std::optional<std::string> upper_;  // exclusive
      map_t::const_iterator   at_;
      std::size_t*            reads_;

      map_t::const_iterator begin() const { return rows_->lower_bound(lower_); }
      map_t::const_iterator end() const { return upper_ ? rows_->lower_bound(*upper_) : rows_->end(); }
      // Counts a row read; `at_` is inside [begin, end].
      bool settle()
      {
         const bool on = at_ != end();
         if (on && reads_)
            ++*reads_;
         return on;
      }
      map_t::const_iterator clamp(map_t::const_iterator it) const
      {
         if (it == rows_->end() || (upper_ && !(it->first < *upper_)))
            return end();
         return it->first < lower_ ? begin() : it;
      }

     public:
      explicit IndexSource(const MemoryIndex& index, std::span<const char> prefix = {}, std::size_t* reads = nullptr)
          : rows_(&index.rows), lower_(prefix.data(), prefix.size()), at_(index.rows.end()), reads_(reads)
      {
         if (!prefix.empty())
            if (auto upper = psio::key_prefix_upper_bound(prefix))
               upper_ = std::string(upper->data(), upper->size());
      }
      bool seek_first()
      {
         at_ = begin();
         return settle();
      }
      bool seek_last()
      {
         at_ = end();
         return prev();
      }
      bool seek_after(std::span<const char> key)
      {
         at_ = clamp(rows_->upper_bound(std::string_view{key.data(), key.size()}));
         return settle();
      }
      bool seek_before(std::span<const char> key)
      {
         at_ = clamp(rows_->lower_bound(std::string_view{key.data(), key.size()}));
         return prev();
      }
      bool next()
      {
         ++at_;
         return settle();
      }
      bool prev()
      {
         if (at_ == begin())
         {
            at_ = end();
            return false;
         }
         --at_;
         return settle();
      }
      std::span<const char> key() const { return {at_->first.data(), at_->first.size()}; }
      pssz_view<Item>       row() const { return pssz_view<Item>{std::span<const char>{at_->second}}; }
   };

   // Row source over values in memory, keyed by position (big-endian).
   class VectorSource
   {
      const std::vector<Item>* rows_;
      std::size_t              at_ = 0;
      char                     key_[4]{};
      bool                     on() const { return at_ < rows_->size(); }
      static std::size_t       index(std::span<const char> key)
      {
         std::size_t i = 0;
         for (const char c : key)
            i = (i << 8) | static_cast<unsigned char>(c);
         return i;
      }

     public:
      explicit VectorSource(const std::vector<Item>& rows) : rows_(&rows) {}
      bool seek_first()
      {
         at_ = 0;
         return on();
      }
      bool seek_last()
      {
         at_ = rows_->empty() ? 0 : rows_->size() - 1;
         return on();
      }
      bool seek_after(std::span<const char> key)
      {
         at_ = index(key) + 1;
         return on();
      }
      bool seek_before(std::span<const char> key)
      {
         const std::size_t k = std::min(index(key), rows_->size());
         at_                 = k ? k - 1 : rows_->size();
         return on();
      }
      bool next()
      {
         ++at_;
         return on();
      }
      bool prev()
      {
         at_ = at_ ? at_ - 1 : rows_->size();
         return on();
      }
      std::span<const char> key()
      {
         for (int k = 0; k < 4; ++k)
            key_[3 - k] = static_cast<char>(at_ >> (8 * k));
         return {key_, 4};
      }
      std::reference_wrapper<const Item> row() const { return std::cref((*rows_)[at_]); }
   };

   // A lazy range: nothing is stored, `made` counts what was produced.
   struct Counter
   {
      std::int32_t  count = 0;
      std::int32_t* made  = nullptr;
      struct sentinel
      {
      };
      struct iterator
      {
         std::int32_t  at = 0, count = 0;
         std::int32_t* made = nullptr;
         std::int32_t  operator*() const
         {
            if (made)
               ++*made;
            return (at % 1000) * (at % 1000);
         }
         iterator& operator++()
         {
            ++at;
            return *this;
         }
         bool operator==(sentinel) const { return at == count; }
      };
      iterator begin() const { return {0, count, made}; }
      sentinel end() const { return {}; }
   };

   struct Query
   {
      std::vector<Item>            items;
      std::vector<char>            encoded;   // pssz of `items`
      std::vector<char>            old_row;   // pssz of an ItemV1
      MemoryIndex                  index;     // key(owner, id) -> pssz Item
      mutable std::int32_t         produced = 0;
      mutable std::size_t          row_reads = 0;
      Annotated                    node;
      Wide                         wide;
      std::vector<char>            wide_bytes;  // pssz of `wide`
      std::optional<std::int32_t>  nothing;
      float                        nan = 0;

      pssz_view<std::vector<Item>> itemsView() const
      {
         return pssz_view<std::vector<Item>>{std::span<const char>{encoded}};
      }
      std::optional<Item> item(std::uint64_t id) const
      {
         for (const Item& i : items)
            if (i.id == id)
               return i;
         return std::nullopt;
      }
      // An owning buffer: a row "read from the database" by value.
      psio::buffer<Item, psio::pssz, psio::storage::owning> itemBuffer(std::uint32_t at) const
      {
         return psio::buffer<Item, psio::pssz, psio::storage::owning>{
            psio::encode(psio::pssz{}, items[at % items.size()])};
      }
      pssz_view<Item> oldItem() const { return pssz_view<Item>{std::span<const char>{old_row}}; }
      pssz_view<Wide> wideView() const { return pssz_view<Wide>{std::span<const char>{wide_bytes}}; }

      std::string  echo(const std::string& text) const { return text; }
      std::int32_t add(std::int32_t a, std::int32_t b) const { return a + b; }
      std::int32_t tiny(std::int8_t v) const { return v; }
      std::string  describe(Filter filter) const
      {
         std::string out = "status=";
         out += filter.status ? std::string{*psio::reflect_enum<Status>::name_of(*filter.status)} : "-";
         out += " min=" + (filter.minCount ? std::to_string(*filter.minCount) : std::string{"-"});
         out += " names=";
         for (const std::string& n : filter.names)
            out += n + ",";
         out += " range=";
         out += filter.range ? std::to_string(filter.range->lo) + ".." + std::to_string(filter.range->hi) : "-";
         return out;
      }
      std::vector<std::int32_t> ints(std::vector<std::int32_t> values) const { return values; }
      std::int32_t  total(std::vector<std::vector<std::int32_t>> grid) const
      {
         std::int32_t sum = 0;
         for (const auto& row : grid)
            for (const std::int32_t v : row)
               sum += v;
         return sum;
      }
      Status        flip(Status status) const { return status == Status::active ? Status::frozen : Status::active; }
      AccountName   owner(AccountName name) const { return AccountName{name.value + 1}; }
      std::uint64_t big(std::uint64_t value) const { return value; }
      std::vector<std::uint8_t> bytes(std::vector<std::uint8_t> data) const { return data; }
      seconds_point later(seconds_point at) const { return at + std::chrono::seconds{60}; }
      double        half(double value) const { return value / 2; }
      std::optional<std::string> maybe(std::optional<std::string> text) const { return text; }

      // Errors.
      psio::graphql::result<std::int32_t> risky(bool ok) const
      {
         if (ok)
            return 5;
         return psio::graphql::fail("risky failed");
      }
      psio::graphql::result<Tag> tagOrError(bool ok) const
      {
         if (ok)
            return Tag{"t", 1};
         return psio::graphql::fail("no tag");
      }
      std::string           badText() const { return "\xff\xfe"; }
      std::optional<Nested> nested() const { return Nested{"\xff", 1}; }
      Nested                strict() const { return Nested{"\xff", 2}; }
      std::vector<Nested>   nestedList() const { return {Nested{"fine", 1}, Nested{"\xc0", 2}}; }
      std::vector<std::optional<Nested>> nullableList() const { return {Nested{"fine", 1}, Nested{"\xc0", 2}}; }
      Status                badEnum() const { return static_cast<Status>(9); }

      Viewer viewer(psio::graphql::context& ctx) const
      {
         Session* s = ctx.user<Session>();
         if (!s)
            return Viewer{"anonymous", 0};
         return Viewer{s->user, ++s->calls};
      }
      std::string greet(const std::string& greeting, const psio::graphql::context& ctx) const
      {
         const Session* s = ctx.user<Session>();
         return greeting + ", " + (s ? s->user : std::string{"nobody"});
      }

      // Lists that are never materialised.
      Counter squares(std::int32_t first) const { return Counter{first, &produced}; }
      std::vector<std::variant<Tag, Extra>> search() const
      {
         return {Tag{"a", 1}, Extra{"e", {1, 2}}, Tag{"b", 2}};
      }
      std::vector<std::vector<std::int32_t>> grid() const { return {{1, 2}, {}, {3}}; }

      // Relay connections over the in-memory index.
      auto accounts(std::optional<std::int32_t> first,
                    std::optional<std::string>  after,
                    std::optional<std::int32_t> last,
                    std::optional<std::string>  before) const
      {
         return psio::graphql::paginate(IndexSource{index, {}, &row_reads}, {first, last, before, after}, 4);
      }
      // The same connection type over values (references, not copies).
      auto itemPages(std::optional<std::int32_t> first, std::optional<std::string> after,
                     std::optional<std::int32_t> last, std::optional<std::string> before) const
      {
         return psio::graphql::paginate(VectorSource{items}, {first, last, before, after});
      }
      std::vector<std::reference_wrapper<const Item>> itemRefs() const { return {items.begin(), items.end()}; }
      const Item*                                      firstItem() const { return items.empty() ? nullptr : &items[0]; }
      auto accountsOf(AccountName owner, std::optional<std::int32_t> first, std::optional<std::string> after) const
      {
         const std::vector<char> prefix = psio::key_prefix<std::tuple<std::uint64_t, std::uint64_t>>(owner.value);
         return psio::graphql::paginate(IndexSource{index, prefix, &row_reads}, {first, {}, {}, after}, 100);
      }
   };
   PSIO_REFLECT(Query,
                items,
                attr(encoded, psio::graphql::hidden),
                attr(old_row, psio::graphql::hidden),
                attr(index, psio::graphql::hidden),
                attr(produced, psio::graphql::hidden),
                attr(row_reads, psio::graphql::hidden),
                node,
                wide,
                attr(wide_bytes, psio::graphql::hidden),
                nothing,
                nan,
                method(itemsView),
                method(item, id),
                method(itemBuffer, at),
                method(oldItem),
                method(wideView),
                method(echo, text),
                method(add, a, b),
                method(tiny, v),
                method(describe, filter),
                method(ints, values),
                method(total, grid),
                method(flip, status),
                method(owner, name),
                method(big, value),
                method(bytes, data),
                method(later, at),
                method(half, value),
                method(maybe, text),
                method(risky, ok),
                method(tagOrError, ok),
                method(badText),
                method(nested),
                method(strict),
                method(nestedList),
                method(nullableList),
                method(badEnum),
                method(viewer, ctx),
                method(greet, greeting, ctx),
                method(squares, first),
                method(search),
                method(grid),
                method(accounts, first, after, last, before),
                method(itemPages, first, after, last, before),
                method(itemRefs),
                method(firstItem),
                method(accountsOf, owner, first, after))

   struct Mutation
   {
      std::int32_t counter = 0;

      std::int32_t bump(std::int32_t by)
      {
         counter += by;
         return counter;
      }
      void         reset() { counter = 0; }
      std::int32_t value() const { return counter; }
      psio::graphql::result<std::int32_t> failing()
      {
         ++counter;
         return psio::graphql::fail("mutation failed");
      }
   };
   PSIO_REFLECT(Mutation, attr(counter, psio::graphql::hidden), method(bump, by), method(reset), method(value), method(failing))

   using api = psio::graphql::schema<Query, Mutation>;

   inline Item make_item(std::uint64_t n)
   {
      Item i;
      i.id      = n == 2 ? 18446744073709551615ull : 1000 + n;
      i.name    = "item \"" + std::to_string(n) + "\"\n";
      i.owner   = AccountName{n % 3};
      i.status  = static_cast<Status>(n % 3);
      i.count   = static_cast<std::int32_t>(n) - 2;
      i.price   = 0.5 * static_cast<double>(n);
      i.flag    = n % 2 == 0;
      i.blob    = {static_cast<std::uint8_t>(n), 0xAB, 0xFF};
      i.created = seconds_point{std::chrono::seconds{1'700'000'000 + static_cast<std::int64_t>(n)}};
      i.ttl     = std::chrono::milliseconds{1500 * static_cast<std::int64_t>(n)};
      if (n % 2)
         i.note = "note " + std::to_string(n);
      for (std::uint64_t t = 0; t < n % 4; ++t)
         i.tags.push_back(Tag{"tag" + std::to_string(t), static_cast<std::uint32_t>(t * 10)});
      i.labels = {"x", "y\tz"};
      i.range  = {static_cast<std::int32_t>(n), static_cast<std::int32_t>(n * 2)};
      i.pair   = {static_cast<std::uint32_t>(n), "p" + std::to_string(n)};
      if (n % 2)
         i.detail = Extra{"extra", {1, -2, 3}};
      else
         i.detail = Tag{"detail", 4};
      i.big   = n == 1 ? -9007199254740993ll : static_cast<std::int64_t>(n) * -3;
      i.small = static_cast<std::uint16_t>(n * 7);
      if (n % 3 == 0)
         i.best = Tag{"best", 99};
      if (n % 2 == 0)
         i.rev = static_cast<std::uint32_t>(n + 100);
      return i;
   }

   inline Query make_query(std::size_t count = 5)
   {
      Query q;
      for (std::size_t n = 0; n < count; ++n)
         q.items.push_back(make_item(n));
      q.encoded = psio::encode(psio::pssz{}, q.items);
      ItemV1 old;
      old.id     = 77;
      old.name   = "old";
      old.owner  = AccountName{4};
      old.labels = {"l"};
      old.detail = Tag{"d", 1};
      old.best   = Tag{"b", 2};
      q.old_row  = psio::encode(psio::pssz{}, old);
      q.wide.u128      = (psio::uint128{1} << 100) + 7;
      q.wide.i128      = -(psio::int128{1} << 90);
      q.wide.u256      = psio::uint256{psio::uint128{12345}};
      q.wide.u256.limb[3] = 1;
      q.wide.fixed     = {0xDE, 0xAD, 0xBE, 0xEF};
      q.wide.maybe_big = 9007199254740992ull;
      q.wide.holes     = {std::string{"a"}, std::nullopt, std::string{}};
      q.wide.nested    = {{Tag{"n", 1}}, {}, {Tag{"m", 2}, Tag{"o", 3}}};
      q.wide.two       = {Tag{"first", 1}, Tag{"second", 2}};
      q.wide.maybe_list = std::vector<std::int32_t>{};
      q.wide_bytes     = psio::encode(psio::pssz{}, q.wide);
      for (const Item& i : q.items)
      {
         const std::vector<char> key = psio::encode(psio::key{}, std::tuple{i.owner.value, i.id});
         q.index.rows.emplace(std::string(key.data(), key.size()), psio::encode(psio::pssz{}, i));
      }
      return q;
   }
}  // namespace gqlt
