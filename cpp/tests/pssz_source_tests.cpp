// Packing from non-owning sources (probes g7_tie, p8b E1 / E3): a tuple of
// references, std::string_view, const char*, std::span, std::reference_wrapper
// and a view given as a value all write the bytes of the owning type they
// are encoded as (psio::wire_type_t) — without copying what they refer to.

#include <psio/nested.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <catch.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace srct
{
   struct Item
   {
      std::uint32_t a = 0;
      std::string   s;
      friend bool   operator==(const Item&, const Item&) = default;
   };
   PSIO_REFLECT(Item, a, s)

   struct Fixed
   {
      std::uint32_t a = 0;
      std::uint16_t b = 0;
   };
   PSIO_REFLECT(Fixed, a, b, definitionWillNotChange())

   // A record with a cap, and the same record holding views of its members.
   struct Row
   {
      std::uint32_t            id = 0;
      std::vector<std::string> tags;
      Item                     item;
   };
   PSIO_REFLECT(Row, id, tags, item, maxDynamicData(1024))
   struct RowView
   {
      std::uint32_t                                      id = 0;
      psio::view<std::vector<std::string>, psio::pssz>   tags;
      psio::pssz_view<Item>                              item;
   };
   PSIO_REFLECT(RowView, id, tags, item, maxDynamicData(1024))

   // psibase's TxSuccessRecord / TxSuccessView pair.
   struct Trace
   {
      std::uint64_t id  = 0;
      std::uint32_t num = 0;
      Item          trace;
   };
   PSIO_REFLECT(Trace, id, num, trace)
   struct TraceView
   {
      std::uint64_t               id  = 0;
      std::uint32_t               num = 0;
      psio::pssz_view<const Item> trace;
   };
   PSIO_REFLECT(TraceView, id, num, trace)

   // Every kind of source as a member, and the owning record it packs as.
   struct Sources
   {
      std::string_view                   name;
      std::span<const std::uint32_t>     nums;
      std::span<const char>              raw;
      std::reference_wrapper<const Item> item;
      psio::pssz_view<Fixed>             fixed;
      std::optional<std::string_view>    opt;
      std::reference_wrapper<const std::uint64_t> n;
   };
   PSIO_REFLECT(Sources, name, nums, raw, item, fixed, opt, n)
   struct Owned
   {
      std::string                name;
      std::vector<std::uint32_t> nums;
      std::vector<char>          raw;
      Item                       item;
      Fixed                      fixed;
      std::optional<std::string> opt;
      std::uint64_t              n = 0;
   };
   PSIO_REFLECT(Owned, name, nums, raw, item, fixed, opt, n)

   struct Bounded
   {
      std::vector<std::string> names;
   };
   PSIO_REFLECT(Bounded, attr(names, max<2> | element(max<4>)))
   struct BoundedView
   {
      std::span<const std::string_view> names;
   };
   PSIO_REFLECT(BoundedView, attr(names, max<2> | element(max<4>)))

   struct Svc
   {
      void look(psio::pssz_view<const Item>     item,
                std::uint64_t                   n,
                std::string_view                name,
                const std::vector<std::string>& tags)
      {
      }
      psio::pssz_view<Item> echo(psio::pssz_view<Item> item) { return item; }
      PSIO_REFLECT(Svc,
                   method(look, item, n, name, tags, maxDynamicData(1024)),
                   method(echo, item))
   };
}  // namespace srct

using namespace srct;

