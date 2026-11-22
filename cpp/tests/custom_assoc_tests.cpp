// Transparent types, custom text forms and invariants with maps, sets, pairs
// and psio::nested<T>: see custom_assoc_cases.hpp for the cases shared with
// the exception-disabled build. This file adds the fail-fast paths and the
// in-place rule.
#include "custom_assoc_cases.hpp"

#include <psio/mutable_view.hpp>

#include <catch.hpp>

using namespace custom_assoc_cases;

namespace
{
   struct Row
   {
      Id                            id;
      std::map<std::string, Id>     ids;
      psio::nested<Id>              packed;
      std::vector<Tag>              tags;
      std::map<Name, std::uint32_t> by_name;  // an invariant KEY: read-only anyway
      PSIO_REFLECT(Row, id, ids, packed, tags, by_name)
   };
}  // namespace

TEST_CASE("custom x map / set / nested: every checked entry point, without a throw",
          "[custom][assoc][nested]")
{
   int failed = -1;
   REQUIRE_NOTHROW(failed = custom_assoc_cases::run());
   CHECK(failed == 0);
}

TEST_CASE("custom x map / set / nested: the fail-fast writers refuse a violated invariant",
          "[custom][assoc][nested]")
{
   const Name  bad_name{std::uint64_t{1} << 60};
   const Range bad_range{5, 1};
   CHECK_THROWS(psio::encode(psio::pssz{}, ByName{{bad_name, Range{0, 1}}}));
   CHECK_THROWS(psio::encode(psio::pssz{}, ByName{{Name{1}, bad_range}}));
   CHECK_THROWS(psio::encode(psio::pssz{}, Levels{static_cast<Level>(9)}));
   CHECK_THROWS(psio::encode(psio::json{}, SlugMap{{Slug{"NO"}, 1}}));
   CHECK_THROWS(psio::encode(psio::ijson{}, ByRef{{Ref{std::string{}}, 1}}));
   CHECK_THROWS(psio::to_key(ByName{{Name{1}, bad_range}}));
   CHECK_THROWS(psio::to_key(Members{{bad_name}}));
   CHECK_THROWS(psio::to_key(Levels{static_cast<Level>(9)}));
   CHECK_THROWS(psio::encode(psio::pssz{}, Budget{{{Tag{"a"}, 101}}}));
   // A nested value is built from a value that satisfies its invariants.
   CHECK_THROWS(Packed::from(bad_range));
   CHECK_THROWS(Packed{bad_range});
   CHECK_NOTHROW(Packed::from(Range{1, 5}));
   // The trusted decode does not check (validated input is its contract).
   const Frame bytes = unchecked_frame(ByName{{Name{1}, bad_range}});
   CHECK(psio::decode<ByName>(psio::pssz{}, sp(bytes)).at(Name{1}) == bad_range);
}

TEST_CASE("custom x mutable_view: transparent wrappers are their member; invariants block",
          "[custom][mutable_view]")
{
   Row row{Id{1}, {{"a", Id{2}}}, psio::nested<Id>::from(Id{3}), {Tag{"t"}}, {{Name{4}, 5}}};
   auto bytes = psio::encode(psio::pssz{}, row);
   const psio::mutable_view<Row, psio::pssz> view{bytes};
   REQUIRE(view.get<0>().set(10));
   REQUIRE(view.get<1>()[0].value().set(20));
   REQUIRE(view.get<2>().get().set(30));
   REQUIRE(psio::size(view.get<3>()[0]) == 1);
   REQUIRE(view.get<4>()[0].key().get<0>() == 4u);
   REQUIRE(view.get<4>()[0].value().set(50));
   row = Row{Id{10}, {{"a", Id{20}}}, psio::nested<Id>::from(Id{30}), {Tag{"t"}}, {{Name{4}, 50}}};
   REQUIRE(bytes == psio::encode(psio::pssz{}, row));
   REQUIRE(psio::validate_strict<Row>(psio::pssz{}, std::span<const char>{bytes}).ok());

   static_assert(psio::mutable_view<Id, psio::pssz>::assignable);
   static_assert(psio::detail::mutation_blocked_v<Name> && psio::detail::mutation_blocked_v<Range> &&
                 psio::detail::mutation_blocked_v<Level> && psio::detail::mutation_blocked_v<Slug> &&
                 !psio::detail::mutation_blocked_v<Id> && !psio::detail::mutation_blocked_v<Tag>);
}
