// psio::shared_pssz<T> / psio::pssz_buffer<T> as types of their own and as
// members of reflected records (probes main/g25, g3_shared_member,
// g3b_shared_ops, g3c): a packed value with shared or sole ownership that is
// a psio::nested<T> on the wire, in a key, in text and in a schema.

#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/mutable_view.hpp>
#include <psio/nested.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/schema_builder.hpp>

#include <catch.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace shp
{
   struct Tx
   {
      std::uint32_t exp = 0;
      std::string   memo;
      friend bool   operator==(const Tx&, const Tx&) = default;
   };
   PSIO_REFLECT(Tx, exp, memo)

   struct SignedTx
   {
      psio::shared_pssz<Tx>    transaction;
      std::vector<std::string> proofs;
   };
   PSIO_REFLECT(SignedTx, transaction, proofs)
   struct SignedTxNested
   {
      psio::nested<Tx>         transaction;
      std::vector<std::string> proofs;
   };
   PSIO_REFLECT(SignedTxNested, transaction, proofs)
   struct SignedTxBytes
   {
      std::vector<char>        transaction;
      std::vector<std::string> proofs;
   };
   PSIO_REFLECT(SignedTxBytes, transaction, proofs)

   struct Holder
   {
      psio::pssz_buffer<Tx>                    owned;
      std::optional<psio::shared_pssz<Tx>>     maybe;
      std::vector<psio::shared_pssz<Tx>>       list;
      std::variant<std::uint32_t, psio::shared_pssz<Tx>> either;
   };
   PSIO_REFLECT(Holder, owned, maybe, list, either)
   struct HolderNested
   {
      psio::nested<Tx>                         owned;
      std::optional<psio::nested<Tx>>          maybe;
      std::vector<psio::nested<Tx>>            list;
      std::variant<std::uint32_t, psio::nested<Tx>> either;
   };
   PSIO_REFLECT(HolderNested, owned, maybe, list, either)

   // Under a cap: the nested bytes stay the automatic-profile encoding.
   struct Capped
   {
      std::uint32_t         id = 0;
      psio::shared_pssz<Tx> tx;
   };
   PSIO_REFLECT(Capped, id, tx, maxDynamicData(200))
}  // namespace shp

using namespace shp;

TEST_CASE("shared_pssz: construction, access, equality (probe g3c)", "[shared_pssz]")
{
   const Tx              one{1, "m"};
   psio::shared_pssz<Tx> p{psio::to_pssz(one)};   // from bytes: validated
   Tx                    t{2, "q"};
   psio::shared_pssz<Tx> q{t};                    // from a value: encodes
   REQUIRE(p->exp() == 1);
   REQUIRE(p->memo() == "m");
   auto v = *p;
   REQUIRE(v.memo() == "m");
   REQUIRE(p.get().exp() == 1);
   REQUIRE(p.view().exp() == 1);
   Tx u = p.unpack();
   REQUIRE(u == one);
   REQUIRE(p.decode() == one);
   REQUIRE(p.data() != nullptr);
   REQUIRE(p.size() == psio::pssz_size(one));
   REQUIRE(p.bytes().size() == p.size());
   REQUIRE_FALSE(p == q);
   REQUIRE(p != q);
   REQUIRE(q == psio::shared_pssz<Tx>::from(t));
   REQUIRE(psio::value_of(*p) == one);

   // Copies share the bytes.
   auto p2 = p;
   REQUIRE(p2.data() == p.data());
   REQUIRE(p2 == p);

   // Adopting a vector copies nothing.
   auto              bytes   = psio::to_pssz(one);
   const char* const address = bytes.data();
   psio::shared_pssz<Tx> adopted{std::move(bytes)};
   REQUIRE(adopted.data() == address);
   auto              raw2 = psio::to_pssz(one);
   const char* const a2   = raw2.data();
   auto              un   = psio::shared_pssz<Tx>::unchecked(std::move(raw2));
   REQUIRE(un.data() == a2);

   // The free functions of every buffer still apply.
   REQUIRE(psio::bytes(p).data() == p.data());
   REQUIRE(psio::size(p) == p.size());
   REQUIRE(psio::as_view(p).exp() == 1);
}

