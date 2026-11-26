// Validation and decode accept the SAME set.
//
// A caller may validate bytes and then read them through a view (or decode
// them as trusted), so everything a checked decode refuses must be refused
// by the validator of the same name, and the other way round. This file runs
// every validating entry point and every checked decode of the helper
// families on the same inputs and requires one answer:
//
//   pssz        validate_pssz, validate_strict, from_pssz, decode_pssz_checked,
//               validated_pssz_view, pssz_buffer::checked, shared_pssz{bytes},
//               the explicit-width profile (pssz32)
//   json/ijson  validate_json / from_json, validate_ijson / from_ijson
//   key         validate_key / from_key
//   a method's  validate_method_result, decode_method_result,
//   result      method_result_from_pssz, validated_method_result_view,
//               method_result_buffer::checked, shared_method_result{bytes}
//   a method's  validate_method_args, from_pssz<Args>, invoke_method_pssz,
//   arguments   method_args_buffer::checked, validate_json / from_json<Args>
//
// The inputs are valid values, values that violate a value-side constraint
// (`sorted` / `unique` / `unique_keys` at a member, below it, in returns(...)
// and on a parameter; a length bound; utf8; an invariant) written by a twin
// type without the constraint, and — for the binary formats — every
// truncation and a sweep of single-byte mutations of the valid frames.

#include <catch.hpp>

