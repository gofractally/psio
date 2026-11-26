// View-typed method parameters and calling a reflected method with packed
// arguments (probes g8_view_param, p8, p8b, g8_capped_args_view,
// g8_capped_args_wrongview).
//
//   reflect<T>::method_arg_tuple<I>   the WIRE type of the arguments: a
//                                     parameter declared psio::pssz_view<const X>
//                                     is a packed X
//   reflect<T>::method_params<I>      how the method takes them
//   psio::invoke_method_pssz<T, I>    calls the method with each parameter as
//                                     declared: views into the bytes, values
//                                     unpacked

#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <catch.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace mvp
{
   struct Item
   {
      std::uint32_t a = 0;
      std::string   s;
      friend bool   operator==(const Item&, const Item&) = default;
   };
   PSIO_REFLECT(Item, a, s)

   struct Svc
   {
      std::uint32_t last = 0;
      std::string   text;

      void put(std::uint64_t id, const std::string& name, std::vector<Item> items)
      {
         last = static_cast<std::uint32_t>(id + name.size() + items.size());
      }
      void look(psio::pssz_view<const Item> item, std::uint32_t n) { last = item.a() + n; }
      std::string get(std::uint64_t id) { return "r" + std::to_string(id); }
      // Every kind of parameter at once, under a cap.
      std::uint32_t mixed(psio::pssz_view<Item>             item,
                          std::string_view                   name,
                          std::span<const char>              blob,
                          const std::vector<std::string>&    tags,
                          std::uint16_t                      n,
                          psio::view<const Item, psio::pssz> other)
      {
         text = std::string(name) + ":" + std::string(item.s().view_()) + ":" +
                std::string(other.s().view_());
         for (const auto& t : tags)
            text += ":" + t;
         last = item.a() + other.a() + static_cast<std::uint32_t>(blob.size()) + n;
         return last;
      }
      std::vector<std::string> names(std::uint32_t count)
      {
         return std::vector<std::string>(count, "n");
      }
      static std::uint32_t twice(psio::pssz_view<Item> item) { return item.a() * 2; }
      std::uint32_t        peek(psio::pssz_view<Item> item) const { return item.a() + last; }

      PSIO_REFLECT(Svc,
                   method(put, id, name, items),
                   method(look, item, n),
                   method(get, id),
                   method(mixed, item, name, blob, tags, n, other, maxDynamicData(1024)),
                   method(names, count, returns(max<4> | element(max<16>))),
                   method(twice, item),
                   method(peek, item))
   };
   using R = psio::reflect<Svc>;

   struct Capped
   {
      std::uint32_t seen = 0;
      void put(std::uint64_t id, std::string name, std::vector<std::string> tags)
      {
         seen = static_cast<std::uint32_t>(id + name.size() + tags.size());
      }
      PSIO_REFLECT(Capped, method(put, id, name, tags, maxDynamicData(1024)))
   };
}  // namespace mvp

using namespace mvp;

TEST_CASE("method_arg_tuple describes the wire type of a view parameter", "[method][view_param]")
{
   // look(psio::pssz_view<const Item>, std::uint32_t)
   using Args = R::method_arg_tuple<1>;
   STATIC_REQUIRE(std::is_same_v<Args::tuple_type, std::tuple<Item, std::uint32_t>>);
   // How the method takes them is unchanged.
   STATIC_REQUIRE(std::is_same_v<R::method_params<1>,
                                 std::tuple<psio::pssz_view<Item>, std::uint32_t>>);
   // mixed(view, string_view, span<const char>, const vector<string>&, u16, view<const>)
   STATIC_REQUIRE(std::is_same_v<R::method_arg_tuple<3>::tuple_type,
                                 std::tuple<Item, std::string, std::vector<char>,
                                            std::vector<std::string>, std::uint16_t, Item>>);
   // No view: as before.
   STATIC_REQUIRE(std::is_same_v<R::method_arg_tuple<0>::tuple_type,
                                 std::tuple<std::uint64_t, std::string, std::vector<Item>>>);
}