TEST_CASE("shared_pssz: a default-constructed one holds T{}", "[shared_pssz]")
{
   psio::shared_pssz<Tx> none;
   REQUIRE(none.unpack() == Tx{});
   REQUIRE(none == psio::shared_pssz<Tx>{Tx{}});
   REQUIRE(none.size() == psio::pssz_size(Tx{}));
   REQUIRE(none->exp() == 0);
   // No allocation per default-constructed value: one process-wide encoding.
   psio::shared_pssz<Tx> other;
   REQUIRE(other.data() == none.data());
   psio::pssz_buffer<Tx> owned;
   REQUIRE(owned.unpack() == Tx{});
   REQUIRE(owned.data() == none.data());
   // Writing through an owning buffer gives it bytes of its own.
   REQUIRE(psio::as_mutable_view(owned).exp().set(5u));
   REQUIRE(owned.unpack() == Tx{5, ""});
   REQUIRE(psio::pssz_buffer<Tx>{}.unpack() == Tx{});
   // An optional packed value is a std::optional.
   std::optional<psio::shared_pssz<Tx>> absent;
   REQUIRE_FALSE(absent.has_value());
}

TEST_CASE("shared_pssz: bytes are validated unless trusted", "[shared_pssz]")
{
   const Tx          t{2, "q"};
   std::vector<char> junk(3, 'x');
#if defined(__cpp_exceptions)
   REQUIRE_THROWS_AS(psio::shared_pssz<Tx>{std::span<const char>{junk}}, psio::codec_exception);
   REQUIRE_THROWS_AS(psio::shared_pssz<Tx>{std::vector<char>(3, 'x')}, psio::codec_exception);
   REQUIRE_THROWS_AS(psio::pssz_buffer<Tx>{std::vector<char>(3, 'x')}, psio::codec_exception);
#endif
   psio::shared_pssz<Tx> out{t};
   REQUIRE_FALSE(psio::shared_pssz<Tx>::checked(std::span<const char>{junk}, out).ok());
   REQUIRE(out.unpack() == t);   // unchanged
   REQUIRE(psio::shared_pssz<Tx>::checked(psio::to_pssz(Tx{9, "z"}), out).ok());
   REQUIRE(out.unpack() == Tx{9, "z"});
   REQUIRE(out.validate().ok());
   REQUIRE(out.validate_strict().ok());
   Tx   decoded;
   bool unknown = true;
   REQUIRE(out.decode_checked(decoded, &unknown).ok());
   REQUIRE(decoded == Tx{9, "z"});
   REQUIRE_FALSE(unknown);

   // trusted: the caller's statement; nothing is looked at.
   psio::shared_pssz<Tx> tr{psio::trusted{psio::to_pssz(t)}};
   REQUIRE(tr == psio::shared_pssz<Tx>{t});
   psio::pssz_buffer<Tx> otr{psio::trusted{psio::to_pssz(t)}};
   REQUIRE(otr.unpack() == t);
   REQUIRE(psio::pssz_buffer<Tx>{t} == otr);
   REQUIRE(psio::pssz_buffer<Tx>::from(t) == otr);
   REQUIRE(psio::to_shared(otr) == tr);
   REQUIRE(psio::to_buffer(tr) == otr);
   REQUIRE(std::move(otr).release() == psio::to_pssz(t));
}

