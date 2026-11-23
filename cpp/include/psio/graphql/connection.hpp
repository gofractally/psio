#pragma once
//
// psio/graphql/connection.hpp — Relay cursor connections over any ordered
// source of rows with opaque key bytes.
//
//   struct Query {
//      auto accounts(psio::graphql::key_range<AccountName> range,
//                    psio::graphql::page_args              page) const
//      {
//         return psio::graphql::paginate(my_index{...}, range, page);
//      }
//   };
//   PSIO_REFLECT(Query, method(accounts, range, page))
//
// gives `accounts(gt: AccountName, ge: AccountName, lt: AccountName,
// le: AccountName, first: Int, last: Int, before: String, after: String):
// AccountConnection` with `edges { node cursor }` and `pageInfo`.
//
// The rows come from the embedder, in one of two forms:
//
//   ordered_index   lower_bound / upper_bound / begin / end over key bytes
//                   and the key and row of a position — what a database
//                   index has;
//   row_source      a cursor with seek and step — what paginate() works on
//                   (index_source adapts the first to the second).
//
// The key bytes are whatever orders the rows — for a database index, the
// psio::key encoding (key.hpp) of the index key, which sorts bytewise as the
// values do. A cursor is the hex of a row's key. A row is whatever the
// source yields: a C++ value, or a view / buffer of an encoded row, in which
// case only the selected fields of each node are read. transform(source, fn)
// maps the rows.
//
// paginate() reads at most one page (and probes one row on each side for
// pageInfo); nothing else is materialised. There is no database code here.