TEST_CASE("wire_type_t names what a source is encoded as", "[pssz][source]")
{
   STATIC_REQUIRE(std::is_same_v<psio::wire_type_t<std::string_view>, std::string>);
   STATIC_REQUIRE(std::is_same_v<psio::wire_type_t<const char*>, std::string>);
   STATIC_REQUIRE(std::is_same_v<psio::wire_type_t<const char[4]>, std::string>);
   STATIC_REQUIRE(std::is_same_v<psio::wire_type_t<std::span<const Item>>, std::vector<Item>>);
   STATIC_REQUIRE(std::is_same_v<psio::wire_type_t<std::reference_wrapper<const Item>>, Item>);
   STATIC_REQUIRE(std::is_same_v<psio::wire_type_t<psio::pssz_view<Item>>, Item>);
   STATIC_REQUIRE(std::is_same_v<psio::wire_type_t<psio::view<const Item, psio::pssz>>, Item>);
   STATIC_REQUIRE(std::is_same_v<psio::wire_type_t<std::tuple<const std::uint64_t&, std::string&>>,
                                 std::tuple<std::uint64_t, std::string>>);
   STATIC_REQUIRE(
      std::is_same_v<psio::wire_type_t<std::tuple<psio::pssz_view<Item>, std::string_view>>,
                     std::tuple<Item, std::string>>);
   STATIC_REQUIRE(std::is_same_v<psio::wire_type_t<std::vector<psio::pssz_view<Item>>>,
                                 std::vector<Item>>);
   STATIC_REQUIRE(std::is_same_v<psio::wire_type_t<std::optional<std::string_view>>,
                                 std::optional<std::string>>);
   // Everything else is itself.
   STATIC_REQUIRE(std::is_same_v<psio::wire_type_t<Item>, Item>);
   STATIC_REQUIRE(std::is_same_v<psio::wire_type_t<std::tuple<std::uint64_t, std::string>>,
                                 std::tuple<std::uint64_t, std::string>>);
   STATIC_REQUIRE(std::is_same_v<psio::wire_type_t<std::pair<const int, std::string>>,
                                 std::pair<const int, std::string>>);
   STATIC_REQUIRE_FALSE(psio::is_source_v<std::vector<std::string>>);
   STATIC_REQUIRE(psio::is_source_v<std::string_view>);
}

TEST_CASE("to_pssz of a tuple of references (probe g7_tie)", "[pssz][source]")
{
   std::uint64_t     id    = 1;
   std::string       trace = "big";
   Item              it{3, "q"};
   std::vector<Item> items{{1, "a"}, {2, "bb"}};
   std::optional<std::string> none;
   REQUIRE(psio::to_pssz(std::tie(id, trace)) == psio::to_pssz(std::tuple{id, trace}));
   REQUIRE(psio::to_pssz(std::tie(id, trace, it, items)) ==
           psio::to_pssz(std::tuple{id, trace, it, items}));
   REQUIRE(psio::to_pssz(std::forward_as_tuple(id, std::string_view{"abc"}, 5u)) ==
           psio::to_pssz(std::tuple{id, std::string("abc"), 5u}));
   // A trailing absent optional is pruned through references as well.
   REQUIRE(psio::to_pssz(std::tie(id, none)) == psio::to_pssz(std::tuple{id, none}));
   REQUIRE(psio::pssz_size(std::tie(id, trace)) == psio::to_pssz(std::tuple{id, trace}).size());
   // Appending, and the checked encoder.
   std::vector<char> out{char(7)};
   psio::to_pssz(std::tie(id, trace), out);
   REQUIRE(out.size() == 1 + psio::pssz_size(std::tuple{id, trace}));
   std::vector<char> checked;
   REQUIRE(psio::encode_pssz_checked(psio::pssz{}, std::tie(id, trace), checked).ok());
   REQUIRE(checked == psio::to_pssz(std::tuple{id, trace}));
   // The bytes validate and decode as the tuple of values.
   auto bytes = psio::to_pssz(std::tie(id, trace, it));
   auto back  = psio::from_pssz<std::tuple<std::uint64_t, std::string, Item>>(bytes);
   REQUIRE(std::get<2>(back) == it);
   // ... and validate under the source's own type name.
   REQUIRE(psio::validate_pssz<std::tuple<const std::uint64_t&, const std::string&, const Item&>>(
              bytes)
              .ok());
   // Explicit widths.
   REQUIRE(psio::encode(psio::pssz_<2>{}, std::tie(id, trace, items)) ==
           psio::encode(psio::pssz_<2>{}, std::tuple{id, trace, items}));
}

