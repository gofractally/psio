#pragma once
// A query root shaped like the ones a psibase service has, shared by
// graphql_service_tests.cpp and graphql_noexceptions.cpp: connections over
// table indexes (key ranges, pages, a byte budget, transformed rows), an
// index returned as it is (resolver<Index>), event rows with their block
// beside them (flatten), instantiations of class templates, marker records
// without members, custom scalars with a text form, time points of two
// units, unions, resolver-style objects. No test framework here.

#include <psio/fixed_string.hpp>
#include <psio/graphql.hpp>
#include <psio/key.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace gqls
{
   namespace gql = psio::graphql;
   template <typename T>
   using pssz_view = psio::view<T, psio::pssz, psio::storage::const_borrow>;

   // ── Scalars ────────────────────────────────────────────────────────────
   // An account name: a number on the wire and in keys, text ("a<number>"
   // here) in JSON and GraphQL.
   struct AccountNumber
   {
      std::uint64_t value = 0;
      friend auto   operator<=>(const AccountNumber&, const AccountNumber&) = default;
   };
   PSIO_REFLECT(AccountNumber, definitionWillNotChange(), value)
   struct AccountNumberText
   {
      static constexpr std::string_view schema_id = "AccountNumber";
      static std::size_t packsize(const AccountNumber& a) { return std::to_string(a.value).size() + 1; }
      static void        encode(const AccountNumber& a, std::string& s)
      {
         s += 'a';
         s += std::to_string(a.value);
      }
      static psio::codec_status validate(std::span<const char> t) noexcept
      {
         std::uint64_t v = 0;
         if (t.size() < 2 || t[0] != 'a')
            return psio::codec_fail("not an account name", 0, "AccountNumber");
         const auto r = std::from_chars(t.data() + 1, t.data() + t.size(), v);
         return r.ec == std::errc{} && r.ptr == t.data() + t.size()
                   ? psio::codec_ok()
                   : psio::codec_fail("not an account name", 0, "AccountNumber");
      }
      static psio::codec_status validate_strict(std::span<const char> t) noexcept { return validate(t); }
      static AccountNumber      decode(std::span<const char> t)
      {
         AccountNumber a;
         std::from_chars(t.data() + 1, t.data() + t.size(), a.value);
         return a;
      }
   };
}  // namespace gqls
PSIO_ADAPTER(gqls::AccountNumber, psio::text_category, gqls::AccountNumberText)

namespace gqls
{
   using TimePointSec  = std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds>;
   using TimePointUSec = std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>;

   // ── Records ────────────────────────────────────────────────────────────
   // Markers: records without members.
   struct TLSInfo
   {
      PSIO_REFLECT(TLSInfo)
   };
   struct LocalEndpoint
   {
      PSIO_REFLECT(LocalEndpoint)
   };
   struct IPV4Endpoint
   {
      std::string   address;
      std::uint16_t port = 0;
      PSIO_REFLECT(IPV4Endpoint, address, port)
   };
   using SocketEndpoint = std::variant<IPV4Endpoint, LocalEndpoint>;

   // A stored row ...
   struct PeerRow
   {
      std::int32_t             socket = 0;
      std::vector<std::string> urls;
      std::optional<TLSInfo>   tls;
      SocketEndpoint           endpoint;
      PSIO_REFLECT(PeerRow, socket, urls, tls, endpoint)
   };
   // ... and what a query shows of it.
   struct Peer
   {
      std::int32_t             id = 0;
      std::vector<std::string> urls;
      std::optional<TLSInfo>   tls;
      SocketEndpoint           endpoint;
      bool                     secure() const { return tls.has_value(); }
      std::optional<std::string> url(std::uint32_t index) const
      {
         return index < urls.size() ? std::optional{urls[index]} : std::nullopt;
      }
      PSIO_REFLECT(Peer, id, urls, tls, endpoint, method(secure), method(url, index))
   };

   struct Account
   {
      AccountNumber                accountNum;
      AccountNumber                authService;
      std::optional<AccountNumber> resourceOwner;
      TimePointSec                 created{};
      TimePointUSec                updated{};
      PSIO_REFLECT(Account, accountNum, authService, resourceOwner, created, updated)
   };

