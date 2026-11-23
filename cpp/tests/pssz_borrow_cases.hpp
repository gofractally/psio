#pragma once
// Decoding into a record that BORROWS from the bytes: members declared
// std::string_view, std::span<const B> of a byte type, psio::pssz_view<X>,
// or std::optional of those point into the input; nothing they refer to is
// copied. One body for pssz_borrow_tests.cpp (Catch2) and
// pssz_borrow_noexceptions.cpp (-fno-exceptions -fno-rtti).

#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace borrow_cases
{
   // psibase::BasicHttpRequest<Body> in miniature.
   struct Header
   {
      std::string name;
      std::string value;
      friend bool operator==(const Header&, const Header&) = default;
   };
   PSIO_REFLECT(Header, name, value)
   struct HeaderRef
   {
      std::string_view name;
      std::string_view value;
   };
   PSIO_REFLECT(HeaderRef, name, value)

   template <typename Body>
   struct Request
   {
      std::string         host;
      std::string         method;
      std::vector<Header> headers;
      Body                body;
      PSIO_REFLECT(Request, host, method, headers, body)
   };
   using OwnedRequest    = Request<std::vector<char>>;
   using BorrowedRequest = Request<std::span<const char>>;

   // Every shape that borrows, against the owning record.
   struct Owned
   {
      std::uint32_t                    id = 0;
      std::string                      text;
      std::vector<char>                chars;
      std::vector<unsigned char>       octets;
      std::vector<std::byte>           bytes;
      Header                           first;
      std::vector<Header>              headers;
      std::optional<std::string>       note;
      std::optional<std::vector<char>> extra;
      std::optional<Header>            maybe;
      std::vector<std::uint32_t>       numbers;
      friend bool operator==(const Owned&, const Owned&) = default;
   };
   PSIO_REFLECT(Owned, id, text, chars, octets, bytes, first, headers, note, extra, maybe, numbers)
   struct Borrowed
   {
      std::uint32_t                           id = 0;
      std::string_view                        text;
      std::span<const char>                   chars;
      std::span<const unsigned char>          octets;
      std::span<const std::byte>              bytes;
      psio::pssz_view<Header>                 first;    // a view of a member record
      std::vector<HeaderRef>                  headers;  // a list of records that borrow
      std::optional<std::string_view>         note;
      std::optional<std::span<const char>>    extra;
      std::optional<psio::pssz_view<Header>>  maybe;
      std::vector<std::uint32_t>              numbers;  // owned: wider than a byte
   };
   PSIO_REFLECT(Borrowed, id, text, chars, octets, bytes, first, headers, note, extra, maybe, numbers)

   // A view member of a type that is not a record borrows where nothing
   // narrows its widths.
   struct Lists
   {
      std::vector<std::string> names;
      std::uint8_t             n = 0;
   };
   PSIO_REFLECT(Lists, names, n)
   struct ListsRef
   {
      psio::pssz_view<std::vector<std::string>> names;
      std::uint8_t                               n = 0;
   };
   PSIO_REFLECT(ListsRef, names, n)

   // Bounds on a borrowed member are enforced as on the owning one.
   struct Bounded
   {
      std::string text;
      PSIO_REFLECT(Bounded, attr(text, utf8 | max<4>))
   };
   struct BoundedRef
   {
      std::string_view text;
      PSIO_REFLECT(BoundedRef, attr(text, utf8 | max<4>))
   };

   struct Svc
   {
      void serve(OwnedRequest request) {}
      void store(Owned row, std::uint32_t n) {}
      PSIO_REFLECT(Svc, method(serve, request), method(store, row, n))
   };

   static_assert(psio::pssz_borrows_v<BorrowedRequest>);
   static_assert(psio::pssz_borrows_v<Borrowed>);
   static_assert(psio::pssz_borrows_v<HeaderRef>);
   static_assert(psio::pssz_borrows_v<std::vector<HeaderRef>>);
   static_assert(psio::pssz_borrows_v<std::string_view>);
   static_assert(!psio::pssz_borrows_v<OwnedRequest>);
   static_assert(!psio::pssz_borrows_v<Owned>);
   static_assert(!psio::pssz_borrows_v<std::string>);
   // A record that borrows stands for the owning record.
   static_assert(psio::packs_as<BorrowedRequest, OwnedRequest>);
   static_assert(psio::packs_as<Borrowed, Owned>);

   inline bool inside(const void* p, std::span<const char> bytes)
   {
      const char* c = static_cast<const char*>(p);
      return c >= bytes.data() && c <= bytes.data() + bytes.size();
   }

   inline Owned sample()
   {
      Owned o;
      o.id      = 7;
      o.text    = "hello";
      o.chars   = {'a', 'b'};
      o.octets  = {1, 2, 3};
      o.bytes   = {std::byte{9}};
      o.first   = {"host", "example"};
      o.headers = {{"a", "1"}, {"bb", "22"}};
      o.note    = "note";
      o.extra   = std::vector<char>{'x'};
      o.maybe   = Header{"m", "v"};
      o.numbers = {10, 20};
      return o;
   }

   // Returns 0, or the line of the first expectation that failed.
   inline int run()
   {
#define BORROW_EXPECT(cond)                                              \
   do                                                                    \
   {                                                                     \
      if (!(cond))                                                       \
      {                                                                  \
         std::printf("FAILED line %d: %s\n", __LINE__, #cond);           \
         return __LINE__;                                                \
      }                                                                  \
   } while (0)

      // The request: the body is not copied.
      {
         const OwnedRequest owned{"example.com", "POST", {{"content-type", "text/plain"}},
                                  {'b', 'o', 'd', 'y'}};
         const std::vector<char>     storage = psio::to_pssz(owned);
         const std::span<const char> bytes{storage};
         const auto                  r = psio::from_pssz<BorrowedRequest>(bytes);
         BORROW_EXPECT(r.host == "example.com" && r.method == "POST");
         BORROW_EXPECT(r.headers == owned.headers);
         BORROW_EXPECT(r.body.size() == 4 && r.body[0] == 'b' && r.body[3] == 'y');
         BORROW_EXPECT(inside(r.body.data(), bytes));
         // The trusted spelling and the CPO.
         const auto t = psio::from_pssz<BorrowedRequest>(psio::trusted{bytes});
         BORROW_EXPECT(t.body.data() == r.body.data());
         const auto d = psio::decode<BorrowedRequest>(psio::pssz{}, bytes);
         BORROW_EXPECT(d.body.data() == r.body.data());
         // It packs as the owning record, and is an argument for it.
         BORROW_EXPECT(psio::to_pssz(r) == storage);
         BORROW_EXPECT((psio::to_pssz_as<OwnedRequest>(r) == storage));
         BORROW_EXPECT((psio::method_args_to_pssz<Svc, 0>(r) ==
                        psio::to_pssz(psio::reflect<Svc>::method_arg_tuple<0>{owned})));
         // An empty body borrows an empty span.
         const auto empty = psio::to_pssz(OwnedRequest{"h", "GET", {}, {}});
         BORROW_EXPECT(psio::from_pssz<BorrowedRequest>(std::span<const char>{empty}).body.empty());
      }
      // Every shape.
      {
         const Owned                 owned   = sample();
         const std::vector<char>     storage = psio::to_pssz(owned);
         const std::span<const char> bytes{storage};
         const Borrowed              b = psio::from_pssz<Borrowed>(bytes);
         BORROW_EXPECT(b.id == 7);
         BORROW_EXPECT(b.text == "hello" && inside(b.text.data(), bytes));
         BORROW_EXPECT(b.chars.size() == 2 && b.chars[1] == 'b' && inside(b.chars.data(), bytes));
         BORROW_EXPECT(b.octets.size() == 3 && b.octets[2] == 3 && inside(b.octets.data(), bytes));
         BORROW_EXPECT(b.bytes.size() == 1 && b.bytes[0] == std::byte{9});
         BORROW_EXPECT(b.first.name() == "host" && b.first.value() == "example");
         BORROW_EXPECT(inside(psio::bytes(b.first).data(), bytes));
         BORROW_EXPECT(b.headers.size() == 2 && b.headers[1].name == "bb" &&
                       b.headers[1].value == "22" && inside(b.headers[1].value.data(), bytes));
         BORROW_EXPECT(b.note && *b.note == "note" && inside(b.note->data(), bytes));
         BORROW_EXPECT(b.extra && b.extra->size() == 1 && (*b.extra)[0] == 'x');
         BORROW_EXPECT(b.maybe && b.maybe->name() == "m");
         BORROW_EXPECT(b.numbers == owned.numbers);
         // The same bytes again, and the same value when unpacked.
         BORROW_EXPECT(psio::to_pssz(b) == storage);
         BORROW_EXPECT(psio::from_pssz<Owned>(psio::to_pssz(b)) == owned);
         BORROW_EXPECT((psio::method_args_to_pssz<Svc, 1>(b, 3) ==
                        psio::to_pssz(psio::reflect<Svc>::method_arg_tuple<1>{owned, 3u})));

         // Absent optionals, and pruned trailing ones.
         Owned bare = owned;
         bare.note.reset();
         bare.extra.reset();
         bare.maybe.reset();
         bare.numbers.clear();
         const auto     bare_bytes = psio::to_pssz(bare);
         const Borrowed nb = psio::from_pssz<Borrowed>(std::span<const char>{bare_bytes});
         BORROW_EXPECT(!nb.note && !nb.extra && !nb.maybe && nb.numbers.empty());
         BORROW_EXPECT(psio::to_pssz(nb) == bare_bytes);
         // Present and empty is not absent.
         Owned hollow  = bare;
         hollow.note   = "";
         hollow.extra  = std::vector<char>{};
         const auto     hollow_bytes = psio::to_pssz(hollow);
         const Borrowed hb = psio::from_pssz<Borrowed>(std::span<const char>{hollow_bytes});
         BORROW_EXPECT(hb.note && hb.note->empty() && hb.extra && hb.extra->empty());
         BORROW_EXPECT(psio::to_pssz(hb) == hollow_bytes);
      }
      // A view of a list member, and the borrowing types themselves.
      {
         const auto lists = psio::to_pssz(Lists{{"a", "bc"}, 4});
         const auto r     = psio::from_pssz<ListsRef>(std::span<const char>{lists});
         BORROW_EXPECT(r.n == 4 && r.names.size() == 2 && r.names[1] == "bc");
         const std::string        text  = "plain";
         const auto               tb    = psio::to_pssz(text);
         const std::span<const char> tspan{tb};
         BORROW_EXPECT(psio::from_pssz<std::string_view>(tspan) == "plain");
         BORROW_EXPECT(psio::from_pssz<std::string_view>(tspan).data() == tb.data());
         const std::vector<unsigned char> raw{5, 6};
         const auto                       rb = psio::to_pssz(raw);
         const std::span<const char>      rspan{rb};
         BORROW_EXPECT(psio::from_pssz<std::span<const unsigned char>>(rspan).size() == 2);
         const auto hb = psio::to_pssz(std::vector<Header>{{"a", "b"}});
         const auto hv = psio::from_pssz<std::vector<HeaderRef>>(std::span<const char>{hb});
         BORROW_EXPECT(hv.size() == 1 && hv[0].value == "b");
      }
      // The checked decode validates first: as the owning type.
      {
         const auto good = psio::to_pssz(Bounded{"abcd"});
         BORROW_EXPECT(psio::validate_pssz<BoundedRef>(good).ok());
         BORROW_EXPECT(psio::from_pssz<BoundedRef>(std::span<const char>{good}).text == "abcd");
         std::vector<char> bad = good;
         bad.push_back('e');  // five bytes: over the bound
         BORROW_EXPECT(!psio::validate_pssz<BoundedRef>(bad).ok());
         BoundedRef out;
         bool       unknown = false;
         BORROW_EXPECT(!psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{bad}, out,
                                                  unknown)
                           .ok());
         BORROW_EXPECT(psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{good}, out,
                                                 unknown)
                          .ok() &&
                       out.text == "abcd");
         std::vector<char> cut(good.begin(), good.begin() + 2);
         BORROW_EXPECT(!psio::validate_pssz<BorrowedRequest>(cut).ok());
      }
#undef BORROW_EXPECT
      return 0;
   }
}  // namespace borrow_cases