TEST_CASE("to_pssz of string_view, C strings, spans and reference wrappers (probe g7_tie)",
          "[pssz][source]")
{
   REQUIRE(psio::to_pssz(std::string_view{"abc"}) == psio::to_pssz(std::string("abc")));
   REQUIRE(psio::to_pssz(std::string_view{}) == psio::to_pssz(std::string()));
   // A C string: its characters, without the NUL.
   REQUIRE(psio::to_pssz("abc") == psio::to_pssz(std::string("abc")));
   REQUIRE(psio::to_pssz("abc").size() == 3);
   const char* cs = "hello";
   REQUIRE(psio::to_pssz(cs) == psio::to_pssz(std::string("hello")));
   REQUIRE(psio::pssz_size(std::string_view{"abc"}) == 3);
   REQUIRE(psio::pssz_size("abc") == 3);
   // A string with an embedded NUL is a string_view's business.
   REQUIRE(psio::to_pssz(std::string_view{"a\0b", 3}) == psio::to_pssz(std::string("a\0b", 3)));

   std::vector<std::uint32_t> nums{1, 2, 3};
   REQUIRE(psio::to_pssz(std::span<const std::uint32_t>{nums}) == psio::to_pssz(nums));
   REQUIRE(psio::to_pssz(std::span<std::uint32_t>{nums}) == psio::to_pssz(nums));
   REQUIRE(psio::to_pssz(std::span<const std::uint32_t>{}) ==
           psio::to_pssz(std::vector<std::uint32_t>{}));
   std::vector<Item> items{{1, "a"}, {2, "bb"}};
   REQUIRE(psio::to_pssz(std::span<const Item>{items}) == psio::to_pssz(items));
   std::vector<std::string> strs{"a", "bcd"};
   REQUIRE(psio::to_pssz(std::span<const std::string>{strs}) == psio::to_pssz(strs));
   std::vector<std::string_view> views{"a", "bcd"};
   REQUIRE(psio::to_pssz(views) == psio::to_pssz(strs));
   REQUIRE(psio::to_pssz(std::span<const std::string_view>{views}) == psio::to_pssz(strs));
   std::vector<Fixed> fixed{{1, 2}, {3, 4}};
   REQUIRE(psio::to_pssz(std::span<const Fixed>{fixed}) == psio::to_pssz(fixed));
   REQUIRE(psio::pssz_size(std::span<const Item>{items}) == psio::to_pssz(items).size());

   Item it{3, "q"};
   REQUIRE(psio::to_pssz(std::cref(it)) == psio::to_pssz(it));
   REQUIRE(psio::to_pssz(std::ref(it)) == psio::to_pssz(it));
   std::uint32_t n = 9;
   REQUIRE(psio::to_pssz(std::tuple{std::cref(n), std::cref(it)}) ==
           psio::to_pssz(std::tuple{n, it}));
   REQUIRE(psio::to_pssz(std::optional<std::string_view>{"o"}) ==
           psio::to_pssz(std::optional<std::string>{"o"}));
   REQUIRE(psio::to_pssz(std::optional<std::string_view>{}) ==
           psio::to_pssz(std::optional<std::string>{}));
}