#include <psio/buffer.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/nested.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace vdp
{
   using U32s    = std::vector<std::uint32_t>;
   using Strs    = std::vector<std::string>;
   using Entries = std::vector<std::pair<std::string, std::uint32_t>>;

   struct Ranked
   {
      friend bool          operator==(const Ranked&, const Ranked&) = default;
      U32s                 scores;
      Strs                 names;
      Entries              entries;
      std::vector<U32s>    rows;
      std::optional<U32s>  maybe;
      PSIO_REFLECT(Ranked,
                   attr(scores, sorted | max<4>),
                   attr(names, unique | element(utf8 | max<8>)),
                   attr(entries, unique_keys),
                   attr(rows, element(sorted)),
                   attr(maybe, sorted))
   };
   // The same members without the constraints: writes what Ranked refuses.
   struct Unranked
   {
      U32s                scores;
      Strs                names;
      Entries             entries;
      std::vector<U32s>   rows;
      std::optional<U32s> maybe;
      PSIO_REFLECT(Unranked, scores, names, entries, rows, maybe)
   };

   struct Even
   {
      friend bool          operator==(const Even&, const Even&) = default;
      std::uint32_t        n = 0;
      std::string          note;
      static psio::codec_status check(const Even& e) noexcept
      {
         return e.n % 2 == 0 ? psio::codec_ok() : psio::codec_fail("odd", 0, "Even");
      }
      PSIO_REFLECT(Even, n, note, invariant(check))
   };
   struct AnyNumber
   {
      std::uint32_t n = 0;
      std::string   note;
      PSIO_REFLECT(AnyNumber, n, note)
   };

   template <typename R>
   struct Holder
   {
      friend bool   operator==(const Holder&, const Holder&) = default;
      std::uint32_t id = 0;
      R             inner;
      PSIO_REFLECT(Holder, id, inner)
   };

   const Ranked   ok_ranked{{1, 1, 2}, {"a", "ab", "b"}, {{"x", 1}, {"a", 1}}, {{1, 2}, {}}, U32s{4, 9}};
   const Unranked ok_twin{ok_ranked.scores, ok_ranked.names, ok_ranked.entries, ok_ranked.rows,
                          ok_ranked.maybe};
   // One violation each.
   inline std::vector<std::pair<const char*, Unranked>> violations()
   {
      std::vector<std::pair<const char*, Unranked>> out;
      auto add = [&](const char* what, auto&& change) {
         Unranked u = ok_twin;
         change(u);
         out.emplace_back(what, std::move(u));
      };
      add("sorted at the member", [](Unranked& u) { u.scores = {3, 1, 2}; });
      add("max at the member", [](Unranked& u) { u.scores = {1, 2, 3, 4, 5}; });
      add("unique at the member", [](Unranked& u) { u.names = {"a", "a"}; });
      add("unique: out of order", [](Unranked& u) { u.names = {"b", "a"}; });
      add("utf8 below the member", [](Unranked& u) { u.names = {"\xff"}; });
      add("max below the member", [](Unranked& u) { u.names = {"123456789"}; });
      add("unique_keys", [](Unranked& u) { u.entries = {{"a", 1}, {"b", 2}, {"a", 3}}; });
      add("sorted below the member", [](Unranked& u) { u.rows = {{1, 2}, {2, 1}}; });
      add("sorted in an optional", [](Unranked& u) { u.maybe = U32s{2, 1}; });
      return out;
   }

   // ── The methods ──────────────────────────────────────────────────────
   struct Svc
   {
      int   calls = 0;
      U32s                 sorted_list() const { return {}; }
      Strs                 unique_narrow() const { return {}; }
      Entries              keyed() const { return {}; }
      std::vector<U32s>    rows() const { return {}; }
      std::optional<U32s>  maybe() const { return {}; }
      std::optional<Strs>  maybe_narrow() const { return {}; }
      Ranked               record() const { return {}; }
      std::string          text() const { return {}; }
      void take(U32s a, std::vector<U32s> b, Strs c, Entries d)
      {
         (void)a, (void)b, (void)c, (void)d;
         ++calls;
      }
      void take_capped(U32s a, Strs c)
      {
         (void)a, (void)c;
         ++calls;
      }
      PSIO_REFLECT(Svc,
                   method(sorted_list, returns(sorted)),
                   method(unique_narrow, returns(unique | max<4> | element(utf8 | max<8>))),
                   method(keyed, returns(unique_keys)),
                   method(rows, returns(element(sorted))),
                   method(maybe, returns(sorted)),
                   method(maybe_narrow, returns(max<4> | unique | element(max<8>))),
                   method(record),
                   method(text, returns(utf8 | max<4>)),
                   method(take, attr(a, sorted), attr(b, element(unique)),
                          attr(c, unique | max<4> | element(max<8>)), attr(d, unique_keys)),
                   method(take_capped, attr(a, sorted), attr(c, unique), maxDynamicData(200)))
   };

   // ── The entry points ─────────────────────────────────────────────────
   template <typename F>
   bool no_throw(F&& f)
   {
      try
      {
         f();
         return true;
      }
      catch (const psio::codec_exception&)
      {
         return false;
      }
   }

   // Every pssz validating entry point and checked decode of T on `bytes`:
   // they give one answer, which is returned.
   template <typename T>
   bool pssz_all_agree(std::span<const char> bytes)
   {
      const bool valid = psio::validate_pssz<T>(bytes).ok();
      CHECK(no_throw([&] { (void)psio::from_pssz<T>(bytes); }) == valid);
      {
         T    out{};
         bool unknown = false;
         CHECK(psio::decode_pssz_checked(psio::pssz{}, bytes, out, unknown).ok() == valid);
      }
      CHECK(psio::validated_pssz_view<T>(bytes).ok() == valid);
      {
         psio::pssz_buffer<T> out;
         CHECK(psio::pssz_buffer<T>::checked(bytes, out).ok() == valid);
         if (valid)
         {
            T decoded{};
            CHECK(out.decode_checked(decoded).ok());
         }
      }
      CHECK(no_throw([&] { (void)psio::shared_pssz<T>{bytes}; }) == valid);
      // Strict accepts a subset.
      if (psio::validate_strict<T>(psio::pssz{}, bytes).ok())
         CHECK(valid);
      // What is accepted decodes to a value that has an encoding (the
      // writer refuses no value a reader accepts).
      if (valid)
         CHECK(no_throw([&] { (void)psio::to_pssz(psio::from_pssz<T>(psio::trusted{bytes})); }));
      return valid;
   }

   // Truncations and single-byte mutations of a valid frame.
   template <typename Check>
   void sweep(const std::vector<char>& valid, Check&& check)
   {
      for (std::size_t n = 0; n < valid.size(); ++n)
         check(std::span<const char>{valid.data(), n});
      for (std::size_t i = 0; i < valid.size(); ++i)
         for (unsigned char delta : {1, 2, 0x80, 0xff})
         {
            auto mutated = valid;
            mutated[i]   = static_cast<char>(static_cast<unsigned char>(mutated[i]) ^ delta);
            check(std::span<const char>{mutated});
         }
   }

   template <typename T, typename Twin>
   void pssz_case(const Twin& good, const std::vector<std::pair<const char*, Twin>>& bad)
   {
      const auto valid = psio::to_pssz(good);
      CHECK(pssz_all_agree<T>(valid));
      CHECK(psio::validate_strict<T>(psio::pssz{}, valid).ok());
      for (const auto& [what, value] : bad)
      {
         INFO(what);
         CHECK_FALSE(pssz_all_agree<T>(psio::to_pssz(value)));
         CHECK_FALSE(psio::validate_strict<T>(psio::pssz{}, psio::to_pssz(value)).ok());
      }
      sweep(valid, [](std::span<const char> bytes) { (void)pssz_all_agree<T>(bytes); });
   }

   // The same twin wrapped: Wrap<Ranked> is checked against Wrap<Unranked>.
   template <template <typename> class Wrap>
   void pssz_wrapped()
   {
      std::vector<std::pair<const char*, Wrap<Unranked>>> bad;
      for (auto& [what, value] : violations())
         bad.emplace_back(what, Wrap<Unranked>{value});
      pssz_case<decltype(Wrap<Ranked>{ok_ranked}.value), decltype(Wrap<Unranked>{ok_twin}.value)>(
         Wrap<Unranked>{ok_twin}.value, [&] {
            std::vector<std::pair<const char*, decltype(Wrap<Unranked>{ok_twin}.value)>> out;
            for (auto& [what, value] : bad)
               out.emplace_back(what, value.value);
            return out;
         }());
   }
   template <typename R>
   struct InVector
   {
      std::vector<R> value;
      explicit InVector(const R& r) : value{R{}, r} {}
   };
   template <typename R>
   struct InOptional
   {
      std::optional<R> value;
      explicit InOptional(const R& r) : value{r} {}
   };
   template <typename R>
   struct InVariant
   {
      std::variant<std::uint32_t, R> value;
      explicit InVariant(const R& r) : value{r} {}
   };
   template <typename R>
   struct InTuple
   {
      std::tuple<std::uint32_t, R, std::string> value;
      explicit InTuple(const R& r) : value{7, r, "x"} {}
   };
   template <typename R>
   struct InMap
   {
      std::map<std::string, R> value;
      explicit InMap(const R& r) : value{{"k", r}} {}
   };
   template <typename R>
   struct InRecord
   {
      Holder<R> value;
      explicit InRecord(const R& r) : value{3, r} {}
   };
   template <typename R>
   struct InNested
   {
      Holder<psio::nested<R>> value;
      explicit InNested(const R& r) : value{3, psio::nested<R>::from(r)} {}
   };
   template <typename R>
   struct InShared
   {
      Holder<psio::shared_pssz<R>> value;
      explicit InShared(const R& r) : value{3, psio::shared_pssz<R>::from(r)} {}
   };

   // ── A method's result ────────────────────────────────────────────────
   template <std::size_t I>
   bool result_all_agree(std::span<const char> bytes)
   {
      using R          = psio::method_result_t<Svc, I>;
      const bool valid = psio::validate_method_result<Svc, I>(bytes).ok();
      {
         R out{};
         CHECK(psio::decode_method_result<Svc, I>(bytes, out).ok() == valid);
      }
      CHECK(no_throw([&] { (void)psio::method_result_from_pssz<Svc, I>(bytes); }) == valid);
      CHECK(psio::validated_method_result_view<Svc, I>(bytes).ok() == valid);
      {
         psio::method_result_buffer<Svc, I> out;
         CHECK(psio::method_result_buffer<Svc, I>::checked(bytes, out).ok() == valid);
         if (valid)
         {
            R decoded{};
            CHECK(out.decode_checked(decoded).ok());
            CHECK(out.validate().ok());
         }
      }
      CHECK(no_throw([&] { (void)psio::shared_method_result<Svc, I>{bytes}; }) == valid);
      if (valid)
      {
         // Accepted bytes are the encoding of the value they decode to.
         const R           value = psio::method_result_from_pssz<Svc, I>(psio::trusted{bytes});
         std::vector<char> again;
         CHECK(psio::encode_method_result<Svc, I>(value, again).ok());
         CHECK(psio::check_method_result<Svc, I>(value).ok());
      }
      return valid;
   }
   template <std::size_t I>
   void result_case(const psio::method_result_t<Svc, I>&              good,
                    std::vector<psio::method_result_t<Svc, I>> const& bad)
   {
      namespace pz     = psio::detail::pssz_impl;
      const auto valid = psio::method_result_to_pssz<Svc, I>(good);
      CHECK(valid == pz::encode_method_result_unchecked<Svc, I>(good));
      CHECK(result_all_agree<I>(valid));
      for (const auto& value : bad)
      {
         std::vector<char> none;
         CHECK_FALSE(psio::encode_method_result_checked<Svc, I>(value, none).ok());
         CHECK(none.empty());
         // The fast write holds nothing against returns(...): the frame a
         // validating reader refuses.
         REQUIRE(psio::encode_method_result<Svc, I>(value, none).ok());
         CHECK(none == pz::encode_method_result_unchecked<Svc, I>(value));
         CHECK_FALSE(psio::validate_method_result<Svc, I>(none).ok());
         // (check_method_result is the check of returns(...) alone.)
         if (std::tuple_size_v<std::remove_cvref_t<
                decltype(psio::method_result_annotations<Svc, I>)>> != 0)
            CHECK_FALSE(psio::check_method_result<Svc, I>(value).ok());
         CHECK_FALSE(result_all_agree<I>(pz::encode_method_result_unchecked<Svc, I>(value)));
      }
      sweep(valid, [](std::span<const char> bytes) { (void)result_all_agree<I>(bytes); });
   }

   // ── A method's arguments ─────────────────────────────────────────────
   template <std::size_t I>
   bool args_all_agree(std::span<const char> bytes)
   {
      using Args       = psio::reflect<Svc>::method_arg_tuple<I>;
      const bool valid = psio::validate_method_args<Svc, I>(bytes).ok();
      CHECK(psio::validate_pssz<Args>(bytes).ok() == valid);
      CHECK(no_throw([&] { (void)psio::from_pssz<Args>(bytes); }) == valid);
      CHECK(psio::validated_pssz_view<Args>(bytes).ok() == valid);
      {
         psio::method_args_buffer<Svc, I> out;
         CHECK(psio::method_args_buffer<Svc, I>::checked(bytes, out).ok() == valid);
      }
      CHECK(no_throw([&] { (void)psio::shared_method_args<Svc, I>{bytes}; }) == valid);
      Svc svc;
      CHECK(no_throw([&] { psio::invoke_method_pssz<Svc, I>(svc, bytes); }) == valid);
      // The method runs exactly when the arguments were accepted.
      CHECK(svc.calls == (valid ? 1 : 0));
      return valid;
   }
}  // namespace vdp