   struct Block
   {
      std::uint32_t     blockNum = 0;
      TimePointUSec     time{};
      AccountNumber     producer;
      std::vector<char> payload;
      PSIO_REFLECT(Block, blockNum, time, producer, payload)
   };
   // A resolver-style object: state that is not reflected, fields that are
   // methods, not default-constructible.
   struct BlockQuery
   {
      const Block* block;
      explicit BlockQuery(const Block* b) : block(b) {}
      std::uint32_t blockNum() const { return block->blockNum; }
      const Block&  header() const { return *block; }
      std::uint32_t size() const { return static_cast<std::uint32_t>(block->payload.size()); }
      PSIO_REFLECT(BlockQuery, method(blockNum), method(header), method(size))
   };

   // An event and the block it was emitted in, at one level.
   struct EventBlockInfo
   {
      std::optional<std::uint32_t> blockNum;
      std::optional<TimePointUSec> blockTime;
      PSIO_REFLECT(EventBlockInfo, blockNum, blockTime)
   };
   template <typename T>
   struct WithBlockContext
   {
      EventBlockInfo block;
      T              data;
      PSIO_REFLECT(WithBlockContext, block, attr(data, psio::graphql::flatten))
   };
   struct InviteEvent
   {
      std::uint32_t inviteId = 0;
      AccountNumber actor;
      std::string   event;
      PSIO_REFLECT(InviteEvent, inviteId, actor, event)
   };
   struct TransferEvent
   {
      AccountNumber from;
      std::int64_t  amount = 0;
      PSIO_REFLECT(TransferEvent, from, amount)
   };
   // A class template with its default name (Pair_Account_Block), and one
   // named by a template argument.
   template <typename A, typename B>
   struct Pair
   {
      A first;
      B second;
      PSIO_REFLECT(Pair, first, second)
   };
   template <typename T, psio::fixed_string Name>
   struct Named
   {
      T value;
      PSIO_REFLECT(Named, value)
   };

   // ── A table: key bytes (psio::key) -> encoded row ──────────────────────
   using table_t = std::map<std::string, std::vector<char>, std::less<>>;

   // An index of a table, as psio::graphql::ordered_index wants it. `Key` is
   // the type its keys encode; rows are read in place.
   template <typename Row, typename Key>
   struct Index
   {
      using iterator = table_t::const_iterator;
      const table_t* rows  = nullptr;
      std::size_t*   reads = nullptr;  // rows handed out

      iterator begin() const { return rows->begin(); }
      iterator end() const { return rows->end(); }
      iterator lower_bound(std::span<const char> key) const
      {
         return rows->lower_bound(std::string_view{key.data(), key.size()});
      }
      iterator upper_bound(std::span<const char> key) const
      {
         return rows->upper_bound(std::string_view{key.data(), key.size()});
      }
      std::span<const char> key(iterator it) const { return {it->first.data(), it->first.size()}; }
      pssz_view<Row>        row(iterator it) const
      {
         if (reads)
            ++*reads;
         return pssz_view<Row>{std::span<const char>{it->second}};
      }
      std::size_t row_bytes(iterator it) const { return it->second.size(); }
   };

   // What a field that returns an Index takes and yields.
   template <typename Key>
   struct IndexArgs
   {
      gql::key_range<Key> range;
      gql::page_args      page;
      PSIO_REFLECT(IndexArgs, range, page)
   };
}  // namespace gqls
template <typename Key>
inline constexpr bool psio::graphql::argument_group<gqls::IndexArgs<Key>> = true;
template <typename Row, typename Key>
struct psio::graphql::resolver<gqls::Index<Row, Key>>
{
   using arguments = gqls::IndexArgs<Key>;
   static auto resolve(gqls::Index<Row, Key>&& index, const arguments& args)
   {
      return psio::graphql::paginate(std::move(index), args.range, args.page);
   }
};

