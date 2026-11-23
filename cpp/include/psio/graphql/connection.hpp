#pragma once
//
// psio/graphql/connection.hpp — Relay cursor connections over any ordered
// source of rows with opaque key bytes.
//
//   struct Query {
//      auto accounts(std::optional<std::int32_t> first, std::optional<std::string> after,
//                    std::optional<std::int32_t> last, std::optional<std::string> before) const
//      {
//         return psio::graphql::paginate(my_index_source{...}, {first, last, before, after});
//      }
//   };
//   PSIO_REFLECT(Query, method(accounts, first, after, last, before))
//
// gives `accounts(first: Int, after: String, last: Int, before: String):
// AccountConnection` with `edges { node cursor }` and `pageInfo`.
//
// A row source is a cursor over rows ordered by key (row_source below). The
// key bytes are whatever orders the rows — for a database index, the
// psio::key encoding (key.hpp) of the index key, which sorts bytewise as the
// values do; restrict a source to the rows under a key prefix with
// psio::key_prefix / key_prefix_upper_bound in its seek functions. A cursor
// is the hex of a row's key. A row is whatever the source yields: a C++
// value, or a view / buffer of an encoded row, in which case only the
// selected fields of each node are read.
//
// paginate() reads at most one page (and probes one row on each side for
// pageInfo); nothing else is materialised. There is no database code here.

#include <psio/graphql/schema.hpp>

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

   // The Relay pagination arguments.
   struct page_args
   {
      std::optional<std::int32_t> first{};
      std::optional<std::int32_t> last{};
      std::optional<std::string>  before{};
      std::optional<std::string>  after{};
   };
   // As a method parameter it is the four arguments `first: Int, last: Int,
   // before: String, after: String`.
   PSIO_REFLECT(page_args, first, last, before, after)
   template <>
   inline constexpr bool argument_group<page_args> = true;

   // One page of `source` (spec: Relay Cursor Connections). `first` pages
   // forward from `after`, `last` backward from `before`; with neither, the
   // first `max_page_size` rows. A page never has more than `max_page_size`
   // edges. An invalid argument is a field error.
   template <row_source Source>
   auto paginate(Source&& source, const page_args& args, std::size_t max_page_size = 100)
      -> result<connection<std::remove_cvref_t<decltype(source.row())>>>
   {
      using Node = std::remove_cvref_t<decltype(source.row())>;
      if (args.first && args.last)
         return fail("Arguments \"first\" and \"last\" cannot both be given.");
      if ((args.first && *args.first < 0) || (args.last && *args.last < 0))
         return fail("Arguments \"first\" and \"last\" must not be negative.");
      std::vector<char> after, before;
      if (args.after && !detail::cursor_bytes(*args.after, after))
         return fail("Argument \"after\" is not a cursor.");
      if (args.before && !detail::cursor_bytes(*args.before, before))
         return fail("Argument \"before\" is not a cursor.");

      const bool        backward = args.last.has_value();
      const std::size_t limit =
         std::min<std::size_t>(max_page_size, static_cast<std::size_t>(backward ? *args.last
                                                                       : args.first ? *args.first
                                                                                    : max_page_size));
      connection<Node>                                 page;
      std::vector<std::pair<std::size_t, std::size_t>> spans;
      page.edges.reserve(std::min<std::size_t>(limit, 256));
      spans.reserve(std::min<std::size_t>(limit, 256));
      auto take = [&] {
         const std::span<const char> key = source.key();
         spans.emplace_back(page.keys.size(), key.size());
         page.keys.insert(page.keys.end(), key.begin(), key.end());
         page.edges.push_back(edge<Node>{source.row(), page_cursor{}});
      };
      bool on_row = false;
      if (!backward)
      {
         on_row = args.after ? source.seek_after(after) : source.seek_first();
         auto in_range = [&] { return on_row && (!args.before || detail::key_less(source.key(), before)); };
         while (page.edges.size() < limit && in_range())
         {
            take();
            on_row = source.next();
         }
         page.pageInfo.hasNextPage = in_range();
      }
      else
      {
         on_row = args.before ? source.seek_before(before) : source.seek_last();
         auto in_range = [&] { return on_row && (!args.after || detail::key_less(after, source.key())); };
         while (page.edges.size() < limit && in_range())
         {
            take();
            on_row = source.prev();
         }
         page.pageInfo.hasPreviousPage = in_range();
         std::reverse(page.edges.begin(), page.edges.end());
         std::reverse(spans.begin(), spans.end());
      }
      for (std::size_t i = 0; i < page.edges.size(); ++i)
         page.edges[i].cursor.key = std::string_view{page.keys.data() + spans[i].first, spans[i].second};
      if (!page.edges.empty())
      {
         page.pageInfo.startCursor = page.edges.front().cursor;
         page.pageInfo.endCursor   = page.edges.back().cursor;
         // The other side: is there a row beyond the page, inside the bounds?
         const std::string_view first_key = page.edges.front().cursor.key;
         const std::string_view last_key  = page.edges.back().cursor.key;
         if (!backward)
            page.pageInfo.hasPreviousPage =
               args.after && source.seek_before(std::span<const char>{first_key.data(), first_key.size()});
         else
            page.pageInfo.hasNextPage =
               args.before && source.seek_after(std::span<const char>{last_key.data(), last_key.size()});
      }
      return page;
   }

}  // namespace psio::graphql