using namespace vdp;

TEST_CASE("parity: pssz validators and checked decodes agree on a record", "[parity][pssz]")
{
   pssz_case<Ranked, Unranked>(ok_twin, violations());
}

TEST_CASE("parity: ... wherever the record sits", "[parity][pssz]")
{
   pssz_wrapped<InVector>();
   pssz_wrapped<InOptional>();
   pssz_wrapped<InVariant>();
   pssz_wrapped<InTuple>();
   pssz_wrapped<InMap>();
   pssz_wrapped<InRecord>();
   pssz_wrapped<InNested>();
   pssz_wrapped<InShared>();
}

TEST_CASE("parity: pssz and an invariant", "[parity][pssz]")
{
   pssz_case<Even, AnyNumber>(AnyNumber{2, "two"}, {{"odd", AnyNumber{3, "three"}}});
   pssz_case<std::vector<Even>, std::vector<AnyNumber>>(
      {AnyNumber{2, "two"}}, {{"odd", {AnyNumber{2, ""}, AnyNumber{3, "three"}}}});
   pssz_case<Holder<psio::nested<Even>>, Holder<psio::nested<AnyNumber>>>(
      {1, psio::nested<AnyNumber>::from(AnyNumber{2, "two"})},
      {{"odd", {1, psio::nested<AnyNumber>::from(AnyNumber{3, "three"})}}});
}