// Names: the union, the template named by its argument, and the event
// connections as psibase names them (<Event>Connection over
// <Event>WithBlockContext nodes).
template <>
struct psio::graphql::type_name<gqls::SocketEndpoint>
{
   static constexpr std::string_view value = "SocketEndpoint";
};
template <typename T, psio::fixed_string Name>
struct psio::graphql::type_name<gqls::Named<T, Name>>
{
   static constexpr std::string_view value = Name.view();
};
template <typename T>
struct psio::graphql::type_name<gqls::WithBlockContext<T>>
{
   static constexpr std::string_view value = psio::graphql::affixed_name<"", T, "WithBlockContext">;
};
template <typename T>
struct psio::graphql::type_name<psio::graphql::connection<gqls::WithBlockContext<T>>>
{
   static constexpr std::string_view value = psio::graphql::affixed_name<"", T, "Connection">;
};
template <typename T>
struct psio::graphql::type_name<psio::graphql::edge<gqls::WithBlockContext<T>>>
{
   static constexpr std::string_view value = psio::graphql::affixed_name<"", T, "Edge">;
};

namespace gqls
{
   struct Db
   {
      table_t                  accounts;  // by accountNum
      table_t                  peers;     // by socket
      table_t                  blocks;    // by blockNum
      std::vector<Block>       chain;
      std::vector<InviteEvent> events;    // event i has row id i + 1, block i / 2
      std::vector<char>        stored_event;
      mutable std::size_t      reads = 0;
   };

   template <typename K>
   std::string key_of(const K& k)
   {
      const auto bytes = psio::to_key(k);
      return std::string(bytes.data(), bytes.size());
   }

   inline Db make_db(std::uint32_t accounts = 12, std::uint32_t blocks = 30)
   {
      Db db;
      for (std::uint32_t i = 0; i < accounts; ++i)
      {
         Account a{AccountNumber{i * 10},
                   AccountNumber{1},
                   i % 2 ? std::optional{AccountNumber{i}} : std::nullopt,
                   TimePointSec{std::chrono::seconds{1700000000 + i}},
                   TimePointUSec{std::chrono::microseconds{1700000000000000 + i * 250000}}};
         db.accounts[key_of(a.accountNum)] = psio::to_pssz(a);
      }
      for (std::int32_t s = 1; s <= 5; ++s)
      {
         PeerRow p{s,
                   {"https://peer" + std::to_string(s) + ".example", "wss://peer" + std::to_string(s)},
                   s % 2 ? std::optional{TLSInfo{}} : std::nullopt,
                   s == 3 ? SocketEndpoint{LocalEndpoint{}}
                          : SocketEndpoint{IPV4Endpoint{"10.0.0." + std::to_string(s), 8080}}};
         db.peers[key_of(s)] = psio::to_pssz(p);
      }
      for (std::uint32_t n = 1; n <= blocks; ++n)
      {
         Block b{n, TimePointUSec{std::chrono::microseconds{1700000000000000 + n * 1000000ll}},
                 AccountNumber{n % 3}, std::vector<char>(n * 10, 'x')};
         db.blocks[key_of(n)] = psio::to_pssz(b);
         db.chain.push_back(b);
      }
      for (std::uint32_t i = 0; i < 7; ++i)
         db.events.push_back(InviteEvent{i % 2, AccountNumber{100 + i}, i % 3 ? "accepted" : "created"});
      db.stored_event = psio::to_pssz(WithBlockContext<InviteEvent>{
         EventBlockInfo{9, std::nullopt}, InviteEvent{4, AccountNumber{44}, "stored"}});
      return db;
   }

   struct Query
   {
      const Db* db = nullptr;