TEST_CASE("to_pssz of a view as a value re-emits its bytes (probe p8b E1, E3)", "[pssz][source]")
{
   Item                  it{3, "q"};
   auto                  ib = psio::to_pssz(it);
   psio::pssz_view<Item> iv{ib};
   REQUIRE(psio::to_pssz(iv) == ib);
   REQUIRE(psio::pssz_size(iv) == ib.size());
   REQUIRE(psio::to_pssz(std::tuple{iv, std::uint32_t{4}}) ==
           psio::to_pssz(std::tuple{it, std::uint32_t{4}}));
   REQUIRE(psio::to_pssz(psio::view<const Item, psio::pssz>{ib}) == ib);
   // A list / optional of views.
   std::vector<psio::pssz_view<Item>> vv{iv, iv};
   REQUIRE(psio::to_pssz(vv) == psio::to_pssz(std::vector<Item>{it, it}));
   REQUIRE(psio::to_pssz(std::optional<psio::pssz_view<Item>>{iv}) ==
           psio::to_pssz(std::optional<Item>{it}));
   REQUIRE(psio::to_pssz(std::optional<psio::pssz_view<Item>>{}) ==
           psio::to_pssz(std::optional<Item>{}));
   // Scalar, string and list views.
   auto                 rb = psio::to_pssz(Row{1, {"aa", "bbb"}, it});
   psio::pssz_view<Row> row{rb};
   REQUIRE(psio::to_pssz(row.item().s()) == psio::to_pssz(std::string("q")));
   REQUIRE(psio::to_pssz(std::tuple{row.item().s(), row.id()}) ==
           psio::to_pssz(std::tuple{std::string("q"), std::uint32_t{1}}));
   // A record holding a view member (TxSuccessView): the record's bytes.
   REQUIRE(psio::to_pssz(TraceView{7, 8, iv}) == psio::to_pssz(Trace{7, 8, it}));
   REQUIRE(psio::pssz_size(TraceView{7, 8, iv}) == psio::pssz_size(Trace{7, 8, it}));
   REQUIRE(psio::validate_pssz<TraceView>(psio::to_pssz(Trace{7, 8, it})).ok());
   REQUIRE_FALSE(psio::validate_pssz<TraceView>(std::vector<char>(3, 'x')).ok());
}

TEST_CASE("a view written at a position with other offset widths is re-encoded",
          "[pssz][source][cap]")
{
   Item it{3, "q"};
   Row  r{1, {"aa", "bbb"}, it};
   auto rb = psio::to_pssz(r);
   auto tb = psio::to_pssz(r.tags);   // standalone: 4-byte offsets
   auto ib = psio::to_pssz(it);

   // A standalone list written as a member of a capped record: narrower.
   RowView rv{1, psio::view<std::vector<std::string>, psio::pssz>{tb}, psio::pssz_view<Item>{ib}};
   REQUIRE(psio::to_pssz(rv) == rb);
   REQUIRE(psio::pssz_size(rv) == rb.size());

   // The other direction: a capped member's view written standalone.
   psio::pssz_view<Row> row{rb};
   REQUIRE(psio::bytes(row.tags()).size() != tb.size());
   REQUIRE(psio::to_pssz(row.tags()) == tb);
   REQUIRE(psio::pssz_size(row.tags()) == tb.size());
   REQUIRE(psio::to_pssz(std::tuple{row.tags(), row.item()}) ==
           psio::to_pssz(std::tuple{r.tags, r.item}));
   // ... and written back into the same kind of position: byte for byte.
   RowView same{1, {}, row.item()};
   Row     want{1, {}, it};
   REQUIRE(psio::to_pssz(same) == psio::to_pssz(want));

   // Explicit widths: an automatic-profile view is re-encoded at width 2,
   // a width-2 view is re-emitted there.
   REQUIRE(psio::encode(psio::pssz_<2>{}, std::tuple{psio::pssz_view<Item>{ib}}) ==
           psio::encode(psio::pssz_<2>{}, std::tuple{it}));
   auto i2 = psio::encode(psio::pssz_<2>{}, it);
   REQUIRE(psio::encode(psio::pssz_<2>{}, std::tuple{psio::view<Item, psio::pssz_<2>>{i2}}) ==
           psio::encode(psio::pssz_<2>{}, std::tuple{it}));
   REQUIRE(psio::to_pssz(psio::view<Item, psio::pssz_<2>>{i2}) == ib);
}