TEST_CASE("parity: the explicit-width profile", "[parity][pssz]")
{
   auto agree = [](std::span<const char> bytes) {
      const bool valid = psio::validate<Ranked>(psio::pssz32{}, bytes).ok();
      Ranked     out{};
      bool       unknown = false;
      CHECK(psio::decode_pssz_checked(psio::pssz32{}, bytes, out, unknown).ok() == valid);
      CHECK(psio::validated_view<Ranked>(psio::pssz32{}, bytes).ok() == valid);
      if (psio::validate_strict<Ranked>(psio::pssz32{}, bytes).ok())
         CHECK(valid);
      return valid;
   };
   const auto valid = psio::encode(psio::pssz32{}, ok_twin);
   CHECK(agree(valid));
   for (const auto& [what, value] : violations())
   {
      INFO(what);
      CHECK_FALSE(agree(psio::encode(psio::pssz32{}, value)));
   }
   sweep(valid, [&](std::span<const char> bytes) { (void)agree(bytes); });
}

TEST_CASE("parity: json and ijson validators and decodes agree", "[parity][json]")
{
   auto agree = [](const std::string& text) {
      const bool valid = psio::validate_json<Ranked>(text).ok();
      CHECK(no_throw([&] { (void)psio::from_json<Ranked>(text); }) == valid);
      CHECK(psio::validate_ijson<Ranked>(text).ok() == valid);
      CHECK(no_throw([&] { (void)psio::from_ijson<Ranked>(text); }) == valid);
      CHECK(psio::validate_json<Holder<Ranked>>(R"({"id":1,"inner":)" + text + "}").ok() == valid);
      CHECK(no_throw([&] {
               (void)psio::from_json<Holder<Ranked>>(R"({"id":1,"inner":)" + text + "}");
            }) == valid);
      if (!text.empty())   // ("[]" is a list)
      {
         CHECK(psio::validate_json<std::vector<Ranked>>("[" + text + "]").ok() == valid);
         CHECK(no_throw([&] { (void)psio::from_json<std::vector<Ranked>>("[" + text + "]"); }) ==
               valid);
      }
      return valid;
   };
   const auto valid = psio::to_json(ok_twin);
   CHECK(valid == psio::to_json(ok_ranked));
   CHECK(agree(valid));
   for (const auto& [what, value] : violations())
   {
      INFO(what);
      if (std::string_view{what} == "utf8 below the member")
         // (No JSON writer emits text that is not UTF-8: spelled by hand.)
         CHECK_FALSE(agree("{\"scores\":[],\"names\":[\"\xff\"],\"entries\":[],\"rows\":[],"
                           "\"maybe\":null}"));
      else
         CHECK_FALSE(agree(psio::to_json(value)));
   }
   // Malformed text: every truncation.
   for (std::size_t n = 0; n < valid.size(); ++n)
      CHECK_FALSE(agree(valid.substr(0, n)));
}