      // A row by key, and several.
      std::optional<Account> getAccount(AccountNumber accountName) const
      {
         const auto it = db->accounts.find(key_of(accountName));
         if (it == db->accounts.end())
            return std::nullopt;
         return psio::decode<Account>(psio::pssz{}, std::span<const char>{it->second});
      }
      std::vector<std::optional<Account>> getAccounts(const std::vector<AccountNumber>& accountNames) const
      {
         std::vector<std::optional<Account>> out;
         for (const AccountNumber a : accountNames)
            out.push_back(getAccount(a));
         return out;
      }
      // An index, as it is: resolver<Index> gives the field its arguments.
      auto accounts() const { return Index<Account, AccountNumber>{&db->accounts, &db->reads}; }
      // The same, spelled out, with a range fixed by the service.
      auto accountsFrom(AccountNumber start, gql::page_args page) const
      {
         return gql::paginate(Index<Account, AccountNumber>{&db->accounts},
                              gql::key_range<AccountNumber>{.ge = start}, page);
      }
      // Rows shown as another record.
      auto peers(gql::page_args page) const
      {
         return gql::paginate(gql::transform(Index<PeerRow, std::int32_t>{&db->peers},
                                             [](pssz_view<PeerRow> row) {
                                                PeerRow r = psio::decode<PeerRow>(psio::pssz{}, row._psio_data());
                                                return Peer{r.socket, std::move(r.urls), std::move(r.tls),
                                                            std::move(r.endpoint)};
                                             }),
                              page);
      }
      // A byte budget: a page has at most 256 bytes of blocks.
      auto blocks(gql::key_range<std::uint32_t> range, gql::page_args page) const
      {
         return gql::paginate(Index<Block, std::uint32_t>{&db->blocks}, range, page,
                              gql::page_budget{.max_bytes = 256, .max_page_size = 1000});
      }
      std::optional<BlockQuery> block(std::uint32_t blockNum) const
      {
         if (blockNum == 0 || blockNum > db->chain.size())
            return std::nullopt;
         return BlockQuery{&db->chain[blockNum - 1]};
      }
      // A connection built by hand: rows of another store, with their block.
      auto history(std::uint32_t inviteId, gql::page_args page) const
         -> gql::result<gql::connection<WithBlockContext<InviteEvent>>>
      {
         if (page.last || page.before)
            return gql::fail("history pages forward only.");
         std::uint64_t after = 0;
         if (page.after && (page.after->size() != 16 ||
                            std::from_chars(page.after->data(), page.after->data() + 16, after, 16).ec != std::errc{}))
            return gql::fail("Argument \"after\" is not a cursor.");
         const std::size_t                                     limit = page.first ? static_cast<std::size_t>(*page.first) : 100;
         gql::connection_builder<WithBlockContext<InviteEvent>> out;
         bool                                                  more = false;
         for (std::uint64_t rowid = after + 1; rowid <= db->events.size(); ++rowid)
         {
            const InviteEvent& e = db->events[rowid - 1];
            if (e.inviteId != inviteId)
               continue;
            if (out.size() == limit)
            {
               more = true;
               break;
            }
            const auto          n = static_cast<std::uint32_t>((rowid - 1) / 2);
            const auto          cursor = psio::to_key(rowid);
            out.add(WithBlockContext<InviteEvent>{
                       EventBlockInfo{n, TimePointUSec{std::chrono::microseconds{1700000000000000 + n * 1000000ll}}}, e},
                    std::span<const char>{cursor.data(), cursor.size()});
         }
         return std::move(out).finish(after != 0, more);
      }
      // Encoded rows, read in place: markers, a union and a flattened
      // member through views.
      auto peerRows(gql::page_args page) const
      {
         return gql::paginate(Index<PeerRow, std::int32_t>{&db->peers}, page);
      }
      pssz_view<WithBlockContext<InviteEvent>> storedEvent() const
      {
         return pssz_view<WithBlockContext<InviteEvent>>{std::span<const char>{db->stored_event}};
      }
      WithBlockContext<TransferEvent> lastTransfer() const
      {
         return {EventBlockInfo{}, TransferEvent{AccountNumber{5}, -3}};
      }
      Pair<Account, IPV4Endpoint> pair() const { return {*getAccount(AccountNumber{0}), {"::1", 1}}; }
      Named<std::int32_t, "Height"> height() const { return {static_cast<std::int32_t>(db->chain.size())}; }
      Named<std::string, "Label">   label() const { return {"main"}; }
   };
   PSIO_REFLECT(Query,
                attr(db, psio::graphql::hidden),
                method(getAccount, accountName),
                method(getAccounts, accountNames),
                method(accounts),
                method(accountsFrom, start, page),
                method(peers, page),
                method(blocks, range, page),
                method(block, blockNum),
                method(history, inviteId, page),
                method(peerRows, page),
                method(storedEvent),
                method(lastTransfer),
                method(pair),
                method(height),
                method(label))