TEST_CASE("a record whose members are sources packs as the owning record", "[pssz][source]")
{
   Item                       it{3, "q"};
   Fixed                      fx{5, 6};
   auto                       fb = psio::to_pssz(fx);
   std::vector<std::uint32_t> nums{1, 2, 3};
   std::vector<char>          raw{'x', 'y'};
   std::uint64_t              n = 77;
   Sources sv{"nm", nums, raw, std::cref(it), psio::pssz_view<Fixed>{fb},
              std::string_view{"opt"}, std::cref(n)};
   Owned   ov{"nm", nums, raw, it, fx, "opt", 77};
   REQUIRE(psio::to_pssz(sv) == psio::to_pssz(ov));
   REQUIRE(psio::pssz_size(sv) == psio::pssz_size(ov));
   REQUIRE(psio::validate_pssz<Sources>(psio::to_pssz(ov)).ok());
   REQUIRE(psio::auto_pssz_width_v<Sources> == psio::auto_pssz_width_v<Owned>);
}

TEST_CASE("bounds are enforced on sources as on owning values", "[pssz][source]")
{
   std::vector<std::string_view> two{"ab", "cd"};
   REQUIRE(psio::to_pssz(BoundedView{two}) == psio::to_pssz(Bounded{{"ab", "cd"}}));
   std::vector<char> out;
   std::vector<std::string_view> three{"a", "b", "c"};
   REQUIRE_FALSE(psio::encode_pssz_checked(psio::pssz{}, BoundedView{three}, out).ok());
   std::vector<std::string_view> wide{"abcde"};
   REQUIRE_FALSE(psio::encode_pssz_checked(psio::pssz{}, BoundedView{wide}, out).ok());
   REQUIRE(out.empty());
   // A capped record holding a view: the cap applies.
   std::vector<std::string> big(40, std::string(40, 'x'));
   auto                     bb = psio::to_pssz(big);
   Item                     it{1, "a"};
   auto                     ib = psio::to_pssz(it);
   RowView rv{1, psio::view<std::vector<std::string>, psio::pssz>{bb}, psio::pssz_view<Item>{ib}};
   REQUIRE_FALSE(psio::encode_pssz_checked(psio::pssz{}, rv, out).ok());
}

TEST_CASE("to_pssz_as and method_args_to_pssz: a source encoded as a named type",
          "[pssz][source][method]")
{
   Item                     it{3, "q"};
   auto                     ib = psio::to_pssz(it);
   psio::pssz_view<Item>    iv{ib};
   std::vector<std::string> tags{"a", "bb"};
   using Args = psio::reflect<Svc>::method_arg_tuple<0>;
   const auto want = psio::to_pssz(Args{it, std::uint64_t{5}, std::string("nm"), tags});
   // The cap makes these other bytes than the plain tuple's.
   REQUIRE(want != psio::to_pssz(std::tuple{it, std::uint64_t{5}, std::string("nm"), tags}));

   // Arguments as given: a view, a literal that converts, a C string, a vector.
   REQUIRE((psio::method_args_to_pssz<Svc, 0>(iv, 5, "nm", tags) == want));
   REQUIRE((psio::method_args_to_pssz<Svc, 0>(it, std::uint64_t{5}, std::string_view{"nm"},
                                              std::span<const std::string>{tags}) == want));
   std::vector<char> out{char(1)};
   psio::method_args_append_pssz<Svc, 0>(out, iv, 5, "nm", tags);
   REQUIRE(out.size() == 1 + want.size());
   REQUIRE((psio::to_pssz_as<Args>(std::forward_as_tuple(iv, std::uint64_t{5},
                                                         std::string_view{"nm"}, tags)) == want));
   REQUIRE((psio::pssz_size_as<Args>(std::forward_as_tuple(iv, std::uint64_t{5},
                                                           std::string_view{"nm"}, tags)) ==
            want.size()));
   REQUIRE(psio::validate_method_args<Svc, 0>(want).ok());

   // A method that returns a view: the result is written as the value.
   Svc               svc;
   std::vector<char> result;
   auto              eb = psio::method_args_to_pssz<Svc, 1>(iv);
   REQUIRE((psio::call_method_pssz<Svc, 1>(svc, psio::trusted{eb}, result).ok()));
   REQUIRE(result == ib);
   Item back;
   REQUIRE((psio::decode_method_result<Svc, 1>(result, back).ok()));
   REQUIRE(back == it);
}