TEST_CASE("parity: key validators and decodes agree", "[parity][key]")
{
   auto agree = [](std::span<const char> bytes) {
      const bool valid = psio::validate_key<Ranked>(bytes).ok();
      CHECK(no_throw([&] { (void)psio::from_key<Ranked>(bytes); }) == valid);
      return valid;
   };
   const auto valid = psio::to_key(ok_twin);
   CHECK(valid == psio::to_key(ok_ranked));
   CHECK(agree(valid));
   for (const auto& [what, value] : violations())
   {
      INFO(what);
      CHECK_FALSE(agree(psio::to_key(value)));
   }
   sweep(valid, [&](std::span<const char> bytes) { (void)agree(bytes); });
}

TEST_CASE("parity: a method's result — validate, decode, view and buffer agree",
          "[parity][method]")
{
   // returns(sorted): changes no byte, so nothing but the value check
   // tells a result from a plain list.
   result_case<0>({1, 1, 2}, {{2, 1}});
   // returns(unique | max<4> | element(utf8 | max<8>)): a 1-byte table.
   result_case<1>({"a", "b"}, {{"a", "a"}, {"b", "a"}, {"a", "b", "c", "d", "e"}, {"\xff"},
                               {"123456789"}});
   result_case<2>({{"a", 1}, {"b", 1}}, {{{"a", 1}, {"a", 2}}});
   result_case<3>({{1, 2}, {}, {3}}, {{{1, 2}, {2, 1}}});
   result_case<4>(U32s{1, 2}, {U32s{2, 1}});
   result_case<4>(std::nullopt, {});
   result_case<5>(Strs{"a", "b"}, {Strs{"b", "a"}, Strs{"a", "a"}, Strs(5, "x")});
   // No returns(...): the record's own member constraints.
   result_case<6>(ok_ranked, [] {
      std::vector<Ranked> bad;
      for (auto& [what, u] : violations())
         bad.push_back(Ranked{u.scores, u.names, u.entries, u.rows, u.maybe});
      return bad;
   }());
   result_case<7>("abcd", {"abcde", "\xff"});
}