TEST_CASE("validate / decode of the argument tuple of a view-parameter method "
          "(probes g8_view_param, p8b)",
          "[method][view_param]")
{
   using Args = R::method_arg_tuple<1>;
   auto good  = psio::to_pssz(std::tuple{Item{3, "q"}, std::uint32_t{4}});
   // This compiled and returned false before: valid bytes were rejected.
   REQUIRE(psio::validate_pssz<Args>(good).ok());
   REQUIRE(psio::validate_method_args<Svc, 1>(good).ok());
   std::vector<char> junk(3, 'x');
   REQUIRE_FALSE(psio::validate_pssz<Args>(junk).ok());
   REQUIRE_FALSE(psio::validate_pssz<Args>(std::span<const char>{}).ok());
   REQUIRE_FALSE(psio::validate_method_args<Svc, 1>(junk).ok());

   // Decoding gives the owned wire values.
   auto args = psio::from_pssz<Args>(good);
   REQUIRE(std::get<0>(args) == Item{3, "q"});
   REQUIRE(std::get<1>(args) == 4);
   // Encoding the wire tuple is the same bytes.
   REQUIRE(psio::to_pssz(Args{Item{3, "q"}, std::uint32_t{4}}) == good);

   // A tuple type that names the view directly is validated as its wire type.
   using Direct = std::tuple<psio::pssz_view<Item>, std::uint32_t>;
   REQUIRE(psio::validate_pssz<Direct>(good).ok());
   REQUIRE_FALSE(psio::validate_pssz<Direct>(junk).ok());
}

TEST_CASE("invoke_method_pssz: views for view parameters, values for the rest",
          "[method][view_param]")
{
   Svc svc;
   // put: values only.
   auto b0 = psio::to_pssz(
      R::method_arg_tuple<0>{std::uint64_t{5}, std::string("nm"), std::vector<Item>{{1, "x"}}});
   psio::invoke_method_pssz<Svc, 0>(svc, b0);
   REQUIRE(svc.last == 5 + 2 + 1);

   // look: a view and a value.
   auto b1 = psio::to_pssz(std::tuple{Item{3, "q"}, std::uint32_t{4}});
   psio::invoke_method_pssz<Svc, 1>(svc, std::span<const char>{b1});
   REQUIRE(svc.last == 7);
   svc.last = 0;
   psio::invoke_method_pssz<Svc, 1>(svc, psio::trusted{b1});
   REQUIRE(svc.last == 7);

   // The view the method receives points into the caller's bytes.
   auto params = psio::method_params_from_pssz<Svc, 1>(psio::trusted{b1});
   auto item   = std::get<0>(params);
   REQUIRE(psio::bytes(item).data() >= b1.data());
   REQUIRE(psio::bytes(item).data() + psio::bytes(item).size() <= b1.data() + b1.size());
   REQUIRE(item.unpack() == Item{3, "q"});

   // get: a result.
   auto        b2 = psio::to_pssz(R::method_arg_tuple<2>{std::uint64_t{9}});
   std::string r  = psio::invoke_method_pssz<Svc, 2>(svc, b2);
   REQUIRE(r == "r9");

   // A static and a const method.
   auto b5 = psio::to_pssz(std::tuple{Item{21, "s"}});
   REQUIRE(psio::invoke_method_pssz<Svc, 5>(svc, b5) == 42);
   const Svc csvc{.last = 1};
   REQUIRE(psio::invoke_method_pssz<Svc, 6>(csvc, b5) == 22);

   // reflect<T>::invoke_method takes the arguments as declared; it and the
   // packed path agree.
   svc.last = 0;
   R::invoke_method<1>(svc, psio::method_params_from_pssz<Svc, 1>(psio::trusted{b1}));
   REQUIRE(svc.last == 7);
}

TEST_CASE("invoke_method_pssz under a cap: mixed views, borrowed strings and values",
          "[method][view_param][cap]")
{
   using Args = R::method_arg_tuple<3>;
   Args a{Item{1, "it"}, std::string("nm"), std::vector<char>{'a', 'b', 'c'},
          std::vector<std::string>{"t1", "t22"}, std::uint16_t{10}, Item{2, "ot"}};
   auto b = psio::to_pssz(a);
   // The cap narrows the tuple's offsets: not the plain tuple's bytes.
   REQUIRE(b != psio::to_pssz(static_cast<const Args::tuple_type&>(a)));
   REQUIRE(psio::validate_method_args<Svc, 3>(b).ok());

   Svc           svc;
   std::uint32_t r = psio::invoke_method_pssz<Svc, 3>(svc, b);
   REQUIRE(r == 1 + 2 + 3 + 10);
   REQUIRE(svc.text == "nm:it:ot:t1:t22");

   // What the method was handed borrows the argument bytes.
   auto params = psio::method_params_from_pssz<Svc, 3>(psio::trusted{b});
   auto name   = std::get<1>(params);
   auto blob   = std::get<2>(params);
   STATIC_REQUIRE(std::is_same_v<decltype(name), std::string_view>);
   STATIC_REQUIRE(std::is_same_v<decltype(blob), std::span<const char>>);
   REQUIRE(name.data() >= b.data());
   REQUIRE(name.data() < b.data() + b.size());
   REQUIRE(blob.data() >= b.data());
   REQUIRE(blob.size() == 3);
   REQUIRE(std::get<3>(params) == std::vector<std::string>{"t1", "t22"});

   // Invalid bytes do not reach the method.
#if defined(__cpp_exceptions)
   std::vector<char> junk(5, 'x');
   svc.last = 0;
   REQUIRE_THROWS_AS((psio::invoke_method_pssz<Svc, 3>(svc, junk)), psio::codec_exception);
   REQUIRE(svc.last == 0);
   // Over the method's cap.
   Args big = a;
   std::get<1>(big).assign(2000, 'z');
   std::vector<char> over;
   REQUIRE_FALSE(psio::encode_pssz_checked(psio::pssz{}, big, over).ok());
#endif
}