   using api = psio::graphql::schema<Query>;
   // The same without introspection, as a guest that wants the size builds it.
   using lean_api = psio::graphql::schema<Query, void, false>;

   // What `api::sdl()` prints: the types in the order the schema reaches
   // them from the query root.
   inline constexpr std::string_view expected_sdl =
      R"(type Query {
  getAccount(accountName: AccountNumber!): Account
  getAccounts(accountNames: [AccountNumber!]!): [Account]!
  accounts(gt: AccountNumber, ge: AccountNumber, lt: AccountNumber, le: AccountNumber, first: Int, last: Int, before: String, after: String): AccountConnection
  accountsFrom(start: AccountNumber!, first: Int, last: Int, before: String, after: String): AccountConnection
  peers(first: Int, last: Int, before: String, after: String): PeerConnection
  blocks(gt: Int, ge: Int, lt: Int, le: Int, first: Int, last: Int, before: String, after: String): BlockConnection
  block(blockNum: Int!): BlockQuery
  history(inviteId: Int!, first: Int, last: Int, before: String, after: String): InviteEventConnection
  peerRows(first: Int, last: Int, before: String, after: String): PeerRowConnection
  storedEvent: InviteEventWithBlockContext!
  lastTransfer: TransferEventWithBlockContext!
  pair: Pair_Account_IPV4Endpoint!
  height: Height!
  label: Label!
}

type Account {
  accountNum: AccountNumber!
  authService: AccountNumber!
  resourceOwner: AccountNumber
  created: DateTimeSeconds!
  updated: DateTimeMicroseconds!
}

scalar AccountNumber

type AccountConnection {
  edges: [AccountEdge!]!
  pageInfo: PageInfo!
}

type PeerConnection {
  edges: [PeerEdge!]!
  pageInfo: PageInfo!
}

type BlockConnection {
  edges: [BlockEdge!]!
  pageInfo: PageInfo!
}

type BlockQuery {
  blockNum: Int!
  header: Block!
  size: Int!
}

type InviteEventConnection {
  edges: [InviteEventEdge!]!
  pageInfo: PageInfo!
}

type PeerRowConnection {
  edges: [PeerRowEdge!]!
  pageInfo: PageInfo!
}

type InviteEventWithBlockContext {
  block: EventBlockInfo!
  inviteId: Int!
  actor: AccountNumber!
  event: String!
}

type TransferEventWithBlockContext {
  block: EventBlockInfo!
  from: AccountNumber!
  amount: Int64!
}

type Pair_Account_IPV4Endpoint {
  first: Account!
  second: IPV4Endpoint!
}

type Height {
  value: Int!
}

type Label {
  value: String!
}

scalar DateTimeSeconds

scalar DateTimeMicroseconds

type AccountEdge {
  node: Account!
  cursor: String!
}

type PageInfo {
  hasNextPage: Boolean!
  hasPreviousPage: Boolean!
  startCursor: String
  endCursor: String
}

type PeerEdge {
  node: Peer!
  cursor: String!
}

type BlockEdge {
  node: Block!
  cursor: String!
}

type Block {
  blockNum: Int!
  time: DateTimeMicroseconds!
  producer: AccountNumber!
  payload: Bytes!
}

type InviteEventEdge {
  node: InviteEventWithBlockContext!
  cursor: String!
}

type PeerRowEdge {
  node: PeerRow!
  cursor: String!
}

type EventBlockInfo {
  blockNum: Int
  blockTime: DateTimeMicroseconds
}

scalar Int64

type IPV4Endpoint {
  address: String!
  port: Int!
}

type Peer {
  id: Int!
  urls: [String!]!
  tls: TLSInfo
  endpoint: SocketEndpoint!
  secure: Boolean!
  url(index: Int!): String
}

scalar Bytes

type PeerRow {
  socket: Int!
  urls: [String!]!
  tls: TLSInfo
  endpoint: SocketEndpoint!
}

type TLSInfo {
  _: Boolean
}

union SocketEndpoint = IPV4Endpoint | LocalEndpoint

type LocalEndpoint {
  _: Boolean
}
)";
}  // namespace gqls