TEST_CASE("parity: a method's result — the view after validation reads a valid result",
          "[parity][method]")
{
   namespace pz = psio::detail::pssz_impl;
   // The reported case: validate, then read through the view.
   const auto bad = pz::encode_method_result_unchecked<Svc, 1>(Strs{"b", "a"});
   CHECK_FALSE(psio::validate_method_result<Svc, 1>(bad).ok());
   CHECK_FALSE(psio::validated_method_result_view<Svc, 1>(bad).ok());
   const auto good = psio::method_result_to_pssz<Svc, 1>(Strs{"a", "b"});
   const auto view = psio::validated_method_result_view<Svc, 1>(good);
   REQUIRE(view.ok());
   CHECK(view->unpack() == Strs{"a", "b"});
   CHECK(psio::validate_method_result<Svc, 0>(psio::to_pssz(U32s{2, 1})).error().what ==
         std::string_view{"sorted: elements are not in order"});
}

TEST_CASE("parity: a method's arguments — validate, decode and invoke agree", "[parity][method]")
{
   using Args  = psio::reflect<Svc>::method_arg_tuple<8>;
   using Plain = std::tuple<U32s, std::vector<U32s>, Strs, Entries>;
   const Plain good{{1, 2}, {{1, 2}, {5}}, {"a", "b"}, {{"k", 1}, {"j", 1}}};
   const auto  valid = psio::method_args_to_pssz<Svc, 8>(std::get<0>(good), std::get<1>(good),
                                                         std::get<2>(good), std::get<3>(good));
   CHECK(args_all_agree<8>(valid));
   // What a writer without the constraints produces: the same widths (the
   // annotated tuple's), no value checks.
   auto unchecked = [](const Plain& value) {
      namespace pz            = psio::detail::pssz_impl;
      constexpr std::size_t W = psio::auto_pssz_width_v<Args>;
      const Args            args{value};
      std::vector<char>     out(pz::size_of_v<W, true>(args));
      psio::fast_buf_stream sink(out.data(), out.size());
      pz::encode_value<W, true>(args, sink);
      return out;
   };
   CHECK(unchecked(good) == valid);
   const std::vector<Plain> bad{
      {{2, 1}, {}, {}, {}},
      {{}, {{1, 1}}, {}, {}},
      {{}, {}, {"b", "a"}, {}},
      {{}, {}, {"a", "b", "c", "d", "e"}, {}},
      {{}, {}, {"123456789"}, {}},
      {{}, {}, {}, {{"k", 1}, {"k", 2}}},
   };
   for (const auto& value : bad)
   {
      CHECK_FALSE(no_throw([&] {
         (void)psio::method_args_to_pssz_checked<Svc, 8>(std::get<0>(value), std::get<1>(value),
                                                         std::get<2>(value), std::get<3>(value));
      }));
      // The fast write of the arguments: the frame the callee refuses.
      CHECK(psio::method_args_to_pssz<Svc, 8>(std::get<0>(value), std::get<1>(value),
                                              std::get<2>(value), std::get<3>(value)) ==
            unchecked(value));
      CHECK_FALSE(args_all_agree<8>(unchecked(value)));
   }
   sweep(valid, [](std::span<const char> bytes) { (void)args_all_agree<8>(bytes); });

   // Under a cap.
   const auto capped = psio::method_args_to_pssz<Svc, 9>(U32s{1, 2}, Strs{"a", "b"});
   CHECK(args_all_agree<9>(capped));
   {
      using CArgs = psio::reflect<Svc>::method_arg_tuple<9>;
      namespace pz            = psio::detail::pssz_impl;
      constexpr std::size_t W = psio::auto_pssz_width_v<CArgs>;
      for (const CArgs& args : {CArgs{U32s{2, 1}, Strs{}}, CArgs{U32s{}, Strs{"a", "a"}}})
      {
         std::vector<char>     out(pz::size_of_v<W, true>(args));
         psio::fast_buf_stream sink(out.data(), out.size());
         pz::encode_value<W, true>(args, sink);
         CHECK_FALSE(args_all_agree<9>(out));
      }
   }
   sweep(capped, [](std::span<const char> bytes) { (void)args_all_agree<9>(bytes); });

   // Text.
   const std::string text = psio::to_json(Args{good});
   CHECK(psio::validate_json<Args>(text).ok());
   CHECK(no_throw([&] { (void)psio::from_json<Args>(text); }));
   for (const auto& value : bad)
   {
      const std::string bad_text = psio::to_json(value);   // the positional array
      CHECK_FALSE(psio::validate_json<Args>(bad_text).ok());
      CHECK_FALSE(no_throw([&] { (void)psio::from_json<Args>(bad_text); }));
   }
}