#include <psio/graphql/schema.hpp>
#include <psio/key.hpp>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace psio::graphql
{
   // A position in an ordered sequence of rows. Each seek / step returns
   // whether the source is now on a row; key() and row() are valid only then.
   // key() may be invalidated by the next seek / step; row() must stay valid
   // on its own (a value, an owning buffer, or a view of stable memory).
   //
   // Optional members:
   //   s.seek_at_or_after(key) -> bool   first row with key >= `key`; without
   //                                     it paginate() uses seek_before + next
   //   s.row_bytes() -> std::size_t      size of the current row, for a byte
   //                                     budget (page_budget)
   template <typename S>
   concept row_source = requires(S& s, std::span<const char> key) {
      { s.seek_first() } -> std::convertible_to<bool>;
      { s.seek_last() } -> std::convertible_to<bool>;
      { s.seek_after(key) } -> std::convertible_to<bool>;   // first row with key > `key`
      { s.seek_before(key) } -> std::convertible_to<bool>;  // last row with key < `key`
      { s.next() } -> std::convertible_to<bool>;
      { s.prev() } -> std::convertible_to<bool>;
      { s.key() } -> std::convertible_to<std::span<const char>>;
      s.row();
   };

   // An ordered index: positions (bidirectional iterators) from begin() to
   // end(), found by key bytes, and the key and the row at a position.
   //
   //   x.begin(), x.end()
   //   x.lower_bound(key)     first position with key >= `key`, else end()
   //   x.upper_bound(key)     first position with key >  `key`, else end()
   //   ++it, --it, it == it
   //   x.key(it)              the key bytes there (a span, or a container)
   //   x.row(it)              the row there (a value, a view, a buffer)
   //   x.row_bytes(it)        optional: its size, for a byte budget
   //
   // An index over part of a table answers within its part: lower_bound /
   // upper_bound never leave [begin(), end()].
   template <typename X>
   concept ordered_index = requires(const X& x, std::span<const char> key) {
      { x.begin() } -> std::copyable;
      { x.lower_bound(key) } -> std::convertible_to<decltype(x.begin())>;
      { x.upper_bound(key) } -> std::convertible_to<decltype(x.begin())>;
      requires requires(decltype(x.begin()) it) {
         { x.end() } -> std::convertible_to<decltype(x.begin())>;
         ++it;
         --it;
         { it == it } -> std::convertible_to<bool>;
         { std::span<const char>{x.key(it)} };
         x.row(it);
      };
   };

   // The row_source over an ordered_index. `Index` is held by value, or by
   // reference when it is an lvalue reference type.
   template <typename Index>
      requires ordered_index<std::remove_cvref_t<Index>>
   class index_source
   {
      using X  = std::remove_cvref_t<Index>;
      using It = std::remove_cvref_t<decltype(std::declval<const X&>().begin())>;
      using K  = std::remove_cvref_t<decltype(std::declval<const X&>().key(std::declval<It&>()))>;
      Index index_;
      It    at_;
      K     key_{};

      const X& x() const noexcept { return index_; }
      bool     on() const { return !(at_ == It(x().end())); }

     public:
      explicit index_source(Index&& index) : index_(std::forward<Index>(index)), at_(x().end()) {}

      bool seek_first()
      {
         at_ = x().begin();
         return on();
      }
      bool seek_last()
      {
         at_ = x().end();
         return prev();
      }
      bool seek_after(std::span<const char> key)
      {
         at_ = x().upper_bound(key);
         return on();
      }
      bool seek_at_or_after(std::span<const char> key)
      {
         at_ = x().lower_bound(key);
         return on();
      }
      bool seek_before(std::span<const char> key)
      {
         at_ = x().lower_bound(key);
         return prev();
      }
      bool next()
      {
         ++at_;
         return on();
      }
      bool prev()
      {
         if (at_ == It(x().begin()))
         {
            at_ = x().end();
            return false;
         }
         --at_;
         return true;
      }
      std::span<const char> key()
      {
         key_ = x().key(at_);
         return std::span<const char>{key_};
      }
      decltype(auto) row() { return x().row(at_); }
      std::size_t    row_bytes()
         requires requires(const X& i, It& it) { i.row_bytes(it); }
      {
         return static_cast<std::size_t>(x().row_bytes(at_));
      }
   };

   namespace detail
   {
      // Either form, as a row_source.
      template <typename S>
      decltype(auto) as_source(S&& source)
      {
         if constexpr (row_source<std::remove_cvref_t<S>>)
            return std::forward<S>(source);
         else
            return index_source<S>{std::forward<S>(source)};
      }
      template <typename S>
      using source_t = std::remove_cvref_t<decltype(as_source(std::declval<S>()))>;
   }  // namespace detail

   template <typename S>
   concept page_source =
      row_source<std::remove_cvref_t<S>> || ordered_index<std::remove_cvref_t<S>>;

   // The rows of `source`, each passed through `fn` (a row of a table shown
   // as another record). The keys, and so the cursors, are the source's.
   template <row_source Source, typename Fn>
   class transformed_source
   {
      Source source_;
      Fn     fn_;

     public:
      transformed_source(Source&& source, Fn&& fn) : source_(std::move(source)), fn_(std::move(fn)) {}
      bool seek_first() { return source_.seek_first(); }
      bool seek_last() { return source_.seek_last(); }
      bool seek_after(std::span<const char> key) { return source_.seek_after(key); }
      bool seek_before(std::span<const char> key) { return source_.seek_before(key); }
      bool seek_at_or_after(std::span<const char> key)
         requires requires(Source& s) { s.seek_at_or_after(key); }
      {
         return source_.seek_at_or_after(key);
      }
      bool                  next() { return source_.next(); }
      bool                  prev() { return source_.prev(); }
      std::span<const char> key() { return source_.key(); }
      decltype(auto)        row() { return fn_(source_.row()); }
      std::size_t           row_bytes()
         requires requires(Source& s) { s.row_bytes(); }
      {
         return source_.row_bytes();
      }
   };
   template <page_source Source, typename Fn>
   auto transform(Source&& source, Fn fn)
   {
      return transformed_source<detail::source_t<Source>, Fn>{
         detail::source_t<Source>(detail::as_source(std::forward<Source>(source))), std::move(fn)};
   }

   struct page_info
   {
      bool                       hasNextPage     = false;
      bool                       hasPreviousPage = false;
      std::optional<page_cursor> startCursor;
      std::optional<page_cursor> endCursor;
   };
   PSIO_REFLECT(page_info, hasNextPage, hasPreviousPage, startCursor, endCursor)
   template <>
   struct type_name<page_info>
   {
      static constexpr std::string_view value = "PageInfo";
   };

   template <typename Node>
   struct edge
   {
      Node        node;
      page_cursor cursor;
      PSIO_REFLECT(edge, node, cursor)
   };

   // One page. Move-only: the cursors point into `keys`.
   template <typename Node>
   struct connection
   {
      std::vector<edge<Node>> edges;
      page_info               pageInfo;
      std::vector<char>       keys;

      connection()                             = default;
      connection(connection&&)                 = default;
      connection& operator=(connection&&)      = default;
      connection(const connection&)            = delete;
      connection& operator=(const connection&) = delete;
      PSIO_REFLECT(connection, edges, pageInfo)
   };

   namespace detail
   {
      template <typename Node>
      struct edge_namer
      {
         static constexpr name_buf make()
         {
            return name_buf{}.add(named_type_name<typename out_wrap<Node>::named>()).add("Edge");
         }
      };
      template <typename Node>
      struct connection_namer
      {
         static constexpr name_buf make()
         {
            return name_buf{}.add(named_type_name<typename out_wrap<Node>::named>()).add("Connection");
         }
      };

      inline bool cursor_bytes(std::string_view hex, std::vector<char>& out)
      {
         if (hex.size() & 1)
            return false;
         out.resize(hex.size() / 2);
         for (std::size_t i = 0; i < out.size(); ++i)
         {
            const int hi = ji::hex_values[static_cast<unsigned char>(hex[2 * i])];
            const int lo = ji::hex_values[static_cast<unsigned char>(hex[2 * i + 1])];
            if (hi < 0 || lo < 0)
               return false;
            out[i] = static_cast<char>((hi << 4) | lo);
         }
         return true;
      }
      inline bool key_less(std::span<const char> a, std::span<const char> b) noexcept
      {
         return std::string_view{a.data(), a.size()} < std::string_view{b.data(), b.size()};
      }
   }  // namespace detail

   // `<Node>Edge` and `<Node>Connection`; specialise type_name for one
   // connection<Node> to name it differently.
   template <typename Node>
   struct type_name<edge<Node>>
   {
      static constexpr std::string_view value = detail::stored_name<detail::edge_namer<Node>>::value;
   };
   template <typename Node>
   struct type_name<connection<Node>>
   {
      static constexpr std::string_view value =
         detail::stored_name<detail::connection_namer<Node>>::value;
   };

   // Builds a page edge by edge, for a connection that does not come from
   // paginate() (rows of a query in another system, say). A cursor is the
   // bytes the client hands back in `before` / `after`; the response holds
   // their hex.
   template <typename Node>
   class connection_builder
   {
      connection<Node>                                 page_;
      std::vector<std::pair<std::size_t, std::size_t>> spans_;

     public:
      void reserve(std::size_t n)
      {
         page_.edges.reserve(n);
         spans_.reserve(n);
      }
      std::size_t size() const noexcept { return spans_.size(); }
      void        add(Node node, std::span<const char> cursor)
      {
         spans_.emplace_back(page_.keys.size(), cursor.size());
         page_.keys.insert(page_.keys.end(), cursor.begin(), cursor.end());
         page_.edges.push_back(edge<Node>{std::move(node), page_cursor{}});
      }
      // The cursor bytes of edge `i`.
      std::span<const char> cursor(std::size_t i) const noexcept
      {
         return {page_.keys.data() + spans_[i].first, spans_[i].second};
      }
      // The edges were added last to first.
      void reverse()
      {
         std::reverse(page_.edges.begin(), page_.edges.end());
         std::reverse(spans_.begin(), spans_.end());
      }
      connection<Node> finish(bool has_previous_page, bool has_next_page) &&
      {
         for (std::size_t i = 0; i < page_.edges.size(); ++i)
            page_.edges[i].cursor.key =
               std::string_view{page_.keys.data() + spans_[i].first, spans_[i].second};
         page_.pageInfo.hasPreviousPage = has_previous_page;
         page_.pageInfo.hasNextPage     = has_next_page;
         if (!page_.edges.empty())
         {
            page_.pageInfo.startCursor = page_.edges.front().cursor;
            page_.pageInfo.endCursor   = page_.edges.back().cursor;
         }
         return std::move(page_);
      }
   };

   // The Relay pagination arguments. As a method parameter it is the four
   // arguments `first: Int, last: Int, before: String, after: String`.
   struct page_args
   {
      std::optional<std::int32_t> first{};
      std::optional<std::int32_t> last{};
      std::optional<std::string>  before{};
      std::optional<std::string>  after{};
   };
   PSIO_REFLECT(page_args, first, last, before, after)
   template <>
   inline constexpr bool argument_group<page_args> = true;

   // A range of an index, by its key type `K`. As a method parameter it is
   // the four arguments `gt: K, ge: K, lt: K, le: K`. `K` may be a leading
   // part of the index key (the first element of a composite key): `gt`
   // then skips, and `le` includes, every row that starts with it.
   template <typename K>
   struct key_range
   {
      std::optional<K> gt{};
      std::optional<K> ge{};
      std::optional<K> lt{};
      std::optional<K> le{};
      PSIO_REFLECT(key_range, gt, ge, lt, le)
   };
   template <typename K>
   inline constexpr bool argument_group<key_range<K>> = true;

   // A range of key bytes: [lower, upper), each side optional.
   struct key_bounds
   {
      std::vector<char> lower{};
      std::vector<char> upper{};
      bool              has_lower = false;
      bool              has_upper = false;
      bool              none      = false;  // no key is in the range

      // Keys >= `key`.
      void at_least(std::span<const char> key)
      {
         if (!has_lower || detail::key_less(lower, key))
            lower.assign(key.begin(), key.end());
         has_lower = true;
      }
      // Keys < `key`.
      void below(std::span<const char> key)
      {
         if (!has_upper || detail::key_less(key, upper))
            upper.assign(key.begin(), key.end());
         has_upper = true;
      }
      // Keys beyond `key` and every key that starts with it.
      void beyond(std::span<const char> key)
      {
         if (const auto next = key_prefix_upper_bound(key))
            at_least(*next);
         else
            none = true;
      }
      // Keys up to `key` and every key that starts with it.
      void through(std::span<const char> key)
      {
         if (const auto next = key_prefix_upper_bound(key))
            below(*next);
      }
   };

   // The bytes of a typed range, with the bounds encoded by psio::key. A
   // bound that has no key (a string that is not UTF-8, a value outside its
   // type's invariant) is a field error.
   template <typename K>
   result<key_bounds> to_bounds(const key_range<K>& range)
   {
      key_bounds        out;
      std::vector<char> bytes;
      auto              encode = [&](const std::optional<K>& bound) {
         bytes.clear();
         return !bound || encode_key_checked(*bound, bytes).ok();
      };
      if (!encode(range.ge))
         return fail("Argument \"ge\" has an invalid value.");
      if (range.ge)
         out.at_least(bytes);
      if (!encode(range.gt))
         return fail("Argument \"gt\" has an invalid value.");
      if (range.gt)
         out.beyond(bytes);
      if (!encode(range.lt))
         return fail("Argument \"lt\" has an invalid value.");
      if (range.lt)
         out.below(bytes);
      if (!encode(range.le))
         return fail("Argument \"le\" has an invalid value.");
      if (range.le)
         out.through(bytes);
      return out;
   }

   // A page never has more than `max_page_size` edges (`first` / `last` are
   // capped; it is also the size of a page asked for without either).
   struct page_limits
   {
      std::size_t max_page_size = 100;
      constexpr page_limits() = default;
      constexpr page_limits(std::size_t max_page_size) : max_page_size(max_page_size) {}
   };
   // ... and its rows together have at most `max_bytes` bytes, as the source
   // counts them (row_bytes()). A page that has rows to show always has at
   // least one edge, however large the row; a page cut short by the budget
   // says so in pageInfo (hasNextPage, or hasPreviousPage paging backward).
   struct page_budget
   {
      std::size_t max_bytes;
      std::size_t max_page_size = 100;
   };

   namespace detail
   {
      template <typename S>
      bool seek_at_or_after(S& source, std::span<const char> key)
      {
         if constexpr (requires { source.seek_at_or_after(key); })
            return source.seek_at_or_after(key);
         else
            return source.seek_before(key) ? source.next() : source.seek_first();
      }

      template <typename Source, typename Limits>
      auto paginate(Source& source, const key_bounds* bounds, const page_args& args, const Limits& lim)
         -> result<connection<std::remove_cvref_t<decltype(source.row())>>>
      {
         using Node = std::remove_cvref_t<decltype(source.row())>;
         if (args.first && args.last)
            return fail("Arguments \"first\" and \"last\" cannot both be given.");
         if ((args.first && *args.first < 0) || (args.last && *args.last < 0))
            return fail("Arguments \"first\" and \"last\" must not be negative.");
         std::vector<char> after, before;
         if (args.after && !cursor_bytes(*args.after, after))
            return fail("Argument \"after\" is not a cursor.");
         if (args.before && !cursor_bytes(*args.before, before))
            return fail("Argument \"before\" is not a cursor.");

         const bool        backward = args.last.has_value();
         const std::size_t limit =
            std::min<std::size_t>(lim.max_page_size,
                                  static_cast<std::size_t>(backward       ? *args.last
                                                           : args.first ? *args.first
                                                                        : lim.max_page_size));
         connection_builder<Node> page;
         if (bounds && bounds->none)
            return std::move(page).finish(false, false);
         page.reserve(std::min<std::size_t>(limit, 256));

         // The rows a page may show: inside the bounds and between the cursors.
         const bool has_lower = bounds && bounds->has_lower;
         const bool has_upper = bounds && bounds->has_upper;
         auto       in_lower  = [&](std::span<const char> key) {
            return !has_lower || !key_less(key, bounds->lower);
         };
         auto in_upper = [&](std::span<const char> key) {
            return !has_upper || key_less(key, bounds->upper);
         };
         [[maybe_unused]] std::size_t bytes = 0;
         auto                         fits  = [&] {
            if constexpr (requires { lim.max_bytes; })
            {
               bytes += static_cast<std::size_t>(source.row_bytes());
               return page.size() == 0 || bytes <= lim.max_bytes;
            }
            else
               return true;
         };
         // pageInfo follows the Relay algorithm: paging forward, hasNextPage
         // says whether the range has a row beyond the page — one that
         // `first` or the budget left out, or one at or after `before` — and
         // hasPreviousPage whether it has one before the page (only `after`
         // can have skipped one). Paging backward, the mirror image.
         bool on_row = false;
         bool more   = false;  // a row beyond the last one taken, in the direction of paging
         if (!backward)
         {
            if (args.after && (!has_lower || !key_less(after, bounds->lower)))
               on_row = source.seek_after(after);
            else if (has_lower)
               on_row = seek_at_or_after(source, bounds->lower);
            else
               on_row = source.seek_first();
            auto in_range = [&] {
               return on_row && in_upper(source.key()) && (!args.before || key_less(source.key(), before));
            };
            while (page.size() < limit && in_range() && fits())
            {
               page.add(source.row(), source.key());
               on_row = source.next();
            }
            more = on_row && in_upper(source.key());
         }
         else
         {
            const bool use_before = args.before && (!has_upper || key_less(before, bounds->upper));
            if (use_before)
               on_row = source.seek_before(before);
            else if (has_upper)
               on_row = source.seek_before(bounds->upper);
            else
               on_row = source.seek_last();
            auto in_range = [&] {
               return on_row && in_lower(source.key()) && (!args.after || key_less(after, source.key()));
            };
            while (page.size() < limit && in_range() && fits())
            {
               page.add(source.row(), source.key());
               on_row = source.prev();
            }
            more = on_row && in_lower(source.key());
            page.reverse();
         }
         // The other side: a row of the range before the page (after it,
         // paging backward). For a page without edges: one up to `after`
         // (from `before` on).
         bool other = false;
         if (!backward && args.after)
         {
            if (page.size() != 0)
               other = source.seek_before(page.cursor(0));
            else
               other = source.seek_after(after) ? source.prev() : source.seek_last();
            other = other && in_lower(source.key()) && in_upper(source.key());
         }
         else if (backward && args.before)
         {
            if (page.size() != 0)
               other = source.seek_after(page.cursor(page.size() - 1));
            else
               other = seek_at_or_after(source, before);
            other = other && in_lower(source.key()) && in_upper(source.key());
         }
         return backward ? std::move(page).finish(more, other) : std::move(page).finish(other, more);
      }

      template <typename Source, typename K, typename Limits>
      auto paginate_range(Source& source, const key_range<K>& range, const page_args& args, const Limits& lim)
         -> result<connection<std::remove_cvref_t<decltype(source.row())>>>
      {
         const result<key_bounds> bounds = to_bounds(range);
         if (!bounds)
            return fail(bounds.error());
         return paginate(source, &*bounds, args, lim);
      }

      template <typename S>
      using page_t = result<connection<std::remove_cvref_t<decltype(std::declval<source_t<S>&>().row())>>>;
      template <typename S>
      concept sized_page_source = page_source<S> && requires(source_t<S>& s) { s.row_bytes(); };
   }  // namespace detail

   // One page of `source` (spec: Relay Cursor Connections). `first` pages
   // forward from `after`, `last` backward from `before`; with neither, the
   // first `max_page_size` rows. An invalid argument is a field error.
   //
   //   paginate(source, page)                 all rows
   //   paginate(source, range, page)          the rows in a key_range<K>
   //   paginate(source, bounds, page)         the rows in key_bounds (bytes)
   //
   // each with a last argument: page_limits (a number: the largest page;
   // default 100) or page_budget (that and a byte budget; the source must
   // have row_bytes()).
   template <page_source Source>
   auto paginate(Source&& source, const page_args& args, page_limits lim = {}) -> detail::page_t<Source>
   {
      decltype(auto) s = detail::as_source(std::forward<Source>(source));
      return detail::paginate(s, nullptr, args, lim);
   }
   template <detail::sized_page_source Source>
   auto paginate(Source&& source, const page_args& args, const page_budget& lim) -> detail::page_t<Source>
   {
      decltype(auto) s = detail::as_source(std::forward<Source>(source));
      return detail::paginate(s, nullptr, args, lim);
   }
   template <page_source Source>
   auto paginate(Source&& source, const key_bounds& bounds, const page_args& args, page_limits lim = {})
      -> detail::page_t<Source>
   {
      decltype(auto) s = detail::as_source(std::forward<Source>(source));
      return detail::paginate(s, &bounds, args, lim);
   }
   template <detail::sized_page_source Source>
   auto paginate(Source&& source, const key_bounds& bounds, const page_args& args, const page_budget& lim)
      -> detail::page_t<Source>
   {
      decltype(auto) s = detail::as_source(std::forward<Source>(source));
      return detail::paginate(s, &bounds, args, lim);
   }
   template <page_source Source, typename K>
   auto paginate(Source&& source, const key_range<K>& range, const page_args& args, page_limits lim = {})
      -> detail::page_t<Source>
   {
      decltype(auto) s = detail::as_source(std::forward<Source>(source));
      return detail::paginate_range(s, range, args, lim);
   }
   template <detail::sized_page_source Source, typename K>
   auto paginate(Source&& source, const key_range<K>& range, const page_args& args, const page_budget& lim)
      -> detail::page_t<Source>
   {
      decltype(auto) s = detail::as_source(std::forward<Source>(source));
      return detail::paginate_range(s, range, args, lim);
   }

}  // namespace psio::graphql