TEST_CASE("a view over the annotated argument tuple (probes g8_capped_args_view, "
          "g8_capped_args_wrongview)",
          "[method][view_param][cap]")
{
   using Args = psio::reflect<Capped>::method_arg_tuple<0>;
   auto  b    = psio::to_pssz(
      Args{std::uint64_t{5}, std::string("nm"), std::vector<std::string>{"t", "uu"}});
   psio::pssz_view<Args> v{b};
   REQUIRE(v.get<0>() == 5);
   REQUIRE(v.get<1>().view_() == "nm");
   REQUIRE(v.get<2>().size() == 2);
   REQUIRE(v.get<2>()[1].view_() == "uu");
   REQUIRE(v.get<2>().unpack() == std::vector<std::string>{"t", "uu"});
   REQUIRE(v.unpack() == Args{std::uint64_t{5}, std::string("nm"),
                              std::vector<std::string>{"t", "uu"}});
   // The same view under its method name, and structured bindings.
   psio::method_args_view<Capped, 0> mv{b};
   auto [id, name, tags] = mv;
   REQUIRE(id == 5);
   REQUIRE(name == "nm");
   REQUIRE(tags.size() == 2);

   // Why the plain-tuple view is wrong here: other offset widths.
   psio::pssz_view<std::tuple<std::uint64_t, std::string, std::vector<std::string>>> plain{b};
   // (These bytes are not a frame of it: its header does not fit them.)
   REQUIRE_THROWS_AS(plain.get<1>(), psio::codec_exception);

   Capped c;
   psio::invoke_method_pssz<Capped, 0>(c, b);
   REQUIRE(c.seen == 5 + 2 + 2);
}

TEST_CASE("call_method_pssz appends the result as returns(...) describes it",
          "[method][view_param]")
{
   Svc               svc;
   std::vector<char> out;
   // get: a plain result.
   auto b2 = psio::to_pssz(R::method_arg_tuple<2>{std::uint64_t{9}});
   REQUIRE(psio::call_method_pssz<Svc, 2>(svc, psio::trusted{b2}, out).ok());
   REQUIRE(out == psio::to_pssz(std::string("r9")));
   // look: void, nothing appended.
   out.clear();
   auto b1 = psio::to_pssz(std::tuple{Item{3, "q"}, std::uint32_t{4}});
   REQUIRE(psio::call_method_pssz<Svc, 1>(svc, psio::trusted{b1}, out).ok());
   REQUIRE(out.empty());
   REQUIRE(svc.last == 7);
   // names: returns(max<4> | element(max<16>)) — a bounded list, 1-byte offsets.
   auto b4 = psio::to_pssz(R::method_arg_tuple<4>{std::uint32_t{2}});
   REQUIRE(psio::call_method_pssz<Svc, 4>(svc, psio::trusted{b4}, out).ok());
   REQUIRE(out.size() < psio::to_pssz(std::vector<std::string>{"n", "n"}).size());
   REQUIRE(psio::validate_method_result<Svc, 4>(out).ok());
   std::vector<std::string> back;
   REQUIRE(psio::decode_method_result<Svc, 4>(out, back).ok());
   REQUIRE(back == std::vector<std::string>{"n", "n"});
   // Outside returns(...): reported, nothing appended.
   out.clear();
   auto b4big = psio::to_pssz(R::method_arg_tuple<4>{std::uint32_t{5}});
   REQUIRE_FALSE(psio::call_method_pssz_checked<Svc, 4>(svc, psio::trusted{b4big}, out).ok());
   REQUIRE(out.empty());
   // call_method_pssz writes the callee's own value fast: the caller's
   // validating read is what refuses it.
   REQUIRE(psio::call_method_pssz<Svc, 4>(svc, psio::trusted{b4big}, out).ok());
   REQUIRE_FALSE(psio::validate_method_result<Svc, 4>(out).ok());
}