TEST_CASE("parity: a result held packed", "[parity][method][buffer]")
{
   const Strs two{"alice", "bob"};
   // Owning and shared, from a value and from bytes.
   const psio::method_result_buffer<Svc, 1> owned{two};
   const psio::shared_method_result<Svc, 1> shared{owned.bytes()};
   const psio::shared_method_result<Svc, 1> other = shared;
   CHECK(other.data() == shared.data());
   CHECK(std::vector<char>(owned.bytes().begin(), owned.bytes().end()) ==
         psio::method_result_to_pssz<Svc, 1>(two));
   // The 1-byte offset table of the bounded list, not the plain list's.
   CHECK(owned.size() == 2 + 8);
   CHECK(psio::to_pssz(two).size() == 2 * 4 + 8);
   // view(): the trusted view of the result; as_view: the safe one.
   static_assert(std::is_same_v<decltype(owned.view()), psio::method_result_trusted_view<Svc, 1>>);
   static_assert(std::is_same_v<decltype(psio::as_view(shared)), psio::method_result_view<Svc, 1>>);
   CHECK(owned.view().size() == 2);
   CHECK(std::string_view{shared->at(1)} == "bob");
   CHECK(std::string_view{psio::as_view(shared)[0]} == "alice");
   // unpack(): the value, read with the result's own widths.
   CHECK(owned.unpack() == two);
   CHECK(shared.unpack() == two);
   CHECK(shared.view().unpack() == two);
   Strs back;
   CHECK(shared.decode_checked(back).ok());
   CHECK(back == two);
   CHECK(owned == psio::method_result_buffer<Svc, 1>::from(two));
   // Default: the encoding of the default result.
   CHECK(psio::method_result_buffer<Svc, 1>{}.unpack().empty());
   // A value or bytes outside returns(...) is refused, by status or by
   // codec_abort.
   psio::method_result_buffer<Svc, 1> out{two};
   CHECK_FALSE(psio::method_result_buffer<Svc, 1>::from_checked(Strs{"b", "a"}, out).ok());
   CHECK_FALSE(psio::method_result_buffer<Svc, 1>::checked(psio::to_pssz(two), out).ok());
   CHECK(out.unpack() == two);
   CHECK_FALSE(no_throw([&] { (void)psio::shared_method_result<Svc, 1>{Strs(5, "x")}; }));
   CHECK_FALSE(
      no_throw([&] { (void)psio::shared_method_result<Svc, 0>{psio::to_pssz(U32s{2, 1})}; }));
   // A result whose returns(...) changes no byte is still validated as
   // the method's result (a plain pssz_buffer<U32s> would accept this).
   CHECK(psio::validate_pssz<U32s>(psio::to_pssz(U32s{2, 1})).ok());
   // Moving the bytes out, and into a shared buffer.
   auto moved = psio::method_result_buffer<Svc, 1>{two};
   const auto as_shared = psio::to_shared(std::move(moved));
   CHECK(as_shared.unpack() == two);

   // The buffer of the view's own format unpacks too.
   using Generic = psio::buffer<Strs, psio::method_result_view<Svc, 1>::format_type,
                                psio::storage::shared>;
   const auto generic = Generic::unchecked(psio::method_result_to_pssz<Svc, 1>(two));
   CHECK(generic.unpack() == two);
   CHECK(generic.validate().ok());
   CHECK(Generic{two}.unpack() == two);
   CHECK_FALSE(no_throw([&] { (void)Generic{psio::to_pssz(two)}; }));

   // Arguments.
   using Args = psio::reflect<Svc>::method_arg_tuple<9>;
   const psio::method_args_buffer<Svc, 9> args{
      psio::method_args_to_pssz<Svc, 9>(U32s{1, 2}, Strs{"a"})};
   static_assert(std::is_same_v<psio::method_args_buffer<Svc, 9>, psio::pssz_buffer<Args>>);
   static_assert(std::is_same_v<decltype(args.view()), psio::method_args_trusted_view<Svc, 9>>);
   CHECK(std::get<0>(args.unpack()) == U32s{1, 2});
   CHECK(std::string_view{args.view().get<1>()[0]} == "a");
   const psio::shared_method_args<Svc, 9> shared_args = psio::to_shared(args);
   CHECK(std::get<1>(shared_args.unpack()) == Strs{"a"});
}