TEST_CASE("shared_pssz as a record member is nested<T> on the wire (probes g25, "
          "g3_shared_member)",
          "[shared_pssz][member]")
{
   const Tx              tx{1, "m"};
   psio::shared_pssz<Tx> p{tx};
   SignedTx              s{p, {"p"}};
   SignedTxNested        n{psio::nested<Tx>::from(tx), {"p"}};
   auto                  b = psio::to_pssz(s);
   REQUIRE(b == psio::to_pssz(n));
   REQUIRE(b == psio::to_pssz(SignedTxBytes{psio::to_pssz(tx), {"p"}}));
   REQUIRE(psio::pssz_size(s) == b.size());
   REQUIRE(psio::validate_pssz<SignedTx>(b).ok());
   REQUIRE(psio::validate_strict<SignedTx>(psio::pssz{}, std::span<const char>{b}).ok());
   REQUIRE(psio::to_pssz(p) == psio::to_pssz(psio::nested<Tx>::from(tx)));

   // A reader validates the member's bytes as a Tx, as for nested<T>.
   auto bad = psio::to_pssz(SignedTxBytes{std::vector<char>(3, 'x'), {"p"}});
   REQUIRE_FALSE(psio::validate_pssz<SignedTx>(bad).ok());

   auto back = psio::from_pssz<SignedTx>(b);
   REQUIRE(back.transaction == p);
   REQUIRE(back.transaction->memo() == "m");
   REQUIRE(back.proofs == std::vector<std::string>{"p"});

   // A default-constructed record encodes (the member holds Tx{}).
   REQUIRE(psio::to_pssz(SignedTx{}) == psio::to_pssz(SignedTxNested{}));

   // Copying the record shares the transaction.
   SignedTx copy = s;
   REQUIRE(copy.transaction.data() == s.transaction.data());

   // Views: read and in-place write, by name.
   psio::pssz_view<SignedTx> sv{b};
   REQUIRE(sv.transaction()->memo() == "m");
   REQUIRE((*sv.transaction()).exp() == 1);
   REQUIRE(sv.transaction().bytes().size() == p.size());
   REQUIRE(sv.transaction().unpack() == p);
   REQUIRE(sv.transaction()->unpack() == tx);
   std::vector<char>                mb = b;
   psio::pssz_mutable_view<SignedTx> mw{mb};
   mw.transaction()->exp() = 9;
   REQUIRE(psio::from_pssz<SignedTx>(mb).transaction->exp() == 9);

   // Under a cap.
   Capped c{4, p};
   auto   cb = psio::to_pssz(c);
   REQUIRE(psio::validate_pssz<Capped>(cb).ok());
   REQUIRE(psio::from_pssz<Capped>(cb).tx == p);
   REQUIRE(psio::pssz_view<Capped>{cb}.tx()->memo() == "m");
}

TEST_CASE("shared_pssz member: key, JSON and schema are nested<T>'s", "[shared_pssz][member]")
{
   const Tx       tx{1, "m"};
   SignedTx       s{psio::shared_pssz<Tx>{tx}, {"p"}};
   SignedTxNested n{psio::nested<Tx>::from(tx), {"p"}};
   REQUIRE(psio::to_key(s) == psio::to_key(n));
   REQUIRE(psio::from_key<SignedTx>(psio::to_key(s)).transaction == s.transaction);
   REQUIRE(psio::to_json(s) == psio::to_json(n));
   REQUIRE(psio::to_json(s) == R"({"transaction":{"exp":1,"memo":"m"},"proofs":["p"]})");
   REQUIRE(psio::to_ijson(s) == psio::to_ijson(n));
   REQUIRE(psio::from_json<SignedTx>(psio::to_json(s)).transaction == s.transaction);

   auto schema_shared = psio::SchemaBuilder{}.insert<SignedTx>("S").build();
   auto schema_nested = psio::SchemaBuilder{}.insert<SignedTxNested>("S").build();
   REQUIRE(psio::to_ijson(schema_shared) ==
           [&] {
              // Same node kinds; only the record's own name differs.
              auto text = psio::to_ijson(schema_nested);
              for (std::size_t at; (at = text.find("SignedTxNested")) != std::string::npos;)
                 text.replace(at, 14, "SignedTx");
              return text;
           }());
}

TEST_CASE("packed members in optionals, lists and variants", "[shared_pssz][member]")
{
   const Tx     a{1, "a"}, b{2, "b"};
   Holder       h{psio::pssz_buffer<Tx>{a}, psio::shared_pssz<Tx>{b},
                  {psio::shared_pssz<Tx>{a}, psio::shared_pssz<Tx>{b}}, psio::shared_pssz<Tx>{b}};
   HolderNested n{psio::nested<Tx>::from(a), psio::nested<Tx>::from(b),
                  {psio::nested<Tx>::from(a), psio::nested<Tx>::from(b)},
                  psio::nested<Tx>::from(b)};
   auto bytes = psio::to_pssz(h);
   REQUIRE(bytes == psio::to_pssz(n));
   REQUIRE(psio::validate_pssz<Holder>(bytes).ok());
   auto back = psio::from_pssz<Holder>(bytes);
   REQUIRE(back.owned.unpack() == a);
   REQUIRE(back.maybe.has_value());
   REQUIRE(back.maybe->unpack() == b);
   REQUIRE(back.list.size() == 2);
   REQUIRE(back.list[1].unpack() == b);
   REQUIRE(std::get<1>(back.either).unpack() == b);
   REQUIRE(psio::to_json(h) == psio::to_json(n));

   Holder none{};
   REQUIRE(psio::to_pssz(none) == psio::to_pssz(HolderNested{}));
   REQUIRE_FALSE(psio::from_pssz<Holder>(psio::to_pssz(none)).maybe.has_value());
}
